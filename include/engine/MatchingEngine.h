#pragma once
#include "core/Order.h"
#include "core/OrderBook.h"

// ─── DEPRECATED ──────────────────────────────────────────────────────────────
// MatchingEngine is a Phase 1 artifact: a thin wrapper around the std::map-based
// OrderBook used only in early tests. It has been superseded by:
//
//   - PriceLevelBook   (Phase 2) — array-indexed, ObjectPool, intrusive list
//   - MultiBookEngine  (Phase 4) — multi-symbol dispatch over PriceLevelBook
//
// Kept only for the legacy tests in src/main.cpp. Do not use in new code.
// ─────────────────────────────────────────────────────────────────────────────

class [[deprecated("Use MultiBookEngine<PriceLevelBook<...>> instead")]] MatchingEngine {
public:
    explicit MatchingEngine(OrderBook ob_) : order_book(ob_) {};
    OrderBook order_book;

    void process(const Order& order) {
        order_book.add(order);
        order_book.match();
    }

};