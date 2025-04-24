# FinAlloc
custom memory allocator for quant infra

# setup

1. clone
2. run makefile via `make`
3. run executable via `./bin/finalloc`

# todos

1. arena allocator
- [ ] Multi-Chunk Growth Strategy [Dynamically allocates new chunks as the arena fills; allows for amortized linear growth with high locality]
- [ ] Custom construct<T>(...) [API	Supports object lifetime mgmt inside arena (T* ptr = arena.construct<T>(...))]
- [ ] Memory Alignment + Padding Metadata [Tracks aligned offsets and optionally embeds headers to record allocation metadata]
- [ ] Guard Pages / Canary Support [For memory corruption detection (e.g., red zone under/overruns)]
- [ ] Thread-local Sub-Arenas [Arena-per-thread to avoid false sharing, tuned for CPU core affinity]
- [ ] Arena Group Manager [Shared chunk pool to reuse arenas between sessions (recycle slabs)]
- [ ] Journaling or Allocation Tracing Mode [For debugging perf regressions, e.g., log large allocations with source info]
- [ ] HugePage Support (Linux) [Backed by mmap with MAP_HUGETLB or madvise for better TLB performance]

2. pool allocator
- [X] Lock-Free Free List [Use std::atomic<void*> with compare-and-swap to allow concurrent allocation/deallocation]
- [X] Size-Class Bucketing [Support multiple object sizes by grouping into power-of-two buckets (like tcmalloc)]
- [X] Thread-local Buffer Caches [Per-thread pools that reduce global contention and allocate in batches]
- [ ] Object Lifecycle Hooks [Optional callbacks for constructor/destructor on reuse, even for PODs]
- [ ] Zeroing or Poisoning Support [Debug mode wipes memory on alloc/dealloc to detect uninitialized accesses]
- [ ] Usage Metrics and Histograms [Track alloc counts, dealloc counts, high-water marks, fragmentation over time]
- [ ] Deferred Freeing / Quarantine [Introduce latency before freeing to detect use-after-free or memory races]

3. numa allocator
- [ ] Per-Node Slab Allocators [Maintain separate arena/pool per NUMA node (e.g., node 0 handles threads 0–15)]
- [ ] Thread Affinity Registry [Track each thread’s CPU/core → node mapping dynamically]
- [ ] Cross-Node Allocation Detection [Warn if a thread allocates from a remote NUMA node]
- [ ] Load-Balanced NUMA-Aware Arena Pools [Dynamically reallocate arenas across NUMA nodes to handle usage skew]
- [ ] Hardware Prefetch Hints [Use cache line prefetching (_mm_prefetch) to reduce stalls on frequent access patterns]
- [ ] NUMA-Integrated Memory Profiler [Visualize how much memory per NUMA node is in use, fragmented, idle]