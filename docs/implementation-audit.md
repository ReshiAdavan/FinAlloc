# Implementation audit (2026-11-18)

The pre-audit README listed guard pages, huge pages, and NUMA as unfinished.
The first two arena option flags were ignored; NUMA's source/header were empty.
Several checked items also needed completion or correction.

| Feature | Original gap | Delivered behavior |
| --- | --- | --- |
| Guard pages | Flag ignored | POSIX inaccessible regions at both chunk boundaries |
| Canary debugging | Patterns written, never checked | validate/reset check independent payload red-zone records |
| Linux huge pages | Flag ignored | Aligned anonymous mappings with MADV_HUGEPAGE advice and acceptance status |
| NUMA | Empty files | Node arenas, Linux memory policy, live CPU/node observations, routing, remote counters, idle reclamation, prefetch, CSV profiles |
| Thread caches | Benchmark-local pools only | Bounded TLS caches with batching, remote returns, drain and exit cleanup |
| Shared free list | Plain side-link races; pointer-only ABA | Atomic side links and tagged index head; retire counters before reuse |
| Journaling | Private ring with zero return addresses | Bounded chronological snapshots with source location |
| Arena reuse/lifetime | Only last chunk reused; destroy was a no-op | Sequential retained-chunk reuse and explicit typed destruction |
| Slab recycler | Cached storage leaked; incompatible/undersized reuse | Destructor cleanup and matching on size, guards, huge preference and NUMA policy |

## Validation

CI builds with warnings as errors and AddressSanitizer + UndefinedBehaviorSanitizer
on Linux and macOS. A separate macOS ThreadSanitizer job exercises shared-pool
contention and thread caches. Tests assert boundary faults, corrupted canaries,
source tracing/ring rollover, slab reuse, oversized arithmetic, sparse simulated
NUMA topology, migration observations, spillover, reset/rebalance lifetimes,
exclusive pool-slot ownership, capacity recovery, and cache cleanup.

Native Linux tests inspect huge-page advice in /proc/self/smaps and attempt
strict NUMA binding/querying when the host permits it. Portable fallback and
strict-policy errors are also tested. These are correctness checks, not evidence
of latency improvement or multi-socket scalability.

## Precise claim boundaries

- Guard pages protect chunk boundaries. Canary checks protect payload red zones
  when validate/reset is called; release/destruction still free corrupted storage.
- Huge-page preference is advisory and may fall back to ordinary pages.
- Preferred NUMA policy may fall back. Policy failures are exposed in snapshots;
  strict mode throws. Remote counters compare requested and observed thread nodes,
  not measured physical residency. Non-Linux fallback has one logical node.
- Rebalancing reclaims idle arenas and provisions a destination. Live objects
  never move. Thread observations do not pin threads to CPUs.
- Profiling reports payload, arena overhead, idle capacity and policy counters;
  it does not sample hardware cache misses or automatically render a dashboard.
- Pool occupancy metrics do not claim historical fragmentation tracking.
- The free list's 32-bit tag requires that no operation remain suspended across
  2^32 head mutations. Cached blocks reserve global capacity until drained.
- Users explicitly destroy nontrivial arena objects before reset/release.
  Individual arena instances require external synchronization or one per thread.
