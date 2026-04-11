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

    // Test 7: Symbol resolution with SymbolRegistry
    section("Test 7: Symbol Resolution");
    {
        SymbolRegistry reg;
        reg.register_symbol("AAPL");   // id=0
        reg.register_symbol("MSFT");   // id=1

        const char* msg1 = "35=D|11=7001|55=AAPL|54=1|44=1005|38=100|";
        auto m1 = FIXParser::parse(msg1, strlen(msg1), &reg);
        assert(m1.type == FIXMessage::NewOrder);
        assert(m1.symbol_id == 0);

        const char* msg2 = "35=D|11=7002|55=MSFT|54=2|44=990|38=50|";
        auto m2 = FIXParser::parse(msg2, strlen(msg2), &reg);
        assert(m2.symbol_id == 1);

        // Unknown symbol
        const char* msg3 = "35=D|11=7003|55=TSLA|54=1|44=500|38=10|";
        auto m3 = FIXParser::parse(msg3, strlen(msg3), &reg);
        assert(m3.symbol_id == SymbolRegistry::INVALID_ID);

        std::cout << "PASSED\n";
        passed++;
    }

    // Test 8: Backward compatible (no registry)
    section("Test 8: No Registry (Backward Compatible)");
    {
        const char* msg = "35=D|11=8001|55=AAPL|54=1|44=1005|38=100|";
        auto m = FIXParser::parse(msg, strlen(msg));  // no registry
        assert(m.type == FIXMessage::NewOrder);
        assert(m.id == 8001);
        assert(m.symbol_id == SymbolRegistry::INVALID_ID);  // not resolved
        std::cout << "PASSED\n";
        passed++;
    }

    constexpr int total = 8;

    // Summary
    std::cout << "\n======================================\n";
    std::cout << "  FIX Results: " << passed << "/" << total << " passed\n";
    if (passed == total)
        std::cout << "  All FIX tests passed!\n";
    else
        std::cout << "  " << (total - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == total) ? 0 : 1;
}
