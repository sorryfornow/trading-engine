#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "core/Order.h"
#include "protocol/SymbolRegistry.h"

// ─── FIX 4.2 Protocol Constants ──────────────────────────────────────────────
// Real FIX uses SOH (0x01) as delimiter. We support both SOH and '|' so that
// tests can use human-readable '|' while production uses real SOH.
// ─────────────────────────────────────────────────────────────────────────────

struct FIXMessage {
    enum Type : uint8_t { NewOrder, Cancel, Unknown };

    uint32_t id;           // 0   ClOrdID (tag 11) or OrigClOrdID (tag 41)
    int      tick;         // 4   Price as integer tick (tag 44)
    int      qty;          // 8   OrderQty (tag 38)
    Type     type;         // 12  Derived from MsgType (tag 35)
    Side     side;         // 13  Side (tag 54): 1=Buy, 2=Sell
    uint16_t symbol_id;    // 14  Resolved from Symbol (tag 55)
    uint32_t seq_num;      // 16  MsgSeqNum (tag 34) — for session layer
    // total 20 bytes
};

class FIXParser {
public:
    static constexpr char SOH = '\x01';  // real FIX delimiter

    // ─── Framing ─────────────────────────────────────────────────────────────
    // Find the boundary of a complete FIX message in a byte stream.
    // Scans for "8=FIX" at the start and "10=xxx<delim>" at the end.
    //
    // Returns: number of bytes consumed (= one complete message), or 0 if
    //          the buffer does not yet contain a complete message.
    //
    // This is what the TCP gateway calls on its per-connection read buffer
    // to extract one message at a time.
    // ─────────────────────────────────────────────────────────────────────────
    static size_t frame(const char* buf, size_t len, char delim = SOH) {
        if (len < 20) return 0;  // minimum possible FIX message

        // Find "10=" followed by 3 digits + delim
        // Scan from the end backwards for efficiency
        for (size_t i = 0; i + 5 <= len; i++) {
            if (buf[i] == '1' && buf[i+1] == '0' && buf[i+2] == '=') {
                // Found "10=", now look for delimiter after the checksum value
                size_t j = i + 3;
                while (j < len && buf[j] != delim) j++;
                if (j < len) {
                    return j + 1;  // bytes consumed including trailing delimiter
                }
            }
        }
        return 0;  // incomplete message
    }

    // ─── CheckSum ────────────────────────────────────────────────────────────
    // FIX checksum: sum of all bytes from tag 8 to the delimiter before "10=",
    // mod 256, formatted as 3-digit zero-padded string (e.g. "178").
    // ─────────────────────────────────────────────────────────────────────────
    static uint8_t compute_checksum(const char* msg, size_t len_before_tag10) {
        uint32_t sum = 0;
        for (size_t i = 0; i < len_before_tag10; i++) {
            sum += static_cast<uint8_t>(msg[i]);
        }
        return static_cast<uint8_t>(sum % 256);
    }

    // Validate checksum embedded in message. Returns true if valid.
    static bool validate_checksum(const char* msg, size_t len, char delim = SOH) {
        // Find "10=" tag
        const char* tag10 = nullptr;
        for (size_t i = 0; i + 3 <= len; i++) {
            if (msg[i] == '1' && msg[i+1] == '0' && msg[i+2] == '='
                && (i == 0 || msg[i-1] == delim)) {
                tag10 = msg + i;
                break;
            }
        }
        if (!tag10) return false;

        // Bytes to checksum: from start to the delimiter before "10="
        size_t cksum_len = static_cast<size_t>(tag10 - msg);

        // Parse claimed checksum value (3 digits after "10=")
        const char* val = tag10 + 3;
        uint8_t claimed = 0;
        while (val < msg + len && *val != delim) {
            claimed = claimed * 10 + (*val - '0');
            val++;
        }

        return compute_checksum(msg, cksum_len) == claimed;
    }

    // ─── BodyLength ──────────────────────────────────────────────────────────
    // Validate tag 9 (BodyLength). The value should equal the number of bytes
    // from the delimiter after tag 9's value to the "10=" tag (inclusive of
    // the delimiter before "10=").
    // ─────────────────────────────────────────────────────────────────────────
    static bool validate_body_length(const char* msg, size_t len, char delim = SOH) {
        // Find tag 9
        const char* ptr = msg;
        const char* end = msg + len;

        // Skip tag 8 first: "8=...<delim>"
        while (ptr < end && *ptr != delim) ptr++;
        if (ptr >= end) return false;
        ptr++;  // skip delimiter after tag 8

        // Now ptr should be at "9=..."
        if (ptr + 2 >= end || ptr[0] != '9' || ptr[1] != '=') return false;
        ptr += 2;  // skip "9="

        // Parse body length value
        int claimed_len = 0;
        while (ptr < end && *ptr != delim) {
            claimed_len = claimed_len * 10 + (*ptr - '0');
            ptr++;
        }
        if (ptr >= end) return false;
        ptr++;  // skip delimiter after tag 9 value — body starts here

        const char* body_start = ptr;

        // Find "10=" to determine body end
        const char* tag10 = nullptr;
        for (size_t i = 0; i + 3 <= len; i++) {
            if (msg[i] == '1' && msg[i+1] == '0' && msg[i+2] == '='
                && (i == 0 || msg[i-1] == delim)) {
                tag10 = msg + i;
                break;
            }
        }
        if (!tag10) return false;

        int actual_len = static_cast<int>(tag10 - body_start);
        return actual_len == claimed_len;
    }

    // ─── Parse ───────────────────────────────────────────────────────────────
    // Parse a single FIX message. Supports both SOH and '|' delimiters.
    // registry is optional: pass nullptr to skip symbol resolution.
    //
    // Validation flags control whether to check BodyLength/CheckSum:
    //   - Production: validate = true  (reject corrupt messages)
    //   - Benchmark:  validate = false (skip overhead)
    // ─────────────────────────────────────────────────────────────────────────
    static FIXMessage parse(const char* msg, size_t len,
                            const SymbolRegistry* registry = nullptr,
                            bool validate = false,
                            char delim = SOH) {
        FIXMessage m{};
        m.type = FIXMessage::Unknown;
        m.symbol_id = SymbolRegistry::INVALID_ID;
        m.seq_num = 0;

        // Auto-detect delimiter: if '|' found before SOH, use '|'
        if (delim == SOH) {
            for (size_t i = 0; i < len; i++) {
                if (msg[i] == '|') { delim = '|'; break; }
                if (msg[i] == SOH) { break; }  // real SOH found first
            }
        }

        // Optional validation
        if (validate) {
            if (!validate_body_length(msg, len, delim)) {
                m.type = FIXMessage::Unknown;
                return m;
            }
            if (!validate_checksum(msg, len, delim)) {
                m.type = FIXMessage::Unknown;
                return m;
            }
        }

        const char* end = msg + len;
        const char* ptr = msg;

        while (ptr < end) {
            // Parse tag number: scan until '='
            int tag = 0;
            while (ptr < end && *ptr != '=') {
                tag = tag * 10 + (*ptr - '0');
                ptr++;
            }
            if (ptr >= end) break;
            ptr++;  // skip '='

            // Value starts here
            const char* val = ptr;

            // Scan until delimiter or end
            while (ptr < end && *ptr != delim)
                ptr++;

            // Process tag
            switch (tag) {
                case 35:  // MsgType: D=NewOrderSingle, F=OrderCancelRequest
                    if (*val == 'D')      m.type = FIXMessage::NewOrder;
                    else if (*val == 'F') m.type = FIXMessage::Cancel;
                    break;
                case 11:  // ClOrdID (new order)
                    m.id = fast_atou(val, ptr);
                    break;
                case 41:  // OrigClOrdID (cancel)
                    m.id = fast_atou(val, ptr);
                    break;
                case 34:  // MsgSeqNum
                    m.seq_num = fast_atou(val, ptr);
                    break;
                case 54:  // Side: 1=Buy, 2=Sell
                    m.side = (*val == '1') ? Side::Buy : Side::Sell;
                    break;
                case 44:  // Price → stored as raw tick
                    m.tick = fast_atoi(val, ptr);
                    break;
                case 38:  // OrderQty
                    m.qty = fast_atoi(val, ptr);
                    break;
                case 55:  // Symbol (e.g. "AAPL")
                    if (registry) {
                        m.symbol_id = registry->lookup(val, static_cast<size_t>(ptr - val));
                    }
                    break;
                case 8:   // BeginString — skip (e.g. "FIX.4.2")
                case 9:   // BodyLength — already validated above if needed
                case 10:  // CheckSum  — already validated above if needed
                case 49:  // SenderCompID — session layer (future)
                case 56:  // TargetCompID — session layer (future)
                case 52:  // SendingTime  — session layer (future)
                    break;
                default:
                    break;
            }

            if (ptr < end) ptr++;  // skip delimiter
        }

        return m;
    }

    // ─── Builder (for testing / client-side) ─────────────────────────────────
    // Build a well-formed FIX message string with correct BodyLength and CheckSum.
    // Writes into caller-provided buffer. Returns number of bytes written.
    // ─────────────────────────────────────────────────────────────────────────
    static size_t build(char* buf, size_t buf_size,
                        FIXMessage::Type type,
                        uint32_t id, const char* symbol, Side side,
                        int tick, int qty, uint32_t seq_num,
                        char delim = SOH) {
        // Step 1: Build body (everything between tag 9 and tag 10)
        char body[512];
        int body_len = 0;

        // Tag 35 (MsgType) — must be first in body
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "35=%c%c", (type == FIXMessage::NewOrder) ? 'D' : 'F', delim);

        // Tag 49 (SenderCompID)
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "49=CLIENT%c", delim);

        // Tag 56 (TargetCompID)
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "56=SERVER%c", delim);

        // Tag 34 (MsgSeqNum)
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "34=%u%c", seq_num, delim);

        // Tag 52 (SendingTime) — placeholder
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "52=20260413-00:00:00%c", delim);

        if (type == FIXMessage::NewOrder) {
            // Tag 11 (ClOrdID)
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "11=%u%c", id, delim);
            // Tag 55 (Symbol)
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "55=%s%c", symbol, delim);
            // Tag 54 (Side)
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "54=%c%c", (side == Side::Buy) ? '1' : '2', delim);
            // Tag 44 (Price/tick)
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "44=%d%c", tick, delim);
            // Tag 38 (OrderQty)
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "38=%d%c", qty, delim);
        } else {
            // Cancel: Tag 41 (OrigClOrdID)
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "41=%u%c", id, delim);
            if (symbol) {
                body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                     "55=%s%c", symbol, delim);
            }
        }

        // Step 2: Build header (tag 8 + tag 9)
        char header[64];
        int header_len = snprintf(header, sizeof(header),
                                  "8=FIX.4.2%c9=%d%c", delim, body_len, delim);

        // Step 3: Compute checksum over header + body
        uint32_t sum = 0;
        for (int i = 0; i < header_len; i++)
            sum += static_cast<uint8_t>(header[i]);
        for (int i = 0; i < body_len; i++)
            sum += static_cast<uint8_t>(body[i]);
        uint8_t cksum = static_cast<uint8_t>(sum % 256);

        // Step 4: Assemble: header + body + "10=xxx<delim>"
        size_t total = 0;
        if (static_cast<size_t>(header_len + body_len + 8) > buf_size) return 0;

        std::memcpy(buf + total, header, header_len);
        total += header_len;
        std::memcpy(buf + total, body, body_len);
        total += body_len;
        total += snprintf(buf + total, buf_size - total,
                          "10=%03u%c", cksum, delim);

        return total;
    }

private:
    static uint32_t fast_atou(const char* begin, const char* end) {
        uint32_t val = 0;
        while (begin < end) {
            val = val * 10 + (*begin - '0');
            begin++;
        }
        return val;
    }

    static int fast_atoi(const char* begin, const char* end) {
        int val = 0;
        while (begin < end) {
            val = val * 10 + (*begin - '0');
            begin++;
        }
        return val;
    }
};
