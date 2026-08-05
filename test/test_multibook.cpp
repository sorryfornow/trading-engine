#include "engine/MultiBookEngine.h"
#include "core/PriceLevelBook.h"
#include "protocol/SymbolRegistry.h"
#include <cassert>
#include <iostream>
#include <string>

// Use a small pool for testing
using TestBook = PriceLevelBook<990, 1010, 5, 1, 1000>;
using Engine   = MultiBookEngine<TestBook>;

void section(const std::string& name) {
    std::cout << "\n--------------------------------------\n";
    std::cout << "  " << name << "\n";
    std::cout << "--------------------------------------\n";
}

int main() {
    int passed = 0;

    // Test 1: Registry basics
    section("Test 1: SymbolRegistry Register/Lookup");
    {
        SymbolRegistry reg;
        uint16_t a = reg.register_symbol("AAPL");
        uint16_t m = reg.register_symbol("MSFT");
        uint16_t t = reg.register_symbol("TSLA");
        assert(a == 0);
        assert(m == 1);
        assert(t == 2);
        // Re-registering returns same ID
        assert(reg.register_symbol("AAPL") == 0);
        assert(reg.lookup("AAPL") == 0);
        assert(reg.lookup("MSFT") == 1);
        assert(reg.lookup("NVDA") == SymbolRegistry::INVALID_ID);
        assert(reg.count() == 3);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 2: Book creation
    section("Test 2: Book Creation");
    {
        Engine engine;
        SymbolRegistry reg;
        uint16_t aapl = reg.register_symbol("AAPL");
        uint16_t msft = reg.register_symbol("MSFT");

        assert(engine.get(aapl) == nullptr);   // not created yet
        assert(engine.create_book(aapl) == true);
        assert(engine.create_book(aapl) == false);  // already exists
        assert(engine.create_book(msft) == true);
        assert(engine.get(aapl) != nullptr);
        assert(engine.get(msft) != nullptr);
        assert(engine.book_count() == 2);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 3: Per-symbol isolation
    section("Test 3: Per-Symbol Isolation");
    {
        Engine engine;
        SymbolRegistry reg;
        uint16_t aapl = reg.register_symbol("AAPL");
        uint16_t msft = reg.register_symbol("MSFT");
        engine.create_book(aapl);
        engine.create_book(msft);

        // Add a buy on AAPL at tick 1000, qty 100
        engine.add_and_match(aapl, Order{1, Side::Buy, 1000, 100});
        // Add a sell on MSFT at tick 1005, qty 50
        engine.add_and_match(msft, Order{1, Side::Sell, 1005, 50});

        // Neither should have matched (different books)
        assert(engine.get(aapl)->best_bid() == 1000);
        assert(engine.get(msft)->best_ask() == 1005);

        // Order IDs are per-symbol: both used id=1, no conflict
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 4: Matching within a single symbol
    section("Test 4: Cross-Symbol Match Isolation");
    {
        Engine engine;
        SymbolRegistry reg;
        uint16_t aapl = reg.register_symbol("AAPL");
        uint16_t msft = reg.register_symbol("MSFT");
        engine.create_book(aapl);
        engine.create_book(msft);

        int trades = 0;
        auto cb = +[](int, double) {};  // noop

        // Sell on AAPL — should NOT match buy on MSFT
        engine.add_and_match(aapl, Order{10, Side::Buy,  1000, 100});
        engine.add_and_match(msft, Order{20, Side::Sell, 1000, 100}, cb);

        // Both should still be resting
        assert(engine.get(aapl)->best_bid() == 1000);
        assert(engine.get(msft)->best_ask() == 1000);

        // Now cross within AAPL
        engine.add_and_match(aapl, Order{11, Side::Sell, 1000, 100}, cb);
        // Book should be empty
        assert(engine.get(aapl)->best_bid() == 0);
        (void)trades;
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 5: Cancel scoped per symbol
    section("Test 5: Cancel Per Symbol");
    {
        Engine engine;
        SymbolRegistry reg;
        uint16_t aapl = reg.register_symbol("AAPL");
        uint16_t msft = reg.register_symbol("MSFT");
        engine.create_book(aapl);
        engine.create_book(msft);

        engine.add_and_match(aapl, Order{100, Side::Buy, 1000, 50});
        engine.add_and_match(msft, Order{100, Side::Buy, 1005, 30});

        // Cancel id=100 on AAPL only
        engine.cancel(aapl, 100);
        assert(engine.get(aapl)->best_bid() == 0);    // gone
        assert(engine.get(msft)->best_bid() == 1005); // unaffected
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 6: Unknown symbol is silently dropped
    section("Test 6: Drop Unknown Symbol");
    {
        SymbolRegistry reg;
        reg.register_symbol("AAPL");            // id=0
        Engine engine;
        engine.create_book(0);

        // A Symbol (tag 55) the registry does not know leaves symbol_id at
        // INVALID_ID (65535). MAX_SYMBOLS is 256, so indexing books_ with it
        // unchecked reads ~512 KB past the array. Exercise that exact value —
        // an in-range id like 42 does not test this path at all.
        const uint16_t unknown = SymbolRegistry::INVALID_ID;
        assert(reg.lookup("NVDA") == unknown);

        assert(engine.add_and_match(unknown, Order{1, Side::Buy, 1000, 10}) == false);
        assert(engine.cancel(unknown, 1) == false);

        // An in-range id with no book allocated is a separate case.
        assert(engine.add_and_match(42, Order{2, Side::Buy, 1000, 10}) == false);
        assert(engine.cancel(42, 2) == false);

        // The one real book must be untouched by any of the above.
        assert(engine.book_count() == 1);
        assert(engine.get(0)->best_bid() == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 7: prices off the end of the book are rejected, not asserted
    section("Test 7: Reject Out-of-Range Price");
    {
        Engine engine;
        engine.create_book(0);

        // Book covers ticks 990..1010. Both ends are client-supplied values
        // arriving from the wire, so they must be rejected, never assert.
        assert(engine.add_and_match(0, Order{1, Side::Buy,  2000, 10}) == false);
        assert(engine.add_and_match(0, Order{2, Side::Sell,  500, 10}) == false);

        // A rejected order must leave no trace in the book.
        assert(engine.get(0)->best_bid() == 0);
        assert(engine.get(0)->best_ask() == 0);

        // In-range orders still work afterwards.
        assert(engine.add_and_match(0, Order{3, Side::Buy, 1000, 10}) == true);
        assert(engine.get(0)->best_bid() == 1000);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 8: order pool exhaustion is a reject, not a crash
    section("Test 8: Reject On Pool Exhaustion");
    {
        Engine engine;
        engine.create_book(0);

        // TestBook pool holds 1000 orders. Rest exactly that many, all on the
        // bid side at one price so nothing matches and nothing is released.
        for (uint32_t i = 0; i < 1000; i++)
            assert(engine.add_and_match(0, Order{i + 1, Side::Buy, 1000, 1}) == true);

        // The next one has nowhere to go.
        assert(engine.add_and_match(0, Order{9999, Side::Buy, 1000, 1}) == false);

        // Freeing one slot makes room again.
        assert(engine.cancel(0, 1) == true);
        assert(engine.add_and_match(0, Order{9999, Side::Buy, 1000, 1}) == true);
        std::cout << "PASSED\n";
        passed++;
    }

    constexpr int total = 8;
    std::cout << "\n======================================\n";
    std::cout << "  MultiBook Results: " << passed << "/" << total << " passed\n";
    if (passed == total)
        std::cout << "  All MultiBook tests passed!\n";
    else
        std::cout << "  " << (total - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == total) ? 0 : 1;
}
