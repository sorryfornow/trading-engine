# Trading Engine

A low-latency order matching engine written in C++17, built from scratch as a learning project targeting HFT infrastructure roles.

## Architecture

```
Order  ──►  PriceLevelBook  ──►  MatchingEngine
              │
              ├── Array-indexed price levels (O(1) lookup)
              ├── Intrusive doubly-linked list (O(1) insert/remove)
              ├── ObjectPool (zero malloc in hot path)
              ├── unordered_map id index (O(1) cancel)
              └── FIFO matching at each price level
```

### Components

| File | Description |
|------|-------------|
| `Order.h` | Cache-line-aligned Order struct with intrusive list pointers |
| `PriceLevelBook.h` | Array-based order book with linked list, ObjectPool, id index |
| `ObjectPool.h` | Heap-backed pre-allocated memory pool, O(1) alloc/release |
| `OrderBook.h` | Reference implementation using `std::map` (baseline comparison) |
| `MatchingEngine.h` | Thin wrapper that calls add() + match() |
| `Timer.h` | Cross-platform nanosecond timer (rdtsc on x86, cntvct on ARM) |

### Key Design Decisions

**Array vs std::map** — Price levels are mapped to array indices via integer tick conversion. `O(1)` lookup instead of `O(log n)` tree traversal. Requires known price range at compile time.

**Intrusive linked list vs std::vector** — Each price level stores orders in a doubly-linked list where `prev`/`next` pointers live inside the Order struct itself. `push_back()` and `remove()` are both O(1), eliminating the O(n) `memmove` caused by `vector::erase(begin())`.

**ObjectPool** — All Order objects are pre-allocated in a heap-backed pool. `add()` calls `pool.allocate()`, `cancel()`/`match()` call `pool.release()`. No `new`/`delete` in the hot path.

**O(1) cancel via id index** — An `unordered_map<order_id, Order*>` provides direct pointer lookup for cancellations. Previous approach scanned all price levels linearly — O(LEVELS × M).

**Cache line alignment** — `Order` is `alignas(64)` so each order sits in exactly one 64-byte cache line, preventing cross-line straddles on access.

**Integer ticks** — Prices are converted to integer ticks (`100.50` → tick `1005`) to avoid floating-point comparison issues and enable direct array indexing.

**TradeCallback** — `match()` takes an optional function pointer instead of writing to `std::cout`. Bench runs with `nullptr` (zero overhead); tests pass a lambda for output.

## Benchmark Results

100,000 samples per run. `-O3` optimization.

### AMD x86_64 (WSL2, rdtsc)

| Version | p50 | p99 | p99.9 |
|---------|-----|-----|-------|
| `std::map` (baseline) | 210 ns | 37690 ns | 60243 ns |
| Array + LinkedList + Pool | **30 ns** | **300 ns** | **1773 ns** |
| **Improvement** | **7x** | **125x** | **34x** |

### Apple M series (ARM64, cntvct_el0)

| Version | p50 | p99 | p99.9 |
|---------|-----|-----|-------|
| `std::map` (baseline) | 125 ns | 9083 ns | 21833 ns |
| Array + LinkedList + Pool | **41 ns** | **250 ns** | **1291 ns** |
| **Improvement** | **3x** | **36x** | **17x** |

> ARM64 timer resolution is ~41.67 ns. Sub-tick variations are quantized. x86 `rdtsc` provides true nanosecond precision.

**Target: p99 < 500 ns — PASSED on both platforms.**

### Optimization Timeline

| Step | Change | p99 (Apple M) |
|------|--------|---------------|
| Baseline | `std::map` | ~5000 ns |
| + Array index | Replace map with fixed array | ~4000 ns |
| + ObjectPool | Zero-malloc order allocation | ~833 ns |
| + Intrusive list | O(1) remove, no memmove | ~333 ns |
| + id index + align | O(1) cancel, cache line align | **~250 ns** |

## Build & Run

Requires C++17.

```bash
# Build with CMake
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
./trading-engine   # tests
./bench            # benchmark

# Or compile directly
g++ -std=c++17 -O3 -Iinclude -o test src/main.cpp && ./test
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
- [x] **Phase 2** — PriceLevelBook (array), intrusive linked list, ObjectPool, cache line alignment, cross-platform benchmark, p99 < 500 ns
- [ ] **Phase 3** — SPSCQueue (lock-free), FIX parser, dual-thread architecture
