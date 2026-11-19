#pragma once
#include "poolAllocator.hpp"
#include <map>

// A bounded cache per calling thread. Any thread may return a valid allocation.
// The wrapper must outlive all outstanding user allocations. Thread exit returns
// reserved blocks when the backing pool still exists; expired entries are inert.
class ThreadCachedPool
{
    struct State
    {
        LockFreePoolAllocator pool;
        std::size_t batch, limit;
        std::atomic<std::size_t> live{0};
        State(std::size_t size, std::size_t count, std::size_t b, std::size_t l)
            : pool(size, count), batch(b), limit(l) {}
    };
    struct Cache
    {
        std::weak_ptr<State> state;
        std::vector<void *> blocks;
        explicit Cache(const std::shared_ptr<State> &s) : state(s) { blocks.reserve(s->limit); }
        ~Cache()
        {
            if (auto s = state.lock())
                for (void *p : blocks) s->pool.deallocate(p);
        }
        Cache(const Cache &) = delete;
        Cache &operator=(const Cache &) = delete;
    };
    using Key = std::weak_ptr<State>;
    inline static thread_local std::map<Key, std::unique_ptr<Cache>, std::owner_less<Key>> caches_;
    std::shared_ptr<State> state_;
    Cache &local_()
    {
        for (auto it = caches_.begin(); it != caches_.end();)
            if (it->first.expired()) it = caches_.erase(it);
            else ++it;
        Key key = state_;
        auto it = caches_.find(key);
        if (it == caches_.end())
            it = caches_.emplace(key, std::make_unique<Cache>(state_)).first;
        return *it->second;
    }
public:
    ThreadCachedPool(std::size_t size, std::size_t capacity,
                     std::size_t batch = 16, std::size_t cacheLimit = 32)
    {
        if (!batch || cacheLimit < batch) throw std::invalid_argument("invalid cache sizes");
        state_ = std::make_shared<State>(size, capacity, batch, cacheLimit);
    }
    ThreadCachedPool(const ThreadCachedPool &) = delete;
    ThreadCachedPool &operator=(const ThreadCachedPool &) = delete;
    void *allocate()
    {
        auto &cache = local_();
        if (cache.blocks.empty())
            for (std::size_t i = 0; i < state_->batch; ++i)
            {
                void *p = state_->pool.allocate();
                if (!p) break;
                cache.blocks.push_back(p);
            }
        if (cache.blocks.empty()) return nullptr;
        void *p = cache.blocks.back();
        cache.blocks.pop_back();
        state_->live.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    void deallocate(void *p)
    {
        if (!p) return;
        if (!state_->pool.owns(p)) throw std::invalid_argument("pointer belongs to another pool");
        auto &cache = local_();
        if (cache.blocks.size() == state_->limit)
            for (std::size_t i = 0; i < state_->batch; ++i)
            {
                state_->pool.deallocate(cache.blocks.back());
                cache.blocks.pop_back();
            }
        state_->live.fetch_sub(1, std::memory_order_relaxed);
        cache.blocks.push_back(p);
    }
    void drainCurrentThread() { caches_.erase(Key(state_)); }
    std::size_t used() const { return state_->live.load(std::memory_order_relaxed); }
    // Backing stats include blocks held in caches; used() counts user allocations.
    PoolStats backingStats() const { return state_->pool.getStats(); }
};
