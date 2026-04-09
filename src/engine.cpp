#include "PriceLevelBook.h"
#include "SPSCQueue.h"
#include "FIXParser.h"
#include "Timer.h"
#include <thread>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <vector>
#include <random>

// ─── Configuration ───────────────────────────────────────────
using Book = PriceLevelBook<990, 1010, 5, 1, 200'000>;
static constexpr size_t QUEUE_SIZE = 8192;
static constexpr size_t NUM_ORDERS = 100'000;

// ─── Generate simulated FIX messages ─────────────────────────
// Pre-build raw FIX strings to simulate network input.
struct RawFIXMessage {
    char data[128];
    size_t len;
};

std::vector<RawFIXMessage> generate_fix_messages(size_t n) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> tick_dist(0, 4);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(1, 2);   // FIX: 1=Buy, 2=Sell
    std::uniform_int_distribution<int> cancel_dist(0, 9);  // 10% cancels

    static const int ticks[] = {990, 995, 1000, 1005, 1010};

    std::vector<RawFIXMessage> msgs;
    msgs.reserve(n);

    for (uint32_t i = 1; i <= n; i++) {
        RawFIXMessage m{};
        if (cancel_dist(rng) == 0 && i > 10) {
            // Cancel a recent order (~10% of messages)
            uint32_t cancel_id = i - rng() % 10 - 1;
            m.len = snprintf(m.data, sizeof(m.data),
                "8=FIX.4.2|35=F|41=%u|", cancel_id);
        } else {
            // New order
            m.len = snprintf(m.data, sizeof(m.data),
                "8=FIX.4.2|35=D|11=%u|54=%d|44=%d|38=%d|",
                i, side_dist(rng), ticks[tick_dist(rng)], qty_dist(rng));
        }
        msgs.push_back(m);
    }
    return msgs;
}

// ─── Main: two-thread engine ─────────────────────────────────
int main() {
    std::cout << "================================================\n";
    std::cout << "  Trading Engine — Dual Thread\n";
    std::cout << "  Platform : " << Timer::platform() << "\n";
    std::cout << "  Orders   : " << NUM_ORDERS << "\n";
    std::cout << "  Queue    : " << QUEUE_SIZE << " slots\n";
    std::cout << "================================================\n\n";

    // Pre-generate FIX messages (simulates network recv buffer)
    auto fix_msgs = generate_fix_messages(NUM_ORDERS);

    SPSCQueue<FIXMessage, QUEUE_SIZE> queue;
    Book book;

    // Latency tracking: measure engine-side processing time
    std::vector<uint64_t> latencies;
    latencies.reserve(NUM_ORDERS);

    uint64_t total_cancels = 0;

    // ── Network Thread (producer) ────────────────────────────
    // Parses FIX strings and pushes FIXMessages into queue.
    std::thread network([&] {
        for (size_t i = 0; i < NUM_ORDERS; i++) {
            FIXMessage msg = FIXParser::parse(fix_msgs[i].data, fix_msgs[i].len);
            while (!queue.push(msg)) {}  // spin until space
        }
    });

    // ── Engine Thread (consumer) ────────────────────────
    // Pops messages, builds Orders, runs matching engine.
    std::thread engine([&] {
        size_t processed = 0;
        while (processed < NUM_ORDERS) {
            FIXMessage msg;
            if (!queue.pop(msg)) continue;  // spin until data

            uint64_t t0 = Timer::now();

            if (msg.type == FIXMessage::NewOrder) {
                Order order(msg.id, msg.side, msg.tick, msg.qty);
                book.add(order);
                book.match();
            } else if (msg.type == FIXMessage::Cancel) {
                book.cancel(msg.id);
                total_cancels++;
            }

            uint64_t t1 = Timer::now();
            latencies.push_back(Timer::to_ns(t1 - t0));
            processed++;
        }
    });

    network.join();
    engine.join();

    // ── Results ───────────────────────────────
    auto stats = Timer::compute(latencies);
    Timer::print("Engine thread: add()+match() per message", stats);

    std::cout << "\n  Messages processed : " << latencies.size() << "\n";
    std::cout << "  Cancels            : " << total_cancels << "\n";

    std::cout << "\n================================================\n";
    std::cout << "  Dual-thread engine complete.\n";
    std::cout << "  p99 : " << stats.p99 << " ns";
    if (stats.p99 < 500)
        std::cout << "  ✓ < 500 ns\n";
    else
        std::cout << "  (target: < 500 ns)\n";
    std::cout << "================================================\n";

    return 0;
}
