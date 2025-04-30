#include "../include/allocators/poolAllocator.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <numeric>
#include <algorithm>

constexpr int THREAD_COUNT = 8;
constexpr int ALLOCATIONS_PER_THREAD = 10000;

struct BenchObj {
    int x;
    double y;
    BenchObj(int a, double b) : x(a), y(b) {}
    ~BenchObj() { x = 0; y = 0.0; }
};

template <typename Allocator>
void runAllocatorTest(const std::string& name) {
    std::cout << "\nRunning test for: " << name << "\n";

    Allocator allocator(sizeof(BenchObj), THREAD_COUNT * ALLOCATIONS_PER_THREAD);
    std::atomic<int> constructed = 0, destroyed = 0;
    std::atomic<bool> ready = false;
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> allLatencies(THREAD_COUNT);

    auto worker = [&](int tid) {
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        auto& latencies = allLatencies[tid];

        for (int i = 0; i < ALLOCATIONS_PER_THREAD; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            BenchObj* obj = allocator.template construct<BenchObj>(tid, i * 0.1);
            auto end = std::chrono::high_resolution_clock::now();
            latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            allocator.destroy(obj);
            ++constructed;
            ++destroyed;
        }
    };

    for (int i = 0; i < THREAD_COUNT; ++i)
        threads.emplace_back(worker, i);

    auto testStart = std::chrono::steady_clock::now();
    ready.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    auto testEnd = std::chrono::steady_clock::now();

    std::vector<long long> merged;
    for (auto& v : allLatencies) merged.insert(merged.end(), v.begin(), v.end());
    std::sort(merged.begin(), merged.end());

    auto percentile = [&](int p) {
        if (merged.empty()) return 0LL;
        return merged[merged.size() * p / 100];
    };

    double avg = merged.empty() ? 0.0 : std::accumulate(merged.begin(), merged.end(), 0.0) / merged.size();

    std::cout << "Total objects constructed: " << constructed.load() << "\n";
    std::cout << "Total objects destroyed:   " << destroyed.load() << "\n";
    std::cout << "Time elapsed: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(testEnd - testStart).count()
              << " ms\n";
    std::cout << "p50 latency: " << percentile(50) << " ns\n";
    std::cout << "p95 latency: " << percentile(95) << " ns\n";
    std::cout << "p99 latency: " << percentile(99) << " ns\n";
    std::cout << "avg latency: " << static_cast<long long>(avg) << " ns\n";
}

void runThreadLocalPoolTest() {
    std::cout << "\nRunning test for: ThreadLocalPool\n";

    std::atomic<int> constructed = 0, destroyed = 0;
    std::atomic<bool> ready = false;
    std::vector<std::thread> threads;
    std::vector<std::vector<long long>> allLatencies(THREAD_COUNT);

    auto worker = [&](int tid) {
        ThreadLocalPool allocator(sizeof(BenchObj), ALLOCATIONS_PER_THREAD);
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        auto& latencies = allLatencies[tid];

        for (int i = 0; i < ALLOCATIONS_PER_THREAD; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            BenchObj* obj = allocator.template construct<BenchObj>(tid, i * 0.1);
            auto end = std::chrono::high_resolution_clock::now();

            if (!obj) {
                std::cerr << "[TEST ERROR] allocator returned nullptr at i=" << i << " (thread " << tid << ")\n";
                std::abort();
            }

            latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            allocator.destroy(obj);
            ++constructed;
            ++destroyed;
        }
    };

    for (int i = 0; i < THREAD_COUNT; ++i)
        threads.emplace_back(worker, i);

    auto testStart = std::chrono::steady_clock::now();
    ready.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    auto testEnd = std::chrono::steady_clock::now();

    std::vector<long long> merged;
    for (auto& v : allLatencies) merged.insert(merged.end(), v.begin(), v.end());
    std::sort(merged.begin(), merged.end());

    auto percentile = [&](int p) {
        if (merged.empty()) return 0LL;
        return merged[merged.size() * p / 100];
    };

    double avg = merged.empty() ? 0.0 : std::accumulate(merged.begin(), merged.end(), 0.0) / merged.size();

    std::cout << "Total objects constructed: " << constructed.load() << "\n";
    std::cout << "Total objects destroyed:   " << destroyed.load() << "\n";
    std::cout << "Time elapsed: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(testEnd - testStart).count()
              << " ms\n";
    std::cout << "p50 latency: " << percentile(50) << " ns\n";
    std::cout << "p95 latency: " << percentile(95) << " ns\n";
    std::cout << "p99 latency: " << percentile(99) << " ns\n";
    std::cout << "avg latency: " << static_cast<long long>(avg) << " ns\n";
}

int main() {
    runThreadLocalPoolTest();
    runAllocatorTest<LockFreePoolAllocator>("LockFreePoolAllocator");
    return 0;
}
