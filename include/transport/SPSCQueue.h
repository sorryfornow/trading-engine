#pragma once
// Single-producer single-consumer lock-free queue.
#include <atomic>
#include <cassert>
#include "core/Common.h"

template<typename T, size_t N>
class SPSCQueue {
private:
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};  // consumer owns, points to next item to read
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};  // producer owns, points to next slot to write
    T buffer_[N];
public:
    bool push(const T& val){
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (tail + 1) % N;
        if (next_tail == head_.load(std::memory_order_acquire))
            return false; // full
        buffer_[tail] = val;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
    bool pop(T& val){
        size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return false; // empty
        val = buffer_[head];
        head_.store((head + 1) % N, std::memory_order_release);
        return true;
    }
};
