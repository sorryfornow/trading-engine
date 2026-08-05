#include "transport/Connection.h"
#include "protocol/SymbolRegistry.h"
#include <cassert>
#include <iostream>
#include <cstring>
#include <string>
#include <vector>

// Connection is the only thing standing between a raw TCP byte stream and the
// parser. FIXParser::frame() is covered in test_fix; what is covered here is
// the buffer management wrapped around it — accumulation across reads, the
// memmove compaction after a message is consumed, and the 4 KB ceiling.
//
// None of this needs a socket. Connection takes an fd as a plain integer and
// never touches it, so these are pure memory tests.

void section(const std::string& name) {
    std::cout << "\n--------------------------------------\n";
    std::cout << "  " << name << "\n";
    std::cout << "--------------------------------------\n";
}

// Build a well-formed SOH-delimited NewOrderSingle into a std::string so tests
// can slice it at arbitrary offsets.
static std::string make_order(uint32_t id, const char* symbol, int tick, int qty) {
    char buf[512];
    std::size_t n = FIXParser::build(buf, sizeof(buf), FIXMessage::NewOrder,
                                     id, symbol, Side::Buy, tick, qty, id,
                                     FIXParser::SOH);
    return std::string(buf, n);
}

int main() {
    int passed = 0;

    SymbolRegistry reg;
    reg.register_symbol("AAPL");   // id=0
    reg.register_symbol("MSFT");   // id=1

    // ── Test 1: one message arriving in one read ─────────────────────────
    section("Test 1: Single Complete Message");
    {
        Connection conn(42);                    // fd is never dereferenced
        const std::string msg = make_order(1001, "AAPL", 1005, 200);

        assert(conn.append(msg.data(), msg.size()) == true);
        assert(conn.used == msg.size());

        FIXMessage out;
        assert(conn.try_extract(out, &reg, true) == true);
        assert(out.type == FIXMessage::NewOrder);
        assert(out.id == 1001);
        assert(out.symbol_id == 0);
        assert(out.tick == 1005);
        assert(out.qty == 200);

        assert(conn.used == 0);                 // fully consumed
        assert(conn.try_extract(out, &reg, true) == false);   // nothing left
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 2: one message split across two reads ───────────────────────
    section("Test 2: Fragmented Across Reads");
    {
        Connection conn(42);
        const std::string msg = make_order(1002, "AAPL", 1000, 50);
        const std::size_t cut = msg.size() / 2;

        // First half: no complete message yet, and nothing may be consumed.
        assert(conn.append(msg.data(), cut) == true);
        FIXMessage out;
        assert(conn.try_extract(out, &reg, true) == false);
        assert(conn.used == cut);               // buffer left untouched
        assert(std::memcmp(conn.buf, msg.data(), cut) == 0);

        // Second half completes it.
        assert(conn.append(msg.data() + cut, msg.size() - cut) == true);
        assert(conn.try_extract(out, &reg, true) == true);
        assert(out.id == 1002);
        assert(conn.used == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 3: byte-at-a-time delivery ──────────────────────────────────
    section("Test 3: Byte-At-A-Time Delivery");
    {
        // The pathological fragmentation case: TCP is allowed to hand over one
        // byte per read. Only the final byte may complete the message.
        Connection conn(42);
        const std::string msg = make_order(1003, "MSFT", 1010, 7);
        FIXMessage out;

        for (std::size_t i = 0; i + 1 < msg.size(); i++) {
            assert(conn.append(msg.data() + i, 1) == true);
            assert(conn.try_extract(out, &reg, true) == false);
        }
        assert(conn.append(msg.data() + msg.size() - 1, 1) == true);
        assert(conn.try_extract(out, &reg, true) == true);
        assert(out.id == 1003);
        assert(out.symbol_id == 1);
        assert(conn.used == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 4: several messages in one read ─────────────────────────────
    section("Test 4: Coalesced Messages");
    {
        Connection conn(42);
        const std::string a = make_order(2001, "AAPL", 1000, 10);
        const std::string b = make_order(2002, "MSFT", 1005, 20);
        const std::string c = make_order(2003, "AAPL", 1010, 30);
        const std::string all = a + b + c;

        assert(conn.append(all.data(), all.size()) == true);

        // This is why TCPGateway calls try_extract in a while loop rather
        // than an if — one read can carry any number of messages.
        std::vector<uint32_t> ids;
        FIXMessage out;
        while (conn.try_extract(out, &reg, true))
            ids.push_back(out.id);

        assert(ids.size() == 3);
        assert(ids[0] == 2001 && ids[1] == 2002 && ids[2] == 2003);
        assert(conn.used == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 5: compaction preserves the trailing fragment exactly ───────
    section("Test 5: Compaction Keeps Remainder Intact");
    {
        // Two whole messages followed by a partial third. After draining the
        // two, the leftover bytes must sit at the front of the buffer,
        // byte-for-byte — a memmove off by one corrupts every later message.
        Connection conn(42);
        const std::string a    = make_order(3001, "AAPL", 1000, 10);
        const std::string b    = make_order(3002, "MSFT", 1005, 20);
        const std::string full = make_order(3003, "AAPL", 1010, 30);
        const std::string tail = full.substr(0, 17);   // deliberately partial

        const std::string chunk = a + b + tail;
        assert(conn.append(chunk.data(), chunk.size()) == true);

        FIXMessage out;
        assert(conn.try_extract(out, &reg, true) == true && out.id == 3001);
        assert(conn.try_extract(out, &reg, true) == true && out.id == 3002);
        assert(conn.try_extract(out, &reg, true) == false);

        assert(conn.used == tail.size());
        assert(std::memcmp(conn.buf, tail.data(), tail.size()) == 0);

        // Feeding the rest must now yield the third message.
        assert(conn.append(full.data() + tail.size(), full.size() - tail.size()) == true);
        assert(conn.try_extract(out, &reg, true) == true);
        assert(out.id == 3003);
        assert(conn.used == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 6: the 4 KB ceiling ─────────────────────────────────────────
    section("Test 6: Buffer Capacity Boundary");
    {
        Connection conn(42);
        std::vector<char> filler(Connection::BUF_SIZE, 'x');

        // Exactly BUF_SIZE fits: the guard is `used + len > BUF_SIZE`.
        assert(conn.append(filler.data(), Connection::BUF_SIZE) == true);
        assert(conn.used == Connection::BUF_SIZE);

        // One more byte does not, and must not disturb what is already held.
        const char extra = 'y';
        assert(conn.append(&extra, 1) == false);
        assert(conn.used == Connection::BUF_SIZE);
        assert(conn.buf[Connection::BUF_SIZE - 1] == 'x');

        // Partial fills follow the same rule.
        Connection conn2(43);
        assert(conn2.append(filler.data(), Connection::BUF_SIZE - 1) == true);
        assert(conn2.append(&extra, 1) == true);            // exactly full
        assert(conn2.append(&extra, 1) == false);           // one past
        assert(conn2.used == Connection::BUF_SIZE);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 7: garbage never yields a message ───────────────────────────
    section("Test 7: Garbage Is Never Framed");
    {
        // A client sending bytes that contain no message boundary must not
        // produce a FIXMessage, no matter how much it sends.
        Connection conn(42);
        std::vector<char> junk(1024, 'A');
        FIXMessage out;

        for (int i = 0; i < 3; i++) {
            assert(conn.append(junk.data(), junk.size()) == true);
            assert(conn.try_extract(out, &reg, true) == false);
        }
        assert(conn.used == 3072);   // it just accumulates

        // The fourth chunk pushes past 4096 and is refused. This is the
        // signal TCPGateway turns into a disconnect.
        assert(conn.append(junk.data(), junk.size()) == true);   // 4096 exactly
        assert(conn.append(junk.data(), 1) == false);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 8: wrong delimiter is silently unframeable ──────────────────
    section("Test 8: Delimiter Mismatch");
    {
        // A pipe-delimited message arriving at a SOH connection can never be
        // framed. Documented behaviour: it accumulates until the buffer is
        // refused, which is what the gateway reports on.
        Connection soh_conn(42, FIXParser::SOH);
        char pipe_buf[512];
        std::size_t n = FIXParser::build(pipe_buf, sizeof(pipe_buf),
                                         FIXMessage::NewOrder, 4001, "AAPL",
                                         Side::Buy, 1000, 10, 1, '|');

        FIXMessage out;
        assert(soh_conn.append(pipe_buf, n) == true);
        assert(soh_conn.try_extract(out, &reg, true) == false);
        assert(soh_conn.used == n);          // stuck, nothing consumed

        // The same bytes on a pipe-delimited connection parse fine.
        Connection pipe_conn(43, '|');
        assert(pipe_conn.append(pipe_buf, n) == true);
        assert(pipe_conn.try_extract(out, &reg, true) == true);
        assert(out.id == 4001);
        assert(pipe_conn.used == 0);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 9: validation and symbol resolution pass through ────────────
    section("Test 9: Validation Pass-Through");
    {
        Connection conn(42);
        std::string msg = make_order(5001, "AAPL", 1000, 10);

        // Flip one character inside SenderCompID. The message length is
        // unchanged, so BodyLength still agrees and only the CheckSum breaks.
        // Framing still succeeds — the trailer is intact — but validation must
        // fail, and a rejected message must still be drained from the buffer
        // or the connection would wedge on it forever.
        const std::size_t at = msg.find("CLIENT");
        assert(at != std::string::npos);
        msg[at] = 'X';

        assert(conn.append(msg.data(), msg.size()) == true);
        FIXMessage out;
        assert(conn.try_extract(out, &reg, true) == true);
        assert(out.type == FIXMessage::Unknown);   // rejected by validation
        assert(conn.used == 0);                    // and still consumed

        // An unregistered symbol leaves symbol_id at INVALID_ID; MultiBook-
        // Engine is what rejects it (see test_multibook Test 6).
        Connection conn2(43);
        const std::string nvda = make_order(5002, "NVDA", 1000, 10);
        assert(conn2.append(nvda.data(), nvda.size()) == true);
        assert(conn2.try_extract(out, &reg, true) == true);
        assert(out.type == FIXMessage::NewOrder);
        assert(out.symbol_id == SymbolRegistry::INVALID_ID);
        std::cout << "PASSED\n";
        passed++;
    }

    // ── Test 10: move semantics carry buffered bytes ─────────────────────
    section("Test 10: Move Preserves Buffered Bytes");
    {
        // TCPGateway stores connections in an unordered_map, which moves them
        // on node construction. A move that loses the partial message would
        // corrupt the next message on that connection.
        Connection src(42);
        const std::string msg = make_order(6001, "AAPL", 1000, 10);
        const std::size_t cut = msg.size() / 2;
        assert(src.append(msg.data(), cut) == true);

        Connection dst(std::move(src));
        assert(dst.fd == 42);
        assert(dst.used == cut);
        assert(std::memcmp(dst.buf, msg.data(), cut) == 0);
        assert(src.fd == -1 && src.used == 0);     // source neutered

        assert(dst.append(msg.data() + cut, msg.size() - cut) == true);
        FIXMessage out;
        assert(dst.try_extract(out, &reg, true) == true);
        assert(out.id == 6001);
        std::cout << "PASSED\n";
        passed++;
    }

    constexpr int total = 10;

    std::cout << "\n======================================\n";
    std::cout << "  Connection Results: " << passed << "/" << total << " passed\n";
    if (passed == total)
        std::cout << "  All Connection tests passed!\n";
    else
        std::cout << "  " << (total - passed) << " test(s) failed.\n";
    std::cout << "======================================\n";

    return (passed == total) ? 0 : 1;
}
