# Reproducible allocator measurements

Build `make PROFILE=release bench` and run:

```
python3 tools/bench_report.py --allocators=empty,new,pool,lockfree,cached,arena,numa,pmr-pool,pmr-sync,pmr-arena
python3 tools/bench_report.py --output benchmark-results --report-only
```

The sweep writes raw JSON plus report.md. Rebuilding a report refuses instrumented
results, non-release profiles, and mixed hosts/build provenance. Use a new output
directory for each commit, build, or host. Existing files are replaced only for
the selected configurations; avoid reusing a directory for unrelated sweeps.

`make PROFILE=asan all test`, `make PROFILE=tsan ...`, and release builds use
separate directories. A build-provenance header changes when flags, compiler
command, commit, dirty status, or host metadata change, triggering recompilation.
The compiler version and sanitized status are also recorded by the executable.
CPU model detection degrades explicitly when sandbox policy hides that information.
Build metadata identifies the build host; rebuild after copying binaries to a
new measurement host. Custom compiler flags remain supported, but instrumented
runs are rejected by the report generator even if labeled release.

The benchmark supports JSON or CSV output. Example:

```
./bin/release/allocBench --allocator=cached --threads=4 --live=64 --iters=100000 --mode=latency --sample-every=64 --repetitions=5
./bin/release/allocBench --allocator=lockfree --threads=4 --free=cross --mode=verify --iters=10000 --repetitions=1
```

## What is measured

- A cycle allocates, writes every payload byte, verifies it, then retires it.
  Payload access is volatile to prevent the compiler discarding the workload.
  There is no simulated financial computation.
- `live` is per thread. Batches have constant-time slot indexing; the final batch
  handles a remainder exactly. Seed controls deterministic payload patterns.
- Backend construction and warmup occur before every worker announces readiness.
  Steady timing starts before releasing those workers and ends when they finish.
  Thread teardown and cache draining follow measurement. Cold backend setup and
  total setup/warmup times are separate from steady duration.
- Throughput mode has no per-operation clocks. Latency mode samples allocation
  and deallocation separately; sample count is included. p99 is an observed sample
  quantile, not a guarantee about rare production events. Pair timing is provided
  only for same-thread live=1 and includes payload work and harness overhead.
- `arena`, `numa`, and `pmr-arena` have epoch lifetimes. Per-pointer free timings
  are null; reset is sampled separately. FinAlloc arenas retain reset storage;
  standard monotonic PMR release returns upstream storage, a deliberate semantic
  difference. Never interpret their rows as equivalent individual-free costs.
- Cross-thread mode rotates batches to a peer. Epoch barriers are included in
  cycle throughput. NUMA also synchronizes resets; its throughput includes those
  barriers even with same-thread retirement. Unsynchronized per-thread resources
  reject cross-thread mode explicitly.
- Empty uses preallocated slots, with the same payload and harness work, to show
  measurement overhead. It is not subtracted from other results.
- Verify mode additionally synchronizes epochs and checks all live ranges for
  overlap before retirement. It is a correctness mode; the report refuses its
  timings. The sweep runs a bounded verification pass for each configuration.
- Allocation failure or corruption fails the run; unsuccessful output is not a
  performance result. Configuration limitations return structured skipped rows.

Reserved-memory estimates are backend-specific: fixed-pool payload capacity
(excluding metadata/TLS), usable arena chunk capacity (excluding mapping guards
and metadata), empty payload capacity, or PMR upstream peak. They are not RSS.
new/delete and mimalloc have unknown reserved memory (null in JSON; CSV uses 0
as an unavailable sentinel). NUMA policy failure counts are separate from timing;
no physical locality or multi-socket performance is inferred on unsupported hosts.

Build optional mimalloc support with its headers/library installed, e.g. append
`-DFINALLOC_MIMALLOC` and the include path to release CXXFLAGS, and set
`BENCH_LIBS=-lmimalloc` plus any required library search path. Its absence is a
structured skip; the default build has no external C++ library dependency.

Use multiple repetitions and retain raw results. Run on an otherwise idle host
and record CPU affinity/frequency settings externally for rigorous experiments.
Shared CI checks correctness/schema and uploads smoke results; it enforces no
speed thresholds. There is no claim that FinAlloc beats a general-purpose
allocator. See https://google.github.io/benchmark/user_guide.html for methodology.
