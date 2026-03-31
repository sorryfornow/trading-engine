#pragma once
#include <cstdint>
#include <cstddef>

inline constexpr std::size_t CACHE_LINE_SIZE = 64;

enum class Side : uint8_t { Buy, Sell };

// Aligned to cache line boundary so each Order sits in exactly one cache line.
// Prevents a single Order from straddling two cache lines (false sharing / extra miss).
struct alignas(CACHE_LINE_SIZE) Order {
    uint64_t id;
    Side     side;
    double   price;
    int      qty;
    Order*   prev = nullptr;
    Order*   next = nullptr;
    // 48 bytes used, 16 bytes padding to fill 64-byte cache line
};
