#pragma once
#include <cstdint>
#include "Common.h"

enum class Side : uint8_t { Buy, Sell };

// Aligned to cache line boundary so each Order sits in exactly one cache line.
// Fields ordered hot → cold for prefetcher friendliness.
// MUST BE trivial (no constructors, destructors, virtuals) to be safely used in ObjectPool without extra overhead.

struct alignas(CACHE_LINE_SIZE) Order {
    // hot: accessed every match() iteration (offset 0-19, contiguous)
    Order*   prev = nullptr;  // 0
    Order*   next = nullptr;  // 8
    int      qty;             // 16
    // cold: accessed on add() / cancel() only
    int      tick;            // 20
    uint32_t id;              // 24
    Side     side;            // 28
    // 32 bytes used, 32 bytes padding to fill 64-byte cache line

    Order() = default;
    Order(uint32_t id_, Side side_, int tick_, int qty_)
        : prev(nullptr), next(nullptr), qty(qty_), tick(tick_), id(id_), side(side_) {}
};

// ─── Fill ────────────────────────────────────────────────────────────────────
// One execution produced by match(), handed to a TradeCallback.
//
// The two sides are named bid/ask rather than aggressor/resting on purpose.
// The book matches whatever crosses at the top of each side; only the caller
// knows which of these two IDs it just submitted, so labelling aggression down
// here would be a guess. Compare the IDs against the incoming order to decide.
//
// The *_filled flags carry what an ExecutionReport needs to set OrdStatus:
// true means that side was fully filled and has left the book, false means it
// rests with reduced quantity.
struct Fill {
    uint32_t bid_id;      // 0   buy order that took part
    uint32_t ask_id;      // 4   sell order that took part
    int      tick;        // 8   execution price, in ticks
    int      qty;         // 12  quantity filled by this execution
    uint16_t symbol_id;   // 16  stamped by the book; 0 when used standalone
    bool     bid_filled;  // 18  buy side fully filled and removed
    bool     ask_filled;  // 19  sell side fully filled and removed
    // 20 bytes
};
