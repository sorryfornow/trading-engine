# Trading Engine

A low-latency order matching engine written in C++17, built from scratch as a learning project targeting HFT infrastructure roles.

## Architecture

```
                         SPSCQueue<FIXMessage>
  Network Thread ──────────────────────────────► Engine Thread
  (FIX parse + push)                             (pop + match)
                                                      │
                                                      ▼
                                                PriceLevelBook
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
| `Order.h` | Cache-line-aligned Order struct (32 bytes used, `uint32_t` id, `int` tick) with intrusive list pointers |
| `PriceLevelBook.h` | Array-based order book with linked list, ObjectPool, id index |
| `ObjectPool.h` | Heap-backed pre-allocated memory pool, O(1) alloc/release |
| `OrderBook.h` | Reference implementation using `std::map` (baseline comparison) |
| `MatchingEngine.h` | Thin wrapper that calls add() + match() |
| `SPSCQueue.h` | Lock-free single-producer single-consumer ring buffer queue |
| `FIXParser.h` | Zero-malloc FIX protocol parser (tag 35/11/41/54/44/38) |
| `Common.h` | Shared constants (`CACHE_LINE_SIZE`) |
| `Timer.h` | Cross-platform nanosecond timer (rdtsc on x86, cntvct on ARM) |

### Key Design Decisions

**Array vs std::map** — Price levels are mapped to array indices via integer tick conversion. `O(1)` lookup instead of `O(log n)` tree traversal. Requires known price range at compile time.

**Intrusive linked list vs std::vector** — Each price level stores orders in a doubly-linked list where `prev`/`next` pointers live inside the Order struct itself. `push_back()` and `remove()` are both O(1), eliminating the O(n) `memmove` caused by `vector::erase(begin())`.

**ObjectPool** — All Order objects are pre-allocated in a heap-backed pool. `add()` calls `pool.allocate()`, `cancel()`/`match()` call `pool.release()`. No `new`/`delete` in the hot path.

**O(1) cancel via id index** — An `unordered_map<order_id, Order*>` provides direct pointer lookup for cancellations. Previous approach scanned all price levels linearly — O(LEVELS × M).

**Cache line alignment** — `Order` is `alignas(64)` so each order sits in exactly one 64-byte cache line, preventing cross-line straddles on access.

**Integer ticks** — Prices are converted to integer ticks (`100.50` → tick `1005`) to avoid floating-point comparison issues and enable direct array indexing.

**TradeCallback** — `match()` takes an optional function pointer instead of writing to `std::cout`. Bench runs with `nullptr` (zero overhead); tests pass a lambda for output.

**Lock-free SPSCQueue** — A single-producer single-consumer ring buffer using `std::atomic` with `acquire`/`release` memory ordering. `head_` and `tail_` are on separate cache lines (`alignas(64)`) to prevent false sharing between threads.

**Zero-malloc FIX parser** — Parses FIX-style `key=value|` messages using raw pointer scanning. No `std::string`, no `strtok`, no heap allocation. Custom `fast_atou`/`fast_atoi` avoid `strtol` overhead (no locale, no errno).

**Dual-thread architecture** — Network thread handles FIX parsing and pushes `FIXMessage` into the SPSC queue. Engine thread pops messages and executes `add()`/`match()`/`cancel()`. Decouples I/O from matching logic.

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

### Dual-Thread Engine (Phase 3)

100,000 FIX messages, ~10% cancels. Latency measured on engine thread (queue pop → match complete).

| Platform | p50 | p99 | p99.9 |
|----------|-----|-----|-------|
| Apple M (ARM64) | 41 ns | 250 ns | 458 ns |

> AMD x86_64 results pending.

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
g++ -std=c++17 -O3 -Iinclude -pthread -o test_spsc src/test_spsc.cpp && ./test_spsc
g++ -std=c++17 -O3 -Iinclude -o test_fix src/test_fix.cpp && ./test_fix
g++ -std=c++17 -O3 -Iinclude -pthread -o engine src/engine.cpp && ./engine
```

## Tests

22 test cases across 3 suites:

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

### SPSCQueue Tests (`test_spsc`)

| # | Test | What it validates |
|---|------|-------------------|
| 1 | Basic FIFO | Push/pop order preserved |
| 2 | Full Queue | Returns false when queue is full |
| 3 | Empty Queue | Returns false when queue is empty |
| 4 | Wrap Around | Ring buffer correctly wraps indices |
| 5 | Order Type | Order structs survive push/pop intact |
| 6 | Two-Thread 1M | 1M messages between two threads, no data loss |

### FIXParser Tests (`test_fix`)

| # | Test | What it validates |
|---|------|-------------------|
| 1 | NewOrderSingle | Parse tag 35=D with all fields |
| 2 | CancelRequest | Parse tag 35=F with OrigClOrdID |
| 3 | Sell Side | Side=2 maps to Side::Sell |
| 4 | Unknown Type | Unrecognized MsgType returns Unknown |
| 5 | Tag Order Independence | Tags parsed regardless of order |
| 6 | Large Order ID | uint32_t handles large IDs correctly |

## Roadmap

- [x] **Phase 1** — OrderBook + MatchingEngine, limit/market/cancel, 10 tests passing
- [x] **Phase 2** — PriceLevelBook (array), intrusive linked list, ObjectPool, cache line alignment, cross-platform benchmark, p99 < 500 ns
- [x] **Phase 3** — SPSCQueue (lock-free), FIX parser, dual-thread architecture, p99 = 250 ns
- [ ] **Phase 4** — TCP/UDP networking, multi-symbol support, kernel bypass (io_uring/DPDK)
