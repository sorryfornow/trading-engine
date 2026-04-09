#include "FIXParser.h"
#include <cassert>
#include <iostream>
#include <cstring>

void section(const std::string& name) {
    std::cout << "\n--------------------------------------\n";
    std::cout << "  " << name << "\n";
    std::cout << "--------------------------------------\n";
}

int main() {
    int passed = 0;

    // Test 1: NewOrderSingle (35=D)
    section("Test 1: NewOrderSingle");
    {
        const char* msg = "8=FIX.4.2|35=D|11=1001|54=1|44=1005|38=200|";
        auto m = FIXParser::parse(msg, strlen(msg));
        assert(m.type == FIXMessage::NewOrder);
        assert(m.id == 1001);
        assert(m.side == Side::Buy);
        assert(m.tick == 1005);
        assert(m.qty == 200);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 2: CancelRequest (35=F)
    section("Test 2: CancelRequest");
    {
        const char* msg = "8=FIX.4.2|35=F|41=2002|";
        auto m = FIXParser::parse(msg, strlen(msg));
        assert(m.type == FIXMessage::Cancel);
        assert(m.id == 2002);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 3: Sell side (54=2)
    section("Test 3: Sell Side");
    {
        const char* msg = "35=D|11=3003|54=2|44=990|38=50|";
        auto m = FIXParser::parse(msg, strlen(msg));
        assert(m.type == FIXMessage::NewOrder);
        assert(m.id == 3003);
        assert(m.side == Side::Sell);
        assert(m.tick == 990);
        assert(m.qty == 50);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 4: Unknown message type
    section("Test 4: Unknown Type");
    {
        const char* msg = "35=X|11=4004|";
        auto m = FIXParser::parse(msg, strlen(msg));
        assert(m.type == FIXMessage::Unknown);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 5: Tags in different order
    section("Test 5: Tag Order Independence");
    {
        const char* msg = "38=100|44=1010|54=1|11=5005|35=D|";
        auto m = FIXParser::parse(msg, strlen(msg));
        assert(m.type == FIXMessage::NewOrder);
        assert(m.id == 5005);
        assert(m.side == Side::Buy);
        assert(m.tick == 1010);
        assert(m.qty == 100);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 6: Large order ID
    section("Test 6: Large Order ID");
    {
        const char* msg = "35=D|11=4000000000|54=1|44=995|38=1|";
        auto m = FIXParser::parse(msg, strlen(msg));
        assert(m.id == 4000000000u);
        std::cout << "PASSED\n";
        passed++;
    }

    // Summary
    std::cout << "\n======================================\n";
    std::cout << "  FIX Results: " << passed << "/6 passed\n";
    if (passed == 6)
        std::cout << "  All FIX tests passed!\n";
    else
        std::cout << "  " << (6 - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == 6) ? 0 : 1;
}
