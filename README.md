# Trading Engine

A low-latency order matching engine written in C++17, built from scratch as a learning project targeting HFT infrastructure roles.

## Architecture

```
  Client A ──TCP──┐
  Client B ──TCP──┤── TCPGateway Thread ── SPSCQueue<FIXMessage> ── Engine Thread
  Client C ──TCP──┘   (epoll/kqueue)                                     │
                      (FIX parse)                                        ▼
                                                              MultiBookEngine
                                                             ┌───────┬───────┐
                                                          Book[0] Book[1] Book[2]
                                                          (AAPL)  (MSFT)  (GOOG)
                                                             │
                                                  ├── Array-indexed price levels (O(1) lookup)
                                                  ├── Intrusive doubly-linked list (O(1) insert/remove)
                                                  ├── ObjectPool (zero malloc in hot path)
                                                  ├── unordered_map id index (O(1) cancel)
                                                  └── FIFO matching at each price level
```

## Project Structure

```
trading-engine/
├── include/
│   ├── core/           ← Matching kernel
│   │   ├── Common.h            Shared constants (CACHE_LINE_SIZE)
│   │   ├── Order.h             Cache-line-aligned Order struct (32 bytes, intrusive list)
│   │   ├── PriceLevelBook.h    Array-indexed order book, ObjectPool, O(1) cancel
│   │   ├── ObjectPool.h        Heap-backed memory pool, O(1) alloc/release
│   │   ├── OrderBook.h         [deprecated] std::map baseline
│   │   └── MatchingEngine.h    [deprecated] Phase 1 thin wrapper
│   ├── protocol/       ← FIX protocol
│   │   ├── FIXParser.h         Zero-malloc FIX 4.2 parser (SOH + pipe), build/validate/frame
│   │   └── SymbolRegistry.h    Symbol string → uint16_t mapping (O(1) array index)
│   ├── transport/      ← Networking + queue
│   │   ├── Poller.h            Cross-platform epoll (Linux) / kqueue (macOS)
│   │   ├── Connection.h        Per-fd read buffer, TCP framing (粘包/分片)
│   │   ├── TCPGateway.h        Non-blocking TCP server, accept/read/parse/push
│   │   └── SPSCQueue.h         Lock-free SPSC ring buffer (acquire/release)
│   ├── engine/         ← Engine layer
│   │   └── MultiBookEngine.h   Multi-symbol dispatcher, routes by symbol_id
│   └── util/
│       └── Timer.h             Cross-platform nanosecond timer (rdtsc / cntvct)
├── src/
│   ├── server.cpp      TCP server entry point (gateway + engine threads)
│   └── engine.cpp      Phase 3 dual-thread benchmark demo
├── test/
│   ├── test_main.cpp   Phase 1 OrderBook tests (10)
│   ├── test_spsc.cpp   SPSC queue tests (6)
│   ├── test_fix.cpp    FIX parser tests (15)
│   └── test_multibook.cpp  Multi-symbol engine tests (6)
├── bench/
│   └── bench.cpp       Latency benchmark (OrderBook vs PriceLevelBook)
├── CMakeLists.txt
└── README.md
```

### Key Design Decisions

**Array vs std::map** — Price levels are mapped to array indices via integer tick conversion. `O(1)` lookup instead of `O(log n)` tree traversal. Requires known price range at compile time.

**Intrusive linked list vs std::vector** — Each price level stores orders in a doubly-linked list where `prev`/`next` pointers live inside the Order struct itself. `push_back()` and `remove()` are both O(1), eliminating the O(n) `memmove` caused by `vector::erase(begin())`.

**ObjectPool** — All Order objects are pre-allocated in a heap-backed pool. `add()` calls `pool.allocate()`, `cancel()`/`match()` call `pool.release()`. No `new`/`delete` in the hot path.

**O(1) cancel via id index** — An `unordered_map<order_id, Order*>` provides direct pointer lookup for cancellations. Previous approach scanned all price levels linearly — O(LEVELS × M).

**Cache line alignment** — `Order` is `alignas(64)` so each order sits in exactly one 64-byte cache line, preventing cross-line straddles on access.

**Integer ticks** — Prices are converted to integer ticks (`100.50` → tick `1005`) to avoid floating-point comparison issues and enable direct array indexing.

**TradeCallback** — `match()` takes an optional function pointer instead of writing to `std::cout`. Bench runs with `nullptr` (zero overhead); tests pass a lambda for output.

**Lock-free SPSCQueue** — A single-producer single-consumer ring buffer using `std::atomic` with `acquire`/`release` memory ordering. `head_` and `tail_` are on separate cache lines (`alignas(64)`) to prevent false sharing between threads.

**FIX 4.2 parser** — Supports real FIX protocol: SOH delimiter, BodyLength (tag 9) validation, CheckSum (tag 10) verification, message framing for TCP byte streams. Zero-malloc parsing with `fast_atou`/`fast_atoi`. Includes `build()` for generating well-formed messages. Backward compatible with `|` delimiter for testing.

**Multi-symbol dispatch** — `MultiBookEngine` holds an `array<Book*, 256>` indexed by `symbol_id`. `SymbolRegistry` maps symbol strings to `uint16_t` IDs at startup. Routing is a single array dereference — O(1).

**TCP Gateway** — Non-blocking I/O via epoll (Linux) / kqueue (macOS). One thread manages all client connections. Per-connection `Connection` buffer handles TCP framing (fragmentation + coalescing). Parsed messages are pushed into the SPSC queue for the engine thread.

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

# Or compile directly
g++ -std=c++17 -O3 -Iinclude -o test_main test/test_main.cpp && ./test_main
g++ -std=c++17 -O3 -Iinclude -pthread -o test_spsc test/test_spsc.cpp && ./test_spsc
g++ -std=c++17 -O3 -Iinclude -o test_fix test/test_fix.cpp && ./test_fix
g++ -std=c++17 -O3 -Iinclude -o test_multibook test/test_multibook.cpp && ./test_multibook
g++ -std=c++17 -O3 -Iinclude -o bench bench/bench.cpp && ./bench
g++ -std=c++17 -O3 -Iinclude -pthread -o engine src/engine.cpp && ./engine
g++ -std=c++17 -O3 -Iinclude -pthread -o server src/server.cpp && ./server
```

### TCP Server Usage

```bash
# Start the server
./server
# → Listening on port 9000, Ctrl+C to stop

# In another terminal, send FIX messages:
echo '8=FIX.4.2|9=61|35=D|11=1001|55=AAPL|54=1|44=1005|38=200|10=103|' | nc localhost 9000
```

## Tests

37 test cases across 4 suites:

### OrderBook Tests (`test_main`) — Phase 1

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

### SPSCQueue Tests (`test_spsc`) — Phase 3

| # | Test | What it validates |
|---|------|-------------------|
| 1 | Basic FIFO | Push/pop order preserved |
| 2 | Full Queue | Returns false when queue is full |
| 3 | Empty Queue | Returns false when queue is empty |
| 4 | Wrap Around | Ring buffer correctly wraps indices |
| 5 | Order Type | Order structs survive push/pop intact |
| 6 | Two-Thread 1M | 1M messages between two threads, no data loss |

### FIXParser Tests (`test_fix`) — Phase 3-4

| # | Test | What it validates |
|---|------|-------------------|
| 1 | NewOrderSingle (pipe) | Parse tag 35=D with all fields |
| 2 | CancelRequest (pipe) | Parse tag 35=F with OrigClOrdID |
| 3 | Sell Side | Side=2 maps to Side::Sell |
| 4 | Unknown Type | Unrecognized MsgType returns Unknown |
| 5 | Tag Order Independence | Tags parsed regardless of order |
| 6 | Large Order ID | uint32_t handles large IDs correctly |
| 7 | Symbol Resolution | SymbolRegistry maps AAPL→0, MSFT→1, TSLA→INVALID |
| 8 | No Registry | Backward compatible without SymbolRegistry |
| 9 | Build + Parse NewOrder | Round-trip with BodyLength + CheckSum |
| 10 | Build + Parse Cancel | Round-trip cancel message |
| 11 | CheckSum Validation | Valid passes, corrupted byte fails |
| 12 | BodyLength Validation | Tag 9 matches actual body length |
| 13 | Frame Detection | Complete / partial / concatenated messages |
| 14 | SOH Delimiter | Real FIX SOH (0x01) end-to-end |
| 15 | MsgSeqNum | Tag 34 parsed into seq_num field |

### MultiBookEngine Tests (`test_multibook`) — Phase 4

| # | Test | What it validates |
|---|------|-------------------|
| 1 | Create Books | Two symbols registered, book_count == 2 |
| 2 | Cross-symbol Isolation | AAPL buy does not match MSFT sell |
| 3 | Same-symbol Match | AAPL buy + AAPL sell → trade |
| 4 | Cancel Routing | Cancel AAPL order doesn't affect MSFT |
| 5 | Unknown Symbol | INVALID_ID message silently dropped |
| 6 | Mixed Flow | Interleaved multi-symbol new + cancel stream |

## Roadmap

- [x] **Phase 1** — OrderBook + MatchingEngine, limit/market/cancel, 10 tests
- [x] **Phase 2** — PriceLevelBook (array), intrusive linked list, ObjectPool, cache line alignment, p99 < 500 ns
- [x] **Phase 3** — SPSCQueue (lock-free), FIX parser, dual-thread architecture, p99 = 250 ns
- [x] **Phase 4** — FIX 4.2 protocol (BodyLength/CheckSum/framing), SymbolRegistry, MultiBookEngine, TCP Gateway (epoll/kqueue), multi-symbol support
- [ ] **Phase 5** — Risk management (pre-trade checks), WAL persistence, crash recovery
- [ ] **Phase 6** — Market data dissemination (UDP multicast), monitoring/metrics
- [ ] **Phase 7** — Kernel bypass (io_uring/DPDK), huge pages, CPU pinning, NUMA awareness
