# Proposed next phase

Status: PR A and PR E are implemented. Remaining entries are scoped proposals.

Assumption: the original feature checklist is complete at the documented scope.
The next phase should make FinAlloc useful inside a real C++ application and
produce reproducible evidence about its latency, memory use, and tradeoffs.

## Current starting point

- The benchmark selects pool, shared lock-free pool, arena, or new/delete;
  it does not exercise ThreadCachedPool or NumaAllocator.
- Default builds enable AddressSanitizer. Correctness builds and performance
  builds need separate artifacts so instrumentation cannot contaminate results.
- Individual samples time allocation, while total runtime also includes worker
  setup/teardown, freeing, and vector-based live-set bookkeeping. These metrics
  need explicit labels and consistent measurement boundaries.
- The executable is an allocator demonstration rather than a quant workload.
- Public allocators have no std::pmr adapter for standard containers.

## PR A: Reproducible benchmark harness (implemented)

Priority: first. Size: medium. Dependencies: none.

Scope:
- Separate release, ASan/UBSan, and TSan outputs/build directories.
- Benchmark existing allocators, including thread caches and NUMA, through a
  common harness with explicitly comparable object-lifetime policies.
- Synchronize worker readiness after setup, warm up, and exclude setup/teardown
  from steady-state timing. Report cold-start costs separately.
- Use constant-time live-set bookkeeping, observable payload writes/checksums,
  and an empty-harness measurement to characterize measurement overhead.
- Measure allocation, deallocation, allocation/free pairs, and end-to-end
  throughput separately. Keep batch throughput and sampled per-operation latency
  as separate measurement modes to expose timer overhead.
- Sweep size, alignment, threads, live-set size, and same/cross-thread freeing.
- Export JSON/CSV with commit, compiler, flags, CPU/OS, workload seed, allocation
  failures, and repetitions. Summarize p50/p95/p99 and memory reservations.
- Include new/delete and applicable standard PMR resource baselines; make an
  external allocator such as mimalloc an optional comparison dependency.

Acceptance:
- A documented command regenerates a small comparison report from raw results.
- Output identifies instrumented builds and prevents mixing them into release
  comparisons. Unsupported configurations report skips explicitly.
- Repeated runs include variability; no noisy shared-CI performance gate.
- Correctness checks fail on missing operations, duplicate live allocations, or
  corrupt payload checksums. Arena reset lifetimes are stated separately from
  individually freed pool workloads.

Sources: https://google.github.io/benchmark/user_guide.html

## PR B: Standard C++ memory-resource integration

Priority: second. Size: medium. Dependencies: none; measure using A.

Scope:
- Add std::pmr::memory_resource adapters for arenas and size-class pools.
- Define resource equality, upstream fallback, exhaustion behavior, allocation
  alignment, and ownership. Document arena deallocation as a no-op until reset.
- Support or explicitly route oversized and over-aligned requests upstream.
- Add an installable library target, a namespaced CMake target, and a small
  external-consumer example; preserve existing Makefile workflows.

Acceptance:
- std::pmr::vector, string, and unordered_map operate correctly through adapters.
- Tests exercise container growth, exceptions, alignment, resource equality,
  balanced upstream ownership, and resource/container lifetime ordering.
- A separate example project builds against the installed library.

Source: https://eel.is/c++draft/mem.res.class

## PR C: Deterministic market-event replay workload

Priority: third. Size: large. Dependencies: A; B for container integration.

Scope:
- Generate seeded synthetic add/cancel/modify events and replay them through a
  small limit-order book. Keep this an allocator evaluation application, not a
  trading strategy or a full exchange simulator.
- Make allocator selection configurable while preserving the same book
  implementation, input stream, event semantics, and final checksum.
- Exercise persistent order nodes and temporary per-batch scratch memory.
- Compare baseline allocation, pooled nodes, and arena scratch combinations.
- Report end-to-end event latency, allocation counts, reserved memory, and
  throughput under steady traffic, bursts, and cancellation-heavy traffic.

Acceptance:
- Every allocation configuration produces identical final book state/checksum.
- Tests cover crossed/empty books according to documented rules, invalid IDs,
  cancellation, modification, and deterministic event ordering.
- Publish raw data and a report explaining where FinAlloc helps or loses.
- Claim only replay-processing latency; exclude network/wire-latency claims.

## PR D: Capacity planning and a preallocated operating mode

Priority: fourth. Size: medium to large. Dependencies: A; validate on C.

Scope:
- Add explicit reserve/prewarm APIs and a fixed-capacity policy that rejects
  growth once initialization finishes.
- Bound arena metadata, canary records, journals, and cache bookkeeping as well
  as payload storage. Define behavior when any reserved capacity is exhausted.
- Expose committed/reserved capacity, user-live bytes, cache-held bytes,
  exhaustion counts, and high-water marks where meaningful.
- Produce a capacity-sizing report from the replay workload.

Acceptance:
- Instrument internal allocation/OS-storage paths and demonstrate that the
  warmed fixed-capacity path performs no new backing allocation.
- Exhaustion is predictable and leaves existing payloads intact.
- Tests distinguish payload exhaustion from metadata exhaustion.
- Do not call this hard real-time: scheduling and cache/page behavior still vary.

## PR E: Fuzzing and failure-injection infrastructure (implemented)

Priority: alongside A/B. Size: medium. Dependencies: none.

Scope:
- Add seeded operation-sequence tests and bounded libFuzzer targets covering
  allocate/free, alignment, reset, moves, size classes, and cache drains.
- Maintain a model of valid live allocations; check alignment, non-overlap,
  payload integrity, ownership accounting, and reset invalidation.
- Inject failures into backing storage and metadata growth to check exception
  safety and cleanup. Add bounded sanitizer fuzz smoke runs to CI.
- Keep concurrency stress/TSan separate from single-thread fuzzing and provide
  a longer-running local stress target.

Acceptance:
- Failing inputs replay deterministically and minimized cases become regressions.
- Expected exhaustion is distinguished from corruption or leaks.
- CI budgets remain bounded; long campaigns are opt-in.

## Later experiments, gated on measurements

- Shard shared pools and add per-owner remote-free queues if A/C show contention.
  Compare tail latency, memory retention, and fairness against the current
  shared pool and thread cache before selecting an algorithm.
- Adaptive cache limits and coordinated draining if idle caches cause meaningful
  capacity starvation; test burst-to-idle recovery and bounded retained memory.
- Add a report viewer after JSON/CSV schemas stabilize. Prioritize comparisons
  and reproducibility over a live dashboard.

## Separate follow-up work on existing limitations

These deepen earlier features rather than constitute the new roadmap above:
- Validate strict NUMA binding and physical residency on suitable Linux hardware;
  run repeatable multi-socket locality experiments with controlled CPU affinity.
- Evaluate a reclamation design that removes the finite mutation-tag assumption.
  Model-check the chosen concurrency algorithm before replacing the free list.
- Add explicit hugetlb support only if experiments justify its configuration and
  deployment cost beyond transparent huge-page advice.
- Add live-object migration only with a handle/indirection API; ordinary raw
  pointers cannot be transparently relocated under the current contract.

Recommended sequence: A and E, then B, then C, then D. Optimize only against the
resulting workload evidence. A useful milestone is a reproducible allocator
comparison inside deterministic market-event replay, including negative results.
