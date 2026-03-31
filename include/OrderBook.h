#pragma once
#include "Order.h"
#include <map>
#include <vector>
#include <iostream>
#include <iomanip>

class OrderBook {
public:
    // bids: price descending
    std::map<double, std::vector<Order>, std::greater<double>> bids;
    // asks: price ascending
    std::map<double, std::vector<Order>> asks;

    void add(const Order& o) {
        if (o.side == Side::Buy) {
            bids[o.price].push_back(o);
        } else {
            asks[o.price].push_back(o);
        }
    }

    void cancel(uint64_t order_id) {
        for (auto& [price, orders] : bids) {
            for (auto it = orders.begin(); it != orders.end(); ++it) {
                if (it->id == order_id) {
                    orders.erase(it);
                    if (orders.empty()) bids.erase(price);
                    return;
                }
            }
        }
        for (auto& [price, orders] : asks) {
            for (auto it = orders.begin(); it != orders.end(); ++it) {
                if (it->id == order_id) {
                    orders.erase(it);
                    if (orders.empty()) asks.erase(price);
                    return;
                }
            }
        }
    }

    void match() {
        while (!bids.empty() && !asks.empty()) {
            auto& [bid_price, bid_orders] = *bids.begin();
            auto& [ask_price, ask_orders] = *asks.begin();

            if (bid_price < ask_price) break;

            auto& bid = bid_orders.front();
            auto& ask = ask_orders.front();

            int traded_qty = std::min(bid.qty, ask.qty);
            std::cout << "TRADE: " << traded_qty
                      << " @ " << ask_price << "\n";

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
        for (auto& [ask_price,ask_order]: asks) {
            int total_qty = 0;
            for (const auto& o : ask_order) total_qty += o.qty;
            // set precision to 1 decimal places
            std::cout << std::fixed << std::setprecision(1)
                      << ask_price;
            std::cout << " x " << total_qty << "\n";
        }
        std::cout << "BIDS:\n";
        for (auto& [bid_price,bid_order]: bids) {
            int total_qty = 0;
            for (const auto& o : bid_order) total_qty += o.qty;
            // set precision to 1 decimal places
            std::cout << std::fixed << std::setprecision(1)
                        << bid_price;
            std::cout << " x " << total_qty << "\n";
        }
        std::cout<< "==================\n";
    }

};
