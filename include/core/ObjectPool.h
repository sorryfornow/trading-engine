#pragma once
#include <cassert>
#include <memory>
#include <new>

template<typename T, int N>
class ObjectPool {
public:
    ObjectPool()
        : storage(static_cast<T*>(::operator new(N * sizeof(T), std::align_val_t{alignof(T)})))
        , free_stack(std::make_unique<int[]>(N))
    {
        for (int i = 0; i < N; ++i)
            free_stack[++top] = i;
    }

    ~ObjectPool() {
        ::operator delete(storage, std::align_val_t{alignof(T)});
    }

    // Non-copyable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Returns nullptr when the pool is exhausted. Callers on the network path
    // must check: pool capacity is reachable by a client that submits enough
    // resting orders, so exhaustion is a runtime condition, not a bug.
    T* allocate() {
        if (top < 0) return nullptr;
        int index = free_stack[top--];
        return &storage[index];
    }

    bool empty()     const { return top < 0; }
    int  available() const { return top + 1; }

    void release(T* p) {
        int index = static_cast<int>(p - storage);
        assert(index >= 0 && index < N && "Invalid pointer");
        free_stack[++top] = index;
    }

private:
    T* storage;
    std::unique_ptr<int[]> free_stack;
    int top = -1;
};
