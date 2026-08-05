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
│   │   ├── Connection.h        Per-fd read buffer, TCP framing (fragmentation/coalescing)
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

The server speaks real FIX 4.2: fields are separated by SOH (`0x01`) and every
message is checked for BodyLength and CheckSum before it reaches the engine.

```bash
# Start the server
./server
# → Listening on port 9000, Ctrl+C to stop

# In another terminal — SOH is non-printable, so build the message with printf.
# BodyLength (9) and CheckSum (10) must both be correct or the message is dropped.
printf '8=FIX.4.2\x019=87\x0135=D\x0149=CLIENT\x0156=SERVER\x0134=1\x0152=20260413-00:00:00\x0111=1001\x0155=AAPL\x0154=1\x0144=1005\x0138=200\x0110=078\x01' | nc localhost 9000
```

Hand-computing CheckSum gets old fast, so there is a test switch:

```bash
# --pipe: '|' instead of SOH, and BodyLength/CheckSum validation off.
# Testing only — lets you type messages by hand.
./server --pipe

echo '8=FIX.4.2|9=61|35=D|11=1001|55=AAPL|54=1|44=1005|38=200|10=103|' | nc localhost 9000
```

Note how a delimiter mismatch fails. Send `'|'` messages to a server running in
SOH mode and `FIXParser::frame()` never finds a message boundary, so bytes
accumulate in the 4 KB connection buffer. Below 4 KB nothing at all happens —
no message is processed and no error is reported. Past 4 KB the client is
dropped with:

```
[Gateway] fd=5: 3924 bytes buffered with no complete FIX message.
          Expected delimiter SOH (0x01) — wrong delimiter, or a message
          larger than 4096 bytes. Disconnecting.
```

Check the startup banner to confirm which mode the server is in.

## Tests

58 test cases across 6 suites:

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
| 1 | SymbolRegistry Register/Lookup | Symbol strings map to stable uint16_t IDs |
| 2 | Book Creation | Books allocated per symbol, `book_count()` correct |
| 3 | Per-Symbol Isolation | Orders land in the book matching their symbol_id |
| 4 | Cross-Symbol Match Isolation | AAPL buy does not match MSFT sell |
| 5 | Cancel Per Symbol | Cancelling in one book leaves others untouched |
| 6 | Drop Unknown Symbol | `INVALID_ID` (65535) rejected without indexing `books_` |
| 7 | Reject Out-of-Range Price | Ticks past either end of the book rejected, book unchanged |
| 8 | Reject On Pool Exhaustion | Full `ObjectPool` rejects; freeing a slot restores capacity |

### Connection Tests (`test_connection`) — Phase 4

Pure memory tests — `Connection` holds its fd as a plain integer and never
touches it, so no socket is involved.

| # | Test | What it validates |
|---|------|-------------------|
| 1 | Single Complete Message | One read carrying one whole message |
| 2 | Fragmented Across Reads | Half a message yields nothing and is not consumed |
| 3 | Byte-At-A-Time Delivery | Only the final byte completes the message |
| 4 | Coalesced Messages | Three messages in one read, drained by the `while` loop |
| 5 | Compaction Keeps Remainder Intact | Trailing fragment survives `memmove` byte-for-byte |
| 6 | Buffer Capacity Boundary | Exactly 4096 fits, 4097 is refused, buffer undisturbed |
| 7 | Garbage Is Never Framed | Boundary-free bytes accumulate, never yield a message |
| 8 | Delimiter Mismatch | Pipe message on a SOH connection is unframeable and stays put |
| 9 | Validation Pass-Through | Bad CheckSum rejected but still drained, so the connection cannot wedge |
| 10 | Move Preserves Buffered Bytes | `unordered_map` node moves keep the partial message |

### Poller Tests (`test_poller`) — Phase 4

Run over `socketpair()` — no ports, no listening socket, nothing to bind.

| # | Test | What it validates |
|---|------|-------------------|
| 1 | Empty Poller Times Out | No registrations, no events |
| 2 | Registered Idle Fd Is Quiet | Registration alone does not fire |
| 3 | Readable Fd Is Reported | Written data surfaces as `POLLIN` |
| 4 | Level-Triggered Semantics | Unconsumed data keeps being reported — the contract `handle_read()`'s single 2048-byte read depends on |
| 5 | Writable Fd Is Reported | `POLLOUT` works on both backends, ready for the Phase 5 response path |
| 6 | Removed Fd Goes Silent | `remove()` takes effect even with data still pending |
| 7 | Selective Reporting Across Many Fds | Four registered, only the two that moved come back |
| 8 | Peer Close Wakes The Poller | Closed peer produces an event so the disconnect path can run |
| 9 | Result Set Respects max_events | Batch is capped and the overflow is not lost |

## Known Limitations

Every entry below was reproduced against a running server, not inferred from
reading the code.

### Missing — the response path

| Area | Gap |
|------|-----|
| **ExecutionReport** | `Connection` has no write buffer and `TCPGateway` never calls `write()`. `Poller` supports `POLLOUT` but nothing subscribes to it. A client can submit orders and receives nothing back — no fill, no ack, no reject. |
| **TradeCallback signature** | `void(*)(int qty, double price)` carries no context pointer and does not report the resting/aggressing order IDs, so a fill cannot be attributed to a ClOrdID or routed back to an fd. |
| **Rejects are invisible** | `MultiBookEngine::process()` returns false for a rejected order, but with no response path the server can only count them. `server` prints the tally at shutdown. |

### Missing — the session layer

| Area | Gap |
|------|-----|
| **FIX session** | No Logon (35=A), Logout (35=5), Heartbeat (35=0), TestRequest (35=1), or ResendRequest (35=2). Standard FIX counterparties cannot connect. |
| **Sequence numbers** | `FIXMessage::seq_num` is parsed but never validated — no monotonicity check, no gap detection. |
| **Tag coverage** | Only 8 tags are read: 35, 11, 41, 34, 54, 44, 38, 55. Notably absent is OrdType (40), so **market orders cannot be expressed over FIX** even though `OrderBook` supports them. Tags 49/56/52 are skipped. |

### Sharp edges in what does exist

| Area | Gap |
|------|-----|
| **Delimiter mismatch** | Fails late and indirectly. Under 4 KB nothing happens at all: no message processed, no error. Past 4 KB the gateway disconnects the client with a diagnostic naming the expected delimiter. There is no handshake that detects this at connect time. |
| **Tick alignment** | `to_index()` truncates. With `TICK=5`, an order at tick 992 silently lands on the 990 level rather than being rejected. Prices off the grid are accepted and quietly moved. |
| **Backpressure** | A full SPSC queue drops the message and logs. No flow control, and the client is never told. |
| **Symbol lookup** | `SymbolRegistry::lookup()` is a linear scan. Fine at 256 symbols, not at exchange scale. |
| **`volatile bool running_`** | `TCPGateway` uses `volatile` for cross-thread stop signalling. It works here but is not a synchronisation primitive; it should be `std::atomic<bool>`. |
| **`TCPGateway` is untested** | `Connection` and `Poller` are covered, but the 237 lines that wire them together — accept, the read loop, disconnect, queue-full handling — have no test. Exercising them needs a real listening socket, so this is an integration test rather than a unit test. |

### Not built

| Area | Gap |
|------|-----|
| **Persistence** | No journaling. All book state is lost on process exit. |
| **Pre-trade risk** | No fat-finger limits, position caps, or self-trade prevention. Price range is enforced structurally by the book, which is not a risk check. |
| **Market data** | No L2 snapshot or incremental feed. |
| **Configuration** | Port, symbol list, and price bounds are hardcoded in `src/server.cpp`. All symbols share one `PriceLevelBook` price range. |

### Fixed

Three input-validation defects were reachable from the wire and are now closed,
each covered by a test:

| Was | Now |
|-----|-----|
| An order priced outside the book's range tripped `assert` in `PriceLevelBook::add()` and aborted the process (exit 134). `CMakeLists.txt` overrides `CMAKE_CXX_FLAGS_RELEASE` without `-DNDEBUG`, so asserts are live in Release — one FIX message killed the server. | `add()` returns `bool` and rejects. Test 7. |
| An unresolved Symbol left `symbol_id` at `INVALID_ID` (65535), which `add_and_match()` used to index a 256-slot array — an out-of-bounds read, SEGV under ASan. | `symbol_id` is bounds-checked before indexing. Test 6. |
| `ObjectPool::allocate()` asserted on exhaustion, reachable by resting `POOL_SIZE` orders. | Returns `nullptr`; `add()` rejects. Test 8. |

## Roadmap

- [x] **Phase 1** — OrderBook + MatchingEngine, limit/market/cancel, 10 tests
- [x] **Phase 2** — PriceLevelBook (array), intrusive linked list, ObjectPool, cache line alignment, p99 < 500 ns
- [x] **Phase 3** — SPSCQueue (lock-free), FIX parser, dual-thread architecture, p99 = 250 ns
- [x] **Phase 4** — FIX 4.2 protocol (BodyLength/CheckSum/framing), SymbolRegistry, MultiBookEngine, TCP Gateway (epoll/kqueue), multi-symbol support
- [ ] **Phase 5** — Response path: ExecutionReport (35=8) over an engine→gateway return queue, write buffering driven by `POLLOUT`, pre-trade risk checks with explicit Reject (35=3)
- [ ] **Phase 6** — FIX session layer: Logon/Logout handshake, heartbeat timers, MsgSeqNum validation, gap detection and ResendRequest
- [ ] **Phase 7** — Operability: write-ahead journaling and crash recovery, tick-to-trade latency histograms, external configuration
- [ ] **Phase 8** — Scale-out: L2 market data dissemination, symbol-sharded engine threads, CPU pinning, huge pages, NUMA awareness

## License

MIT — see [LICENSE](LICENSE).
