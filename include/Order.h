#pragma once
#include <cstdint>
#include <cstddef>

inline constexpr std::size_t CACHE_LINE_SIZE = 64;

enum class Side : uint8_t { Buy, Sell };

// Aligned to cache line boundary so each Order sits in exactly one cache line.
// Fields ordered hot → cold for prefetcher friendliness.
struct alignas(CACHE_LINE_SIZE) Order {
    // hot: accessed every match() iteration
    int      qty;
    Order*   prev = nullptr;
    Order*   next = nullptr;
    // warm: accessed only when order is fully filled
    uint64_t id;
    // cold: accessed only on add() / cancel()
    Side     side;
    double   price;

    Order() = default;
    Order(uint64_t id_, Side side_, double price_, int qty_)
        : qty(qty_), prev(nullptr), next(nullptr), id(id_), side(side_), price(price_) {}
};
