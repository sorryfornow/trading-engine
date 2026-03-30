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

    T* allocate() {
        assert(top >= 0 && "ObjectPool exhausted");
        int index = free_stack[top--];
        return &storage[index];
    }

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
