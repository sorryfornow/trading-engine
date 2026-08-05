#include "transport/Poller.h"
#include <cassert>
#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>

// Poller wraps kqueue (macOS/BSD) and epoll (Linux) behind one interface, so
// these tests exist mainly to pin down the behaviour both backends must share.
// Everything runs over socketpair() — a connected pair of fds with no network,
// no ports, and no listening socket — so there is nothing to bind and nothing
// that can hang.

void section(const std::string& name) {
    std::cout << "\n--------------------------------------\n";
    std::cout << "  " << name << "\n";
    std::cout << "--------------------------------------\n";
}

// A connected fd pair. Writing to sv[1] makes sv[0] readable.
//
// Both ends are non-blocking. A socketpair send buffer is only a few KB, so a
// blocking write with nobody reading would wedge the test process forever.
struct Pair {
    int sv[2];
    Pair() {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        for (int fd : sv)
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }
    ~Pair() { close(sv[0]); close(sv[1]); }
    int a() const { return sv[0]; }
    int b() const { return sv[1]; }
};

// Look for fd in a result set. Returns the event mask, or 0 if absent.
static uint32_t find_fd(const PollEvent* ev, int n, int fd) {
    for (int i = 0; i < n; i++)
        if (ev[i].fd == fd) return ev[i].events ? ev[i].events : 1;
    return 0;
}

int main() {
    int passed = 0;
    PollEvent ev[16];

    // ── Test 1: nothing registered, nothing reported ─────────────────────
    section("Test 1: Empty Poller Times Out");
    {
        Poller p;
        // timeout 0 = poll and return immediately.
        assert(p.wait(ev, 16, 0) == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 2: registered but idle ──────────────────────────────────────
    section("Test 2: Registered Idle Fd Is Quiet");
    {
        Poller p;
        Pair pr;
        assert(p.add(pr.a(), POLLIN) == true);
        assert(p.wait(ev, 16, 0) == 0);   // no data written yet
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 3: data arrives ─────────────────────────────────────────────
    section("Test 3: Readable Fd Is Reported");
    {
        Poller p;
        Pair pr;
        p.add(pr.a(), POLLIN);

        const char msg[] = "hello";
        assert(write(pr.b(), msg, sizeof(msg)) == (ssize_t)sizeof(msg));

        int n = p.wait(ev, 16, 100);
        assert(n == 1);
        assert(ev[0].fd == pr.a());
        assert(ev[0].events & POLLIN);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 4: level-triggered, not edge-triggered ──────────────────────
    section("Test 4: Level-Triggered Semantics");
    {
        // This is the contract TCPGateway::handle_read() depends on. It issues
        // exactly one read() of 2048 bytes per event and never loops to EAGAIN,
        // which is only safe if an fd with data still pending keeps being
        // reported. Switching either backend to edge-triggered would silently
        // strand every byte past the first 2048.
        Poller p;
        Pair pr;
        p.add(pr.a(), POLLIN);

        char data[100];
        std::memset(data, 'x', sizeof(data));
        assert(write(pr.b(), data, sizeof(data)) == 100);

        // Reported once...
        assert(p.wait(ev, 16, 100) == 1);
        // ...and again, with nothing new written and nothing consumed. Under
        // edge-triggered delivery this second wait would return 0.
        assert(p.wait(ev, 16, 100) == 1);

        // Consume only part of it — bytes still pending, so still reported.
        char sink[40];
        assert(read(pr.a(), sink, sizeof(sink)) == 40);
        assert(p.wait(ev, 16, 100) == 1);

        // Drain the rest; now it must go quiet.
        char rest[128];
        assert(read(pr.a(), rest, sizeof(rest)) == 60);
        assert(p.wait(ev, 16, 0) == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 5: writability ──────────────────────────────────────────────
    section("Test 5: Writable Fd Is Reported");
    {
        // Nothing subscribes to POLLOUT yet, but the response path in Phase 5
        // will. A fresh socket with an empty send buffer is immediately
        // writable on both backends.
        Poller p;
        Pair pr;
        assert(p.add(pr.a(), POLLOUT) == true);

        int n = p.wait(ev, 16, 100);
        assert(n == 1);
        assert(ev[0].fd == pr.a());
        assert(ev[0].events & POLLOUT);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 6: remove() stops delivery ──────────────────────────────────
    section("Test 6: Removed Fd Goes Silent");
    {
        Poller p;
        Pair pr;
        p.add(pr.a(), POLLIN);

        const char msg[] = "data";
        assert(write(pr.b(), msg, sizeof(msg)) > 0);
        assert(p.wait(ev, 16, 100) == 1);

        // handle_disconnect() calls remove() before close(); if removal did
        // not take, the poller would keep reporting a dead fd forever.
        p.remove(pr.a());
        assert(p.wait(ev, 16, 0) == 0);   // still unread, but no longer watched
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 7: only the ready fds come back ─────────────────────────────
    section("Test 7: Selective Reporting Across Many Fds");
    {
        // The whole point of the poller: register many, hear only from the
        // ones that moved.
        Poller p;
        Pair p0, p1, p2, p3;
        p.add(p0.a(), POLLIN);
        p.add(p1.a(), POLLIN);
        p.add(p2.a(), POLLIN);
        p.add(p3.a(), POLLIN);

        assert(p.wait(ev, 16, 0) == 0);

        const char msg[] = "x";
        assert(write(p1.b(), msg, 1) == 1);
        assert(write(p3.b(), msg, 1) == 1);

        int n = p.wait(ev, 16, 100);
        assert(n == 2);
        assert(find_fd(ev, n, p1.a()) != 0);
        assert(find_fd(ev, n, p3.a()) != 0);
        assert(find_fd(ev, n, p0.a()) == 0);
        assert(find_fd(ev, n, p2.a()) == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 8: peer close shows up as readable ──────────────────────────
    section("Test 8: Peer Close Wakes The Poller");
    {
        // TCPGateway relies on this: a closed peer must produce an event, and
        // handle_read()'s read() then returns 0 and triggers the disconnect
        // path. If close did not wake the poller, dead connections would leak.
        Poller p;
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        p.add(sv[0], POLLIN);

        assert(p.wait(ev, 16, 0) == 0);
        close(sv[1]);                       // peer goes away

        int n = p.wait(ev, 16, 100);
        assert(n == 1);
        assert(ev[0].fd == sv[0]);
        assert(ev[0].events & (POLLIN | POLLERR));

        char sink[8];
        assert(read(sv[0], sink, sizeof(sink)) == 0);   // 0 == EOF
        close(sv[0]);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 9: max_events caps the batch ────────────────────────────────
    section("Test 9: Result Set Respects max_events");
    {
        // run() passes a 64-slot array; wait() must never write past the cap.
        Poller p;
        Pair p0, p1, p2;
        p.add(p0.a(), POLLIN);
        p.add(p1.a(), POLLIN);
        p.add(p2.a(), POLLIN);

        const char msg[] = "x";
        write(p0.b(), msg, 1);
        write(p1.b(), msg, 1);
        write(p2.b(), msg, 1);

        PollEvent small[2];
        int n = p.wait(small, 2, 100);
        assert(n >= 1 && n <= 2);      // capped, never 3

        // Nothing is lost by the cap. Drain what was reported; the fds that
        // did not fit in this batch are still readable, so level-triggered
        // delivery surfaces them on the next call.
        char sink[8];
        for (int i = 0; i < n; i++)
            read(small[i].fd, sink, sizeof(sink));

        int n2 = p.wait(small, 2, 100);
        assert(n + n2 == 3);
        std::cout << "PASSED\n";
        passed++;
    }

    constexpr int total = 9;

    std::cout << "\n======================================\n";
    std::cout << "  Poller Results: " << passed << "/" << total << " passed\n";
    if (passed == total)
        std::cout << "  All Poller tests passed!\n";
    else
        std::cout << "  " << (total - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == total) ? 0 : 1;
}
