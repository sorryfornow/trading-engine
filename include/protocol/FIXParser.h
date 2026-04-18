#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "core/Order.h"
#include "protocol/SymbolRegistry.h"

// ─── FIX 4.2 Message ─────────────────────────────────────────────────────────
// Real FIX uses SOH (0x01) as field delimiter. We support both SOH and '|' so
// tests can use human-readable messages while production uses real protocol.
// ─────────────────────────────────────────────────────────────────────────────

struct FIXMessage {
    enum Type : uint8_t { NewOrder, Cancel, Unknown };

    uint32_t id;           // 0   ClOrdID     (tag 11) — this order's ID
    uint32_t orig_id;      // 4   OrigClOrdID (tag 41) — order to cancel
    int      tick;         // 8   Price as integer tick (tag 44)
    int      qty;          // 12  OrderQty (tag 38)
    Type     type;         // 16  Derived from MsgType (tag 35)
    Side     side;         // 17  Side (tag 54): 1=Buy, 2=Sell
    uint16_t symbol_id;    // 18  Resolved from Symbol (tag 55)
    uint32_t seq_num;      // 20  MsgSeqNum (tag 34) — for session layer
    // 24 bytes total
};

class FIXParser {
public:
    static constexpr char SOH = '\x01';  // real FIX field delimiter

    // ─── find_tag10 ──────────────────────────────────────────────────────────
    // Locate the "10=" trailer tag, scanning backwards from the end.
    // tag 10 is always the last field in a FIX message, so scanning from the
    // end is faster than a forward scan. Returns nullptr if not found.
    // ─────────────────────────────────────────────────────────────────────────
    static const char* find_tag10(const char* msg, std::size_t len, char delim) {
        if (len < 6) return nullptr;  // "10=0|" minimum

        // Search backwards. Stop at i=0 explicitly to avoid size_t underflow.
        for (std::size_t i = len - 4; ; --i) {
            if (msg[i] == '1' && msg[i+1] == '0' && msg[i+2] == '=' &&
                (i == 0 || msg[i-1] == delim)) {
                return msg + i;
            }
            if (i == 0) break;
        }
        return nullptr;
    }

    // ─── Framing ─────────────────────────────────────────────────────────────
    // Find the end of a complete FIX message in a TCP byte stream.
    // Returns bytes consumed (including the trailing delimiter after "10=xxx"),
    // or 0 if the buffer doesn't yet hold a complete message.
    // ─────────────────────────────────────────────────────────────────────────
    static std::size_t frame(const char* buf, std::size_t len, char delim = SOH) {
        if (len < 20) return 0;

        // Must scan FORWARD to find the first "10=" boundary.
        // find_tag10 scans backward and would return the last tag10 in the
        // buffer — wrong when multiple messages are concatenated.
        for (std::size_t i = 0; i + 5 <= len; i++) {
            if (buf[i] == '1' && buf[i+1] == '0' && buf[i+2] == '=' &&
                (i == 0 || buf[i-1] == delim)) {
                std::size_t j = i + 3;
                while (j < len && buf[j] != delim) j++;
                if (j < len) return j + 1;
            }
        }
        return 0;
    }

    // ─── CheckSum ────────────────────────────────────────────────────────────
    // Sum of all bytes from the start of the message up to (not including)
    // the "10=" tag, mod 256. Formatted as 3-digit zero-padded in tag 10.
    // ─────────────────────────────────────────────────────────────────────────
    static uint8_t compute_checksum(const char* msg, std::size_t len) {
        uint32_t sum = 0;
        for (std::size_t i = 0; i < len; i++)
            sum += static_cast<uint8_t>(msg[i]);
        return static_cast<uint8_t>(sum % 256);
    }

    static bool validate_checksum(const char* msg, std::size_t len, char delim = SOH) {
        const char* tag10 = find_tag10(msg, len, delim);
        if (!tag10) return false;

        auto cksum_len = static_cast<std::size_t>(tag10 - msg);

        // Parse the 3-digit claimed checksum after "10="
        const char* val = tag10 + 3;
        uint32_t claimed = 0;
        while (val < msg + len && *val != delim) {
            if (*val < '0' || *val > '9') return false;  // reject non-digits
            claimed = claimed * 10 + static_cast<uint32_t>(*val - '0');
            val++;
        }

        return compute_checksum(msg, cksum_len) == static_cast<uint8_t>(claimed);
    }

    // ─── BodyLength ──────────────────────────────────────────────────────────
    // tag 9 value = number of bytes from the delimiter after tag 9 to the
    // "10=" tag (inclusive of the delimiter before "10=").
    // ─────────────────────────────────────────────────────────────────────────
    static bool validate_body_length(const char* msg, std::size_t len, char delim = SOH) {
        const char* ptr = msg;
        const char* end = msg + len;

        // Skip tag 8 (BeginString): advance past its delimiter
        while (ptr < end && *ptr != delim) ptr++;
        if (ptr >= end) return false;
        ptr++;

        // Expect tag 9=<BodyLength>
        if (ptr + 2 >= end || ptr[0] != '9' || ptr[1] != '=') return false;
        ptr += 2;

        int claimed_len = 0;
        while (ptr < end && *ptr != delim) {
            if (*ptr < '0' || *ptr > '9') return false;
            claimed_len = claimed_len * 10 + (*ptr - '0');
            ptr++;
        }
        if (ptr >= end) return false;
        ptr++;  // skip delimiter — body starts here

        const char* body_start = ptr;

        const char* tag10 = find_tag10(msg, len, delim);
        if (!tag10) return false;

        return static_cast<int>(tag10 - body_start) == claimed_len;
    }

    // ─── Parse ───────────────────────────────────────────────────────────────
    // Parse a single FIX message. Supports both SOH and '|' delimiters.
    // registry is optional; pass nullptr to skip symbol→id resolution.
    // validate=true checks BodyLength + CheckSum (production path).
    // ─────────────────────────────────────────────────────────────────────────
    static FIXMessage parse(const char* msg, std::size_t len,
                            const SymbolRegistry* registry = nullptr,
                            bool validate = false,
                            char delim = SOH) {
        FIXMessage m{};
        m.type      = FIXMessage::Unknown;
        m.symbol_id = SymbolRegistry::INVALID_ID;

        // Auto-detect delimiter: '|' wins if found before SOH
        if (delim == SOH) {
            for (std::size_t i = 0; i < len; i++) {
                if (msg[i] == '|') { delim = '|'; break; }
                if (msg[i] == SOH) { break; }
            }
        }

        if (validate) {
            if (!validate_body_length(msg, len, delim)) return m;
            if (!validate_checksum(msg, len, delim))    return m;
        }

        const char* end = msg + len;
        const char* ptr = msg;

        while (ptr < end) {
            // Parse tag number
            int tag = 0;
            while (ptr < end && *ptr != '=') {
                if (*ptr < '0' || *ptr > '9') goto next_field;  // non-digit: skip field
                tag = tag * 10 + (*ptr - '0');
                ptr++;
            }
            if (ptr >= end) break;
            ptr++;  // skip '='

            {
                const char* val = ptr;

                // Advance to next delimiter
                while (ptr < end && *ptr != delim) ptr++;

                switch (tag) {
                    case 35:  // MsgType
                        if (*val == 'D')      m.type = FIXMessage::NewOrder;
                        else if (*val == 'F') m.type = FIXMessage::Cancel;
                        break;
                    case 11:  m.id      = fast_atou(val, ptr); break;  // ClOrdID
                    case 41:  m.orig_id = fast_atou(val, ptr); break;  // OrigClOrdID
                    case 34:  m.seq_num = fast_atou(val, ptr); break;  // MsgSeqNum
                    case 54:  m.side    = (*val == '1') ? Side::Buy : Side::Sell; break;
                    case 44:  m.tick    = fast_atoi(val, ptr); break;
                    case 38:  m.qty     = fast_atoi(val, ptr); break;
                    case 55:
                        if (registry)
                            m.symbol_id = registry->lookup(val, static_cast<std::size_t>(ptr - val));
                        break;
                    default: break;  // 8=BeginString, 9=BodyLength, 10=CheckSum, 49/56/52=session
                }
            }

            next_field:
            if (ptr < end) ptr++;  // skip delimiter
        }

        return m;
    }

    // ─── Builder ─────────────────────────────────────────────────────────────
    // Generate a well-formed FIX 4.2 message with correct BodyLength + CheckSum.
    // Used by test clients and test_fix.cpp round-trip tests.
    // ─────────────────────────────────────────────────────────────────────────
    static std::size_t build(char* buf, std::size_t buf_size,
                             FIXMessage::Type type,
                             uint32_t id, const char* symbol, Side side,
                             int tick, int qty, uint32_t seq_num,
                             char delim = SOH) {
        // Step 1: build body (all tags except 8, 9, 10)
        char body[512];
        int body_len = 0;

        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "35=%c%c", (type == FIXMessage::NewOrder) ? 'D' : 'F', delim);
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "49=CLIENT%c", delim);
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "56=SERVER%c", delim);
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "34=%u%c", seq_num, delim);
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                             "52=20260413-00:00:00%c", delim);

        if (type == FIXMessage::NewOrder) {
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "11=%u%c", id, delim);
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "55=%s%c", symbol ? symbol : "", delim);
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "54=%c%c", (side == Side::Buy) ? '1' : '2', delim);
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "44=%d%c", tick, delim);
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "38=%d%c", qty, delim);
        } else {
            body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                 "41=%u%c", id, delim);
            if (symbol) {
                body_len += snprintf(body + body_len, sizeof(body) - body_len,
                                     "55=%s%c", symbol, delim);
            }
        }

        // Step 2: build header with correct BodyLength
        char header[64];
        int header_len = snprintf(header, sizeof(header), "8=FIX.4.2%c9=%d%c", delim, body_len, delim);
        if (header_len <= 0) return 0;  // snprintf error guard

        // Step 3: bounds check — "10=NNN<delim>" trailer is at most 7 bytes
        //   "10=" (3) + up to 3 digits + delimiter (1) = 7; use 8 for safety.
        // Cast each operand individually to avoid int overflow before widening.
        if (static_cast<std::size_t>(header_len) +
            static_cast<std::size_t>(body_len) + 8u > buf_size) return 0;

        // Step 4: compute checksum over header + body
        uint32_t sum = 0;
        for (int i = 0; i < header_len; i++) sum += static_cast<uint8_t>(header[i]);
        for (int i = 0; i < body_len;   i++) sum += static_cast<uint8_t>(body[i]);


        // Step 5: assemble header + body + trailer
        std::size_t total = 0;
        std::memcpy(buf + total, header, static_cast<std::size_t>(header_len));
        total += static_cast<std::size_t>(header_len);
        std::memcpy(buf + total, body,   static_cast<std::size_t>(body_len));
        total += static_cast<std::size_t>(body_len);
        // auto cksum = static_cast<uint8_t>(sum % 256u);
        total += static_cast<std::size_t>(
            snprintf(buf + total, buf_size - total, "10=%03u%c", static_cast<uint8_t>(sum % 256u), delim));

        return total;
    }

private:
    // Parse unsigned decimal integer from [begin, end). Stops on non-digit.
    static uint32_t fast_atou(const char* begin, const char* end) {
        uint32_t val = 0;
        while (begin < end && *begin >= '0' && *begin <= '9')
            val = val * 10 + static_cast<uint32_t>(*begin++ - '0');
        return val;
    }

    // Parse signed decimal integer from [begin, end). Stops on non-digit.
    static int fast_atoi(const char* begin, const char* end) {
        int val = 0;
        while (begin < end && *begin >= '0' && *begin <= '9')
            val = val * 10 + (*begin++ - '0');
        return val;
    }
};
