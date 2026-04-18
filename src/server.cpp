#include <cstdio>
#include <thread>
#include <atomic>
#include <csignal>

#include "transport/TCPGateway.h"
#include "engine/MultiBookEngine.h"
#include "transport/SPSCQueue.h"
#include "protocol/SymbolRegistry.h"
#include "protocol/FIXParser.h"
#include "core/PriceLevelBook.h"

// ─── Configuration ───────────────────────────────────────────────────────────
static constexpr uint16_t PORT       = 9000;
static constexpr std::size_t   QUEUE_SIZE = 8192;

// Book type: all symbols share the same price range / tick / pool size.
// Adjust per use case. This covers ticks 990–1010 with step 5, pool 200K orders.
using Book = PriceLevelBook<990, 1010, 5, 1, 200000>;

// ─── Global stop signal ──────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
}

// ─── Engine thread ───────────────────────────────────────────────────────────
// Pops FIXMessages from the SPSC queue and routes them through MultiBookEngine.
static void engine_thread_func(SPSCQueue<FIXMessage, QUEUE_SIZE>& queue,
                                MultiBookEngine<Book>& engine) {
    std::fprintf(stderr, "[Engine] Thread started.\n");

    uint64_t processed = 0;
    FIXMessage msg;

    while (g_running.load(std::memory_order_relaxed)) {
        if (queue.pop(msg)) {
            engine.process(msg, nullptr);  // nullptr = no trade callback (silent)
            processed++;
        }
        // No sleep — busy poll for lowest latency.
        // In production, could use a conditional variable or yield strategy.
    }

    // Drain remaining messages after stop signal
    while (queue.pop(msg)) {
        engine.process(msg, nullptr);
        processed++;
    }

    std::fprintf(stderr, "[Engine] Thread stopped. Processed %llu messages.\n", processed);
}

// ─── Main ────────────────────────────────────────────────────────────────────
int main() {
    // Install signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);    // Ctrl+C
    std::signal(SIGTERM, signal_handler);   // kill

    std::fprintf(stderr, "================================================\n");
    std::fprintf(stderr, "  Trading Engine Server\n");
    std::fprintf(stderr, "  Port: %u | Queue: %zu slots\n", PORT, QUEUE_SIZE);
    std::fprintf(stderr, "  Ctrl+C to stop\n");
    std::fprintf(stderr, "================================================\n\n");

    // ── 1. Symbol registry ──────────────────────────────────────────────
    SymbolRegistry registry;
    registry.register_symbol("AAPL");   // id=0
    registry.register_symbol("MSFT");   // id=1
    registry.register_symbol("GOOG");   // id=2
    registry.register_symbol("TSLA");   // id=3

    std::fprintf(stderr, "[Init] Registered %u symbols.\n", registry.count());

    // ── 2. Multi-book engine ────────────────────────────────────────────
    MultiBookEngine<Book> engine;
    for (uint16_t i = 0; i < registry.count(); i++) {
        engine.create_book(i);
        std::fprintf(stderr, "[Init] Created book for %s (id=%u)\n", registry.name(i), i);
    }

    // ── 3. SPSC queue (gateway → engine) ────────────────────────────────
    SPSCQueue<FIXMessage, QUEUE_SIZE> queue;

    // ── 4. Start engine thread ──────────────────────────────────────────
    std::thread engine_thread(engine_thread_func,
                              std::ref(queue), std::ref(engine));

    // ── 5. Run gateway on main thread ───────────────────────────────────
    //    (blocks until g_running becomes false)
    TCPGateway<QUEUE_SIZE> gateway(PORT, queue, registry,
                                    false,  // validate=false for now
                                    '|');   // pipe delimiter for testing

    // Gateway checks g_running via its own running_ flag
    std::thread gateway_thread([&]() {
        gateway.run();
    });

    // Wait for stop signal
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ── 6. Graceful shutdown ────────────────────────────────────────────
    std::fprintf(stderr, "\n[Main] Shutting down...\n");
    gateway.stop();

    if (gateway_thread.joinable()) gateway_thread.join();
    if (engine_thread.joinable())  engine_thread.join();

    std::fprintf(stderr, "[Main] Gateway received %llu messages from %llu connections.\n",
                 gateway.messages_received(), gateway.connections_total());
    std::fprintf(stderr, "[Main] Shutdown complete.\n");

    return 0;
}
