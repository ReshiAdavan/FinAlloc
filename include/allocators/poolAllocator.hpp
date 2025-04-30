#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <new>
#include <utility>
#include <thread>
#include <atomic>

class PoolAllocator {
public:
    PoolAllocator(size_t objectSize, size_t capacity);
    virtual ~PoolAllocator();

    virtual void* allocate();
    virtual void deallocate(void* ptr);

    size_t used() const { return usedCount; }
    size_t capacity() const { return poolCapacity; }
    void* memory() const { return memoryBlock; }
    size_t blockSize() const { return alignedObjSize * poolCapacity; }

    template<typename T, typename... Args>
    T* construct(Args&&... args) {
        void* mem = allocate();
        return mem ? new (mem) T(std::forward<Args>(args)...) : nullptr;
    }

    template<typename T>
    void destroy(T* ptr) {
        if (ptr) {
            ptr->~T();
            deallocate(static_cast<void*>(ptr));
        }
    }

protected:
    void* memoryBlock;
    void* nonAtomicFreeListHead = nullptr;
    size_t alignedObjSize;
    size_t poolCapacity;
    size_t usedCount;

    size_t alignUp(size_t n, size_t alignment);
};

class LockFreePoolAllocator : public PoolAllocator {
public:
    LockFreePoolAllocator(size_t objectSize, size_t capacity);
    ~LockFreePoolAllocator() override;

    void* allocate() override;
    void deallocate(void* ptr) override;

private:
    std::atomic<void*> freeListHead;
};

class ThreadLocalPool {
public:
    ThreadLocalPool(size_t objSize, size_t capacity);

    void* allocate();
    void deallocate(void* ptr);

    template<typename T, typename... Args>
    T* construct(Args&&... args) {
        return localAllocator.construct<T>(std::forward<Args>(args)...);
    }

    template<typename T>
    void destroy(T* ptr) {
        localAllocator.destroy(ptr);
    }

    void* memory() const {
        return localAllocator.memory();
    }

    size_t blockSize() const {
        return localAllocator.blockSize();
    }

private:
    PoolAllocator localAllocator;
};