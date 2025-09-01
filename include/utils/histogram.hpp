#pragma once
#include <vector>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <algorithm>

// Simple linear histogram with atomic counters.
// Buckets cover [min_value, max_value] inclusive range.
// Values outside are clamped to first/last bucket.
class Histogram {
public:
    Histogram(std::uint64_t min_value, std::uint64_t max_value, std::size_t buckets)
        : min_(min_value), max_(std::max<std::uint64_t>(max_value, min_value)),
          buckets_(std::max<std::size_t>(buckets, std::size_t(1))),
          counts_(buckets_) { 
        for (auto& c : counts_) c.store(0, std::memory_order_relaxed);
        width_ = (max_ > min_) ? ((max_ - min_ + 1 + buckets_ - 1) / buckets_) : 1;
        if (width_ == 0) width_ = 1;
    }

    void record(std::uint64_t v) {
        std::size_t idx = indexFor(v);
        counts_[idx].fetch_add(1, std::memory_order_relaxed);
    }

    struct Snapshot {
        std::uint64_t min;
        std::uint64_t max;
        std::size_t buckets;
        std::vector<std::uint64_t> counts;
    };

    Snapshot snapshot() const {
        Snapshot s{min_, max_, buckets_, {}};
        s.counts.resize(buckets_);
        for (std::size_t i = 0; i < buckets_; ++i) {
            s.counts[i] = counts_[i].load(std::memory_order_relaxed);
        }
        return s;
    }

private:
    std::size_t indexFor(std::uint64_t v) const {
        if (v <= min_) return 0;
        if (v >= max_) return buckets_ - 1;
        std::uint64_t off = v - min_;
        std::size_t idx = static_cast<std::size_t>(off / width_);
        if (idx >= buckets_) idx = buckets_ - 1;
        return idx;
        }

    std::uint64_t min_;
    std::uint64_t max_;
    std::size_t   buckets_;
    std::uint64_t width_;
    std::vector<std::atomic<std::uint64_t>> counts_;
};
