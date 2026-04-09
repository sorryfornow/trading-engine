#pragma once
#include <cstdint>
#include "Order.h"

struct FIXMessage {
    enum Type : uint8_t { NewOrder, Cancel, Unknown };

    uint32_t id;       // 0
    int      tick;     // 8
    int      qty;      // 12
    Type     type;     // 16
    Side     side;     // 17
    // 6 bytes padding to align to 24
};

class FIXParser {
public:
    static FIXMessage parse(const char* msg, size_t len);

};
