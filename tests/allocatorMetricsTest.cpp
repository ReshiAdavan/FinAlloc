#include "allocators/poolAllocator.hpp"
#include "allocators/poolConfig.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

static std::size_t alignUp(std::size_t n, std::size_t alignment)
{
    return (n + alignment - 1) & ~(alignment - 1);
}

static void require(bool cond, const char *msg)
{
    if (!cond)
    {
        std::cerr << "[TEST] " << msg << "\n";
        std::abort();
    }
}

int main()
{
    std::cout << "\n==== allocatorMetricsTest ====\n";

    {
        std::cout << "[A] zero_on_alloc + poison/verify\n";
        PoolOptions opts = PoolOptions::DebugStrong(/*quarantine*/ 8);
        // Keep it small so test is quick
        const std::size_t reqSize = 64;
        const std::size_t cap = 32;
        LockFreePoolAllocator pool(reqSize, cap, opts);

        const std::size_t aligned = alignUp(reqSize, alignof(std::max_align_t));

        // 1) allocate raw, verify zeros
        void *p = pool.allocate();
        require(p != nullptr, "A1: allocate returned nullptr");
        {
            // entire block is zeroed by zero_on_alloc
            const unsigned char *bytes = static_cast<const unsigned char *>(p);
            for (std::size_t i = 0; i < aligned; ++i)
            {
                if (bytes[i] != 0)
                {
                    std::cerr << "[TEST] A1: zero_on_alloc failed at byte " << i << "\n";
                    std::abort();
                }
            }
        }

        // write some junk, then free (which will poison after first word)
        std::memset(p, 0xCC, aligned);
        pool.deallocate(p);

        // 2) allocate again; poison is verified internally before zeroing.
        // If poison verification fails, the allocator will abort.
        void *q = pool.allocate();
        require(q != nullptr, "A2: allocate after poison returned nullptr");

        // and q should be zeroed again
        {
            const unsigned char *bytes = static_cast<const unsigned char *>(q);
            for (std::size_t i = 0; i < aligned; ++i)
            {
                if (bytes[i] != 0)
                {
                    std::cerr << "[TEST] A2: zero_on_alloc failed at byte " << i << "\n";
                    std::abort();
                }
            }
        }
        pool.deallocate(q);
    }

    {
        std::cout << "[B] quarantine semantics\n";
        // Case B1: capacity=4, quarantine=4 -> freelist empty after freeing all, next alloc should fail
        {
            PoolOptions o = PoolOptions::MinimalOverhead();
            o.poison_on_free = true; // optional
            o.verify_poison_on_alloc = true;
            o.zero_on_alloc = true;
            o.quarantine_size = 4;

            LockFreePoolAllocator pool(32, 4, o);
            void *a[4];
            for (int i = 0; i < 4; ++i)
            {
                a[i] = pool.allocate();
                require(a[i] != nullptr, "B1: initial allocate failed");
            }
            for (int i = 0; i < 4; ++i)
                pool.deallocate(a[i]);

            void *should_be_null = pool.allocate();
            require(should_be_null == nullptr, "B1: allocate should fail due to full quarantine");
        }

        {
            PoolOptions o = PoolOptions::MinimalOverhead();
            o.poison_on_free = true;
            o.verify_poison_on_alloc = true;
            o.zero_on_alloc = true;
            o.quarantine_size = 4;

            LockFreePoolAllocator pool(32, 5, o);
            void *a[5];
            for (int i = 0; i < 5; ++i)
            {
                a[i] = pool.allocate();
                require(a[i] != nullptr, "B2: initial allocate failed");
            }
            for (int i = 0; i < 5; ++i)
                pool.deallocate(a[i]); // 5th pushes one back to freelist

            void *should_be_non_null = pool.allocate();
            require(should_be_non_null != nullptr, "B2: allocate should succeed after quarantine overflow");
            pool.deallocate(should_be_non_null);
        }
    }

    {
        std::cout << "[C] metrics sanity (MT)\n";
        constexpr int THREADS = 6;
        constexpr int ITERS = 4000;

        PoolOptions o = PoolOptions::MinimalOverhead();
        // Keep debug knobs off to minimize overhead in this section
        o.quarantine_size = 0;
        o.sample_histograms = true; // ok to sample
        LockFreePoolAllocator pool(64, 64 * THREADS, o);

        std::atomic<bool> go{false};
        std::vector<std::thread> ths;
        ths.reserve(THREADS);

        for (int t = 0; t < THREADS; ++t)
        {
            ths.emplace_back([&]
                             {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < ITERS; ++i) {
                    void* p = pool.allocate();
                    if (!p) {
                        std::cerr << "[TEST] C: unexpected alloc failure\n";
                        std::abort();
                    }
                    pool.deallocate(p);
                } });
        }
        go.store(true, std::memory_order_release);
        for (auto &th : ths)
            th.join();

        PoolStats s = pool.getStats();
        require(s.in_use == 0, "C: in_use must be 0 after all thread joins");
        require(s.alloc_calls == s.free_calls, "C: alloc_calls must equal free_calls");
        require(s.high_watermark > 0, "C: high_watermark should be > 0");
        std::cout << "   alloc_calls=" << s.alloc_calls
                  << " free_calls=" << s.free_calls
                  << " high_watermark=" << s.high_watermark
                  << " cas_failures=" << s.cas_failures
                  << " alloc_failures=" << s.alloc_failures
                  << "\n";
    }

    std::cout << "[OK] allocatorMetricsTest passed.\n";
    return 0;
}
