#include "allocators/poolAllocator.hpp"
#include "allocators/poolConfig.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

constexpr int THREAD_COUNT = 8;
constexpr int ALLOCATIONS_PER_THREAD = 10000;

struct BenchObj
{
    int x;
    double y;
    BenchObj(int a, double b) : x(a), y(b) {}
    ~BenchObj()
    {
        x = 0;
        y = 0.0;
    }
};

static long long percentile(std::vector<long long> &v, int p)
{
    if (v.empty())
        return 0;
    return v[(v.size() * p) / 100];
}

static void printStats(const char *name,
                       const std::vector<std::vector<long long>> &allLat,
                       std::chrono::steady_clock::time_point t0,
                       std::chrono::steady_clock::time_point t1,
                       int constructed, int destroyed)
{
    std::vector<long long> merged;
    merged.reserve(THREAD_COUNT * ALLOCATIONS_PER_THREAD);
    for (auto &row : allLat)
        merged.insert(merged.end(), row.begin(), row.end());
    std::sort(merged.begin(), merged.end());

    double avg = merged.empty() ? 0.0
                                : std::accumulate(merged.begin(), merged.end(), 0.0) / merged.size();

    std::cout << "\nRunning test for: " << name << "\n";
    std::cout << "Total objects constructed: " << constructed << "\n";
    std::cout << "Total objects destroyed:   " << destroyed << "\n";
    std::cout << "Time elapsed: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " ms\n";
    std::cout << "p50 latency: " << percentile(merged, 50) << " ns\n";
    std::cout << "p95 latency: " << percentile(merged, 95) << " ns\n";
    std::cout << "p99 latency: " << percentile(merged, 99) << " ns\n";
    std::cout << "avg latency: " << static_cast<long long>(avg) << " ns\n";
}

static void runPerThreadPoolPerf()
{
    std::atomic<bool> ready{false};
    std::atomic<int> constructed{0}, destroyed{0};
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> allLatencies(THREAD_COUNT);

    auto worker = [&](int tid)
    {
        // Each thread has its own single-threaded pool
        PoolOptions opts = PoolOptions::MinimalOverhead();
        PoolAllocator pool(sizeof(BenchObj), ALLOCATIONS_PER_THREAD, opts);

        // ready barrier
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();

        auto &lat = allLatencies[tid];
        lat.reserve(ALLOCATIONS_PER_THREAD);

        // (Optional) small warmup
        for (int i = 0; i < 128; ++i)
        {
            auto *o = pool.template construct<BenchObj>(tid, i * 0.1);
            pool.destroy(o);
        }

        for (int i = 0; i < ALLOCATIONS_PER_THREAD; ++i)
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            BenchObj *obj = pool.template construct<BenchObj>(tid, i * 0.1);
            auto t1 = std::chrono::high_resolution_clock::now();

            if (!obj)
            {
                std::cerr << "[PerThreadPool] nullptr alloc at i=" << i
                          << " (thread " << tid << ")\n";
                std::abort();
            }

            lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            pool.destroy(obj);
            ++constructed;
            ++destroyed;
        }
    };

    for (int i = 0; i < THREAD_COUNT; ++i)
        threads.emplace_back(worker, i);

    auto t0 = std::chrono::steady_clock::now();
    ready.store(true, std::memory_order_release);
    for (auto &th : threads)
        th.join();
    auto t1 = std::chrono::steady_clock::now();

    printStats("Per-thread PoolAllocator", allLatencies, t0, t1,
               constructed.load(), destroyed.load());
}

static void runLockFreePerf()
{
    // Use MinimalOverhead to keep latency numbers clean
    PoolOptions opts = PoolOptions::MinimalOverhead();
    // capacity large enough for total outstanding allocations
    const std::size_t capacity = static_cast<std::size_t>(THREAD_COUNT) * ALLOCATIONS_PER_THREAD;

    LockFreePoolAllocator pool(sizeof(BenchObj), capacity, opts);

    std::atomic<bool> ready{false};
    std::atomic<int> constructed{0}, destroyed{0};
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> allLatencies(THREAD_COUNT);

    auto worker = [&](int tid)
    {
        while (!ready.load(std::memory_order_acquire))
            std::this_thread::yield();

        auto &lat = allLatencies[tid];
        lat.reserve(ALLOCATIONS_PER_THREAD);

        // small warmup
        for (int i = 0; i < 128; ++i)
        {
            auto *o = pool.template construct<BenchObj>(tid, i * 0.1);
            pool.destroy(o);
        }

        for (int i = 0; i < ALLOCATIONS_PER_THREAD; ++i)
        {
            auto t0 = std::chrono::high_resolution_clock::now();
            BenchObj *obj = pool.template construct<BenchObj>(tid, i * 0.1);
            auto t1 = std::chrono::high_resolution_clock::now();

            if (!obj)
            {
                std::cerr << "[LockFree] nullptr alloc at i=" << i
                          << " (thread " << tid << ")\n";
                std::abort();
            }

            lat.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            pool.destroy(obj);
            ++constructed;
            ++destroyed;
        }
    };

    for (int i = 0; i < THREAD_COUNT; ++i)
        threads.emplace_back(worker, i);

    auto t0 = std::chrono::steady_clock::now();
    ready.store(true, std::memory_order_release);
    for (auto &t : threads)
        t.join();
    auto t1 = std::chrono::steady_clock::now();

    printStats("LockFreePoolAllocator", allLatencies, t0, t1,
               constructed.load(), destroyed.load());

    PoolStats s = pool.getStats();
    std::cout << "alloc_calls=" << s.alloc_calls
              << " free_calls=" << s.free_calls
              << " high_watermark=" << s.high_watermark
              << " cas_failures=" << s.cas_failures
              << " alloc_failures=" << s.alloc_failures << "\n";
}

int main()
{
    runPerThreadPoolPerf();
    runLockFreePerf();
    return 0;
}
