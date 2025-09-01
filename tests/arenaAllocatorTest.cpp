#include "allocators/arenaAllocator.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

// simple payload to use with construct<T>()
struct BenchObj {
    int    x;
    double y;
    BenchObj(int a, double b) : x(a), y(b) {}
    ~BenchObj() { x = 0; y = 0.0; }
};

static void test_basic_construct_and_alignment() {
    std::cout << "[A] basic construct + alignment\n";
    ArenaOptions opts;
    opts.initial_chunk_size = 32 * 1024;   // small to force growth quickly
    opts.growth_factor      = 2.0;
    opts.max_chunk_size     = 1 << 20;     // 1 MiB cap for this test
    opts.use_canaries       = true;
    opts.canary_size        = 16;

    ArenaAllocator arena(opts);

    // many small constructs
    for (int i = 0; i < 2000; ++i) {
        BenchObj* p = arena.construct<BenchObj>(i, i * 0.5);
        if (!p) { std::cerr << "construct returned null\n"; std::abort(); }
        // optional: call destructor explicitly if you want to test lifetime
        // p->~BenchObj();
    }

    // alignment sweeps (raw allocate)
    for (std::size_t align : {8ull, 64ull, 256ull, 4096ull}) {
        void* p = arena.allocate(100, align);
        if (!p) { std::cerr << "allocate returned null (align=" << align << ")\n"; std::abort(); }
        auto u = reinterpret_cast<std::uintptr_t>(p);
        if ((u & (align - 1)) != 0) {
            std::cerr << "misaligned pointer " << p << " for align " << align << "\n";
            std::abort();
        }
    }
}

static void test_growth_and_reset() {
    std::cout << "[B] growth and reset\n";
    ArenaOptions opts;
    opts.initial_chunk_size = 32 * 1024;  // small
    opts.growth_factor      = 2.0;
    opts.max_chunk_size     = 1 << 20;

    ArenaAllocator arena(opts);

    std::size_t before = arena.chunkCount();

    // allocate enough to require multiple chunks
    const std::size_t big = 20 * 1024;
    for (int i = 0; i < 10; ++i) {
        void* p = arena.allocate(big, 64);
        if (!p) { std::cerr << "allocate returned null during growth\n"; std::abort(); }
    }

    std::size_t after = arena.chunkCount();
    if (after <= before) {
        std::cerr << "expected chunkCount to grow (before=" << before << ", after=" << after << ")\n";
        std::abort();
    }

    // reset should keep chunks but rewind offsets; small allocs shouldn't grow further
    arena.reset();
    std::size_t chunksBeforeReuse = arena.chunkCount();
    for (int i = 0; i < 1000; ++i) {
        void* p = arena.allocate(64, alignof(std::max_align_t));
        if (!p) { std::cerr << "allocate returned null after reset\n"; std::abort(); }
    }
    std::size_t chunksAfterReuse = arena.chunkCount();
    if (chunksAfterReuse != chunksBeforeReuse) {
        std::cerr << "unexpected growth after reset (" << chunksBeforeReuse << " -> " << chunksAfterReuse << ")\n";
        std::abort();
    }
}

static void test_thread_local_arena() {
    std::cout << "[C] ThreadLocalArena (MT sanity)\n";
    std::atomic<bool> ready{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            // get (or create) per-thread arena with default options
            ArenaAllocator& tla = ThreadLocalArena::instance();

            while (!ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < 5000; ++i) {
                void* p = tla.allocate(32, 16);
                if (!p) { std::cerr << "[TLA] allocate returned null\n"; std::abort(); }
            }
        });
    }

    ready.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    // optional cleanup
    ThreadLocalArena::reset();
}

static void test_arena_group_recycler() {
    std::cout << "[D] ArenaGroup recycler\n";
    ArenaGroup grp;

    // direct acquire/release
    {
        auto c1 = grp.acquire(64 * 1024, false, false);
        if (!c1.base || c1.size < 64 * 1024) { std::cerr << "group acquire failed\n"; std::abort(); }
        grp.release(std::move(c1));
    }
    {
        auto c2 = grp.acquire(32 * 1024, false, false); // should reuse a slab >= request
        if (!c2.base || c2.size < 32 * 1024) { std::cerr << "group re-acquire failed\n"; std::abort(); }
        grp.release(std::move(c2));
    }

    // integration: attach group to an arena, grow, release; then reuse with another arena
    ArenaOptions opts;
    opts.initial_chunk_size = 32 * 1024;
    opts.growth_factor      = 2.0;
    opts.max_chunk_size     = 1 << 20;

    {
        ArenaAllocator a(opts);
        a.attachGroup(&grp);
        for (int i = 0; i < 6; ++i) {
            (void)a.allocate(24 * 1024, 64); // force growth, push multiple chunks into group on release
        }
        a.release(); // chunks returned to grp
    }
    {
        ArenaAllocator b(opts);
        b.attachGroup(&grp);
        // first chunk in ctor is OS-backed in this portable impl, but the *next* growth should come from group
        std::size_t before = b.chunkCount();
        for (int i = 0; i < 4; ++i) {
            (void)b.allocate(40 * 1024, 64);
        }
        std::size_t after = b.chunkCount();
        if (after <= before) {
            std::cerr << "expected chunk growth with group-attached arena\n";
            std::abort();
        }
        b.release();
    }
}

int main() {
    std::cout << "\n==== arenaAllocatorTest ====\n";
    test_basic_construct_and_alignment();
    test_growth_and_reset();
    test_thread_local_arena();
    test_arena_group_recycler();
    std::cout << "[OK] arenaAllocatorTest passed.\n";
    return 0;
}
