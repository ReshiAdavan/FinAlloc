#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

struct PoolOptions
{
    bool zero_on_alloc = false;
    bool poison_on_free = false;
    bool verify_poison_on_alloc = false;
    unsigned char poison_byte = 0xA5;

    std::size_t quarantine_size = 0;

    bool sample_histograms = false;
    std::size_t histogram_buckets = 64;

    std::function<void(void *ptr, std::size_t size)> on_alloc = {};
    std::function<void(void *ptr, std::size_t size)> on_free = {};

    static PoolOptions DebugStrong(std::size_t quarantine = 64)
    {
        PoolOptions o;
        o.zero_on_alloc = true;
        o.poison_on_free = true;
        o.verify_poison_on_alloc = true;
        o.quarantine_size = quarantine;
        o.sample_histograms = true;
        return o;
    }
    static PoolOptions MinimalOverhead()
    {
        PoolOptions o;
        // all defaults already minimal
        return o;
    }
};
