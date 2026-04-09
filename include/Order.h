#pragma once
#include <cstdint>
#include <cstddef>

inline constexpr std::size_t CACHE_LINE_SIZE = 64;

enum class Side : uint8_t { Buy, Sell };

// Aligned to cache line boundary so each Order sits in exactly one cache line.
// Fields ordered hot → cold for prefetcher friendliness.
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
