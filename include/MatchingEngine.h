#pragma once
#include "Order.h"
#include "OrderBook.h"

class MatchingEngine {
public:
    explicit MatchingEngine(OrderBook ob_) : order_book(ob_) {};
    OrderBook order_book;

    void process(const Order& order) {
        order_book.add(order);
        order_book.match();
    }

};