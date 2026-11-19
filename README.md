# FinAlloc

custom memory allocator for quant infra

# setup

1. clone
2. run makefile via `make all`

# run

- run tests via `make test`
- executable via `./bin/finalloc`
- benchmark via:

```
# per-thread pool, immediate alloc/free
./bin/allocBench --allocator=pool --threads=8 --iters=100000 --size=64

# lock-free pool, with a live set of 1024 (churn)
./bin/allocBench --allocator=lockfree --threads=8 --iters=200000 --size=64 --live=1024

# arena (per-thread). With --live>0 it does epoch resets every live/threads ops
./bin/allocBench --allocator=arena --threads=8 --iters=200000 --size=64

# baseline new/delete (immediate or churn with --live)
./bin/allocBench --allocator=new --threads=8 --iters=50000 --size=64
```

# todos

1. arena allocator

- [x] Multi-Chunk Growth Strategy [Dynamically allocates new chunks as the arena fills; allows for amortized linear growth with high locality]
- [x] Custom construct<T>(...) [API Supports object lifetime mgmt inside arena (T* ptr = arena.construct<T>(...))]
- [x] Memory Alignment + Padding Metadata [Tracks aligned offsets and optionally embeds headers to record allocation metadata]
- [x] Guard Pages / Canary Support [For memory corruption detection (e.g., red zone under/overruns)]
- [x] Thread-local Sub-Arenas [Arena-per-thread to avoid false sharing, tuned for CPU core affinity]
- [x] Arena Group Manager [Shared chunk pool to reuse arenas between sessions (recycle slabs)]
- [x] Journaling or Allocation Tracing Mode [For debugging perf regressions, e.g., log large allocations with source info]
- [x] HugePage Support (Linux) [Backed by mmap with MAP_HUGETLB or madvise for better TLB performance]

2. pool allocator

- [x] Lock-Free Free List [Use std::atomic<void*> with compare-and-swap to allow concurrent allocation/deallocation]
- [x] Size-Class Bucketing [Support multiple object sizes by grouping into power-of-two buckets (like tcmalloc)]
- [x] Thread-local Buffer Caches [Per-thread pools that reduce global contention and allocate in batches]
- [x] Object Lifecycle Hooks [Optional callbacks for constructor/destructor on reuse, even for PODs]
- [x] Zeroing or Poisoning Support [Debug mode wipes memory on alloc/dealloc to detect uninitialized accesses]
- [x] Usage Metrics and Histograms [Track alloc counts, dealloc counts, high-water marks, fragmentation over time]
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
`construct<T>` does not register destructors; explicitly destroy nontrivial objects
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
