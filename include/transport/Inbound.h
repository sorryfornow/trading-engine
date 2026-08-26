#pragma once
#include <cstdint>
#include "protocol/FIXParser.h"

// ─── Inbound ─────────────────────────────────────────────────────────────────
// What travels gateway → engine on the SPSC queue: a parsed message plus
// enough information to send a reply back to whoever sent it.
//
// FIXMessage itself stays a pure protocol structure. Routing is a transport
// concern, so it is wrapped here rather than bolted onto the message.
//
// Why both fd and conn_id
// -----------------------
// fd alone is not a safe identity. The kernel hands out the lowest free
// descriptor, so a closed connection's number is reused almost immediately —
// three clients connecting in sequence all get fd=5. By the time a fill made
// the round trip through the engine, that fd could belong to somebody else,
// and the reply would be delivered to the wrong client.
//
// conn_id is monotonic and never reused. fd gives an O(1) lookup into the
// gateway's connection map; conn_id is then compared against the connection
// found there, and a mismatch means the original client is gone and the reply
// must be dropped. This is the usual handle + generation pattern.
//
// 32 bytes: two per cache line.

struct Inbound {
    FIXMessage msg;         // 0   parsed order or cancel
    int        fd;          // 24  where it came from — a lookup key, not an identity
    uint32_t   conn_id;     // 28  generation counter; 0 means "no connection"
};

static_assert(sizeof(Inbound) <= 32, "Inbound should stay within half a cache line");
