#include "transport/TCPGateway.h"
#include "transport/SPSCQueue.h"
#include "transport/Inbound.h"
#include "protocol/SymbolRegistry.h"
#include "protocol/FIXParser.h"
#include <cassert>
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Unlike test_connection and test_poller, these are integration tests: they
// open a real listening socket and drive it over loopback TCP. That is the
// only way to reach accept(), the read loop, and the disconnect path.
//
// Every assertion runs *after* the gateway thread has been stopped and joined,
// so nothing reads gateway state while the gateway is still mutating it. The
// SPSC queue is the one exception, and it is built for exactly that: the test
// thread is its single consumer.

using namespace std::chrono_literals;

void section(const std::string& name) {
    std::cout << "\n--------------------------------------\n";
    std::cout << "  " << name << "\n";
    std::cout << "--------------------------------------\n";
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Ask the kernel for an unused port, then let it go. There is a race between
// releasing it and the gateway binding it, but on loopback in a test run it is
// far more reliable than hardcoding a port that may already be taken.
static uint16_t free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;                       // 0 = pick one for me
    assert(bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(fd, (sockaddr*)&addr, &len) == 0);
    uint16_t port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

// Connect to the gateway, retrying until it has finished binding.
static int connect_to(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    for (int attempt = 0; attempt < 200; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0)
            return fd;
        close(fd);
        std::this_thread::sleep_for(10ms);
    }
    return -1;
}

static void send_all(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        assert(n > 0);
        sent += static_cast<std::size_t>(n);
    }
}

static std::string make_order(uint32_t id, const char* symbol, int tick,
                              int qty, char delim = FIXParser::SOH) {
    char buf[512];
    std::size_t n = FIXParser::build(buf, sizeof(buf), FIXMessage::NewOrder,
                                     id, symbol, Side::Buy, tick, qty, id, delim);
    return std::string(buf, n);
}

// Owns the gateway and its thread. Everything is torn down in stop(), which
// every test calls before asserting.
template<std::size_t QSIZE>
struct Fixture {
    SPSCQueue<Inbound, QSIZE> queue;
    SymbolRegistry               registry;
    uint16_t                     port;
    TCPGateway<QSIZE>            gw;
    std::thread                  thread;
    bool                         stopped = false;

    explicit Fixture(bool validate = true, char delim = FIXParser::SOH)
        : port(free_port())
        , gw(port, queue, registry, validate, delim)
    {
        registry.register_symbol("AAPL");   // id=0
        registry.register_symbol("MSFT");   // id=1
        thread = std::thread([this] { gw.run(); });
    }

    ~Fixture() { stop(); }

    void stop() {
        if (stopped) return;
        gw.stop();
        if (thread.joinable()) thread.join();
        stopped = true;
    }

    // Wait until the queue holds at least n messages, draining as we go.
    // Returns everything drained.
    std::vector<Inbound> drain(std::size_t n, std::chrono::milliseconds cap = 2000ms) {
        std::vector<Inbound> got;
        auto deadline = std::chrono::steady_clock::now() + cap;
        Inbound m;
        while (got.size() < n && std::chrono::steady_clock::now() < deadline) {
            if (queue.pop(m)) got.push_back(m);
            else std::this_thread::sleep_for(2ms);
        }
        return got;
    }

    // Drain whatever is there after giving the gateway time to act.
    std::vector<Inbound> drain_all(std::chrono::milliseconds settle = 250ms) {
        std::this_thread::sleep_for(settle);
        std::vector<Inbound> got;
        Inbound m;
        while (queue.pop(m)) got.push_back(m);
        return got;
    }
};

int main() {
    int passed = 0;

    // ── Test 1: accept and forward one message ───────────────────────────
    section("Test 1: Accept And Forward");
    {
        Fixture<64> f;
        int c = connect_to(f.port);
        assert(c >= 0);

        const std::string msg = make_order(1001, "AAPL", 1005, 200);
        send_all(c, msg.data(), msg.size());

        auto got = f.drain(1);
        close(c);
        f.stop();

        assert(got.size() == 1);
        assert(got[0].msg.type == FIXMessage::NewOrder);
        assert(got[0].msg.id == 1001);
        assert(got[0].msg.symbol_id == 0);
        assert(got[0].msg.tick == 1005);
        assert(got[0].msg.qty == 200);
        assert(f.gw.connections_total() == 1);
        assert(f.gw.messages_received() == 1);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 2: a message split across TCP segments ──────────────────────
    section("Test 2: Fragmented Send");
    {
        Fixture<64> f;
        int c = connect_to(f.port);
        assert(c >= 0);

        const std::string msg = make_order(1002, "MSFT", 1000, 50);
        const std::size_t cut = msg.size() / 2;

        // Two writes with a gap wide enough that the kernel will not coalesce
        // them, so the gateway genuinely sees a partial message first.
        send_all(c, msg.data(), cut);
        std::this_thread::sleep_for(120ms);
        send_all(c, msg.data() + cut, msg.size() - cut);

        auto got = f.drain(1);
        close(c);
        f.stop();

        assert(got.size() == 1);
        assert(got[0].msg.id == 1002);
        assert(got[0].msg.symbol_id == 1);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 3: several messages in one write ────────────────────────────
    section("Test 3: Coalesced Send");
    {
        Fixture<64> f;
        int c = connect_to(f.port);
        assert(c >= 0);

        const std::string all = make_order(2001, "AAPL", 1000, 10)
                              + make_order(2002, "MSFT", 1005, 20)
                              + make_order(2003, "AAPL", 1010, 30);
        send_all(c, all.data(), all.size());

        auto got = f.drain(3);
        close(c);
        f.stop();

        assert(got.size() == 3);
        assert(got[0].msg.id == 2001 && got[1].msg.id == 2002 && got[2].msg.id == 2003);
        assert(f.gw.messages_received() == 3);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 4: two clients at once ──────────────────────────────────────
    section("Test 4: Concurrent Clients");
    {
        // Each connection carries its own buffer, so a half-message on one
        // must not disturb the other. Interleave them to prove it.
        Fixture<64> f;
        int c1 = connect_to(f.port);
        int c2 = connect_to(f.port);
        assert(c1 >= 0 && c2 >= 0);

        const std::string m1 = make_order(3001, "AAPL", 1000, 10);
        const std::string m2 = make_order(3002, "MSFT", 1005, 20);
        const std::size_t cut = m1.size() / 2;

        send_all(c1, m1.data(), cut);              // c1 half a message
        send_all(c2, m2.data(), m2.size());        // c2 a whole one
        std::this_thread::sleep_for(80ms);
        send_all(c1, m1.data() + cut, m1.size() - cut);

        auto got = f.drain(2);
        close(c1);
        close(c2);
        f.stop();

        assert(got.size() == 2);
        bool saw1 = false, saw2 = false;
        for (const auto& m : got) {
            if (m.msg.id == 3001) { saw1 = true; assert(m.msg.symbol_id == 0); }
            if (m.msg.id == 3002) { saw2 = true; assert(m.msg.symbol_id == 1); }
        }
        assert(saw1 && saw2);
        assert(f.gw.connections_total() == 2);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 5: disconnect is cleaned up and the gateway keeps serving ───
    section("Test 5: Disconnect Then Reconnect");
    {
        // If handle_disconnect() failed to remove the fd from the poller or
        // the connection map, the gateway would spin on a dead fd and the
        // second client would never be served.
        Fixture<64> f;

        int c1 = connect_to(f.port);
        assert(c1 >= 0);
        const std::string m1 = make_order(4001, "AAPL", 1000, 10);
        send_all(c1, m1.data(), m1.size());
        assert(f.drain(1).size() == 1);
        close(c1);
        std::this_thread::sleep_for(120ms);        // let the gateway notice

        int c2 = connect_to(f.port);
        assert(c2 >= 0);
        const std::string m2 = make_order(4002, "MSFT", 1005, 20);
        send_all(c2, m2.data(), m2.size());
        auto got = f.drain(1);
        close(c2);
        f.stop();

        assert(got.size() == 1);
        assert(got[0].msg.id == 4002);
        assert(f.gw.connections_total() == 2);
        assert(f.gw.connections_active() == 0);    // both cleaned up
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 6: a client that closes without sending anything ────────────
    section("Test 6: Immediate Close");
    {
        Fixture<64> f;
        int c = connect_to(f.port);
        assert(c >= 0);
        close(c);                                   // connect then vanish

        std::this_thread::sleep_for(150ms);

        // The gateway must still be alive and serving.
        int c2 = connect_to(f.port);
        assert(c2 >= 0);
        const std::string msg = make_order(5001, "AAPL", 1000, 10);
        send_all(c2, msg.data(), msg.size());
        auto got = f.drain(1);
        close(c2);
        f.stop();

        assert(got.size() == 1 && got[0].msg.id == 5001);
        assert(f.gw.connections_active() == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 7: validation rejects before the queue ──────────────────────
    section("Test 7: Invalid CheckSum Never Reaches The Queue");
    {
        Fixture<64> f(/*validate=*/true);
        int c = connect_to(f.port);
        assert(c >= 0);

        // Same length, so BodyLength still agrees and only CheckSum breaks.
        std::string bad = make_order(6001, "AAPL", 1000, 10);
        const std::size_t at = bad.find("CLIENT");
        assert(at != std::string::npos);
        bad[at] = 'X';
        send_all(c, bad.data(), bad.size());

        // A good message behind it proves the connection did not wedge.
        const std::string good = make_order(6002, "AAPL", 1000, 10);
        send_all(c, good.data(), good.size());

        auto got = f.drain_all();
        close(c);
        f.stop();

        assert(got.size() == 1);
        assert(got[0].msg.id == 6002);                 // only the valid one
        assert(f.gw.messages_received() == 1);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 8: unframeable garbage gets the client dropped ──────────────
    section("Test 8: Buffer Ceiling Disconnects The Client");
    {
        Fixture<64> f;
        int c = connect_to(f.port);
        assert(c >= 0);

        // Bytes with no message boundary. Past 4 KB the gateway gives up on
        // this client rather than buffering forever.
        std::vector<char> junk(6000, 'A');
        ssize_t sent = write(c, junk.data(), junk.size());
        assert(sent > 0);

        // Observe the disconnect from the client side: once the server closes,
        // read() returns 0 for EOF. Without this the test would still pass if
        // the gateway merely ignored the overflow and kept the client around.
        timeval tv{2, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char sink[16];
        ssize_t eof = read(c, sink, sizeof(sink));
        assert(eof == 0);                          // 0 == peer closed

        // The gateway must survive and still accept new clients.
        int c2 = connect_to(f.port);
        assert(c2 >= 0);
        const std::string msg = make_order(7001, "AAPL", 1000, 10);
        send_all(c2, msg.data(), msg.size());
        auto got = f.drain(1);
        close(c2);
        close(c);
        f.stop();

        assert(got.size() == 1 && got[0].msg.id == 7001);
        assert(f.gw.messages_received() == 1);     // the junk yielded nothing
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 9: a full queue drops messages instead of blocking ──────────
    section("Test 9: Queue Full Drops, Gateway Survives");
    {
        // Capacity 4 means 3 usable slots. Nothing drains it, so the gateway
        // must keep running and shed the overflow rather than block the read
        // loop — a blocked gateway would stall every other connection.
        Fixture<4> f;
        int c = connect_to(f.port);
        assert(c >= 0);

        std::string all;
        for (uint32_t i = 0; i < 20; i++)
            all += make_order(8000 + i, "AAPL", 1000, 10);
        send_all(c, all.data(), all.size());

        std::this_thread::sleep_for(300ms);

        // Still alive: a new client is still served.
        int c2 = connect_to(f.port);
        assert(c2 >= 0);
        close(c2);
        std::this_thread::sleep_for(80ms);
        close(c);
        f.stop();

        Inbound m;
        int drained = 0;
        while (f.queue.pop(m)) drained++;
        assert(drained == 3);                      // capacity 4, one slot lost
        assert(f.gw.messages_received() == 20);    // all parsed, most dropped
        assert(f.gw.connections_total() == 2);     // second client accepted
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 10: pipe mode ───────────────────────────────────────────────
    section("Test 10: Pipe Delimiter Mode");
    {
        // The --pipe test switch, end to end.
        Fixture<64> f(/*validate=*/false, /*delim=*/'|');
        int c = connect_to(f.port);
        assert(c >= 0);

        const char* msg = "8=FIX.4.2|9=61|35=D|11=9001|55=AAPL|54=1|44=1005|38=200|10=103|";
        send_all(c, msg, std::strlen(msg));

        auto got = f.drain(1);
        close(c);
        f.stop();

        assert(got.size() == 1);
        assert(got[0].msg.id == 9001);
        assert(got[0].msg.symbol_id == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 11: stop() actually stops ───────────────────────────────────
    section("Test 11: Stop Terminates The Event Loop");
    {
        // run() polls with a 100 ms timeout, so stop() must be observed within
        // roughly that. A hang here means the shutdown path is broken.
        Fixture<64> f;
        int c = connect_to(f.port);
        assert(c >= 0);
        close(c);

        auto t0 = std::chrono::steady_clock::now();
        f.stop();                                   // joins the thread
        auto elapsed = std::chrono::steady_clock::now() - t0;

        assert(elapsed < 2s);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 12: routing identity survives fd reuse ──────────────────────
    section("Test 12: conn_id Distinguishes Recycled Fds");
    {
        // The reason Inbound carries conn_id and not just fd. The kernel hands
        // out the lowest free descriptor, so a client that connects after
        // another disconnects usually lands on the same fd. If a reply were
        // routed by fd alone, a fill for the first client could be delivered
        // to the second.
        Fixture<64> f;

        int c1 = connect_to(f.port);
        assert(c1 >= 0);
        const std::string m1 = make_order(1, "AAPL", 1000, 10);
        send_all(c1, m1.data(), m1.size());
        auto first = f.drain(1);
        assert(first.size() == 1);
        close(c1);
        std::this_thread::sleep_for(150ms);   // let the gateway reap it

        int c2 = connect_to(f.port);
        assert(c2 >= 0);
        const std::string m2 = make_order(2, "MSFT", 1005, 20);
        send_all(c2, m2.data(), m2.size());
        auto second = f.drain(1);
        assert(second.size() == 1);
        close(c2);
        f.stop();

        // Two different clients, and the fd was almost certainly reused.
        assert(first[0].msg.id == 1 && second[0].msg.id == 2);
        assert(first[0].conn_id != second[0].conn_id);
        assert(first[0].conn_id != 0 && second[0].conn_id != 0);
        assert(second[0].conn_id > first[0].conn_id);   // monotonic

        // Both records point at a real fd, and the whole point is that this
        // may well be the *same* number for two unrelated connections.
        assert(first[0].fd > 0 && second[0].fd > 0);
        std::cout << "PASSED";
        if (first[0].fd == second[0].fd)
            std::cout << "  (fd " << first[0].fd << " was reused, as expected)";
        std::cout << "\n";
        passed++;
    }

    // ── Test 13: every message from one connection shares its conn_id ────
    section("Test 13: conn_id Is Stable Within A Connection");
    {
        Fixture<64> f;
        int c = connect_to(f.port);
        assert(c >= 0);

        const std::string all = make_order(11, "AAPL", 1000, 10)
                              + make_order(12, "AAPL", 1005, 20)
                              + make_order(13, "MSFT", 1010, 30);
        send_all(c, all.data(), all.size());

        auto got = f.drain(3);
        close(c);
        f.stop();

        assert(got.size() == 3);
        const uint32_t id = got[0].conn_id;
        assert(id != 0);
        for (const auto& in : got) {
            assert(in.conn_id == id);
            assert(in.fd == got[0].fd);
        }
        std::cout << "PASSED\n";
        passed++;
    }

    constexpr int total = 13;

    std::cout << "\n======================================\n";
    std::cout << "  Gateway Results: " << passed << "/" << total << " passed\n";
    if (passed == total)
        std::cout << "  All Gateway tests passed!\n";
    else
        std::cout << "  " << (total - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == total) ? 0 : 1;
}
