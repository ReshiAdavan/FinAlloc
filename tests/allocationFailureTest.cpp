#include "allocators/arenaAllocator.hpp"
#include "allocators/sizeClassPool.hpp"
#include "allocators/threadCachedPool.hpp"
#include "utils/allocationControl.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace finalloc::testing;
#ifdef FINALLOC_ENABLE_FAILURE_INJECTION
template <typename F> static void sweep(const char *label, F operation)
{
    std::size_t injected = 0;
    for (auto domain : {AllocationDomain::backing, AllocationDomain::metadata, AllocationDomain::any})
    {
        bool reachedSuccess = false;
        for (std::size_t skip = 0; skip < 64; ++skip)
        {
            auto before = outstandingBacking.load();
            bool caught = false, triggered = false;
            {
                FailureScope failure(domain, skip);
                try { operation(); } catch (const std::bad_alloc &) { caught = true; }
                triggered = failure.triggered();
                // Noexcept recycler release is allowed to consume a failure by
                // returning the slab to the OS instead of propagating it.
                if (caught) assert(triggered);
            }
            assert(outstandingBacking.load() == before);
            if (!triggered) { reachedSuccess = true; break; }
            ++injected;
        }
        assert(reachedSuccess);
    }
    assert(injected > 0);
    std::cout << label << ": injected " << injected << " failures without backing leaks\n";
}
#endif
int main()
{
#ifdef FINALLOC_ENABLE_FAILURE_INJECTION
    PoolOptions debug = PoolOptions::DebugStrong(4);
    sweep("base pool", [&] { PoolAllocator pool(64, 16, debug); });
    sweep("shared pool", [&] { LockFreePoolAllocator pool(64, 16, debug); });
    sweep("thread cache", [] { ThreadCachedPool pool(64, 32, 4, 8); auto p = pool.allocate(); pool.deallocate(p); pool.drainCurrentThread(); });
    sweep("size classes", [] { SizeClassPool<> pool(128, 4); auto p = pool.allocate(33); pool.deallocate(p,33); });
    ArenaOptions options;
    options.initial_chunk_size = options.max_chunk_size = 4096;
    options.use_canaries = true; options.canary_size = 8; options.journaling = true;
    sweep("arena heap", [&] { ArenaAllocator a(options); a.allocate(32); a.allocate(8192,256); a.reset(); });
#if defined(__unix__) || defined(__APPLE__)
    options.guard_pages = true;
    sweep("arena mappings", [&] { ArenaAllocator a(options); a.allocate(8192); });
#endif
    sweep("recycler", [] { ArenaGroup group; auto slab = group.acquire(4096,false,false); group.release(std::move(slab)); });
    ArenaAllocator arena(options);
    auto *old = static_cast<unsigned char *>(arena.allocate(64));
    std::memset(old, 0x55, 64);
    for (auto domain : {AllocationDomain::backing, AllocationDomain::metadata})
    {
        FailureScope failure(domain, 0);
        bool caught = false;
        try { arena.allocate(2 * 1024 * 1024); } catch (const std::bad_alloc &) { caught = true; }
        assert(caught && failure.triggered());
        for (int i = 0; i < 64; ++i) assert(old[i] == 0x55);
        arena.validate();
    }
    // Metadata failure during cache creation leaves its backing pool usable.
    ThreadCachedPool cached(64, 32, 4, 8);
    {
        FailureScope failure(AllocationDomain::metadata, 0);
        bool caught = false;
        try { cached.allocate(); } catch (const std::bad_alloc &) { caught = true; }
        assert(caught && cached.used() == 0);
    }
    auto p = cached.allocate(); assert(p); cached.deallocate(p); cached.drainCurrentThread();
    assert(cached.backingStats().in_use == 0);
#else
    std::cout << "failure injection skipped: enable ASan or fuzz profile\n";
#endif
}
