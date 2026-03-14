#pragma once
#include <cstdint>

enum class Side { Buy, Sell };

struct Order {
    uint64_t id;
    Side     side;
    double   price;
    int      qty;
};
