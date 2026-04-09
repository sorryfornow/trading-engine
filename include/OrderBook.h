#pragma once
#include "Order.h"
#include <map>
#include <vector>
#include <iostream>
#include <iomanip>

class OrderBook {
public:
    // bids: tick descending (highest tick = best bid)
    std::map<int, std::vector<Order>, std::greater<int>> bids;
    // asks: tick ascending (lowest tick = best ask)
    std::map<int, std::vector<Order>> asks;

    void add(const Order& o) {
        if (o.side == Side::Buy) {
            bids[o.tick].push_back(o);
        } else {
            asks[o.tick].push_back(o);
        }
    }

    void cancel(uint32_t order_id) {
        for (auto& [tick, orders] : bids) {
            for (auto it = orders.begin(); it != orders.end(); ++it) {
                if (it->id == order_id) {
                    orders.erase(it);
                    if (orders.empty()) bids.erase(tick);
                    return;
                }
            }
        }
        for (auto& [tick, orders] : asks) {
            for (auto it = orders.begin(); it != orders.end(); ++it) {
                if (it->id == order_id) {
                    orders.erase(it);
                    if (orders.empty()) asks.erase(tick);
                    return;
                }
            }
        }
    }

    void match() {
        while (!bids.empty() && !asks.empty()) {
            auto& [bid_tick, bid_orders] = *bids.begin();
            auto& [ask_tick, ask_orders] = *asks.begin();

            if (bid_tick < ask_tick) break;

            auto& bid = bid_orders.front();
            auto& ask = ask_orders.front();

            int traded_qty = std::min(bid.qty, ask.qty);
            std::cout << "TRADE: " << traded_qty
                      << " @ tick " << ask_tick << "\n";

            bid.qty -= traded_qty;
            ask.qty -= traded_qty;

            if (bid.qty == 0) bid_orders.erase(bid_orders.begin());
            if (ask.qty == 0) ask_orders.erase(ask_orders.begin());

            if (bid_orders.empty()) bids.erase(bids.begin());
            if (ask_orders.empty()) asks.erase(asks.begin());
        }
    }

    void print_book(){
        std::cout<< "=== Order Book ===\n" << "ASKS:\n";
        for (auto& [tick, orders]: asks) {
            int total_qty = 0;
            for (const auto& o : orders) total_qty += o.qty;
            std::cout << tick << " x " << total_qty << "\n";
        }
        std::cout << "BIDS:\n";
        for (auto& [tick, orders]: bids) {
            int total_qty = 0;
            for (const auto& o : orders) total_qty += o.qty;
            std::cout << tick << " x " << total_qty << "\n";
        }
        std::cout<< "==================\n";
    }

};
