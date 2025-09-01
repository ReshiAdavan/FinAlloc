#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

struct PoolOptions {
    // Debug hygiene
    bool zero_on_alloc = false;         // memset(0) after a successful pop
    bool poison_on_free = false;        // fill with poison byte on free/quarantine
    bool verify_poison_on_alloc = false;// assert memory is poisoned before use (except first sizeof(void*))
    unsigned char poison_byte = 0xA5;   // classic pattern

    // Deferred free / quarantine
    std::size_t quarantine_size = 0;    // 0 = disabled. Otherwise retain this many recently freed blocks.

    // Metrics / histogram sampling
    bool sample_histograms = false;     // collect occupancy histogram samples (cost is tiny but nonzero)
    std::size_t histogram_buckets = 64; // used if sample_histograms==true

    // Raw hooks (called on raw memory, independent of construct/destroy<T>)
    // Notice: on_alloc is invoked *after* zeroing; on_free is invoked *before* poisoning.
    std::function<void(void* ptr, std::size_t size)> on_alloc = {};
    std::function<void(void* ptr, std::size_t size)> on_free  = {};

    // Handy presets
    static PoolOptions DebugStrong(std::size_t quarantine = 64) {
        PoolOptions o;
        o.zero_on_alloc = true;
        o.poison_on_free = true;
        o.verify_poison_on_alloc = true;
        o.quarantine_size = quarantine;
        o.sample_histograms = true;
        return o;
    }
    static PoolOptions MinimalOverhead() {
        PoolOptions o;
        // all defaults already minimal
        return o;
    }
};
