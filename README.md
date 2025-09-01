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
- [ ] Guard Pages / Canary Support [For memory corruption detection (e.g., red zone under/overruns)]
- [x] Thread-local Sub-Arenas [Arena-per-thread to avoid false sharing, tuned for CPU core affinity]
- [x] Arena Group Manager [Shared chunk pool to reuse arenas between sessions (recycle slabs)]
- [x] Journaling or Allocation Tracing Mode [For debugging perf regressions, e.g., log large allocations with source info]
- [ ] HugePage Support (Linux) [Backed by mmap with MAP_HUGETLB or madvise for better TLB performance]

2. pool allocator

- [x] Lock-Free Free List [Use std::atomic<void*> with compare-and-swap to allow concurrent allocation/deallocation]
- [x] Size-Class Bucketing [Support multiple object sizes by grouping into power-of-two buckets (like tcmalloc)]
- [x] Thread-local Buffer Caches [Per-thread pools that reduce global contention and allocate in batches]
- [x] Object Lifecycle Hooks [Optional callbacks for constructor/destructor on reuse, even for PODs]
- [x] Zeroing or Poisoning Support [Debug mode wipes memory on alloc/dealloc to detect uninitialized accesses]
- [x] Usage Metrics and Histograms [Track alloc counts, dealloc counts, high-water marks, fragmentation over time]
- [x] Deferred Freeing / Quarantine [Introduce latency before freeing to detect use-after-free or memory races]

3. numa allocator

- [ ] Per-Node Slab Allocators [Maintain separate arena/pool per NUMA node (e.g., node 0 handles threads 0–15)]
- [ ] Thread Affinity Registry [Track each thread’s CPU/core → node mapping dynamically]
- [ ] Cross-Node Allocation Detection [Warn if a thread allocates from a remote NUMA node]
- [ ] Load-Balanced NUMA-Aware Arena Pools [Dynamically reallocate arenas across NUMA nodes to handle usage skew]
- [ ] Hardware Prefetch Hints [Use cache line prefetching (_mm_prefetch) to reduce stalls on frequent access patterns]
- [ ] NUMA-Integrated Memory Profiler [Visualize how much memory per NUMA node is in use, fragmented, idle]
