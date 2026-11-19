# Allocation fuzzing, replay, and failure injection

Run the deterministic model under ASan/UBSan:

```
make PROFILE=asan bin/asan/allocatorSequenceTest
./bin/asan/allocatorSequenceTest --seed=20261119 --cases=1000
./bin/asan/allocatorSequenceTest --seed=20261119 --start=37 --cases=1
./bin/asan/allocatorSequenceTest --input=fuzz/corpus/lifetimes
make stress STRESS_CASES=10000
```

Every generated case uses 2048 bytes derived from seed and case index, independent
of earlier cases. The same byte-sequence interpreter is used by libFuzzer. It
checks live payloads, alignment, non-overlapping ranges, fixed-pool usage, cache
usage/draining, canary validation, epoch invalidation, move construction/assignment,
size classes, constructor exceptions, and oversized requests. Requests stay within
bounded sizes/capacities; nullptr on fixed-pool or quarantine exhaustion is expected.
Only valid frees are generated; arbitrary invalid/double frees are outside the API
contract. The model does not dereference pointers invalidated by reset/release.

## Coverage-guided runs

Use a Clang installation containing the libFuzzer runtime:

```
make fuzz-smoke FUZZ_RUNS=10000 FUZZ_SECONDS=30
./bin/fuzz/allocatorFuzz build/fuzz/corpus -max_total_time=300 -max_len=8192 -artifact_prefix=build/fuzz/artifacts/
./bin/fuzz/allocatorFuzz -minimize_crash=1 -exact_artifact_path=/tmp/minimized-input build/fuzz/artifacts/crash-HASH
./bin/asan/allocatorSequenceTest --input=/tmp/minimized-input
```

The target compiles both allocator sources and the interpreter with coverage,
ASan, and UBSan; sanitizer flags are separate from the deterministic test profile.
It caps interpreted inputs at 8192 bytes. `make fuzz-smoke` copies committed seeds
into an ignored working corpus and sets run/time/individual-input limits. Linux CI
runs the bounded campaign and uploads the working corpus and failure artifacts,
including on failure. Promote a minimized failing input into fuzz/corpus and add
a deterministic regression when a bug is fixed. Concurrency is tested separately
with `make tsan`; this fuzzer does not claim to explore thread interleavings.

Some Apple Clang installations omit libclang_rt.fuzzer_osx.a. The deterministic
interpreter still works there; use an LLVM installation with libFuzzer or Linux
CI for coverage-guided runs. No dependency is installed automatically.

## Controlled allocation failures

```
make PROFILE=asan bin/asan/allocationFailureTest
./bin/asan/allocationFailureTest
```

ASan and fuzz profiles define `FINALLOC_ENABLE_FAILURE_INJECTION`; release and
TSan do not. Tests arm thread-local, one-shot failure checkpoints separately for
backing storage, metadata, or either, skipping N matching sites before throwing
std::bad_alloc. Scope destruction restores previous injection settings.

Covered sites include pool malloc, arena malloc/mmap, journal initialization,
canary/chunk vector growth, NUMA masks, recycler bins/slabs, histogram construction,
quarantine reserve, shared-list links, size-class bucket creation, and thread-cache
state/local-cache creation. Backing-resource counters verify cleanup after each
failure. The sweep also verifies old payloads survive failed arena growth and
that cache allocation recovers after failed metadata initialization.

This is explicit-site fault injection, not an override of every process-wide
new/malloc or OS syscall. It does not simulate mprotect/mbind errors or arbitrary
user-callback exceptions. Real ASan/UBSan checks complement the backing counters.
Tests are serialized while comparing the global live-backing count.

Pool construction prepares throwing metadata before acquiring raw backing memory,
so metadata failure cannot strand a slab. Recycler release is noexcept: failure
to cache a slab returns it to the OS instead of propagating an exception. Disabled
injection helpers are empty inline functions; release builds have no fault-state
branches or allocation counters.
