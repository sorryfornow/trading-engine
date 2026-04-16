#pragma once
#include <cstddef>
#include <cstring>
#include <functional>
#include "protocol/FIXParser.h"

// ─── Per-connection state ────────────────────────────────────────────────────
// Each TCP client gets a Connection object that owns a read buffer.
//
// TCP is a byte stream — a single read() may return half a FIX message,
// two FIX messages glued together, or anything in between. Connection
// accumulates bytes and uses FIXParser::frame() to extract complete
// messages one at a time.
//
// Buffer strategy: linear buffer with compaction. When a complete message
// is consumed, remaining bytes slide to the front. This is simpler than
// a ring buffer and fine for our message sizes (<512 bytes).
// ─────────────────────────────────────────────────────────────────────────────

class Connection {
public:
    static constexpr size_t BUF_SIZE = 4096;

    int fd = -1;
    char buf[BUF_SIZE];
    size_t used = 0;       // how many bytes are in buf
    char delim = '|';      // FIX delimiter: SOH in production, '|' for testing

    Connection() : fd(-1), used(0) {
        buf[0] = '\0';
    }

    explicit Connection(int fd_, char delim_ = '|')
        : fd(fd_), used(0), delim(delim_) {
        buf[0] = '\0';
    }

    // Append raw bytes from read() into the buffer.
    // Returns false if buffer is full (would overflow).
    bool append(const char* data, size_t len) {
        if (used + len > BUF_SIZE) return false;  // buffer overflow protection
        std::memcpy(buf + used, data, len);
        used += len;
        return true;
    }

    // Try to extract one complete FIX message from the buffer.
    // If found: parses it into FIXMessage, compacts the buffer, returns true.
    // If incomplete: returns false, buffer is unchanged.
    bool try_extract(FIXMessage& out,
                     const SymbolRegistry* registry = nullptr,
                     bool validate = false) {
        if (used == 0) return false;

        size_t msg_len = FIXParser::frame(buf, used, delim);
        if (msg_len == 0) return false;  // incomplete message

        // Parse the complete message
        out = FIXParser::parse(buf, msg_len, registry, validate, delim);

        // Compact: slide remaining bytes to front
        size_t remaining = used - msg_len;
        if (remaining > 0) {
            std::memmove(buf, buf + msg_len, remaining);
        }
        used = remaining;

        return true;
    }

    // Reset connection state (for reuse or cleanup).
    void reset() {
        fd = -1;
        used = 0;
    }
};
