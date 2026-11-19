#include "allocators/arenaAllocator.hpp"
#include "allocators/numaAllocator.hpp"
#include "allocators/threadCachedPool.hpp"
#include "build_info.hpp"
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <latch>
#include <memory_resource>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#ifdef FINALLOC_MIMALLOC
#include <mimalloc.h>
#endif
#ifndef __has_feature
#define __has_feature(x) 0
#endif
using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;
static double nanos(Clock::duration d) { return std::chrono::duration<double, std::nano>(d).count(); }
struct Options
{
    std::string allocator = "pool", mode = "throughput", format = "json", freeing = "same";
    std::size_t threads = 1, iters = 100000, size = 64, alignment = alignof(std::max_align_t);
    std::size_t live = 64, warmup = 1000, repetitions = 5, sampleEvery = 64, seed = 42;
};
static std::size_t number(const std::string &v)
{
    if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected unsigned integer: " + v);
    std::size_t used = 0;
    auto n = std::stoull(v, &used);
    if (used != v.size()) throw std::invalid_argument("invalid integer");
    return n;
}
static Options parse(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help")
        {
            std::cout << "allocBench --allocator=pool|lockfree|cached|arena|numa|new|pmr-pool|pmr-sync|pmr-arena|empty|mimalloc\n"
                         " --mode=throughput|latency|verify --format=json|csv --free=same|cross --threads=N --iters=N\n"
                         " --size=N --alignment=N --live=N --warmup=N --repetitions=N --sample-every=N --seed=N\n"
                         "live is per thread; arenas reset once per epoch; cross transfers batches to next thread.\n";
            std::exit(0);
        }
        auto split = arg.find('=');
        if (arg.rfind("--", 0) != 0 || split == std::string::npos) throw std::invalid_argument("invalid option " + arg);
        auto k = arg.substr(2, split - 2), v = arg.substr(split + 1);
        if (k == "allocator") o.allocator = v;
        else if (k == "mode") o.mode = v;
        else if (k == "format") o.format = v;
        else if (k == "free") o.freeing = v;
        else if (k == "threads") o.threads = number(v);
        else if (k == "iters") o.iters = number(v);
        else if (k == "size") o.size = number(v);
        else if (k == "alignment") o.alignment = number(v);
        else if (k == "live") o.live = number(v);
        else if (k == "warmup") o.warmup = number(v);
        else if (k == "repetitions") o.repetitions = number(v);
        else if (k == "sample-every") o.sampleEvery = number(v);
        else if (k == "seed") o.seed = number(v);
        else throw std::invalid_argument("unknown option " + k);
    }
    if (!o.threads || o.threads > 256 || !o.iters || !o.size || !o.live || !o.repetitions ||
        !o.alignment || (o.alignment & (o.alignment - 1)) || !o.sampleEvery || o.repetitions > 1000 ||
        o.live > 1000000 || o.iters > 100000000 || o.warmup > 100000000 || o.size > (1ULL << 30) ||
        o.alignment > (1ULL << 20)) throw std::invalid_argument("invalid or excessive workload dimensions");
    o.alignment = std::max(o.alignment, alignof(std::max_align_t));
    if (o.mode != "throughput" && o.mode != "latency" && o.mode != "verify") throw std::invalid_argument("unknown mode");
    if (o.format != "json" && o.format != "csv") throw std::invalid_argument("unknown format");
    if (o.freeing != "same" && o.freeing != "cross") throw std::invalid_argument("unknown free policy");
    return o;
}
static std::string quote(const std::string &s)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s)
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << int(c) << std::dec;
        else out << c;
    return out.str() + '"';
}
static std::string csv(const std::string &s)
{
    std::string out = "\"";
    for (char c : s) { out += c; if (c == '"') out += c; }
    return out + '"';
}
static bool instrumented()
{
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#else
    return std::string(FA_FLAGS).find("-fsanitize") != std::string::npos;
#endif
}
struct CountingResource : std::pmr::memory_resource
{
    std::atomic<std::size_t> current{0}, peak{0};
    void *do_allocate(std::size_t n, std::size_t a) override
    {
        void *p = std::pmr::new_delete_resource()->allocate(n, a);
        auto now = current.fetch_add(n) + n, prev = peak.load();
        while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
        return p;
    }
    void do_deallocate(void *p, std::size_t n, std::size_t a) override
    { std::pmr::new_delete_resource()->deallocate(p, n, a); current.fetch_sub(n); }
    bool do_is_equal(const std::pmr::memory_resource &r) const noexcept override { return this == &r; }
};
struct Backend
{
    const Options &o;
    CountingResource upstream;
    std::vector<std::unique_ptr<PoolAllocator>> pools;
    std::unique_ptr<LockFreePoolAllocator> shared;
    std::unique_ptr<ThreadCachedPool> cached;
    std::vector<std::unique_ptr<ArenaAllocator>> arenas;
    std::unique_ptr<NumaAllocator> numa;
    std::vector<std::unique_ptr<std::pmr::memory_resource>> resources;
    std::vector<void *> emptyStorage;
    std::size_t stride;
    explicit Backend(const Options &opts) : o(opts), stride((o.size + o.alignment - 1) & ~(o.alignment - 1))
    {
        auto capacity = (o.live + 32) * o.threads;
        ArenaOptions ao;
        ao.initial_chunk_size = std::max(std::size_t{4096}, (o.size + o.alignment + 64) * o.live);
        ao.max_chunk_size = std::max(ao.initial_chunk_size, std::size_t{1 << 26});
        if (o.allocator == "pool")
            for (std::size_t t = 0; t < o.threads; ++t) pools.push_back(std::make_unique<PoolAllocator>(o.size, o.live));
        else if (o.allocator == "lockfree") shared = std::make_unique<LockFreePoolAllocator>(o.size, capacity);
        else if (o.allocator == "cached") cached = std::make_unique<ThreadCachedPool>(o.size, capacity);
        else if (o.allocator == "arena")
            for (std::size_t t = 0; t < o.threads; ++t) arenas.push_back(std::make_unique<ArenaAllocator>(ao));
        else if (o.allocator == "numa")
        { NumaOptions no; no.arena = ao; numa = std::make_unique<NumaAllocator>(no); }
        else if (o.allocator == "pmr-sync") resources.push_back(std::make_unique<std::pmr::synchronized_pool_resource>(&upstream));
        else if (o.allocator == "pmr-pool" || o.allocator == "pmr-arena")
            for (std::size_t t = 0; t < o.threads; ++t)
            {
                if (o.allocator == "pmr-pool") resources.push_back(std::make_unique<std::pmr::unsynchronized_pool_resource>(&upstream));
                else resources.push_back(std::make_unique<std::pmr::monotonic_buffer_resource>(ao.initial_chunk_size, &upstream));
            }
        else if (o.allocator == "empty")
            for (std::size_t t = 0; t < o.threads; ++t) emptyStorage.push_back(::operator new(stride * o.live, std::align_val_t(o.alignment)));
    }
    ~Backend() { for (auto p : emptyStorage) ::operator delete(p, std::align_val_t(o.alignment)); }
    bool epochLifetime() const { return o.allocator == "arena" || o.allocator == "numa" || o.allocator == "pmr-arena"; }
    void *allocate(std::size_t t, std::size_t slot)
    {
        if (!pools.empty()) return pools[t]->allocate();
        if (shared) return shared->allocate();
        if (cached) return cached->allocate();
        if (!arenas.empty()) return arenas[t]->allocate(o.size, o.alignment);
        if (numa) return numa->allocate(o.size, o.alignment);
        if (!resources.empty()) return resources[o.allocator == "pmr-sync" ? 0 : t]->allocate(o.size, o.alignment);
        if (!emptyStorage.empty()) return static_cast<char *>(emptyStorage[t]) + slot * stride;
#ifdef FINALLOC_MIMALLOC
        if (o.allocator == "mimalloc") return mi_malloc_aligned(o.size, o.alignment);
#endif
        return ::operator new(o.size, std::align_val_t(o.alignment));
    }
    void deallocate(std::size_t t, void *p)
    {
        if (!pools.empty()) pools[t]->deallocate(p);
        else if (shared) shared->deallocate(p);
        else if (cached) cached->deallocate(p);
        else if (!resources.empty() && !epochLifetime()) resources[o.allocator == "pmr-sync" ? 0 : t]->deallocate(p, o.size, o.alignment);
#ifdef FINALLOC_MIMALLOC
        else if (o.allocator == "mimalloc") mi_free(p);
#endif
        else if (o.allocator == "new") ::operator delete(p, std::align_val_t(o.alignment));
    }
    void reset(std::size_t t)
    {
        if (!arenas.empty()) arenas[t]->reset();
        if (numa && t == 0) numa->reset();
        if (o.allocator == "pmr-arena") static_cast<std::pmr::monotonic_buffer_resource *>(resources[t].get())->release();
    }
    std::size_t reserved() const
    {
        std::size_t total = 0;
        for (const auto &p : pools) total += p->blockSize();
        if (shared) total += shared->blockSize();
        if (cached) { auto s = cached->backingStats(); total += s.capacity * s.aligned_object_size; }
        for (const auto &a : arenas) total += a->memoryStats().reserved;
        if (numa) for (const auto &s : numa->stats()) total += s.memory.reserved;
        if (!resources.empty()) total += upstream.peak.load();
        if (!emptyStorage.empty()) total += stride * o.live * o.threads;
        return total;
    }
};
struct Worker
{
    std::vector<void *> pointers;
    std::vector<double> allocation, freeing, pair, reset;
    std::uint64_t checksum = 0, completed = 0;
    std::uint64_t failures = 0;
};
static unsigned char pattern(const Options &o, std::size_t t, std::size_t i)
{ return static_cast<unsigned char>((o.seed + t * 73 + i * 17) % 251 + 1); }
static void fill(void *p, std::size_t n, unsigned char b)
{ auto *v = static_cast<volatile unsigned char *>(p); for (std::size_t i = 0; i < n; ++i) v[i] = b; }
static std::uint64_t verify(void *p, std::size_t n, unsigned char b)
{
    auto *v = static_cast<volatile unsigned char *>(p);
    for (std::size_t i = 0; i < n; ++i) if (v[i] != b) throw std::runtime_error("payload corruption or duplicate live allocation");
    return static_cast<std::uint64_t>(b) * n;
}
static double quantile(std::vector<double> samples, double q)
{
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    return samples[static_cast<std::size_t>(q * (samples.size() - 1))];
}
static std::string unsupported(const Options &o)
{
    const std::vector<std::string> names = {"pool","lockfree","cached","arena","numa","new","pmr-pool","pmr-sync","pmr-arena","empty","mimalloc"};
    if (std::find(names.begin(), names.end(), o.allocator) == names.end()) throw std::invalid_argument("unknown allocator");
#ifndef FINALLOC_MIMALLOC
    if (o.allocator == "mimalloc") return "mimalloc support was not enabled at build time";
#endif
    if ((o.allocator == "pool" || o.allocator == "lockfree" || o.allocator == "cached") && o.alignment > alignof(std::max_align_t))
        return "fixed pools support fundamental alignment only";
    if (o.freeing == "cross" && (o.threads < 2 || o.allocator == "pool" || o.allocator == "arena" ||
        o.allocator == "pmr-pool" || o.allocator == "pmr-arena")) return "cross-thread freeing requires >=2 threads and a shared/freeable backend";
    return "";
}
struct Result
{
    std::size_t repetition, reserved, numaFailures = 0;
    double setup, coldBackend, seconds;
    std::uint64_t operations = 0, checksum = 0, failures = 0;
    std::vector<double> allocation, freeing, pair, reset;
};
static Result run(const Options &o, std::size_t rep)
{
    auto setupStart = Clock::now();
    Backend backend(o);
    const double coldBackend = nanos(Clock::now() - setupStart);
    std::vector<Worker> workers(o.threads);
    std::latch ready(o.threads), go(1), done(o.threads), teardown(1);
    std::barrier sync(static_cast<std::ptrdiff_t>(o.threads));
    std::vector<std::thread> threads;
    std::vector<std::uintptr_t> addresses;
    addresses.reserve(o.live * o.threads);
    const bool synchronized = o.freeing == "cross" || o.allocator == "numa" || o.mode == "verify";
    const bool sampled = o.mode == "latency";
    for (std::size_t t = 0; t < o.threads; ++t)
        threads.emplace_back([&, t] {
            try
            {
                auto &w = workers[t];
                w.pointers.resize(o.live);
                auto count = (o.iters + o.sampleEvery - 1) / o.sampleEvery;
                w.allocation.reserve(count); w.freeing.reserve(count); w.pair.reserve(count); w.reset.reserve(count + 1);
                auto exercise = [&](std::size_t iterations, bool measured) {
                    for (std::size_t base = 0; base < iterations; base += o.live)
                    {
                        auto n = std::min(o.live, iterations - base);
                        Clock::time_point pairStart;
                        for (std::size_t j = 0; j < n; ++j)
                        {
                            bool sample = measured && sampled && (base + j) % o.sampleEvery == 0;
                            auto start = sample ? Clock::now() : Clock::time_point{};
                            if (sample && o.live == 1) pairStart = start;
                            void *p = backend.allocate(t, j);
                            if (sample) w.allocation.push_back(nanos(Clock::now() - start));
                            if (!p) { ++w.failures; throw std::runtime_error("allocator exhausted"); }
                            if (reinterpret_cast<std::uintptr_t>(p) % o.alignment) throw std::runtime_error("misaligned allocation");
                            w.pointers[j] = p;
                            fill(p, o.size, pattern(o, t, base + j));
                        }
                        if (synchronized) sync.arrive_and_wait();
                        if (o.mode == "verify")
                        {
                            if (t == 0)
                            {
                                addresses.clear();
                                for (const auto &other : workers)
                                    for (std::size_t j = 0; j < n; ++j)
                                        addresses.push_back(reinterpret_cast<std::uintptr_t>(other.pointers[j]));
                                std::sort(addresses.begin(), addresses.end());
                                for (std::size_t j = 1; j < addresses.size(); ++j)
                                    if (addresses[j] - addresses[j - 1] < o.size)
                                        throw std::runtime_error("overlapping live allocations");
                            }
                            sync.arrive_and_wait();
                        }
                        auto owner = o.freeing == "cross" ? (t + o.threads - 1) % o.threads : t;
                        for (std::size_t j = 0; j < n; ++j)
                        {
                            void *p = workers[owner].pointers[j];
                            auto sum = verify(p, o.size, pattern(o, owner, base + j));
                            if (measured) { w.checksum += sum; ++w.completed; }
                            bool sample = measured && sampled && (base + j) % o.sampleEvery == 0;
                            auto start = sample ? Clock::now() : Clock::time_point{};
                            backend.deallocate(t, p);
                            if (sample && !backend.epochLifetime()) w.freeing.push_back(nanos(Clock::now() - start));
                            if (sample && o.live == 1 && !backend.epochLifetime() && o.freeing == "same")
                                w.pair.push_back(nanos(Clock::now() - pairStart));
                        }
                        if (synchronized) sync.arrive_and_wait();
                        bool resetSample = measured && sampled && backend.epochLifetime() && (base / o.live) % o.sampleEvery == 0;
                        auto start = resetSample ? Clock::now() : Clock::time_point{};
                        backend.reset(t);
                        if (resetSample && (!backend.numa || t == 0)) w.reset.push_back(nanos(Clock::now() - start));
                        if (synchronized) sync.arrive_and_wait();
                    }
                };
                exercise(o.warmup, false);
                ready.count_down(); go.wait();
                exercise(o.iters, true);
                done.count_down(); teardown.wait();
                if (backend.cached) backend.cached->drainCurrentThread();
            }
            catch (const std::exception &e) { std::cerr << "benchmark correctness failure: " << e.what() << '\n'; std::abort(); }
        });
    ready.wait();
    const auto start = Clock::now();
    Result r{}; r.repetition = rep; r.setup = nanos(start - setupStart); r.coldBackend = coldBackend;
    go.count_down(); done.wait();
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.reserved = backend.reserved();
    if (backend.numa) for (const auto &n : backend.numa->stats()) r.numaFailures += n.memory.numa_failed_chunks;
    teardown.count_down();
    for (auto &th : threads) th.join();
    for (auto &w : workers)
    {
        r.operations += w.completed; r.checksum += w.checksum; r.failures += w.failures;
        r.allocation.insert(r.allocation.end(), w.allocation.begin(), w.allocation.end());
        r.freeing.insert(r.freeing.end(), w.freeing.begin(), w.freeing.end());
        r.pair.insert(r.pair.end(), w.pair.begin(), w.pair.end());
        r.reset.insert(r.reset.end(), w.reset.begin(), w.reset.end());
    }
    std::uint64_t expected = 0;
    for (std::size_t t = 0; t < o.threads; ++t)
        for (std::size_t i = 0; i < o.iters; ++i) expected += std::uint64_t(pattern(o, t, i)) * o.size;
    if (r.operations != o.iters * o.threads || r.checksum != expected) throw std::runtime_error("missing operations/checksum mismatch");
    return r;
}
int main(int argc, char **argv)
{
    try
    {
        auto o = parse(argc, argv);
        auto reason = unsupported(o);
        std::cout << std::setprecision(12);
        if (!reason.empty())
        {
            if (o.format == "json") std::cout << "{\"status\":\"skipped\",\"allocator\":" << quote(o.allocator) << ",\"reason\":" << quote(reason) << "}\n";
            else std::cout << "status,allocator,reason\nskipped," << csv(o.allocator) << ',' << csv(reason) << '\n';
            return 0;
        }
        if (o.format == "json")
            std::cout << "{\"schema\":1,\"status\":\"ok\",\"allocator\":" << quote(o.allocator)
                << ",\"profile\":" << quote(FA_PROFILE) << ",\"instrumented\":" << (instrumented() ? "true" : "false")
                << ",\"source_digest\":" << quote(FA_SOURCE_DIGEST) << ",\"commit\":" << quote(FA_COMMIT) << ",\"dirty\":" << FA_DIRTY << ",\"compiler\":" << quote(__VERSION__)
                << ",\"compiler_command\":" << quote(FA_COMPILER_COMMAND) << ",\"flags\":" << quote(FA_FLAGS)
                << ",\"cpu\":" << quote(FA_CPU) << ",\"os\":" << quote(FA_OS) << ",\"mode\":" << quote(o.mode)
                << ",\"free\":" << quote(o.freeing) << ",\"lifetime\":" << quote((o.allocator == "arena" || o.allocator == "numa" || o.allocator == "pmr-arena") ? "epoch-reset" : "individual-free")
                << ",\"threads\":" << o.threads << ",\"iters\":" << o.iters << ",\"live_per_thread\":" << o.live
                << ",\"size\":" << o.size << ",\"alignment\":" << o.alignment << ",\"seed\":" << o.seed
                << ",\"warmup\":" << o.warmup << ",\"sample_every\":" << o.sampleEvery << ",\"runs\":[";
        else std::cout << "allocator,profile,instrumented,commit,source_digest,dirty,compiler,flags,cpu,os,mode,free,threads,iters,live,size,alignment,seed,warmup,sample_every,repetition,setup_warmup_ns,seconds,operations,checksum,failures,reserved_payload_or_upstream_peak_bytes,alloc_p50_ns,alloc_p95_ns,alloc_p99_ns,free_p50_ns,free_p99_ns,pair_p50_ns,reset_p50_ns\n";
        std::vector<double> rates;
        for (std::size_t rep = 0; rep < o.repetitions; ++rep)
        {
            auto r = run(o, rep);
            rates.push_back(r.operations / r.seconds);
            if (o.format == "json")
            {
                if (rep) std::cout << ',';
                std::cout << "{\"repetition\":" << rep << ",\"setup_warmup_ns\":" << r.setup << ",\"cold_backend_setup_ns\":" << r.coldBackend << ",\"seconds\":" << r.seconds
                    << ",\"operations\":" << r.operations << ",\"checksum\":" << r.checksum << ",\"allocation_failures\":" << r.failures
                    << ",\"numa_policy_failures\":" << r.numaFailures
                    << ",\"reserved_payload_or_upstream_peak_bytes\":";
                if (o.allocator == "new" || o.allocator == "mimalloc") std::cout << "null";
                else std::cout << r.reserved;
                std::cout << ",\"throughput_ops_s\":" << rates.back();
                auto samples = [&](const char *name, const std::vector<double> &v) {
                    std::cout << ',' << quote(name) << ':';
                    if (v.empty()) std::cout << "null";
                    else std::cout << "{\"samples\":" << v.size() << ",\"p50\":" << quantile(v,.50) << ",\"p95\":" << quantile(v,.95) << ",\"p99\":" << quantile(v,.99) << '}';
                };
                samples("allocate_ns", r.allocation); samples("deallocate_ns", r.freeing); samples("pair_including_payload_ns", r.pair); samples("reset_ns", r.reset);
                std::cout << '}';
            }
            else std::cout << csv(o.allocator) << ',' << csv(FA_PROFILE) << ',' << instrumented() << ',' << csv(FA_COMMIT)
                << ',' << csv(FA_SOURCE_DIGEST) << ',' << FA_DIRTY << ',' << csv(__VERSION__) << ',' << csv(FA_FLAGS) << ',' << csv(FA_CPU) << ',' << csv(FA_OS)
                << ',' << csv(o.mode) << ',' << csv(o.freeing) << ',' << o.threads << ',' << o.iters << ',' << o.live << ',' << o.size
                << ',' << o.alignment << ',' << o.seed << ',' << o.warmup << ',' << o.sampleEvery << ',' << rep << ',' << r.setup << ',' << r.seconds
                << ',' << r.operations << ',' << r.checksum << ',' << r.failures << ',' << r.reserved << ',' << quantile(r.allocation,.50)
                << ',' << quantile(r.allocation,.95) << ',' << quantile(r.allocation,.99) << ',' << quantile(r.freeing,.50)
                << ',' << quantile(r.freeing,.99) << ',' << quantile(r.pair,.50) << ',' << quantile(r.reset,.50) << '\n';
        }
        if (o.format == "json")
        {
            double mean = std::accumulate(rates.begin(), rates.end(), 0.0) / rates.size(), variance = 0;
            for (double r : rates) variance += (r - mean) * (r - mean);
            std::cout << "],\"throughput_mean_ops_s\":" << mean << ",\"throughput_stddev_ops_s\":" << std::sqrt(variance / rates.size()) << "}\n";
        }
        return 0;
    }
    catch (const std::exception &e) { std::cerr << "allocBench: " << e.what() << '\n'; return 2; }
}
