#pragma once
#include <array>
#include <cstdint>
#include "protocol/SymbolRegistry.h"
#include "protocol/FIXParser.h"
#include "core/PriceLevelBook.h"

// MultiBookEngine: routes orders to per-symbol PriceLevelBook instances.
//
// - symbol_id is used as a direct array index (O(1) dispatch)
// - Books are lazily allocated — create_book(id) must be called at startup
//   for each symbol before any orders flow. Unallocated slots are nullptr.
// - All books share the same template parameters (price range, tick, pool
//   size). This matches real exchange partitions where symbols in the same
//   matching unit have similar characteristics.
// - Order IDs are scoped per-symbol (matches FIX ClOrdID semantics).

template<typename Book, uint16_t MAX_SYMBOLS = SymbolRegistry::MAX_SYMBOLS>
class MultiBookEngine {
public:
    MultiBookEngine() {
        for (auto& p : books_) p = nullptr;
    }

    ~MultiBookEngine() {
        for (auto* b : books_)
            delete b;
    }

    // Non-copyable (owns heap resources)
    MultiBookEngine(const MultiBookEngine&) = delete;
    MultiBookEngine& operator=(const MultiBookEngine&) = delete;

    // Allocate a book for the given symbol. Call once per symbol at startup.
    // Returns false if symbol_id is invalid or book already exists.
    bool create_book(uint16_t symbol_id) {
        if (symbol_id >= MAX_SYMBOLS) return false;
        if (books_[symbol_id] != nullptr) return false;
        books_[symbol_id] = new Book();
        return true;
    }

    // Direct book access (for testing / advanced use).
    // Returns nullptr if symbol has no book.
    Book* get(uint16_t symbol_id) {
        if (symbol_id >= MAX_SYMBOLS) return nullptr;
        return books_[symbol_id];
    }

    const Book* get(uint16_t symbol_id) const {
        if (symbol_id >= MAX_SYMBOLS) return nullptr;
        return books_[symbol_id];
    }

    // Add order + match. Returns false if the order was rejected.
    // cb may be nullptr (zero-overhead bench path).
    //
    // symbol_id is bounds-checked before indexing books_. An unresolved
    // Symbol (tag 55) leaves FIXMessage::symbol_id at SymbolRegistry::
    // INVALID_ID (65535), which is far outside a MAX_SYMBOLS-slot array —
    // indexing it unchecked is an out-of-bounds read reachable from the wire.
    bool add_and_match(uint16_t symbol_id, const Order& o,
                       typename Book::TradeCallback cb = nullptr) {
        if (symbol_id >= MAX_SYMBOLS) return false;
        Book* b = books_[symbol_id];
        if (!b) return false;
        if (!b->add(o)) return false;      // price out of range / pool exhausted
        b->match(cb);
        return true;
    }

    // Cancel order by (symbol_id, order_id). Returns false if the symbol is
    // unknown; a miss on order_id inside a valid book is not an error.
    bool cancel(uint16_t symbol_id, uint32_t order_id) {
        if (symbol_id >= MAX_SYMBOLS) return false;
        Book* b = books_[symbol_id];
        if (!b) return false;
        b->cancel(order_id);
        return true;
    }

    // Process a FIXMessage: route to the correct book by symbol_id.
    // Convenience method for the engine thread — one call does everything.
    // Returns false if the message was rejected (unknown symbol, price out of
    // range, pool exhausted, or unparseable MsgType).
    bool process(const FIXMessage& msg,
                 typename Book::TradeCallback cb = nullptr) {
        if (msg.type == FIXMessage::NewOrder) {
            Order order(msg.id, msg.side, msg.tick, msg.qty);
            return add_and_match(msg.symbol_id, order, cb);
        }
        if (msg.type == FIXMessage::Cancel) {
            return cancel(msg.symbol_id, msg.orig_id);  // orig_id = tag 41
        }
        return false;
    }

    // Count of currently allocated books.
    uint16_t book_count() const {
        uint16_t n = 0;
        for (auto* b : books_)
            if (b) n++;
        return n;
    }

private:
    std::array<Book*, MAX_SYMBOLS> books_;
};
