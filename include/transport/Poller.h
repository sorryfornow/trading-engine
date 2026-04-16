#pragma once
#include <cstdint>
#include <vector>

// ─── Cross-platform I/O event poller ─────────────────────────────────────────
// Thin abstraction over epoll (Linux) and kqueue (macOS/BSD).
// Single-threaded: used only by the Gateway thread.
//
// Usage:
//   Poller poller;
//   poller.add(listen_fd, POLLIN);       // watch for new connections
//   poller.add(client_fd, POLLIN);       // watch for data from client
//   int n = poller.wait(events, 64, timeout_ms);
//   for (int i = 0; i < n; i++) {
//       int fd = events[i].fd;
//       // handle readable/writable/error
//   }
// ─────────────────────────────────────────────────────────────────────────────

struct PollEvent {
    int      fd;
    uint32_t events;   // bitmask: POLLIN / POLLOUT / POLLERR
};

static constexpr uint32_t POLLIN  = 0x01;
static constexpr uint32_t POLLOUT = 0x02;
static constexpr uint32_t POLLERR = 0x04;

// ─── macOS: kqueue ───────────────────────────────────────────────────────────
#if defined(__APPLE__) || defined(__FreeBSD__)

#include <sys/event.h>
#include <unistd.h>

class Poller {
public:
    Poller() : kq_(kqueue()) {}

    ~Poller() {
        if (kq_ >= 0) close(kq_);
    }

    // Register fd for read events (edge-triggered not needed for kqueue,
    // it's level-triggered by default which is simpler and correct).
    bool add(int fd, uint32_t events) {
        struct kevent ev[2];
        int n = 0;
        if (events & POLLIN)
            EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (events & POLLOUT)
            EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        return kevent(kq_, ev, n, nullptr, 0, nullptr) == 0;
    }

    bool remove(int fd) {
        struct kevent ev[2];
        EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        // Ignore errors — fd may not be registered for both filters.
        kevent(kq_, ev, 2, nullptr, 0, nullptr);
        return true;
    }

    // Wait for events. Returns number of ready events (0 = timeout, -1 = error).
    // timeout_ms: -1 = block forever, 0 = non-blocking poll.
    int wait(PollEvent* out, int max_events, int timeout_ms) {
        struct timespec ts;
        struct timespec* tsp = nullptr;
        if (timeout_ms >= 0) {
            ts.tv_sec  = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
            tsp = &ts;
        }

        // Use a local buffer for raw kevent results
        struct kevent raw[128];
        int cap = max_events < 128 ? max_events : 128;
        int n = kevent(kq_, nullptr, 0, raw, cap, tsp);

        for (int i = 0; i < n; i++) {
            out[i].fd = static_cast<int>(raw[i].ident);
            out[i].events = 0;
            if (raw[i].filter == EVFILT_READ)  out[i].events |= POLLIN;
            if (raw[i].filter == EVFILT_WRITE) out[i].events |= POLLOUT;
            if (raw[i].flags & EV_ERROR)       out[i].events |= POLLERR;
            if (raw[i].flags & EV_EOF)         out[i].events |= POLLIN;  // treat EOF as readable
        }
        return n;
    }

private:
    int kq_;
};

// ─── Linux: epoll ────────────────────────────────────────────────────────────
#elif defined(__linux__)

#include <sys/epoll.h>
#include <unistd.h>

class Poller {
public:
    Poller() : epfd_(epoll_create1(0)) {}

    ~Poller() {
        if (epfd_ >= 0) close(epfd_);
    }

    bool add(int fd, uint32_t events) {
        struct epoll_event ev{};
        ev.data.fd = fd;
        if (events & POLLIN)  ev.events |= EPOLLIN;
        if (events & POLLOUT) ev.events |= EPOLLOUT;
        return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    bool remove(int fd) {
        return epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    int wait(PollEvent* out, int max_events, int timeout_ms) {
        struct epoll_event raw[128];
        int cap = max_events < 128 ? max_events : 128;
        int n = epoll_wait(epfd_, raw, cap, timeout_ms);

        for (int i = 0; i < n; i++) {
            out[i].fd = raw[i].data.fd;
            out[i].events = 0;
            if (raw[i].events & EPOLLIN)  out[i].events |= POLLIN;
            if (raw[i].events & EPOLLOUT) out[i].events |= POLLOUT;
            if (raw[i].events & (EPOLLERR | EPOLLHUP)) out[i].events |= POLLERR;
        }
        return n;
    }

private:
    int epfd_;
};

#else
#error "Unsupported platform: need epoll (Linux) or kqueue (macOS/BSD)"
#endif
