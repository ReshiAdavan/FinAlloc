# FinAlloc

custom memory allocator for quant infra

# setup

1. clone
2. run makefile via `make all`

# run

- run tests via `make test`
- executable via `./bin/asan/finalloc`
- benchmark via:

```
# Build uninstrumented benchmarks: make PROFILE=release bench
# per-thread pool, one live allocation
./bin/release/allocBench --allocator=pool --threads=8 --iters=100000 --size=64 --live=1

# lock-free pool, with a live set of 1024 (churn)
./bin/release/allocBench --allocator=lockfree --threads=8 --iters=200000 --size=64 --live=1024

# arena (per-thread): reset at each per-thread live-set epoch
./bin/release/allocBench --allocator=arena --threads=8 --iters=200000 --size=64

# baseline new/delete (immediate or churn with --live)
./bin/release/allocBench --allocator=new --threads=8 --iters=50000 --size=64
```

# features

1. arena allocator

- [x] Multi-Chunk Growth Strategy [Dynamically allocates new chunks as the arena fills; allows for amortized linear growth with high locality]
- [x] Custom construct<T>(...) [API Supports object lifetime mgmt inside arena (T* ptr = arena.construct<T>(...))]
- [x] Memory Alignment + Padding Metadata [Tracks aligned offsets and optionally embeds headers to record allocation metadata]
- [x] Guard Pages / Canary Support [For memory corruption detection (e.g., red zone under/overruns)]
- [x] Thread-local Sub-Arenas [Arena-per-thread; CPU pinning remains the caller's responsibility]
- [x] Arena Group Manager [Shared chunk pool to reuse arenas between sessions (recycle slabs)]
- [x] Journaling or Allocation Tracing Mode [For debugging perf regressions, e.g., log large allocations with source info]
- [x] HugePage Support (Linux) [Backed by mmap with MAP_HUGETLB or madvise for better TLB performance]

2. pool allocator

- [x] Lock-Free Free List [Use an atomic tagged index with compare-and-swap to allow concurrent allocation/deallocation]
- [x] Size-Class Bucketing [Support multiple object sizes by grouping into power-of-two buckets (like tcmalloc)]
- [x] Thread-local Buffer Caches [Per-thread pools that reduce global contention and allocate in batches]
- [x] Object Lifecycle Hooks [Optional callbacks for constructor/destructor on reuse, even for PODs]
- [x] Zeroing or Poisoning Support [Debug mode wipes memory on alloc/dealloc to detect uninitialized accesses]
- [x] Usage Metrics and Histograms [Track allocation/free counts, occupancy histograms, failures, and high-water marks]
- [x] Deferred Freeing / Quarantine [Introduce latency before freeing to detect use-after-free or memory races]

3. numa allocator

- [x] Per-node slab arenas with Linux memory policy before first touch
- [x] Thread registry refreshed from current CPU/node on allocation
- [x] Cross-node allocation counters
- [x] Local-first routing with load-based spillover and idle arena rebalancing
- [x] Portable hardware prefetch hints
- [x] Per-node payload, overhead, idle capacity, policy and allocation profiling (CSV)

# arena safety

Set `guard_pages=true` for inaccessible pages at both **chunk** boundaries
(POSIX systems). These catch chunk-boundary accesses, not every payload overrun.
Set `use_canaries=true` and `canary_size=16` for payload red zones; call
`validate()` or `reset()` to check them (`std::runtime_error` on corruption).
`release()` and destruction always reclaim storage, including corrupted arenas.
ArenaGroup must outlive attached arenas; it releases cached slabs on destruction.
Arenas are single-threaded; use one per thread or external synchronization.
`construct<T>` does not register destructors; call `arena.destroy(p)` for nontrivial objects
before reset/release. Requests larger than max_chunk_size use dedicated chunks.

# Linux huge-page preference

Set `prefer_huge=true` to use a dedicated anonymous mapping, aligned and sized
in 2 MiB units, with `madvise(MADV_HUGEPAGE)`. This is transparent huge-page
**advice**, not a guarantee that huge pages are resident. Failed advice leaves a
usable ordinary mapping. On non-Linux systems the preference falls back to
ordinary allocation. `ArenaChunk::huge_page_advised` reports whether Linux
accepted advice; `prefer_huge` records the request for compatible slab reuse.
It can be combined with guard pages. Small arenas may waste up to a few MiB;
use this option for large working sets. No reserved hugetlb pool is required.
See [Linux THP documentation](https://docs.kernel.org/admin-guide/mm/transhuge.html).

# NUMA API

```cpp
#include "allocators/numaAllocator.hpp"
#include <iostream>
NumaOptions opts;
opts.arena.initial_chunk_size = 1 << 20;
opts.arena.strict_numa = false;
opts.spillover_bytes = 16 << 20;
NumaAllocator alloc(opts);
void *p = alloc.allocate(4096, 64); // local node first; least-used node on spillover
NumaAllocator::prefetch(p);
alloc.writeProfile(std::cout);
```

Linux discovers process-allowed memory nodes from `/proc/self/status` and samples
CPU/node via `getcpu` on each allocation, so migration updates the registry.
`allocate(bytes, alignment, node)` explicitly chooses a configured node and bypasses
spillover. Remote counters compare the requested node with the observed CPU node;
they do not claim to measure physical page residency. Registry entries are latest
observations, retained until allocator destruction (thread IDs may be reused).

Each node owns its own arena; Linux applies `mbind(MPOL_PREFERRED)` to new anonymous
mappings before arena headers or payloads are touched. Policy rejection (including
container restrictions) is counted in `numa_failed_chunks`. Set `strict_numa=true`
for `MPOL_BIND` and an exception if binding fails. Other platforms use a single
logical node by default and report unsupported policy; strict mode throws.
See [Linux mbind semantics](https://man7.org/linux/man-pages/man2/mbind.2.html).

`resetNode`/`reset` invalidate affected allocations; callers must ensure no users
remain. `rebalanceIdle(target)` reclaims other reset/empty arenas and provisions
the target when empty; future slabs are allocated under their destination policy.
It never relocates live objects. All allocator methods serialize metadata access;
payload access and reset lifetimes remain the caller's responsibility. The
spillover threshold measures payload bytes, not reserved memory or a hard limit.
CSV profiling separates payload, padding/headers, unused capacity, and policy
success/failure. No multi-socket performance claim is implied by these features.

# shared pools and thread caches

`ThreadCachedPool` in `allocators/threadCachedPool.hpp` provides bounded per-thread
caches backed by a shared pool: `ThreadCachedPool pool(64, 4096, 16, 32)` chooses
64-byte objects, 4096 blocks, refill batch 16, cache limit 32. Call `allocate()`,
`deallocate(p)`, and optionally `drainCurrentThread()`. Thread exit flushes cached
blocks. Cross-thread frees are supported. Cached blocks reserve shared capacity,
so allocation may fail while another thread holds idle blocks; drain caches when
capacity must be globally available. `used()` counts user allocations;
`backingStats()` includes cache reservations. This wrapper uses minimal pool
options; lifecycle/debug hooks belong to the underlying pool API.

The shared free list uses atomic side links and a 64-bit tagged index head. Its
32-bit mutation tag assumes no operation stalls across 2^32 head mutations;
applications requiring arbitrarily suspended threads need a reclamation scheme.
Head atomics must be lock-free on the target platform. Pool callbacks must be
thread-safe when used concurrently. Pool destruction requires all callers to
have stopped. As with standard allocators, invalid/double frees are caller errors.
Fixed pools support fundamental alignment; over-aligned typed construction is
rejected. Failed constructors return their slot to the pool.

# allocation tracing and reuse

With `journaling=true`, `journalSnapshot()` returns the latest 1024 qualifying
allocations in chronological order, including size, normalized alignment, and
C++20 `source_location` (file, line, function). `journal_threshold_bytes` filters
small allocations. `allocate()` captures its caller; allocations made by
`construct()` report the template's allocation site. The ring is bounded and
preallocated; reset/release preserve its history. Snapshot retrieval copies it.
After reset, retained chunks are reused in order before growing the arena.
`bytesRemaining()` sums remaining capacity from the current chunk onward.

# benchmark builds and reports

`make PROFILE=release bench` builds `bin/release/allocBench` without sanitizers.
`make PROFILE=asan all test` and `make tsan` keep correctness builds separate.
The harness includes thread caches, NUMA, standard PMR resources, JSON/CSV output,
repetitions, sampled operation latency, and a separate ownership-verification
mode. Generate a comparison with `python3 tools/bench_report.py`.
See [benchmark methodology](docs/benchmarking.md) for exact measurement boundaries,
platform skips, memory accounting, and optional mimalloc support.

# randomized correctness and failure recovery

`make PROFILE=asan test` includes deterministic allocation-sequence and failure-
injection tests. Use `make stress STRESS_CASES=10000` for longer seeded campaigns,
`make fuzz-smoke` with a libFuzzer-enabled Clang for coverage-guided testing, and
`make tsan` for separate concurrency checks. See [fuzzing and replay](docs/fuzzing.md)
for seeds, minimized-input replay, injected failure sites, and platform support.
