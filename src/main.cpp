#include "OrderBook.h"
#include <cassert>
#include <iostream>
#include <sstream>

// helper: capture stdout to verify print_book output
struct CaptureStdout {
    std::streambuf* orig;
    std::ostringstream buf;
    CaptureStdout()  { orig = std::cout.rdbuf(buf.rdbuf()); }
    std::string stop() { std::cout.rdbuf(orig); return buf.str(); }
};

void section(const std::string& name) {
    std::cout << "\n--------------------------------------\n";
    std::cout << "  " << name << "\n";
    std::cout << "--------------------------------------\n";
}

int main() {
    int passed = 0;

    // Test 1: Full match
    section("Test 1: Full Match");
    {
        OrderBook book;
        book.add(Order{1, Side::Buy,  1010, 100});
        book.add(Order{2, Side::Sell, 1010, 100});
        book.match();
        assert(book.bids.empty() && "bids should be empty");
        assert(book.asks.empty() && "asks should be empty");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 2: Partial match
    section("Test 2: Partial Match");
    {
        OrderBook book;
        book.add(Order{3, Side::Buy,  1020, 200});
        book.add(Order{4, Side::Sell, 1010,  50});
        book.match();
        assert(!book.bids.empty());
        assert(book.asks.empty());
        assert(book.bids.begin()->second.front().qty == 150 && "remaining qty should be 150");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 3: No match when spread exists
    section("Test 3: No Match (Spread)");
    {
        OrderBook book;
        book.add(Order{5, Side::Buy,  1000, 100});
        book.add(Order{6, Side::Sell, 1010, 100});
        book.match();
        assert(!book.bids.empty());
        assert(!book.asks.empty());
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 4: FIFO ordering at same price
    section("Test 4: FIFO Ordering");
    {
        OrderBook book;
        book.add(Order{7, Side::Buy,  1010, 100}); // arrives first
        book.add(Order{8, Side::Buy,  1010, 200}); // arrives second
        book.add(Order{9, Side::Sell, 1010, 100});
        book.match();
        assert(book.asks.empty());
        auto& remaining = book.bids.begin()->second;
        assert(remaining.size() == 1 && "only one bid should remain");
        assert(remaining.front().id == 8 && "remaining should be id=8");
        assert(remaining.front().qty == 200 && "id=8 should be untouched");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 5: Cancel order
    section("Test 5: Cancel Order");
    {
        OrderBook book;
        book.add(Order{10, Side::Buy, 1010, 100});
        book.cancel(10);
        assert(book.bids.empty() && "bids should be empty after cancel");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 6: Cancel non-existent order
    section("Test 6: Cancel Non-existent");
    {
        OrderBook book;
        book.add(Order{11, Side::Buy, 1010, 100});
        book.cancel(9999); // should silently do nothing
        assert(!book.bids.empty() && "bid should still be there");
        assert(book.bids.begin()->second.front().id == 11);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 7: Multi-level sweep
    section("Test 7: Multi-level Sweep");
    {
        OrderBook book;
        book.add(Order{12, Side::Sell, 1010,  50}); // best ask
        book.add(Order{13, Side::Sell, 1020,  50}); // second level
        book.add(Order{14, Side::Buy,  1030, 100}); // aggressive buy
        book.match();
        assert(book.bids.empty() && "buy should be fully consumed");
        assert(book.asks.empty() && "all asks should be consumed");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 8: print_book() output format
    section("Test 8: print_book() Format");
    {
        OrderBook book;
        book.add(Order{15, Side::Buy,  1000, 500});
        book.add(Order{16, Side::Sell, 1020, 300});
        CaptureStdout cap;
        book.print_book();
        std::string out = cap.stop();
        assert(out.find("ASKS") != std::string::npos);
        assert(out.find("BIDS") != std::string::npos);
        assert(out.find("1020") != std::string::npos && "ask tick 1020 should appear");
        assert(out.find("300")  != std::string::npos && "ask qty 300 should appear");
        assert(out.find("500")  != std::string::npos && "bid qty 500 should appear");
        std::cout << out;
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 9: Aggregated qty at same price level
    section("Test 9: Aggregated Qty");
    {
        OrderBook book;
        book.add(Order{17, Side::Buy, 1010, 100});
        book.add(Order{18, Side::Buy, 1010, 200});
        CaptureStdout cap;
        book.print_book();
        std::string out = cap.stop();
        assert(out.find("300") != std::string::npos && "should show total qty 300, not order count 2");
        std::cout << out;
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 10: Cancel order in middle of queue
    section("Test 10: Cancel Middle Order");
    {
        OrderBook book;
        book.add(Order{19, Side::Buy, 1010, 100});
        book.add(Order{20, Side::Buy, 1010, 200}); // cancel this one
        book.add(Order{21, Side::Buy, 1010, 300});
        book.cancel(20);
        auto& orders = book.bids.begin()->second;
        assert(orders.size() == 2 && "should have 2 orders left");
        assert(orders[0].id == 19 && "first should be id=19");
        assert(orders[1].id == 21 && "second should be id=21");
        std::cout << "PASSED\n";
        passed++;
    }

    // Summary
    std::cout << "\n======================================\n";
    std::cout << "  Results: " << passed << "/10 passed\n";
    if (passed == 10)
        std::cout << "  Phase 1 complete! Ready for Phase 2.\n";
    else
        std::cout << "  " << (10 - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == 10) ? 0 : 1;
}
