#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "transport/Poller.h"
#include "transport/Connection.h"
#include "transport/SPSCQueue.h"
#include "protocol/SymbolRegistry.h"

// ─── TCP Gateway ─────────────────────────────────────────────────────────────
// Runs on its own thread. Accepts TCP connections, reads FIX messages,
// parses them, and pushes FIXMessage into an SPSC queue for the engine
// thread to consume.
//
// Non-blocking I/O via epoll/kqueue. One thread handles all connections.
//
// Lifecycle:
//   TCPGateway gw(port, queue, registry);
//   gw.run();        // blocks — call from a dedicated thread
//   gw.stop();       // signal from another thread to break the loop
// ─────────────────────────────────────────────────────────────────────────────

template<std::size_t QUEUE_SIZE = 8192>
class TCPGateway {
public:
    TCPGateway(uint16_t port,
               SPSCQueue<FIXMessage, QUEUE_SIZE>& queue,
               const SymbolRegistry& registry,
               bool validate_fix = true,
               char delim = FIXParser::SOH)
        : port_(port)
        , queue_(queue)
        , registry_(registry)
        , validate_(validate_fix)
        , delim_(delim)
    {}

    ~TCPGateway() {
        stop();
        // Close all client connections
        for (auto& [fd, conn] : connections_) {
            close(fd);
        }
        // Close listen socket
        if (listen_fd_ >= 0) {
            close(listen_fd_);
        }
    }

    // Start the event loop (blocking). Call from gateway thread.
    bool run() {
        listen_fd_ = create_listen_socket();
        if (listen_fd_ < 0) {
            std::fprintf(stderr, "[Gateway] Failed to create listen socket on port %u\n", port_);
            return false;
        }

        poller_.add(listen_fd_, POLLIN);

        std::fprintf(stderr, "[Gateway] Listening on port %u (fd=%d)\n", port_, listen_fd_);

        running_ = true;
        PollEvent events[64];

        while (running_) {
            int n = poller_.wait(events, 64, 100);  // 100ms timeout

            for (int i = 0; i < n; i++) {
                int fd = events[i].fd;

                if (fd == listen_fd_) {
                    // New connection
                    handle_accept();
                } else if (events[i].events & POLLERR) {
                    // Error on client fd
                    handle_disconnect(fd);
                } else if (events[i].events & POLLIN) {
                    // Data from client
                    handle_read(fd);
                }
            }
        }

        std::fprintf(stderr, "[Gateway] Stopped.\n");
        return true;
    }

    // Signal the event loop to stop (thread-safe).
    void stop() { running_ = false; }

    // Stats
    [[nodiscard]]uint64_t messages_received() const { return msg_count_; }
    [[nodiscard]]uint64_t connections_total() const { return conn_count_; }
    [[nodiscard]]std::size_t connections_active() const { return connections_.size(); }

private:
    uint16_t port_;
    SPSCQueue<FIXMessage, QUEUE_SIZE>& queue_;
    const SymbolRegistry& registry_;
    bool validate_;
    char delim_;

    int listen_fd_ = -1;
    Poller poller_;
    std::unordered_map<int, Connection> connections_;

    volatile bool running_ = false;
    uint64_t msg_count_ = 0;
    uint64_t conn_count_ = 0;

    // ─── Socket setup ────────────────────────────────────────────────────

    int create_listen_socket() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        // SO_REUSEADDR: allow immediate rebind after restart
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // TCP_NODELAY: disable Nagle's algorithm (send immediately, don't buffer)
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // Bind to 0.0.0.0:port
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }

        // Listen with backlog of 16 pending connections
        if (listen(fd, 16) < 0) {
            close(fd);
            return -1;
        }

        // Set non-blocking
        set_nonblocking(fd);

        return fd;
    }

    static void set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    // ─── Event handlers ──────────────────────────────────────────────────

    void handle_accept() {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd_,
                               reinterpret_cast<struct sockaddr*>(&client_addr),
                               &addr_len);
        if (client_fd < 0) return;

        set_nonblocking(client_fd);

        // TCP_NODELAY on client socket too
        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // Register with poller
        poller_.add(client_fd, POLLIN);

        // Create connection state
        connections_.emplace(client_fd, Connection(client_fd, delim_));
        conn_count_++;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::fprintf(stderr, "[Gateway] Client connected: fd=%d from %s:%d\n",
                     client_fd, ip, ntohs(client_addr.sin_port));
    }

    void handle_read(int fd) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;

        Connection& conn = it->second;

        // Read available data into connection buffer
        char tmp[2048];
        ssize_t n = read(fd, tmp, sizeof(tmp));

        if (n <= 0) {
            // n == 0: client closed connection
            // n < 0 && errno != EAGAIN: error
            if (n == 0 || (n < 0 && errno != EAGAIN)) {
                handle_disconnect(fd);
            }
            return;
        }

        if (!conn.append(tmp, static_cast<std::size_t>(n))) {
            // The buffer filled without frame() ever finding a message
            // boundary. By far the most common cause is a delimiter mismatch,
            // so name the delimiter this server expects rather than reporting
            // a bare overflow — the buffer is the symptom, not the cause.
            std::fprintf(stderr,
                         "[Gateway] fd=%d: %zu bytes buffered with no complete FIX "
                         "message. Expected delimiter %s — wrong delimiter, or a "
                         "message larger than %zu bytes. Disconnecting.\n",
                         fd, conn.used,
                         delim_ == FIXParser::SOH ? "SOH (0x01)" : "'|'",
                         Connection::BUF_SIZE);
            handle_disconnect(fd);
            return;
        }

        // Extract all complete messages from the buffer
        FIXMessage msg;
        while (conn.try_extract(msg, &registry_, validate_)) {
            if (msg.type != FIXMessage::Unknown) {
                // Push to engine queue. If queue is full, drop the message.
                // In production, you'd want backpressure or a larger queue.
                if (!queue_.push(msg)) {
                    std::fprintf(stderr, "[Gateway] Queue full, dropping message id=%u\n", msg.id);
                }
                msg_count_++;
            }
        }
    }

    void handle_disconnect(int fd) {
        std::fprintf(stderr, "[Gateway] Client disconnected: fd=%d\n", fd);
        poller_.remove(fd);
        connections_.erase(fd);
        close(fd);
    }
};
