#pragma once
#include <cstdint>
#include <ctime>
#include <algorithm>
#include <numeric>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>

// ─── 跨平台计时 / Cross-platform timing ─────────────────────────────────────
//
//   x86 / AMD64 (Linux)  → __rdtsc()            reads CPU timestamp counter
//   ARM64 / Apple M      → clock_gettime()       monotonic nanoseconds
//
// 用法 / Usage:
//   uint64_t t0 = Timer::now();
//   ... do work ...
//   uint64_t ns = Timer::to_ns(Timer::now() - t0);

namespace Timer {

#if defined(__x86_64__) || defined(_M_X64)
    // x86: 用 rdtsc 读 cycle，再除以频率换算成 ns
    // x86: read cycles via rdtsc, then divide by CPU frequency to get ns

    inline uint64_t now() {
        uint32_t lo, hi;
        __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

    // 估算 CPU 频率 (GHz) / Estimate CPU frequency in GHz
    // 用 sleep 100ms 前后的 cycle 差来推算
    // Measures cycle delta over a 100ms sleep
    inline double cpu_ghz() {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t c0 = now();
        // sleep ~100ms
        struct timespec sleep_time = {0, 100'000'000L};
        nanosleep(&sleep_time, nullptr);
        uint64_t c1 = now();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed_ns = (t1.tv_sec - t0.tv_sec) * 1e9
                          + (t1.tv_nsec - t0.tv_nsec);
        return (c1 - c0) / elapsed_ns;  // cycles per ns = GHz
    }

    // cycles → nanoseconds
    // 第一次调用时自动测量 CPU 频率
    // Auto-measures CPU frequency on first call
    inline uint64_t to_ns(uint64_t cycles) {
        static double ghz = cpu_ghz();
        return static_cast<uint64_t>(cycles / ghz);
    }

    inline const char* platform() { return "x86_64 (rdtsc)"; }

#elif defined(__aarch64__) || defined(__arm64__)
    // ARM64 / Apple M: use system virtual count register 用 CNTVCT_EL0 寄存器

    // allows the function to be defined in multiple translation units without violating the One Definition Rule (ODR)
    inline uint64_t now() {
        uint64_t val;
        __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
        return val;
    }

    // Get ARM timer frequency in Hz
    inline uint64_t timer_freq() {
        uint64_t freq;
        __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
        return freq;
    }

    // ticks → nanoseconds
    inline uint64_t to_ns(uint64_t ticks) {
        static uint64_t freq = timer_freq();
        return ticks * 1'000'000'000ULL / freq;
    }

    inline const char* platform() { return "ARM64 / Apple M (cntvct_el0)"; }

#else
    // ── Fallback: clock_gettime ──────────────────────────────────────────────
    inline uint64_t now() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
    }
    inline uint64_t to_ns(uint64_t ticks) { return ticks; }  // already ns
    inline const char* platform() { return "Generic (clock_gettime)"; }
#endif

    // ─── 统计工具 / Stats utilities ──────────────────────────────────────────────

    struct Stats {
        uint64_t p50, p90, p99, p999, min_ns, max_ns;
        double   mean_ns;
    };

    // 从一组纳秒样本计算百分位 / Compute percentiles from nanosecond samples
    inline Stats compute(std::vector<uint64_t>& samples) {
        std::sort(samples.begin(), samples.end());
        size_t n = samples.size();
        double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        return Stats{
                samples[n * 50 / 100],
                samples[n * 90 / 100],
                samples[n * 99 / 100],
                samples[n * 999 / 1000],
                samples.front(),
                samples.back(),
                sum / n
        };
    }

    // Print stats table
    inline void print(const std::string& label, const Stats& s) {
        std::cout << "\n[" << label << "]\n";
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  min   : " << s.min_ns  << " ns\n";
        std::cout << "  mean  : " << s.mean_ns << " ns\n";
        std::cout << "  p50   : " << s.p50     << " ns\n";
        std::cout << "  p90   : " << s.p90     << " ns\n";
        std::cout << "  p99   : " << s.p99     << " ns\n";
        std::cout << "  p99.9 : " << s.p999    << " ns\n";
        std::cout << "  max   : " << s.max_ns  << " ns\n";
    }

} // namespace Timer