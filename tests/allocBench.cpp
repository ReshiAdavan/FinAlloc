// tests/allocBench.cpp
#include "allocators/poolAllocator.hpp"
#include "allocators/arenaAllocator.hpp"
#include "allocators/poolConfig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Opts
{
    std::string allocator = "pool"; // pool | lockfree | arena | new
    int threads = 8;
    int iters = 100000;
    std::size_t size = 64;
    std::size_t live = 0; // 0 = immediate free; >0 = live set (per process, divided per thread)
};

static bool starts_with(const char *s, const char *pref)
{
    return std::strncmp(s, pref, std::strlen(pref)) == 0;
}

static Opts parse(int argc, char **argv)
{
    Opts o;
    for (int i = 1; i < argc; ++i)
    {
        if (starts_with(argv[i], "--allocator="))
        {
            o.allocator = std::string(argv[i] + std::strlen("--allocator="));
        }
        else if (starts_with(argv[i], "--threads="))
        {
            o.threads = std::stoi(argv[i] + std::strlen("--threads="));
        }
        else if (starts_with(argv[i], "--iters="))
        {
            o.iters = std::stoi(argv[i] + std::strlen("--iters="));
        }
        else if (starts_with(argv[i], "--size="))
        {
            o.size = static_cast<std::size_t>(std::stoul(argv[i] + std::strlen("--size=")));
        }
        else if (starts_with(argv[i], "--live="))
        {
            o.live = static_cast<std::size_t>(std::stoul(argv[i] + std::strlen("--live=")));
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            std::cout <<
                R"(Usage: ./bin/allocBench [--allocator=pool|lockfree|arena|new]
                         [--threads=N] [--iters=N]
                         [--size=BYTES] [--live=LIVESET]
  --live=0           immediate alloc/free (or reset for arena)
  --live>0           maintain per-thread live set of ceil(LIVESET/threads))"
                      << "\n";
            std::exit(0);
        }
    }
    if (o.threads <= 0)
        o.threads = 1;
    if (o.iters <= 0)
        o.iters = 1;
    if (o.size == 0)
        o.size = 1;
    return o;
}

static long long percentile(std::vector<long long> &v, int p)
{
    if (v.empty())
        return 0;
    return v[(v.size() * p) / 100];
}

static void print_summary(const char *name,
                          const std::vector<std::vector<long long>> &allLat,
                          Clock::time_point t0, Clock::time_point t1,
                          int threads, int iters, std::size_t size)
{
    std::vector<long long> merged;
    merged.reserve(static_cast<std::size_t>(threads) * static_cast<std::size_t>(iters));
    for (auto const &row : allLat)
        merged.insert(merged.end(), row.begin(), row.end());
    std::sort(merged.begin(), merged.end());
    double avg = merged.empty() ? 0.0
                                : std::accumulate(merged.begin(), merged.end(), 0.0) / merged.size();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    double ops = (threads * 1.0 * iters) / (ms / 1000.0 + 1e-9);
    std::cout << "\nRunning: " << name << "\n";
    std::cout << "Threads=" << threads << " Iters/Thread=" << iters << " Size=" << size << " bytes\n";
    std::cout << "Time: " << ms << " ms  |  Throughput: " << static_cast<long long>(ops) << " ops/s\n";
    std::cout << "p50: " << percentile(merged, 50) << " ns, "
              << "p95: " << percentile(merged, 95) << " ns, "
              << "p99: " << percentile(merged, 99) << " ns, "
              << "avg: " << static_cast<long long>(avg) << " ns\n";
}

// ---------------- pool (per-thread) ----------------
static void run_pool_per_thread(const Opts &o)
{
    PoolOptions popts = PoolOptions::MinimalOverhead();
    std::atomic<bool> ready{false};
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> lat(o.threads);

    const std::size_t live_pt = (o.live == 0) ? 0 : (o.live + o.threads - 1) / o.threads;

    auto worker = [&](int tid)
    {
        // capacity: enough for this thread's live set, or at least iters for immediate free
        std::size_t cap = live_pt ? live_pt : static_cast<std::size_t>(o.iters);
        PoolAllocator pool(o.size, cap, popts);

        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        auto &l = lat[tid];
        l.reserve(o.iters);

        std::vector<void *> ring;
        ring.reserve(live_pt ? live_pt : 1);

        for (int i = 0; i < o.iters; ++i)
        {
            // FIX: free-before-alloc when ring full to avoid +1 burst
            if (live_pt && ring.size() == live_pt)
            {
                pool.deallocate(ring.front());
                ring.erase(ring.begin());
            }

            auto t0 = std::chrono::high_resolution_clock::now();
            void *p = pool.allocate();
            auto t1 = std::chrono::high_resolution_clock::now();

            if (!p)
            {
                std::cerr << "[pool] nullptr alloc\n";
                std::abort();
            }

            l.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            if (live_pt == 0)
            {
                pool.deallocate(p);
            }
            else
            {
                ring.push_back(p);
            }
        }
        // drain live set
        for (void *p : ring)
            pool.deallocate(p);
    };

    for (int i = 0; i < o.threads; ++i)
        threads.emplace_back(worker, i);
    auto t0 = Clock::now();
    ready.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    auto t1 = Clock::now();

    print_summary("pool (per-thread)", lat, t0, t1, o.threads, o.iters, o.size);
}

// --------------- lockfree (shared) ----------------
static void run_lockfree(const Opts &o)
{
    PoolOptions popts = PoolOptions::MinimalOverhead();
    const std::size_t live_pt = (o.live == 0) ? 0 : (o.live + o.threads - 1) / o.threads;
    // capacity: enough for all threads' live sets + a tiny safety margin
    const std::size_t cap = (live_pt
                                 ? (live_pt + 1) * static_cast<std::size_t>(o.threads) // +1 per thread safety
                                 : static_cast<std::size_t>(o.threads) * 1024);

    LockFreePoolAllocator pool(o.size, cap, popts);

    std::atomic<bool> ready{false};
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> lat(o.threads);

    auto worker = [&](int tid)
    {
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        auto &l = lat[tid];
        l.reserve(o.iters);

        std::vector<void *> ring;
        ring.reserve(live_pt ? live_pt : 1);

        for (int i = 0; i < o.iters; ++i)
        {
            // FIX: free-before-alloc when ring full
            if (live_pt && ring.size() == live_pt)
            {
                pool.deallocate(ring.front());
                ring.erase(ring.begin());
            }

            auto t0 = std::chrono::high_resolution_clock::now();
            void *p = pool.allocate();
            auto t1 = std::chrono::high_resolution_clock::now();

            if (!p)
            {
                std::cerr << "[lockfree] nullptr alloc\n";
                std::abort();
            }
            l.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            if (live_pt == 0)
            {
                pool.deallocate(p);
            }
            else
            {
                ring.push_back(p);
            }
        }
        for (void *p : ring)
            pool.deallocate(p);
    };

    for (int i = 0; i < o.threads; ++i)
        threads.emplace_back(worker, i);
    auto t0 = Clock::now();
    ready.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    auto t1 = Clock::now();

    print_summary("lockfree (shared)", lat, t0, t1, o.threads, o.iters, o.size);

    // quick stats snapshot
    PoolStats s = pool.getStats();
    std::cout << "alloc_calls=" << s.alloc_calls
              << " free_calls=" << s.free_calls
              << " high_watermark=" << s.high_watermark
              << " cas_failures=" << s.cas_failures
              << " alloc_failures=" << s.alloc_failures << "\n";
}

// ---------------- arena (per-thread) ---------------
static void run_arena(const Opts &o)
{
    ArenaOptions aopts;
    aopts.use_canaries = false; // keep overhead low for perf
    std::atomic<bool> ready{false};
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> lat(o.threads);
    const std::size_t live_pt = (o.live == 0) ? 0 : (o.live + o.threads - 1) / o.threads;

    auto worker = [&](int tid)
    {
        ArenaAllocator arena(aopts);
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        auto &l = lat[tid];
        l.reserve(o.iters);

        std::size_t live_now = 0;
        for (int i = 0; i < o.iters; ++i)
        {
            // (Optional) reset-before-alloc when live set reached
            if (live_pt && live_now == live_pt)
            {
                arena.reset();
                live_now = 0;
            }

            auto t0 = std::chrono::high_resolution_clock::now();
            void *p = arena.allocate(o.size, alignof(std::max_align_t));
            auto t1 = std::chrono::high_resolution_clock::now();

            if (!p)
            {
                std::cerr << "[arena] nullptr alloc\n";
                std::abort();
            }
            l.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            if (live_pt != 0)
                ++live_now;
        }
        arena.release();
    };

    for (int i = 0; i < o.threads; ++i)
        threads.emplace_back(worker, i);
    auto t0 = Clock::now();
    ready.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    auto t1 = Clock::now();

    print_summary("arena (per-thread)", lat, t0, t1, o.threads, o.iters, o.size);
}

// --------------- baseline new/delete ---------------
static void run_newdelete(const Opts &o)
{
    const std::size_t live_pt = (o.live == 0) ? 0 : (o.live + o.threads - 1) / o.threads;

    std::atomic<bool> ready{false};
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> lat(o.threads);

    auto worker = [&](int tid)
    {
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();
        auto &l = lat[tid];
        l.reserve(o.iters);

        std::vector<void *> ring;
        ring.reserve(live_pt ? live_pt : 1);

        for (int i = 0; i < o.iters; ++i)
        {
            // FIX: free-before-alloc to keep live set at target size
            if (live_pt && ring.size() == live_pt)
            {
                ::operator delete(ring.front());
                ring.erase(ring.begin());
            }

            auto t0 = std::chrono::high_resolution_clock::now();
            void *p = ::operator new(o.size, std::nothrow);
            auto t1 = std::chrono::high_resolution_clock::now();

            if (!p)
            {
                std::cerr << "[new] nullptr alloc\n";
                std::abort();
            }

            l.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            if (live_pt == 0)
            {
                ::operator delete(p);
            }
            else
            {
                ring.push_back(p);
            }
        }
        for (void *p : ring)
            ::operator delete(p);
    };

    for (int i = 0; i < o.threads; ++i)
        threads.emplace_back(worker, i);
    auto t0 = Clock::now();
    ready.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    auto t1 = Clock::now();

    print_summary("baseline new/delete", lat, t0, t1, o.threads, o.iters, o.size);
}

int main(int argc, char **argv)
{
    Opts o = parse(argc, argv);

    if (o.allocator == "pool")
    {
        run_pool_per_thread(o);
    }
    else if (o.allocator == "lockfree")
    {
        run_lockfree(o);
    }
    else if (o.allocator == "arena")
    {
        run_arena(o);
    }
    else if (o.allocator == "new")
    {
        run_newdelete(o);
    }
    else
    {
        std::cerr << "Unknown allocator: " << o.allocator
                  << " (expected: pool | lockfree | arena | new)\n";
        return 2;
    }
    return 0;
}
