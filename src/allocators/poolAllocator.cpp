#include "allocators/poolAllocator.hpp"
#include <iostream>

ThreadLocalPool::ThreadLocalPool(size_t objSize, size_t capacity)
    : localAllocator(objSize, capacity) {}

void* ThreadLocalPool::allocate() {
    return localAllocator.allocate();
}

void ThreadLocalPool::deallocate(void* ptr) {
    localAllocator.deallocate(ptr);
}

PoolAllocator::PoolAllocator(size_t objectSize, size_t capacity)
    : memoryBlock(nullptr), poolCapacity(capacity), usedCount(0) {
    if (objectSize < sizeof(void*)) {
        objectSize = sizeof(void*);
    }

    alignedObjSize = alignUp(objectSize, alignof(std::max_align_t));
    size_t totalSize = alignedObjSize * poolCapacity;

    memoryBlock = std::malloc(totalSize);
    if (!memoryBlock) throw std::bad_alloc();

    void* block = memoryBlock;
    for (size_t i = 0; i < poolCapacity - 1; ++i) {
        void* next = static_cast<char*>(block) + alignedObjSize;
        std::memcpy(block, &next, sizeof(void*));
        block = next;
    }
    void* nullPtr = nullptr;
    std::memcpy(block, &nullPtr, sizeof(void*));

    nonAtomicFreeListHead = memoryBlock;
}

PoolAllocator::~PoolAllocator() {
    std::free(memoryBlock);
    memoryBlock = nullptr;
    usedCount = 0;
}

void* PoolAllocator::allocate() {
    if (!nonAtomicFreeListHead) {
        std::cerr << "[ALLOCATOR] Pool exhausted!\n";
        return nullptr;
    }
    void* allocated = nonAtomicFreeListHead;
    std::memcpy(&nonAtomicFreeListHead, nonAtomicFreeListHead, sizeof(void*));
    ++usedCount;
    return allocated;
}

void PoolAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    std::memcpy(ptr, &nonAtomicFreeListHead, sizeof(void*));
    nonAtomicFreeListHead = ptr;
    --usedCount;
}

size_t PoolAllocator::alignUp(size_t n, size_t alignment) {
    return (n + alignment - 1) & ~(alignment - 1);
}

LockFreePoolAllocator::LockFreePoolAllocator(size_t objectSize, size_t capacity)
    : PoolAllocator(objectSize, capacity) {
    // Reuse the valid list from PoolAllocator
    freeListHead.store(nonAtomicFreeListHead, std::memory_order_relaxed);
}

LockFreePoolAllocator::~LockFreePoolAllocator() {
    freeListHead.store(nullptr);
}

void* LockFreePoolAllocator::allocate() {
    void* head = freeListHead.load(std::memory_order_acquire);
    void* next = nullptr;

    do {
        if (!head) return nullptr;
        std::memcpy(&next, head, sizeof(void*));
    } while (!freeListHead.compare_exchange_weak(head, next,
              std::memory_order_release, std::memory_order_relaxed));

    ++usedCount;
    return head;
}

void LockFreePoolAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    void* head = freeListHead.load(std::memory_order_relaxed);
    do {
        std::memcpy(ptr, &head, sizeof(void*));
    } while (!freeListHead.compare_exchange_weak(head, ptr,
              std::memory_order_release, std::memory_order_relaxed));
    --usedCount;
}
