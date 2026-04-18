#pragma once
#include <cstdint>
#include <cstring>
#include <array>

// Maps symbol strings (e.g. "AAPL") to uint16_t IDs for O(1) array indexing.
// Fixed-capacity, zero-malloc after init. All lookups are linear scan over
// a small array — fine for <256 symbols which covers most exchange partitions.

class SymbolRegistry {
public:
    static constexpr uint16_t MAX_SYMBOLS = 256;
    static constexpr uint16_t INVALID_ID  = UINT16_MAX;
    static constexpr int      MAX_SYMBOL_LEN = 8;  // "AAPL", "ES    " etc.

    SymbolRegistry() : count_(0) {
        // Zero-init all entries
        for (auto& e : entries_)
            e.name[0] = '\0';
    }

    // Register a symbol. Returns its ID. If already registered, returns existing ID.
    // Call at startup, NOT in hot path.
    uint16_t register_symbol(const char* name) {
        // Check if already exists
        uint16_t existing = lookup(name);
        if (existing != INVALID_ID) return existing;

        if (count_ >= MAX_SYMBOLS) return INVALID_ID;  // full

        uint16_t id = count_++;
        // Safe copy (truncate if too long)
        std::strncpy(entries_[id].name, name, MAX_SYMBOL_LEN);
        entries_[id].name[MAX_SYMBOL_LEN] = '\0';
        entries_[id].len = static_cast<uint8_t>(std::strlen(entries_[id].name));
        return id;
    }

    // Lookup symbol by name. Returns INVALID_ID if not found.
    // Used in FIX gateway (not hot path — called once per message parse).
    uint16_t lookup(const char* name) const {
        std::size_t name_len = std::strlen(name);
        for (uint16_t i = 0; i < count_; i++) {
            if (entries_[i].len == name_len &&
                std::memcmp(entries_[i].name, name, name_len) == 0) {
                return i;
            }
        }
        return INVALID_ID;
    }

    // Lookup symbol by name with known length (avoids strlen).
    // This is what FIXParser calls — pointer + length, no null terminator needed.
    uint16_t lookup(const char* name, std::size_t len) const {
        for (uint16_t i = 0; i < count_; i++) {
            if (entries_[i].len == len &&
                std::memcmp(entries_[i].name, name, len) == 0) {
                return i;
            }
        }
        return INVALID_ID;
    }

    // Get name by ID (for logging/display only).
    [[nodiscard]]const char* name(uint16_t id) const {
        if (id >= count_) return "???";
        return entries_[id].name;
    }

    [[nodiscard]] uint16_t count() const { return count_; }

private:
    struct Entry {
        char    name[MAX_SYMBOL_LEN + 1];  // null-terminated
        uint8_t len = 0;
    };

    std::array<Entry, MAX_SYMBOLS> entries_;
    uint16_t count_;
};
