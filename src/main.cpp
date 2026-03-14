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

    // Test 1: Full match / 完全撮合
    section("Test 1: Full Match");
    {
        OrderBook book;
        book.add(Order{1, Side::Buy,  101.0, 100});
        book.add(Order{2, Side::Sell, 101.0, 100});
        book.match();
        assert(book.bids.empty() && "bids should be empty");
        assert(book.asks.empty() && "asks should be empty");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 2: Partial match / 部分撮合
    section("Test 2: Partial Match");
    {
        OrderBook book;
        book.add(Order{3, Side::Buy,  102.0, 200});
        book.add(Order{4, Side::Sell, 101.0,  50});
        book.match();
        assert(!book.bids.empty());
        assert(book.asks.empty());
        assert(book.bids.begin()->second.front().qty == 150 && "remaining qty should be 150");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 3: No match when spread exists / 价格不交叉不成交
    section("Test 3: No Match (Spread)");
    {
        OrderBook book;
        book.add(Order{5, Side::Buy,  100.0, 100});
        book.add(Order{6, Side::Sell, 101.0, 100});
        book.match();
        assert(!book.bids.empty());
        assert(!book.asks.empty());
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 4: FIFO ordering at same price / 同价格时间优先
    section("Test 4: FIFO Ordering");
    {
        OrderBook book;
        book.add(Order{7, Side::Buy,  101.0, 100}); // arrives first
        book.add(Order{8, Side::Buy,  101.0, 200}); // arrives second
        book.add(Order{9, Side::Sell, 101.0, 100});
        book.match();
        assert(book.asks.empty());
        auto& remaining = book.bids.begin()->second;
        assert(remaining.size() == 1 && "only one bid should remain");
        assert(remaining.front().id == 8 && "remaining should be id=8");
        assert(remaining.front().qty == 200 && "id=8 should be untouched");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 5: Cancel order / 撤单
    section("Test 5: Cancel Order");
    {
        OrderBook book;
        book.add(Order{10, Side::Buy, 101.0, 100});
        book.cancel(10);
        assert(book.bids.empty() && "bids should be empty after cancel");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 6: Cancel non-existent order / 撤不存在的单
    section("Test 6: Cancel Non-existent");
    {
        OrderBook book;
        book.add(Order{11, Side::Buy, 101.0, 100});
        book.cancel(9999); // should silently do nothing
        assert(!book.bids.empty() && "bid should still be there");
        assert(book.bids.begin()->second.front().id == 11);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 7: Multi-level sweep / 多层级扫单
    section("Test 7: Multi-level Sweep");
    {
        OrderBook book;
        book.add(Order{12, Side::Sell, 101.0,  50}); // best ask
        book.add(Order{13, Side::Sell, 102.0,  50}); // second level
        book.add(Order{14, Side::Buy,  103.0, 100}); // aggressive buy
        book.match();
        assert(book.bids.empty() && "buy should be fully consumed");
        assert(book.asks.empty() && "all asks should be consumed");
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 8: print_book() output format / 打印格式
    section("Test 8: print_book() Format");
    {
        OrderBook book;
        book.add(Order{15, Side::Buy,  100.0, 500});
        book.add(Order{16, Side::Sell, 102.0, 300});
        CaptureStdout cap;
        book.print_book();
        std::string out = cap.stop();
        assert(out.find("ASKS") != std::string::npos);
        assert(out.find("BIDS") != std::string::npos);
        assert(out.find("102")  != std::string::npos && "ask price 102 should appear");
        assert(out.find("300")  != std::string::npos && "ask qty 300 should appear");
        assert(out.find("500")  != std::string::npos && "bid qty 500 should appear");
        std::cout << out;
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 9: Aggregated qty at same price level / 同价格合计 qty
    section("Test 9: Aggregated Qty");
    {
        OrderBook book;
        book.add(Order{17, Side::Buy, 101.0, 100});
        book.add(Order{18, Side::Buy, 101.0, 200});
        CaptureStdout cap;
        book.print_book();
        std::string out = cap.stop();
        // should show 300 (total qty), not 2 (order count)
        // 应该显示 300（总 qty），不是 2（订单数量）
        assert(out.find("300") != std::string::npos && "should show total qty 300, not order count 2");
        std::cout << out;
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 10: Cancel order in middle of queue / 撤中间的单
    section("Test 10: Cancel Middle Order");
    {
        OrderBook book;
        book.add(Order{19, Side::Buy, 101.0, 100});
        book.add(Order{20, Side::Buy, 101.0, 200}); // cancel this one
        book.add(Order{21, Side::Buy, 101.0, 300});
        book.cancel(20);
        auto& orders = book.bids.begin()->second;
        assert(orders.size() == 2 && "should have 2 orders left");
        assert(orders[0].id == 19 && "first should be id=19");
        assert(orders[1].id == 21 && "second should be id=21");
        std::cout << "PASSED\n";
        passed++;
    }

    // Summary / 总结
    std::cout << "\n======================================\n";
    std::cout << "  Results: " << passed << "/10 passed\n";
    if (passed == 10)
        std::cout << "  Phase 1 complete! Ready for Phase 2.\n";
    else
        std::cout << "  " << (10 - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == 10) ? 0 : 1;
}