#pragma once
#include <cstddef>
#include <new>
#ifdef FINALLOC_ENABLE_FAILURE_INJECTION
#include <atomic>
#endif

// Compiled into correctness/fuzz builds only. Release paths contain no counters
// or failure branches. These checkpoints cover documented allocator-owned sites,
// not arbitrary allocations made by the C++ runtime or user callbacks.
namespace finalloc::testing
{
enum class AllocationDomain { backing, metadata, any };
#ifdef FINALLOC_ENABLE_FAILURE_INJECTION
struct FailureState
{
    AllocationDomain domain = AllocationDomain::any;
    std::size_t skip = 0, observed = 0;
    bool armed = false, triggered = false;
};
inline thread_local FailureState failure;
inline std::atomic<std::size_t> outstandingBacking{0};
inline void checkpoint(AllocationDomain domain)
{
    if (!failure.armed || (failure.domain != AllocationDomain::any && failure.domain != domain)) return;
    ++failure.observed;
    if (failure.skip) { --failure.skip; return; }
    failure.armed = false;
    failure.triggered = true;
    throw std::bad_alloc();
}
inline void acquired() noexcept { ++outstandingBacking; }
inline void released() noexcept { --outstandingBacking; }
class FailureScope
{
    FailureState previous_;
public:
    FailureScope(AllocationDomain domain, std::size_t skip) : previous_(failure)
    { failure = {domain, skip, 0, true, false}; }
    ~FailureScope() { failure = previous_; }
    bool triggered() const { return failure.triggered; }
    std::size_t observed() const { return failure.observed; }
    FailureScope(const FailureScope &) = delete;
};
#else
inline void checkpoint(AllocationDomain) {}
inline void acquired() noexcept {}
inline void released() noexcept {}
#endif
}
