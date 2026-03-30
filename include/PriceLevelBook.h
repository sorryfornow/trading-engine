#pragma once
#include "Order.h"
#include "ObjectPool.h"
#include <array>
#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>

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
    static int to_tick(double price) {
        return std::lround(price * MULTIPLIER / TICK) * TICK;
    }

    static int to_index(int tick) {
        return (tick - MIN_PRICE) / TICK;
    }

    static double to_price(int index) {
        return (MIN_PRICE + index * TICK) / static_cast<double>(MULTIPLIER);
    }

    void add(const Order& o) {
        int index = to_index(to_tick(o.price));
        assert(index >= 0 && index < LEVELS);

        Order* p = pool.allocate();
        *p = o;

        if (o.side == Side::Buy) {
            bids[index].push_back(p);
            if (best_bid_idx == -1 || index > best_bid_idx)
                best_bid_idx = index;
        } else {
            asks[index].push_back(p);
            if (best_ask_idx == -1 || index < best_ask_idx)
                best_ask_idx = index;
        }
    }

    void cancel(uint64_t order_id) {
        auto try_cancel = [&](std::array<std::vector<Order*>, LEVELS>& side,
                              int& best_idx,
                              bool is_bid) -> bool {
            for (int i = 0; i < LEVELS; ++i) {
                auto& orders = side[i];
                for (auto it = orders.begin(); it != orders.end(); ++it) {
                    if ((*it)->id == order_id) {
                        pool.release(*it);
                        orders.erase(it);
                        if (orders.empty() && i == best_idx) {
                            int dir = is_bid ? -1 : 1;
                            while (best_idx >= 0 && best_idx < LEVELS
                                   && side[best_idx].empty())
                                best_idx += dir;
                            if (best_idx < 0 || best_idx >= LEVELS)
                                best_idx = -1;
                        }
                        return true;
                    }
                }
            }
            return false;
        };

        if (!try_cancel(bids, best_bid_idx, true))
            try_cancel(asks, best_ask_idx, false);
    }

    void match() {
        while (best_bid_idx != -1 && best_ask_idx != -1) {
            if (best_bid_idx < best_ask_idx) break;

            auto& bid_orders = bids[best_bid_idx];
            auto& ask_orders = asks[best_ask_idx];

            Order* bid = bid_orders.front();
            Order* ask = ask_orders.front();

            int traded_qty = std::min(bid->qty, ask->qty);
            std::cout << "TRADE: " << traded_qty
                      << std::fixed << std::setprecision(PRECISION)
                      << " @ " << to_price(best_ask_idx) << "\n";

            bid->qty -= traded_qty;
            ask->qty -= traded_qty;

            if (bid->qty == 0) {
                pool.release(bid);
                bid_orders.erase(bid_orders.begin());
            }
            if (ask->qty == 0) {
                pool.release(ask);
                ask_orders.erase(ask_orders.begin());
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

    void print_book() {
        std::cout<< "=== Order Book ===\n" << "ASKS:\n";
        for (int i = 0; i < LEVELS; ++i) {
            if (!asks[i].empty()) {
                double price = to_price(i);
                int total_qty = 0;
                for (const auto* o : asks[i]) total_qty += o->qty;
                std::cout << std::fixed << std::setprecision(PRECISION)
                          << price << " : " << total_qty << "\n";
            }
        }
        std::cout << "BIDS:\n";
        for (int i = LEVELS - 1; i >= 0; --i) {
            if (!bids[i].empty()) {
                double price = to_price(i);
                int total_qty = 0;
                for (const auto* o : bids[i]) total_qty += o->qty;
                std::cout << std::fixed << std::setprecision(PRECISION)
                          << price << " : " << total_qty << "\n";
            }
        }
        std::cout<< "==================\n";
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
    std::array<std::vector<Order*>, LEVELS> bids;
    std::array<std::vector<Order*>, LEVELS> asks;
    int best_bid_idx = -1;
    int best_ask_idx = -1;
};
