#include "OrderBook.h"
#include "PriceLevelBook.h"
#include "Timer.h"
#include <iostream>
#include <random>

// 5th param = pool size, must hold all orders across warmup + benchmark
using BenchBook = PriceLevelBook<990, 1010, 5, 1, 200000>;

// ─── Generate test orders ─────────────────────────────────────
std::vector<Order> make_orders(size_t n) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> price_dist(0, 4);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0, 1);

    static const double prices[] = {99.0, 99.5, 100.0, 100.5, 101.0};

    std::vector<Order> orders;
    orders.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        orders.push_back(Order{
                i + 1,
                side_dist(rng) ? Side::Buy : Side::Sell,
                prices[price_dist(rng)],
                qty_dist(rng)
        });
    }
    return orders;
}

// Helper to pre-populate a book — shared by both versions
template<typename BookT>
void prepopulate(BookT& book) {
    for (size_t i = 0; i < 20; ++i) {
        // i % 3 → 0, 1, 2 → 价格 99.0, 99.5, 100.0
        book.add(Order{1000 + i, Side::Buy,  99.0  + (i % 3) * 0.5, 100});
        // i % 2 → 0, 1   → 价格 100.5, 101.0，
        book.add(Order{2000 + i, Side::Sell, 100.5 + (i % 2) * 0.5, 100});
    }
}

// ─── Benchmark: add() latency ────────────────────────
Timer::Stats bench_add(size_t n_samples) {
    auto orders = make_orders(n_samples);
    std::vector<uint64_t> latencies;
    latencies.reserve(n_samples);

    for (size_t i = 0; i < n_samples; ++i) {
        OrderBook book;
        uint64_t t0 = Timer::now();
        book.add(orders[i]);
        uint64_t t1 = Timer::now();
        latencies.push_back(Timer::to_ns(t1 - t0));
    }
    return Timer::compute(latencies);
}

// Templated benchmark — works for both OrderBook and PriceLevelBook
template<typename BookT>
Timer::Stats bench_add_match(size_t n_samples) {
    auto orders = make_orders(n_samples);
    std::vector<uint64_t> latencies;
    latencies.reserve(n_samples);

    BookT book;
    prepopulate(book);

    std::streambuf* orig = std::cout.rdbuf(nullptr);
    for (size_t i = 0; i < n_samples; ++i) {
        uint64_t t0 = Timer::now();
        book.add(orders[i]);
        book.match();
        uint64_t t1 = Timer::now();
        latencies.push_back(Timer::to_ns(t1 - t0));
    }
    std::cout.rdbuf(orig);
    return Timer::compute(latencies);
}

int main() {
    const size_t N = 100'000;

    std::cout << "================================================\n";
    std::cout << "  Trading Engine Benchmark\n";
    std::cout << "  Platform : " << Timer::platform() << "\n";
    std::cout << "  Samples  : " << N << "\n";
    std::cout << "================================================\n";

    // Warmup
    {
        auto warmup = make_orders(1000);
        OrderBook  b1; prepopulate(b1);
        BenchBook  b2; prepopulate(b2);
        std::streambuf* orig = std::cout.rdbuf(nullptr);
        for (auto& o : warmup) {
            b1.add(o); b1.match();
            b2.add(o); b2.match();
        }
        std::cout.rdbuf(orig);
    }

    // ── std::map 版本 / std::map version ─────────────────────────────────────
    auto slow_add   = bench_add(N);
    Timer::print("OrderBook add() [std::map]", slow_add);

    auto slow_match = bench_add_match<OrderBook>(N);
    Timer::print("OrderBook add()+match() [std::map]", slow_match);

    // ── 数组版本 / Array version ──────────────────────────────────────────────
    auto fast_match = bench_add_match<BenchBook>(N);
    Timer::print("PriceLevelBook add()+match() [array]", fast_match);

    // ── 对比 / Comparison ─────────────────────────────────────────────────────
    std::cout << "\n================================================\n";
    std::cout << "  Summary\n";
    std::cout << "================================================\n";
    std::cout << "                   p50        p99       p99.9\n";
    std::cout << "  std::map    : "
              << std::setw(7) << slow_match.p50  << "ns  "
              << std::setw(7) << slow_match.p99  << "ns  "
              << std::setw(7) << slow_match.p999 << "ns\n";
    std::cout << "  array       : "
              << std::setw(7) << fast_match.p50  << "ns  "
              << std::setw(7) << fast_match.p99  << "ns  "
              << std::setw(7) << fast_match.p999 << "ns\n";
    std::cout << "  p99 gain    : "
              << slow_match.p99 / std::max(fast_match.p99, (uint64_t)1)
              << "x faster\n";
    std::cout << "  target p99  : < 500 ns  "
              << (fast_match.p99 < 500 ? "✓ PASSED" : "✗ NOT YET") << "\n";
    std::cout << "================================================\n";

    return 0;
}