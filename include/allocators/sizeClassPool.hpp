// sizeClassPool.hpp
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <limits>
#include <stdexcept>
#include "poolAllocator.hpp"

template <typename AllocatorType = PoolAllocator>
class SizeClassPool
{
public:
    explicit SizeClassPool(size_t maxSize = 1024, size_t objectsPerClass = 1024)
        : maxObjectSize(maxSize), objectsPerBucket(objectsPerClass)
    {
        if (!maxSize || maxSize > (std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1)) ||
            !objectsPerClass || objectsPerClass >= UINT32_MAX)
            throw std::invalid_argument("invalid size class configuration");
    }

    void *allocate(size_t size)
    {
        if (size > maxObjectSize)
            return nullptr;
        size_t bucketSize = alignToBucket(size);
        if (buckets.find(bucketSize) == buckets.end())
        {
            buckets[bucketSize] = std::make_unique<AllocatorType>(bucketSize, objectsPerBucket);
        }
        return buckets[bucketSize]->allocate();
    }

    void deallocate(void *ptr, size_t size)
    {
        if (!ptr || size > maxObjectSize)
            return;
        size_t bucketSize = alignToBucket(size);
        auto it = buckets.find(bucketSize);
        if (it != buckets.end())
        {
            it->second->deallocate(ptr);
        }
    }

    template <typename T, typename... Args>
    T *construct(Args &&...args)
    {
        void *mem = allocate(sizeof(T));
        if (alignof(T) > alignof(std::max_align_t))
        {
            deallocate(mem, sizeof(T));
            throw std::bad_alloc();
        }
        if (!mem) return nullptr;
        try { return new (mem) T(std::forward<Args>(args)...); }
        catch (...) { deallocate(mem, sizeof(T)); throw; }
    }

    template <typename T>
    void destroy(T *ptr)
    {
        if (ptr)
        {
            ptr->~T();
            deallocate(static_cast<void *>(ptr), sizeof(T));
        }
    }

private:
    size_t alignToBucket(size_t size)
    {
        size_t power = 1;
        while (power < size)
            power <<= 1;
        return power;
    }

    std::map<size_t, std::unique_ptr<AllocatorType>> buckets;
    size_t maxObjectSize;
    size_t objectsPerBucket;
};
