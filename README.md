# Trading Engine

A low-latency order matching engine written in C++17, built from scratch as a learning project targeting HFT infrastructure roles.

## Architecture

```
Order  ──►  PriceLevelBook  ──►  MatchingEngine
              │
              ├── Array-indexed price levels (O(1) lookup)
              ├── ObjectPool (zero malloc in hot path)
              └── FIFO matching at each price level
```

### Components

| File | Description |
|------|-------------|
| `Order.h` | Order struct: id, side, price, qty |
| `PriceLevelBook.h` | Array-based order book with ObjectPool integration |
| `ObjectPool.h` | Pre-allocated memory pool, O(1) alloc/release |
| `OrderBook.h` | Reference implementation using `std::map` (for comparison) |
| `MatchingEngine.h` | Thin wrapper that calls add() + match() |
| `Timer.h` | Cross-platform nanosecond timer (rdtsc on x86, cntvct on ARM) |

### Key Design Decisions

**Array vs std::map** — Price levels are mapped to array indices via integer tick conversion. `O(1)` lookup instead of `O(log n)` tree traversal. Requires known price range at compile time.

**ObjectPool** — All Order objects are pre-allocated in a heap-backed pool. `add()` calls `pool.allocate()`, `cancel()`/`match()` call `pool.release()`. No `new`/`delete` in the hot path.

**Integer ticks** — Prices are converted to integer ticks (`100.50` → tick `1005`) to avoid floating-point comparison issues and enable direct array indexing.

## Benchmark Results

100,000 samples per run, Apple M series (ARM64).

| Version | p50 | p99 | p99.9 |
|---------|-----|-----|-------|
| `std::map` (baseline) | 125 ns | 4625 ns | 11041 ns |
| Array + ObjectPool | **41 ns** | **833 ns** | **1875 ns** |
| **Improvement** | **3x** | **5.5x** | **5.9x** |

> Note: ARM64 timer resolution is ~41.67 ns. Sub-tick variations are quantized. AMD/x86 results with `rdtsc` will provide nanosecond-level precision.

**Target:** p99 < 500 ns (to be validated on x86/AMD hardware)

## Build & Run

Requires C++17 and CMake 3.16+.

```bash
# Build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .

# Run tests (10 Phase 1 test cases)
./trading-engine

# Run benchmark
./bench
```

Or compile directly with g++/clang++:

```bash
# Tests
g++ -std=c++17 -O3 -Iinclude -o test src/main.cpp && ./test

# Benchmark
g++ -std=c++17 -O3 -Iinclude -o bench bench/bench.cpp && ./bench
```

## Tests

10 test cases covering core matching logic:

| # | Test | What it validates |
|---|------|-------------------|
| 1 | Full Match | Both orders fully filled, book empty |
| 2 | Partial Match | Remaining quantity correct after partial fill |
| 3 | No Match (Spread) | Orders stay when bid < ask |
| 4 | FIFO Ordering | Time priority at same price level |
| 5 | Cancel Order | Order removal works |
| 6 | Cancel Non-existent | Silent no-op on invalid ID |
| 7 | Multi-level Sweep | Aggressive order consumes multiple price levels |
| 8 | print_book() Format | Output contains expected fields |
| 9 | Aggregated Qty | Multiple orders at same price show total qty |
| 10 | Cancel Middle Order | Removing middle order preserves first/last |

## Roadmap

- [x] **Phase 1** — OrderBook + MatchingEngine, limit/market/cancel, 10 tests passing
- [x] **Phase 2** — PriceLevelBook (array), ObjectPool, cross-platform benchmark
- [ ] **Phase 3** — SPSCQueue (lock-free), FIX parser, dual-thread architecture
