#include "allocators/threadCachedPool.hpp"
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstring>
#include <limits>
#include <set>
#include <thread>

static void sharedPoolStress()
{
    constexpr std::size_t capacity = 32;
    PoolOptions opts;
    opts.zero_on_alloc = true;
    opts.poison_on_free = true;
    opts.verify_poison_on_alloc = true;
    LockFreePoolAllocator pool(64, capacity, opts);
    std::array<std::atomic<bool>, capacity> owners{};
    std::barrier start(8);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < 20000; ++i)
            {
                void *p = pool.allocate();
                if (!p) { std::this_thread::yield(); continue; }
                auto slot = (static_cast<char *>(p) - static_cast<char *>(pool.memory())) / 64;
                assert(!owners[slot].exchange(true));
                std::memset(p, 0x55, 64);
                if (i % 11 == 0) std::this_thread::yield();
                assert(owners[slot].exchange(false));
                pool.deallocate(p);
            }
        });
    for (auto &t : workers) t.join();
    auto s = pool.getStats();
    assert(s.in_use == 0 && s.high_watermark <= 8);
    assert(s.free_calls + s.alloc_failures == s.alloc_calls);
    std::set<void *> all;
    for (std::size_t i = 0; i < capacity; ++i)
    {
        void *p = pool.allocate();
        assert(p && all.insert(p).second);
    }
    assert(pool.allocate() == nullptr);
    for (auto p : all) pool.deallocate(p);
}
static void caches()
{
    ThreadCachedPool cached(64, 256, 8, 16);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([&] {
            for (int i = 0; i < 5000; ++i)
            {
                void *p = cached.allocate();
                assert(p);
                std::memset(p, 0x55, 64);
                cached.deallocate(p);
            }
            // Thread exit flushes remaining reservations.
        });
    for (auto &t : workers) t.join();
    assert(cached.used() == 0);
    assert(cached.backingStats().in_use == 0);
    assert(cached.backingStats().alloc_calls < 1000); // batched reuse
    void *remote = cached.allocate();
    std::thread receiver([&] { cached.deallocate(remote); });
    receiver.join();
    cached.drainCurrentThread();
    assert(cached.used() == 0 && cached.backingStats().in_use == 0);
    std::set<void *> all;
    for (int i = 0; i < 256; ++i)
    {
        void *p = cached.allocate();
        assert(p && all.insert(p).second);
    }
    assert(cached.allocate() == nullptr);
    for (auto p : all) cached.deallocate(p);
    cached.drainCurrentThread();
    assert(cached.backingStats().in_use == 0);
    // Expired thread-local entries must never dereference a destroyed pool.
    for (int i = 0; i < 100; ++i)
    {
        ThreadCachedPool shortLived(64, 8, 2, 4);
        auto p = shortLived.allocate();
        shortLived.deallocate(p);
    }
}
int main()
{
    sharedPoolStress();
    caches();
    for (auto capacity : {std::size_t{0}, std::size_t{UINT32_MAX}})
    {
        bool caught = false;
        try { PoolAllocator invalid(64, capacity); }
        catch (const std::invalid_argument &) { caught = true; }
        assert(caught);
    }
    bool caught = false;
    try { PoolAllocator invalid(std::numeric_limits<std::size_t>::max(), 2); }
    catch (const std::bad_alloc &) { caught = true; }
    assert(caught);
    PoolAllocator small(16, 1);
    struct TooLarge { char bytes[64]; };
    caught = false;
    try { small.construct<TooLarge>(); } catch (const std::bad_alloc &) { caught = true; }
    assert(caught);
}
