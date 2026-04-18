#include "protocol/FIXParser.h"
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

    // ── Legacy tests (pipe delimiter, no validation) ─────────────────────

    // Test 1: NewOrderSingle (35=D) — simple pipe format
    section("Test 1: NewOrderSingle (pipe)");
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
    section("Test 2: CancelRequest (pipe)");
    {
        const char* msg = "8=FIX.4.2|35=F|41=2002|";
        auto m = FIXParser::parse(msg, strlen(msg));
        assert(m.type == FIXMessage::Cancel);
        assert(m.orig_id == 2002);  // tag 41 OrigClOrdID → orig_id
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 3: Sell side (54=2)
    section("Test 3: Sell Side (pipe)");
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
        auto m = FIXParser::parse(msg, strlen(msg));
        assert(m.type == FIXMessage::NewOrder);
        assert(m.id == 8001);
        assert(m.symbol_id == SymbolRegistry::INVALID_ID);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Real FIX format tests (build + parse + validate) ─────────────────

    // Test 9: Build + Parse round-trip (NewOrder, pipe delimiter)
    section("Test 9: Build + Parse Round-Trip (NewOrder)");
    {
        SymbolRegistry reg;
        reg.register_symbol("AAPL");

        char buf[512];
        std::size_t n = FIXParser::build(buf, sizeof(buf),
                                     FIXMessage::NewOrder, 9001, "AAPL",
                                     Side::Buy, 1005, 200, 1, '|');
        assert(n > 0);
        auto m = FIXParser::parse(buf, n, &reg, true, '|');
        assert(m.type == FIXMessage::NewOrder);
        assert(m.id == 9001);
        assert(m.symbol_id == 0);
        assert(m.side == Side::Buy);
        assert(m.tick == 1005);
        assert(m.qty == 200);
        assert(m.seq_num == 1);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 10: Build + Parse round-trip (Cancel)
    section("Test 10: Build + Parse Round-Trip (Cancel)");
    {
        SymbolRegistry reg;
        reg.register_symbol("MSFT");

        char buf[512];
        std::size_t n = FIXParser::build(buf, sizeof(buf),
                                     FIXMessage::Cancel, 5050, "MSFT",
                                     Side::Buy, 0, 0, 7, '|');
        assert(n > 0);
        auto m = FIXParser::parse(buf, n, &reg, true, '|');
        assert(m.type == FIXMessage::Cancel);
        assert(m.orig_id == 5050);  // tag 41 OrigClOrdID → orig_id
        assert(m.symbol_id == 0);
        assert(m.seq_num == 7);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 11: CheckSum validation
    section("Test 11: CheckSum Validation");
    {
        char buf[512];
        std::size_t n = FIXParser::build(buf, sizeof(buf),
                                     FIXMessage::NewOrder, 1100, "AAPL",
                                     Side::Sell, 990, 50, 3, '|');
        assert(n > 0);

        // Valid message should pass
        assert(FIXParser::validate_checksum(buf, n, '|') == true);

        // Corrupt one byte — should fail
        char corrupt[512];
        std::memcpy(corrupt, buf, n);
        corrupt[20] = 'Z';  // tamper with body
        assert(FIXParser::validate_checksum(corrupt, n, '|') == false);

        std::cout << "PASSED\n";
        passed++;
    }

    // Test 12: BodyLength validation
    section("Test 12: BodyLength Validation");
    {
        char buf[512];
        std::size_t n = FIXParser::build(buf, sizeof(buf),
                                     FIXMessage::NewOrder, 1200, "AAPL",
                                     Side::Buy, 1005, 100, 4, '|');
        assert(n > 0);
        assert(FIXParser::validate_body_length(buf, n, '|') == true);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 13: Frame detection
    section("Test 13: Frame Detection");
    {
        char buf[512];
        std::size_t n = FIXParser::build(buf, sizeof(buf),
                                     FIXMessage::NewOrder, 1300, "AAPL",
                                     Side::Buy, 1005, 100, 5, '|');
        assert(n > 0);

        // Complete message — frame returns full length
        assert(FIXParser::frame(buf, n, '|') == n);

        // Partial message — frame returns 0
        assert(FIXParser::frame(buf, n / 2, '|') == 0);

        // Two messages concatenated — frame returns first message length
        char buf2[1024];
        std::memcpy(buf2, buf, n);
        std::size_t n2 = FIXParser::build(buf2 + n, sizeof(buf2) - n,
                                      FIXMessage::Cancel, 1301, "AAPL",
                                      Side::Buy, 0, 0, 6, '|');
        assert(FIXParser::frame(buf2, n + n2, '|') == n);

        std::cout << "PASSED\n";
        passed++;
    }

    // Test 14: SOH delimiter build + parse
    section("Test 14: SOH Delimiter");
    {
        SymbolRegistry reg;
        reg.register_symbol("ES");

        char buf[512];
        std::size_t n = FIXParser::build(buf, sizeof(buf),
                                     FIXMessage::NewOrder, 1400, "ES",
                                     Side::Sell, 5000, 10, 99,
                                     FIXParser::SOH);
        assert(n > 0);

        auto m = FIXParser::parse(buf, n, &reg, true, FIXParser::SOH);
        assert(m.type == FIXMessage::NewOrder);
        assert(m.id == 1400);
        assert(m.symbol_id == 0);
        assert(m.side == Side::Sell);
        assert(m.tick == 5000);
        assert(m.qty == 10);
        assert(m.seq_num == 99);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 15: MsgSeqNum (tag 34)
    section("Test 15: MsgSeqNum");
    {
        const char* msg = "8=FIX.4.2|9=20|35=D|34=42|11=1500|54=1|44=1005|38=100|10=000|";
        // Don't validate (body length won't match this hand-crafted string)
        auto m = FIXParser::parse(msg, strlen(msg), nullptr, false, '|');
        assert(m.type == FIXMessage::NewOrder);
        assert(m.seq_num == 42);
        assert(m.id == 1500);
        std::cout << "PASSED\n";
        passed++;
    }

    constexpr int total = 15;

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
