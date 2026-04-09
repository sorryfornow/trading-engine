#pragma once
#include <cstdint>
#include <cstdlib>
#include "Order.h"

struct FIXMessage {
    enum Type : uint8_t { NewOrder, Cancel, Unknown };

    uint32_t id;       // 0
    int      tick;     // 4
    int      qty;      // 8
    Type     type;     // 12
    Side     side;     // 13
};

class FIXParser {
public:
    // Parse FIX message: "8=FIX.4.2|35=D|11=1001|54=1|44=100.5|38=200|" real FIX uses \x01（SOH）
    // No std::string, no malloc — pure pointer scanning.
    static FIXMessage parse(const char* msg, size_t len) {
        FIXMessage m{};
        m.type = FIXMessage::Unknown;

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

            // Scan until '|' (delimiter) or end
            while (ptr < end && *ptr != '|')
                ptr++;

            // Process tag
            switch (tag) {
                case 35:  // MsgType
                    if (*val == 'D')      m.type = FIXMessage::NewOrder;
                    else if (*val == 'F') m.type = FIXMessage::Cancel;
                    break;
                case 11:  // ClOrdID (new order)
                    m.id = fast_atou(val, ptr);
                    break;
                case 41:  // OrigClOrdID (cancel)
                    m.id = fast_atou(val, ptr);
                    break;
                case 54:  // Side: 1=Buy, 2=Sell
                    m.side = (*val == '1') ? Side::Buy : Side::Sell;
                    break;
                case 44:  // Price → stored as raw tick (caller converts)
                    m.tick = fast_atoi(val, ptr);
                    break;
                case 38:  // OrderQty
                    m.qty = fast_atoi(val, ptr);
                    break;
                default:
                    break;  // ignore unknown tags (8=FIX.4.2, etc.)
            }

            if (ptr < end) ptr++;  // skip '|'
        }

        return m;
    }

private:
    // Fast integer parse, no malloc, avoid locale and errno, assumes valid input (caller must ensure digits only)
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
