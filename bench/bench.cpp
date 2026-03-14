#include "OrderBook.h"
#include "Timer.h"
#include <iostream>
#include <vector>
#include <random>

// ─── 生成测试订单 / Generate test orders ─────────────────────────────────────
// 模拟真实场景：随机价格在一定范围内波动
// Simulates realistic scenario: prices fluctuate within a range
std::vector<Order> make_orders(size_t n) {
    std::mt19937 rng(42);  // fixed seed，结果可复现 / reproducible
    // 价格在 99.0 ~ 101.0 之间，步长 0.5
    // Prices between 99.0 and 101.0, step 0.5
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

// ─── Benchmark: add() 延迟 / Benchmark: add() latency ────────────────────────
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

// ─── Benchmark: add() + match() 端到端延迟 ───────────────────────────────────
// Benchmark: end-to-end latency of add() + match()
Timer::Stats bench_add_match(size_t n_samples) {
    auto orders = make_orders(n_samples);
    std::vector<uint64_t> latencies;
    latencies.reserve(n_samples);

    // 预先建好一个有深度的 book，模拟真实环境
    // Pre-populate book to simulate a realistic environment
    OrderBook book;
    for (size_t i = 0; i < 20; ++i) {
        book.add(Order{1000 + i, Side::Buy,  99.0 + (i % 3) * 0.5, 100});
        book.add(Order{2000 + i, Side::Sell, 100.5 + (i % 3) * 0.5, 100});
    }

    // 重定向 cout 避免 TRADE 打印拖慢 benchmark
    // Redirect cout to suppress TRADE prints during timing
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

    // Warmup — 让 CPU 进入稳定状态 / warm up CPU
    {
        auto warmup = make_orders(1000);
        OrderBook book;
        for (auto& o : warmup) { book.add(o); book.match(); }
    }

    auto stats_add = bench_add(N);
    Timer::print("add() latency", stats_add);

    auto stats_match = bench_add_match(N);
    Timer::print("add() + match() latency", stats_match);

    std::cout << "\n------------------------------------------------\n";
    std::cout << "  目标 / Target (Phase 2 完成后 / after Phase 2):\n";
    std::cout << "  p99 < 500 ns\n";
    std::cout << "------------------------------------------------\n";

    return 0;
}