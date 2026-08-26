#pragma once
#include "Order.h"
#include "ObjectPool.h"
#include <array>
#include <unordered_map>
#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>

// ─── Intrusive doubly-linked list ────────────────────────────────────────────
// All nodes live in ObjectPool — no heap allocation per push/remove.
struct OrderList {
    Order* head = nullptr;
    Order* tail = nullptr;
    int    count = 0;

    bool empty() const { return count == 0; }

    void push_back(Order* o) {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o;
        else      head = o;
        tail = o;
        count++;
    }

    // Remove any node in O(1)
    void remove(Order* o) {
        if (o->prev) o->prev->next = o->next;
        else         head = o->next;
        if (o->next) o->next->prev = o->prev;
        else         tail = o->prev;
        o->prev = o->next = nullptr;
        count--;
    }

    Order* front() const { return head; }
};

// ─── PriceLevelBook ──────────────────────────────────────────────────────────
//
//  Prices represented as integer ticks to avoid floating point precision issues.
//  e.g. If MIN_PRICE=10, MAX_PRICE=100, TICK=1, PRECISION=2, then:
//  MIN_PRICE=1000 → 10.00
//  to_index(1000) = (1000 - 1000) / 1 = 0
// - price 10.00 → tick 1000 → index 0
// - price 10.01 → tick 1001 → index 1
// - price 10.50 → tick 1050 → index 50

template<int MIN_PRICE, int MAX_PRICE, int TICK, int PRECISION, int POOL_SIZE = 10000>
class PriceLevelBook {
public:
    PriceLevelBook() {
        // Pre-allocate hash map to avoid rehash in hot path
        id_map.reserve(POOL_SIZE);
    }

    static int to_tick(double price) {
        return std::lround(price * MULTIPLIER / TICK) * TICK;
    }

    static int to_index(int tick) {
        return (tick - MIN_PRICE) / TICK;
    }

    static double to_price(int index) {
        return (MIN_PRICE + index * TICK) / static_cast<double>(MULTIPLIER);
    }

    // Fill::tick is the canonical form; use this only for display.
    static double price_of(int tick) {
        return tick / static_cast<double>(MULTIPLIER);
    }

    // Which symbol this book serves. MultiBookEngine stamps it at create_book()
    // so every Fill carries it without the callback needing extra context.
    // Left at 0 when a book is used standalone (bench, single-symbol tests).
    void     set_symbol_id(uint16_t id) { symbol_id_ = id; }
    uint16_t symbol_id() const          { return symbol_id_; }

    // Add a resting order. Returns false and leaves the book untouched if the
    // order cannot be accepted.
    //
    // o.tick arrives straight off the wire, so an out-of-range price is a
    // client error, not a programming error — it must be rejected, never
    // asserted. Same for pool exhaustion, which any client can reach by
    // resting POOL_SIZE orders.
    bool add(const Order& o) {
        if (o.tick < MIN_PRICE || o.tick > MAX_PRICE) return false;

        // The price must sit on the tick grid. to_index() divides, so an
        // off-grid tick would be truncated onto a neighbouring level and the
        // order would silently rest at a price the client never asked for —
        // worse than a reject, because nobody is told.
        if ((o.tick - MIN_PRICE) % TICK != 0) return false;

        int index = to_index(o.tick);
        if (index < 0 || index >= LEVELS) return false;   // unreachable, kept as a guard

        Order* p = pool.allocate();
        if (!p) return false;                             // pool exhausted

        *p = o;
        p->prev = p->next = nullptr;

        // O(1) cancel lookup
        id_map[o.id] = p;

        if (o.side == Side::Buy) {
            bids[index].push_back(p);
            if (best_bid_idx == -1 || index > best_bid_idx)
                best_bid_idx = index;
        } else {
            asks[index].push_back(p);
            if (best_ask_idx == -1 || index < best_ask_idx)
                best_ask_idx = index;
        }
        return true;
    }

    void cancel(uint32_t order_id) {
        auto it = id_map.find(order_id);
        if (it == id_map.end()) return;

        Order* o = it->second;
        int index = to_index(o->tick);
        bool is_bid = (o->side == Side::Buy);

        auto& side = is_bid ? bids : asks;
        int& best_idx = is_bid ? best_bid_idx : best_ask_idx;

        side[index].remove(o);
        id_map.erase(it);
        pool.release(o);

        // Rescan best level if this level is now empty
        if (side[index].empty() && index == best_idx) {
            int dir = is_bid ? -1 : 1;
            while (best_idx >= 0 && best_idx < LEVELS && side[best_idx].empty())
                best_idx += dir;
            if (best_idx < 0 || best_idx >= LEVELS)
                best_idx = -1;
        }
    }

    // Reports one Fill per execution. ctx is passed straight back to the
    // callback untouched — a plain function pointer cannot capture, and the
    // response path needs to carry the destination of the report through here.
    using TradeCallback = void(*)(void* ctx, const Fill& fill);

    void match(TradeCallback on_trade = nullptr, void* ctx = nullptr) {
        while (best_bid_idx != -1 && best_ask_idx != -1) {
            if (best_bid_idx < best_ask_idx) break;

            auto& bid_orders = bids[best_bid_idx];
            auto& ask_orders = asks[best_ask_idx];

            Order* bid = bid_orders.front();
            Order* ask = ask_orders.front();

            const int traded_qty = std::min(bid->qty, ask->qty);

            // Read the IDs while the orders are still alive. A fully filled
            // order is released back to the pool below, after which its
            // storage may be handed to the next add().
            const uint32_t bid_id = bid->id;
            const uint32_t ask_id = ask->id;

            bid->qty -= traded_qty;
            ask->qty -= traded_qty;

            const bool bid_done = (bid->qty == 0);
            const bool ask_done = (ask->qty == 0);

            // Reported after the decrement so the *_filled flags are known,
            // and before the release so the callback sees a consistent book.
            if (on_trade) {
                const Fill f{ bid_id, ask_id,
                              MIN_PRICE + best_ask_idx * TICK,
                              traded_qty, symbol_id_, bid_done, ask_done };
                on_trade(ctx, f);
            }

            if (bid_done) {
                bid_orders.remove(bid);
                id_map.erase(bid_id);
                pool.release(bid);
            }
            if (ask_done) {
                ask_orders.remove(ask);
                id_map.erase(ask_id);
                pool.release(ask);
            }

            if (bid_orders.empty()) {
                best_bid_idx--;
                while (best_bid_idx >= 0 && bids[best_bid_idx].empty())
                    best_bid_idx--;
            }
            if (ask_orders.empty()) {
                best_ask_idx++;
                while (best_ask_idx < LEVELS && asks[best_ask_idx].empty())
                    best_ask_idx++;
                if (best_ask_idx == LEVELS) best_ask_idx = -1;
            }
        }
    }

    // Best bid/ask in tick units. Returns 0 if side is empty.
    int best_bid() const {
        return best_bid_idx == -1 ? 0 : MIN_PRICE + best_bid_idx * TICK;
    }
    int best_ask() const {
        return best_ask_idx == -1 ? 0 : MIN_PRICE + best_ask_idx * TICK;
    }

    void print_book() {
        std::cout << "=== Order Book ===\n" << "ASKS:\n";
        for (int i = 0; i < LEVELS; ++i) {
            if (!asks[i].empty()) {
                double price = to_price(i);
                int total_qty = 0;
                for (Order* o = asks[i].head; o; o = o->next)
                    total_qty += o->qty;
                std::cout << std::fixed << std::setprecision(PRECISION)
                          << price << " : " << total_qty << "\n";
            }
        }
        std::cout << "BIDS:\n";
        for (int i = LEVELS - 1; i >= 0; --i) {
            if (!bids[i].empty()) {
                double price = to_price(i);
                int total_qty = 0;
                for (Order* o = bids[i].head; o; o = o->next)
                    total_qty += o->qty;
                std::cout << std::fixed << std::setprecision(PRECISION)
                          << price << " : " << total_qty << "\n";
            }
        }
        std::cout << "==================\n";
    }

private:
    static constexpr int MULTIPLIER = []() {
        int r = 1;
        for (int i = 0; i < PRECISION; ++i) r *= 10;
        return r;
    }();
    static constexpr int LEVELS = (MAX_PRICE - MIN_PRICE) / TICK + 1;

    static_assert(PRECISION >= 0,
                  "PRECISION must be >= 0");
    static_assert(TICK >= 1,
                  "TICK must be >= 1");
    static_assert(MIN_PRICE < MAX_PRICE,
                  "MIN_PRICE must be < MAX_PRICE");
    static_assert(MULTIPLIER % TICK == 0 || TICK % MULTIPLIER == 0,
                  "PRECISION insufficient to represent this TICK size");
    static_assert((MAX_PRICE - MIN_PRICE) % TICK == 0,
                  "Price range must be evenly divisible by TICK");

    ObjectPool<Order, POOL_SIZE> pool;
    std::array<OrderList, LEVELS> bids;
    std::array<OrderList, LEVELS> asks;
    std::unordered_map<uint32_t, Order*> id_map;
    int best_bid_idx = -1;
    int best_ask_idx = -1;
    uint16_t symbol_id_ = 0;
};
