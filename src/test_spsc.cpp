#include "SPSCQueue.h"
#include <cassert>
#include <iostream>
#include <thread>

void section(const std::string& name) {
    std::cout << "\n--------------------------------------\n";
    std::cout << "  " << name << "\n";
    std::cout << "--------------------------------------\n";
}

int main() {
    int passed = 0;

    // Test 1: Push and pop basic FIFO
    section("Test 1: Basic FIFO");
    {
        SPSCQueue<int, 4> q;
        assert(q.push(10));
        assert(q.push(20));
        assert(q.push(30));
        int val;
        assert(q.pop(val) && val == 10);
        assert(q.pop(val) && val == 20);
        assert(q.pop(val) && val == 30);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 2: Full queue returns false
    section("Test 2: Full Queue");
    {
        SPSCQueue<int, 4> q;  // capacity 4, usable 3
        assert(q.push(1));
        assert(q.push(2));
        assert(q.push(3));
        assert(!q.push(4));  // should fail, queue full
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 3: Empty queue returns false
    section("Test 3: Empty Queue");
    {
        SPSCQueue<int, 4> q;
        int val;
        assert(!q.pop(val));  // should fail, queue empty
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 4: Wrap around (ring buffer behavior)
    section("Test 4: Wrap Around");
    {
        SPSCQueue<int, 4> q;
        int val;
        // Fill and drain twice to force wrap-around
        for (int round = 0; round < 3; round++) {
            assert(q.push(round * 3 + 1));
            assert(q.push(round * 3 + 2));
            assert(q.push(round * 3 + 3));
            assert(q.pop(val) && val == round * 3 + 1);
            assert(q.pop(val) && val == round * 3 + 2);
            assert(q.pop(val) && val == round * 3 + 3);
        }
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 5: Works with Order type
    section("Test 5: Order Type");
    {
        SPSCQueue<Order, 8> q;
        assert(q.push(Order{1, Side::Buy, 1005, 200}));
        assert(q.push(Order{2, Side::Sell, 990, 50}));
        Order o;
        assert(q.pop(o) && o.id == 1 && o.side == Side::Buy && o.qty == 200);
        assert(q.pop(o) && o.id == 2 && o.side == Side::Sell && o.qty == 50);
        std::cout << "PASSED\n";
        passed++;
    }

    // Test 6: Two-thread correctness (1M messages, no loss, no reorder)
    section("Test 6: Two-Thread 1M Messages");
    {
        SPSCQueue<int, 1024> q;
        const int COUNT = 1'000'000;

        std::thread producer([&] {
            for (int i = 0; i < COUNT; i++)
                while (!q.push(i)) {}  // spin until space available
        });

        std::thread consumer([&] {
            for (int i = 0; i < COUNT; i++) {
                int val;
                while (!q.pop(val)) {}  // spin until data available
                assert(val == i && "out of order or lost message");
            }
        });

        producer.join();
        consumer.join();
        std::cout << "PASSED\n";
        passed++;
    }

    // Summary
    std::cout << "\n======================================\n";
    std::cout << " Results: " << passed << "/6 passed\n";
    if (passed == 6)
        std::cout << "  All tests passed!\n";
    else
        std::cout << "  " << (6 - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == 6) ? 0 : 1;
}
