#include "allocators/poolAllocator.hpp"
#include <iostream>

// ---------------------------- PoolAllocator ----------------------------
PoolAllocator::PoolAllocator(std::size_t objectSize, std::size_t capacity,
                             PoolOptions options)
    : poolCapacity(capacity), options_(options)
{
    if (objectSize < sizeof(void*)) objectSize = sizeof(void*);
    alignedObjSize = alignUp(objectSize, alignof(std::max_align_t));
    const std::size_t totalSize = alignedObjSize * poolCapacity;

    memoryBlock = std::malloc(totalSize);
    if (!memoryBlock) throw std::bad_alloc();

    // Build single-thread free list (in-block links used only by base class)
    void* block = memoryBlock;
    for (std::size_t i = 0; i < poolCapacity - 1; ++i) {
        void* next = static_cast<char*>(block) + alignedObjSize;
        std::memcpy(block, &next, sizeof(void*));
        block = next;
    }
    void* nullPtr = nullptr;
    std::memcpy(block, &nullPtr, sizeof(void*));

    nonAtomicFreeListHead = memoryBlock;

    // Pre-poison all blocks if requested
    if (options_.poison_on_free) {
        char* p = static_cast<char*>(memoryBlock);
        for (std::size_t i = 0; i < poolCapacity; ++i) {
            if (alignedObjSize > sizeof(void*)) {
                std::memset(p + sizeof(void*), options_.poison_byte, alignedObjSize - sizeof(void*));
            }
            p += alignedObjSize;
        }
    }

    // Metrics base
    metrics_.in_use.store(0, std::memory_order_relaxed);
    metrics_.high_watermark.store(0, std::memory_order_relaxed);

    // Optional histogram
    if (options_.sample_histograms) {
        occupancyHist_ = new Histogram(0, poolCapacity, options_.histogram_buckets);
    }

    // Quarantine storage (single-thread) — reserve once
    if (options_.quarantine_size > 0) {
        quarantine_.reserve(options_.quarantine_size + 1);
    }
}

PoolAllocator::~PoolAllocator() {
    delete occupancyHist_;
    occupancyHist_ = nullptr;
    std::free(memoryBlock);
    memoryBlock = nullptr;
    nonAtomicFreeListHead = nullptr;
    usedCount.store(0, std::memory_order_relaxed);
}

void* PoolAllocator::allocate() {
    metrics_.alloc_calls.fetch_add(1, std::memory_order_relaxed);

    // Pop non-atomically (single-threaded)
    if (!nonAtomicFreeListHead) {
        metrics_.alloc_failures.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    void* allocated = nonAtomicFreeListHead;
    std::memcpy(&nonAtomicFreeListHead, nonAtomicFreeListHead, sizeof(void*));

    usedCount.fetch_add(1, std::memory_order_relaxed);

    auto in_use_now = metrics_.in_use.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uint64_t old_hwm = metrics_.high_watermark.load(std::memory_order_relaxed);
    while (in_use_now > old_hwm &&
           !metrics_.high_watermark.compare_exchange_weak(old_hwm, in_use_now, std::memory_order_relaxed)) {}

    if (options_.verify_poison_on_alloc && options_.poison_on_free) {
        verifyPoison_(allocated);
    }
    if (options_.zero_on_alloc) {
        std::memset(allocated, 0, alignedObjSize);
    }
    if (options_.on_alloc) options_.on_alloc(allocated, alignedObjSize);

    if (occupancyHist_) sampleOccupancy_();
    return allocated;
}

void PoolAllocator::deallocate(void* ptr) {
    if (!ptr) return;

    if (options_.on_free) options_.on_free(ptr, alignedObjSize);
    if (options_.poison_on_free) applyPoison_(ptr);

    if (options_.quarantine_size > 0) {
        quarantinePush_(ptr);
    } else {
        freeListPush_(ptr);
    }

    usedCount.fetch_sub(1, std::memory_order_relaxed);

    metrics_.free_calls.fetch_add(1, std::memory_order_relaxed);
    metrics_.in_use.fetch_sub(1, std::memory_order_relaxed);
    if (occupancyHist_) sampleOccupancy_();
}

std::size_t PoolAllocator::alignUp(std::size_t n, std::size_t alignment) {
    return (n + alignment - 1) & ~(alignment - 1);
}

void PoolAllocator::applyPoison_(void* ptr) {
    if (alignedObjSize > sizeof(void*)) {
        std::memset(static_cast<char*>(ptr) + sizeof(void*), options_.poison_byte,
                    alignedObjSize - sizeof(void*));
    }
}

void PoolAllocator::verifyPoison_(void* ptr) {
    if (alignedObjSize <= sizeof(void*)) return;
    unsigned char* p = static_cast<unsigned char*>(ptr) + sizeof(void*);
    for (std::size_t i = 0; i < alignedObjSize - sizeof(void*); ++i) {
        if (p[i] != options_.poison_byte) {
            std::cerr << "[POOL] Poison verification failed at byte " << i
                      << " ptr=" << ptr << "\n";
            std::abort();
        }
    }
}

void PoolAllocator::sampleOccupancy_() {
    if (!occupancyHist_) return;
    auto in_use = metrics_.in_use.load(std::memory_order_relaxed);
    if (in_use > poolCapacity) in_use = poolCapacity;
    occupancyHist_->record(in_use);
}

void PoolAllocator::quarantinePush_(void* ptr) {
    quarantine_.push_back(ptr);
    if (quarantine_.size() > options_.quarantine_size) {
        void* victim = quarantine_.front();
        quarantine_.erase(quarantine_.begin());
        freeListPush_(victim);
    }
}

void PoolAllocator::freeListPush_(void* ptr) {
    std::memcpy(ptr, &nonAtomicFreeListHead, sizeof(void*));
    nonAtomicFreeListHead = ptr;
}

PoolStats PoolAllocator::getStats() const {
    PoolStats s;
    s.capacity = poolCapacity;
    s.object_size = alignedObjSize;
    s.aligned_object_size = alignedObjSize;
    s.alloc_calls  = metrics_.alloc_calls.load(std::memory_order_relaxed);
    s.free_calls   = metrics_.free_calls.load(std::memory_order_relaxed);
    s.alloc_failures = metrics_.alloc_failures.load(std::memory_order_relaxed);
    s.cas_failures   = metrics_.cas_failures.load(std::memory_order_relaxed);
    s.high_watermark = metrics_.high_watermark.load(std::memory_order_relaxed);
    s.in_use = metrics_.in_use.load(std::memory_order_relaxed);
    return s;
}

// ---------------------------- LockFreePoolAllocator ----------------------------
LockFreePoolAllocator::LockFreePoolAllocator(std::size_t objectSize, std::size_t capacity,
                                             PoolOptions options)
    : PoolAllocator(objectSize, capacity, options),
      freeListHead(nonAtomicFreeListHead)
{
    // Build side-array links
    next_.resize(poolCapacity, nullptr);
    char* base = static_cast<char*>(memoryBlock);
    for (std::size_t i = 0; i + 1 < poolCapacity; ++i) {
        next_[i] = base + (i + 1) * alignedObjSize;
    }
    next_[poolCapacity - 1] = nullptr;

    if (options_.quarantine_size > 0) {
        lfQuarantine_.reserve(options_.quarantine_size + 1);
    }
}

LockFreePoolAllocator::~LockFreePoolAllocator() {
    freeListHead.store(nullptr, std::memory_order_relaxed);
}

void* LockFreePoolAllocator::allocate() {
    metrics_.alloc_calls.fetch_add(1, std::memory_order_relaxed);
    void* head = freeListHead.load(std::memory_order_acquire);

    while (true) {
        if (!head) {
            metrics_.alloc_failures.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (!inRange_(head)) {
            std::cerr << "[ERROR] Invalid head pointer in allocate(): " << head << "\n";
            std::abort();
        }

        // Read next from side array (not from user memory)
        void* next = next_[indexOf_(head)];

        if (freeListHead.compare_exchange_weak(head, next,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            usedCount.fetch_add(1, std::memory_order_relaxed);

            auto in_use_now = metrics_.in_use.fetch_add(1, std::memory_order_relaxed) + 1;
            std::uint64_t old_hwm = metrics_.high_watermark.load(std::memory_order_relaxed);
            while (in_use_now > old_hwm &&
                   !metrics_.high_watermark.compare_exchange_weak(old_hwm, in_use_now, std::memory_order_relaxed)) {}

            if (options_.verify_poison_on_alloc && options_.poison_on_free) {
                verifyPoison_(head);
            }
            if (options_.zero_on_alloc) {
                std::memset(head, 0, alignedObjSize);
            }
            if (options_.on_alloc) options_.on_alloc(head, alignedObjSize);
            if (occupancyHist_) sampleOccupancy_();
            return head;
        } else {
            metrics_.cas_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void LockFreePoolAllocator::lfFreeListPush_(void* ptr) {
    // CAS-based push using side-array link
    const std::size_t idx = indexOf_(ptr);
    void* head = freeListHead.load(std::memory_order_relaxed);
    do {
        next_[idx] = head; // publish link before releasing CAS
    } while (!freeListHead.compare_exchange_weak(head, ptr,
               std::memory_order_release, std::memory_order_relaxed));
}

void LockFreePoolAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    if (!inRange_(ptr)) {
        std::cerr << "[ERROR] Invalid pointer passed to deallocate(): " << ptr << "\n";
        std::abort();
    }

    if (options_.on_free) options_.on_free(ptr, alignedObjSize);
    if (options_.poison_on_free) applyPoison_(ptr);

    if (options_.quarantine_size > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        lfQuarantine_.push_back(ptr);
        if (lfQuarantine_.size() > options_.quarantine_size) {
            void* victim = lfQuarantine_.front();
            lfQuarantine_.erase(lfQuarantine_.begin());
            lfFreeListPush_(victim);
        }
    } else {
        lfFreeListPush_(ptr);
    }

    usedCount.fetch_sub(1, std::memory_order_relaxed);

    metrics_.free_calls.fetch_add(1, std::memory_order_relaxed);
    metrics_.in_use.fetch_sub(1, std::memory_order_relaxed);
    if (occupancyHist_) sampleOccupancy_();
}
