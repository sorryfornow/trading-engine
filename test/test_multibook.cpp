#include "engine/MultiBookEngine.h"
#include "core/PriceLevelBook.h"
#include "protocol/SymbolRegistry.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

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

        struct Tally { int count = 0; Fill last{}; } tally;
        auto cb = +[](void* ctx, const Fill& f) {
            auto* t = static_cast<Tally*>(ctx);
            t->count++;
            t->last = f;
        };

        // Sell on AAPL — should NOT match buy on MSFT
        engine.add_and_match(aapl, Order{10, Side::Buy,  1000, 100});
        engine.add_and_match(msft, Order{20, Side::Sell, 1000, 100}, cb, &tally);

        // Both should still be resting, and nothing traded
        assert(engine.get(aapl)->best_bid() == 1000);
        assert(engine.get(msft)->best_ask() == 1000);
        assert(tally.count == 0);

        // Now cross within AAPL
        engine.add_and_match(aapl, Order{11, Side::Sell, 1000, 100}, cb, &tally);
        assert(engine.get(aapl)->best_bid() == 0);   // book empty

        // The Fill must identify AAPL, not MSFT — create_book() stamped it.
        assert(tally.count == 1);
        assert(tally.last.symbol_id == aapl);
        assert(tally.last.bid_id == 10 && tally.last.ask_id == 11);
        assert(tally.last.qty == 100 && tally.last.tick == 1000);
        assert(tally.last.bid_filled && tally.last.ask_filled);
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

    // Test 9: an off-grid price is rejected rather than snapped
    section("Test 9: Reject Off-Grid Price");
    {
        // TestBook has TICK=5, so the only legal ticks are 990/995/.../1010.
        // 992 is inside the range but not on the grid. to_index() divides, so
        // without a check it would truncate to index 0 and the order would
        // rest at 990 — a price the client never sent, with no reject.
        Engine engine;
        engine.create_book(0);

        assert(engine.add_and_match(0, Order{1, Side::Buy, 992, 10}) == false);
        assert(engine.get(0)->best_bid() == 0);        // nothing rested

        assert(engine.add_and_match(0, Order{2, Side::Sell, 1007, 10}) == false);
        assert(engine.get(0)->best_ask() == 0);

        // Both grid boundaries are still accepted.
        assert(engine.add_and_match(0, Order{3, Side::Buy,   990, 10}) == true);
        assert(engine.add_and_match(0, Order{4, Side::Sell, 1010, 10}) == true);
        assert(engine.get(0)->best_bid() == 990);
        assert(engine.get(0)->best_ask() == 1010);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 10: Fill contents across partial and multi-level executions
    section("Test 10: Fill Reporting");
    {
        Engine engine;
        engine.create_book(0);

        struct Log { std::vector<Fill> fills; } log;
        auto cb = +[](void* ctx, const Fill& f) {
            static_cast<Log*>(ctx)->fills.push_back(f);
        };

        // Resting buy 100 @ 1000; incoming sell 40 @ 1000 partially fills it.
        engine.add_and_match(0, Order{1, Side::Buy, 1000, 100}, cb, &log);
        assert(log.fills.empty());                     // nothing crossed yet

        engine.add_and_match(0, Order{2, Side::Sell, 1000, 40}, cb, &log);
        assert(log.fills.size() == 1);
        {
            const Fill& f = log.fills[0];
            assert(f.bid_id == 1 && f.ask_id == 2);
            assert(f.qty == 40);
            assert(f.tick == 1000);
            assert(f.bid_filled == false);             // 60 still resting
            assert(f.ask_filled == true);              // aggressor consumed
        }
        assert(engine.get(0)->best_bid() == 1000);     // remainder rests

        // A sell that sweeps the rest and then some.
        log.fills.clear();
        engine.add_and_match(0, Order{3, Side::Sell, 1000, 60}, cb, &log);
        assert(log.fills.size() == 1);
        assert(log.fills[0].bid_id == 1 && log.fills[0].ask_id == 3);
        assert(log.fills[0].qty == 60);
        assert(log.fills[0].bid_filled && log.fills[0].ask_filled);
        assert(engine.get(0)->best_bid() == 0);        // book drained

        // One aggressor sweeping two price levels emits two Fills, best price
        // first, each naming the resting order it hit.
        log.fills.clear();
        engine.add_and_match(0, Order{10, Side::Sell, 1000, 50}, cb, &log);
        engine.add_and_match(0, Order{11, Side::Sell, 1005, 50}, cb, &log);
        assert(log.fills.empty());

        engine.add_and_match(0, Order{12, Side::Buy, 1005, 80}, cb, &log);
        assert(log.fills.size() == 2);
        assert(log.fills[0].tick == 1000 && log.fills[0].qty == 50);
        assert(log.fills[0].ask_id == 10 && log.fills[0].ask_filled);
        assert(log.fills[1].tick == 1005 && log.fills[1].qty == 30);
        assert(log.fills[1].ask_id == 11 && log.fills[1].ask_filled == false);
        assert(log.fills[1].bid_id == 12 && log.fills[1].bid_filled == true);

        // Every Fill carries the symbol the book was created for.
        for (const Fill& f : log.fills) assert(f.symbol_id == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    constexpr int total = 10;
    std::cout << "\n======================================\n";
    std::cout << "  MultiBook Results: " << passed << "/" << total << " passed\n";
    if (passed == total)
        std::cout << "  All MultiBook tests passed!\n";
    else
        std::cout << "  " << (total - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == total) ? 0 : 1;
}
