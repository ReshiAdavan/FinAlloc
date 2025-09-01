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
#include <mutex>
#include <vector>

#include "allocators/poolConfig.hpp"
#include "utils/histogram.hpp"

struct PoolStats
{
    std::size_t capacity = 0;
    std::size_t object_size = 0;
    std::size_t aligned_object_size = 0;
    std::uint64_t alloc_calls = 0;
    std::uint64_t free_calls = 0;
    std::uint64_t alloc_failures = 0;
    std::uint64_t cas_failures = 0; // only meaningful for lock-free
    std::uint64_t high_watermark = 0;
    std::uint64_t in_use = 0;
};

class PoolAllocator
{
public:
    PoolAllocator(std::size_t objectSize, std::size_t capacity,
                  PoolOptions options = PoolOptions{});
    virtual ~PoolAllocator();

    virtual void *allocate();
    virtual void deallocate(void *ptr);

    std::size_t used() const { return usedCount.load(std::memory_order_relaxed); }
    std::size_t capacity() const { return poolCapacity; }
    void *memory() const { return memoryBlock; }
    std::size_t blockSize() const { return alignedObjSize * poolCapacity; }
    const PoolOptions &config() const { return options_; }

    PoolStats getStats() const; // snapshot

    template <typename T, typename... Args>
    T *construct(Args &&...args)
    {
        void *mem = allocate();
        return mem ? new (mem) T(std::forward<Args>(args)...) : nullptr;
    }

    template <typename T>
    void destroy(T *ptr)
    {
        if (ptr)
        {
            ptr->~T();
            deallocate(static_cast<void *>(ptr));
        }
    }

protected:
    // core storage
    void *memoryBlock = nullptr;
    void *nonAtomicFreeListHead = nullptr; // base free-list head for single-thread mode
    std::size_t alignedObjSize = 0;
    std::size_t poolCapacity = 0;
    std::atomic<std::size_t> usedCount{0}; // atomic for MT safety

    // config + metrics
    PoolOptions options_;
    struct Metrics
    {
        std::atomic<std::uint64_t> alloc_calls{0};
        std::atomic<std::uint64_t> free_calls{0};
        std::atomic<std::uint64_t> alloc_failures{0};
        std::atomic<std::uint64_t> high_watermark{0};
        std::atomic<std::uint64_t> in_use{0};
        std::atomic<std::uint64_t> cas_failures{0}; // for lock-free
    } metrics_;

    // optional histogram for occupancy
    Histogram *occupancyHist_ = nullptr; // owned
    void sampleOccupancy_();

    // helpers
    std::size_t alignUp(std::size_t n, std::size_t alignment);
    void applyPoison_(void *ptr);  // fill (sizeof(void*), end) with poison
    void verifyPoison_(void *ptr); // verify poison pattern in (sizeof(void*), end)

    // quarantine (single-thread: simple ring)
    std::vector<void *> quarantine_;
    void quarantinePush_(void *ptr); // may flush one back to freelist when over capacity
    void freeListPush_(void *ptr);   // push onto nonAtomic free list
};

class LockFreePoolAllocator : public PoolAllocator
{
public:
    LockFreePoolAllocator(std::size_t objectSize, std::size_t capacity,
                          PoolOptions options = PoolOptions{});
    ~LockFreePoolAllocator() override;

    void *allocate() override;
    void deallocate(void *ptr) override;

private:
    // Lock-free LIFO head
    std::atomic<void *> freeListHead;

    // Side-array for next pointers (out-of-line links)
    std::vector<void *> next_;

    // optional global quarantine for deferred free
    std::vector<void *> lfQuarantine_; // protected by mutex_
    std::mutex mutex_;

    // helpers
    bool inRange_(void *p) const
    {
        auto u = reinterpret_cast<std::uintptr_t>(p);
        auto base = reinterpret_cast<std::uintptr_t>(memoryBlock);
        auto end = base + alignedObjSize * poolCapacity;
        return u >= base && u < end && ((u - base) % alignedObjSize == 0);
    }

    std::size_t indexOf_(void *p) const
    {
        auto u = reinterpret_cast<std::uintptr_t>(p);
        auto base = reinterpret_cast<std::uintptr_t>(memoryBlock);
        return static_cast<std::size_t>((u - base) / alignedObjSize);
    }

    void lfFreeListPush_(void *ptr); // push with CAS using next_[]
};
