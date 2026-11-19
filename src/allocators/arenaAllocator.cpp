#include "allocators/arenaAllocator.hpp"

#include <cassert>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <limits>
#include <cmath>
#include <stdexcept>
#include <cerrno>
#include <system_error>
#include "utils/allocationControl.hpp"
using finalloc::testing::AllocationDomain;
using finalloc::testing::checkpoint;
#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{
    inline std::size_t next_pow2(std::size_t x)
    {
        if (x <= 1)
            return 1;
        --x;
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
#if SIZE_MAX > 0xFFFFFFFFull
        x |= x >> 32;
#endif
        return x + 1;
    }
    inline std::uintptr_t align_up(std::uintptr_t p, std::size_t a)
    {
        const std::uintptr_t mask = static_cast<std::uintptr_t>(a - 1);
        return (p + mask) & ~mask;
    }
}

thread_local std::unique_ptr<ArenaAllocator> ThreadLocalArena::tls_ = nullptr;

ArenaAllocator::ArenaAllocator(const ArenaOptions &opts)
    : opts_(opts),
      chunks_(),
      nextChunkBytes_(std::max<std::size_t>(opts_.initial_chunk_size, std::size_t{4096})),
      totalBytes_(0),
      group_(nullptr),
      journalOn_(opts_.journaling),
      journal_(),
      journalHead_(0)
{
    if (opts_.numa_node < -1 || opts_.numa_node > 1048575)
        throw std::invalid_argument("invalid NUMA node");
    if (!std::isfinite(opts_.growth_factor) || opts_.growth_factor < 1.0 ||
        opts_.initial_chunk_size == 0 || opts_.max_chunk_size < opts_.initial_chunk_size)
        throw std::invalid_argument("invalid arena growth options");
    // start with one chunk
    if (journalOn_) { checkpoint(AllocationDomain::metadata); journal_.resize(1024); }
    appendChunk_(0);
}

ArenaAllocator::~ArenaAllocator()
{
    try
    {
        release();
    }
    catch (...)
    {
    }
}

ArenaAllocator::ArenaAllocator(ArenaAllocator &&o) noexcept
    : opts_(o.opts_),
      chunks_(std::move(o.chunks_)),
      nextChunkBytes_(o.nextChunkBytes_),
      totalBytes_(o.totalBytes_),
      group_(o.group_),
      journalOn_(o.journalOn_),
      journal_(std::move(o.journal_)),
      journalHead_(o.journalHead_)
{
    activeChunk_ = o.activeChunk_;
    journalCount_ = o.journalCount_;
    o.activeChunk_ = 0;
    o.journalCount_ = 0;
    canaries_ = std::move(o.canaries_);
    o.canaries_.clear();
    o.nextChunkBytes_ = 0;
    o.totalBytes_ = 0;
    o.group_ = nullptr;
    o.journalHead_ = 0;
}

ArenaAllocator &ArenaAllocator::operator=(ArenaAllocator &&o) noexcept
{
    if (this == &o)
        return *this;
    release();
    opts_ = o.opts_;
    chunks_ = std::move(o.chunks_);
    nextChunkBytes_ = o.nextChunkBytes_;
    totalBytes_ = o.totalBytes_;
    group_ = o.group_;
    journalOn_ = o.journalOn_;
    journal_ = std::move(o.journal_);
    journalHead_ = o.journalHead_;
    activeChunk_ = o.activeChunk_;
    journalCount_ = o.journalCount_;
    o.activeChunk_ = 0;
    o.journalCount_ = 0;
    canaries_ = std::move(o.canaries_);
    o.canaries_.clear();
    o.nextChunkBytes_ = 0;
    o.totalBytes_ = 0;
    o.group_ = nullptr;
    o.journalHead_ = 0;
    return *this;
}

void *ArenaAllocator::allocate(std::size_t bytes, std::size_t alignment, std::source_location source)
{
    if (bytes == 0)
        bytes = 1;

    // normalize alignment to power-of-two and >= max_align_t
    const std::size_t kMinAlign = alignof(std::max_align_t);
    if (alignment < kMinAlign)
        alignment = kMinAlign;
    if ((alignment & (alignment - 1)) != 0)
    {
        if (alignment > (std::numeric_limits<std::size_t>::max() >> 1))
            throw std::bad_alloc();
        alignment = next_pow2(alignment);
    }
    const auto limit = std::numeric_limits<std::size_t>::max();
    const auto overhead = sizeof(BlockHeader) + alignof(std::max_align_t);
    if (bytes > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        alignment > limit - overhead || bytes > limit - overhead - alignment ||
        (opts_.use_canaries && opts_.canary_size > (limit - overhead - alignment - bytes) / 2))
        throw std::bad_alloc();

    if (journalOn_ && journal_.empty()) { checkpoint(AllocationDomain::metadata); journal_.resize(1024); }
    void *out = nullptr;
    for (; activeChunk_ < chunks_.size(); ++activeChunk_)
        if (tryAllocFromChunk_(chunks_[activeChunk_], bytes, alignment, &out))
        {
            totalBytes_ += bytes;
            maybeJournal_(bytes, alignment, source);
            return out;
        }
    return allocateSlow_(bytes, alignment, source);
}

void ArenaAllocator::validate() const
{
    for (const auto &r : canaries_)
        for (std::size_t i = 0; i < opts_.canary_size; ++i)
            if (r.user[-static_cast<std::ptrdiff_t>(i) - 1] != opts_.canary_byte ||
                r.user[r.size + i] != opts_.canary_byte)
                throw std::runtime_error("arena canary corruption");
}

void ArenaAllocator::reset()
{
    validate();
    canaries_.clear();
    activeChunk_ = 0;
    for (auto &c : chunks_)
        c.offset = 0;
    totalBytes_ = 0;
    // keep chunks; journaling left intact
}

void ArenaAllocator::release() noexcept
{
    activeChunk_ = 0;
    // Release must always reclaim storage, even after a failed validation.
    canaries_.clear();
    // return slabs to group or OS
    if (group_)
    {
        for (auto &c : chunks_)
            group_->release(std::move(c));
    }
    else
    {
        for (auto &c : chunks_)
            osFreeChunk_(c);
    }
    chunks_.clear();
    totalBytes_ = 0;
    nextChunkBytes_ = std::max<std::size_t>(opts_.initial_chunk_size, std::size_t{4096});
}

std::size_t ArenaAllocator::bytesRemaining() const
{
    std::size_t remaining = 0;
    for (std::size_t i = activeChunk_; i < chunks_.size(); ++i)
        remaining += chunks_[i].size - chunks_[i].offset;
    return remaining;
}

// ---- private: slow path ----
void *ArenaAllocator::allocateSlow_(std::size_t size, std::size_t alignment, std::source_location source)
{
    // Worst-case within a fresh chunk:
    // [header aligned to max_align] + pre_canary + alignment slack + user + post_canary
    const std::size_t header = ArenaAllocator::alignUp_(sizeof(BlockHeader), alignof(std::max_align_t));
    const std::size_t pre = opts_.use_canaries ? opts_.canary_size : 0;
    const std::size_t post = opts_.use_canaries ? opts_.canary_size : 0;
    const std::size_t worst = header + pre + alignment + size + post;

    // choose next chunk size: geometric growth bounded, at least 'worst'
    std::size_t want = std::max(nextChunkBytes_, worst);
    want = std::max(worst, std::min(want, opts_.max_chunk_size));

    // acquire new chunk
    appendChunk_(want);
    activeChunk_ = chunks_.size() - 1;

    // advance growth for next time
    const double g = (opts_.growth_factor > 1.0) ? opts_.growth_factor : 2.0;
    const long double grown = static_cast<long double>(want) * g;
    std::size_t next = grown >= opts_.max_chunk_size ? opts_.max_chunk_size : static_cast<std::size_t>(grown);
    if (next < worst)
        next = worst;
    if (next < opts_.initial_chunk_size)
        next = opts_.initial_chunk_size;
    if (next > opts_.max_chunk_size)
        next = opts_.max_chunk_size;
    nextChunkBytes_ = next;

    // retry on fresh chunk
    void *out = nullptr;
    if (!tryAllocFromChunk_(chunks_.back(), size, alignment, &out))
    {
        // Extremely unlikely; allocate an exact-fit chunk and try again.
        appendChunk_(worst);
        activeChunk_ = chunks_.size() - 1;
        bool ok = tryAllocFromChunk_(chunks_.back(), size, alignment, &out);
        if (!ok)
        {
            std::cerr << "[Arena] allocateSlow_: could not satisfy allocation\n";
            std::abort();
        }
    }
    totalBytes_ += size;
    maybeJournal_(size, alignment, source);
    return out;
}

// ---- private: attempt to carve from a specific chunk ----
bool ArenaAllocator::tryAllocFromChunk_(ArenaChunk &c, std::size_t userSize, std::size_t alignment, void **out)
{
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(c.base);
    const std::uintptr_t cur = base + c.offset;

    const std::size_t hdrAlign = alignof(std::max_align_t);
    const std::uintptr_t hdrAddr = align_up(cur, hdrAlign);
    const std::uintptr_t hdrEnd = hdrAddr + sizeof(BlockHeader);

    const std::size_t pre = opts_.use_canaries ? opts_.canary_size : 0;
    const std::size_t post = opts_.use_canaries ? opts_.canary_size : 0;

    const std::uintptr_t userAddr = align_up(hdrEnd + pre, alignment);
    const std::uintptr_t end = userAddr + userSize + post;

    if (userAddr < hdrEnd || end < userAddr || end > base + c.size)
        return false;

    if (opts_.use_canaries && opts_.canary_size)
    {
        if (canaries_.size() == canaries_.capacity()) checkpoint(AllocationDomain::metadata);
        canaries_.push_back({reinterpret_cast<unsigned char *>(userAddr), userSize});
    }

    // write header
    auto *hdr = new (reinterpret_cast<void *>(hdrAddr)) BlockHeader{};
    hdr->magic = 0xABCD1234u;
    hdr->reserved = 0;
    hdr->payload_size = userSize;
    hdr->alignment = alignment;
    hdr->pre_canary = pre;
    hdr->post_canary = post;

    auto *userPtr = reinterpret_cast<unsigned char *>(userAddr);

    // canaries placed directly before/after payload
    writeCanaries_(userPtr, userSize, pre, post);

    // advance chunk offset
    c.offset = static_cast<std::size_t>(end - base);

    *out = static_cast<void *>(userPtr);
    return true;
}

// ---- private: new chunk acquisition ----
ArenaAllocator::ArenaChunk ArenaAllocator::newChunk_(std::size_t minBytes)
{
    const std::size_t want = std::max<std::size_t>(minBytes, std::max<std::size_t>(nextChunkBytes_, 4096));
    // group_ may be null (no recycler)
    if (group_)
    {
        return group_->acquire(want, opts_.guard_pages, opts_.prefer_huge, opts_.numa_node, opts_.strict_numa);
    }
    return osAllocChunk_(want, opts_.guard_pages, opts_.prefer_huge, opts_.numa_node, opts_.strict_numa);
}

// ---- private: canaries and journaling ----
void ArenaAllocator::writeCanaries_(unsigned char *user, std::size_t size, std::size_t pre, std::size_t post)
{
    if (pre > 0)
        std::memset(user - pre, opts_.canary_byte, pre);
    if (post > 0)
        std::memset(user + size, opts_.canary_byte, post);
}

void ArenaAllocator::maybeJournal_(std::size_t size, std::size_t alignment, std::source_location source)
{
    if (!journalOn_)
        return;
    if (size < opts_.journal_threshold_bytes)
        return;

    // Preallocated in the constructor: tracing cannot fail after carving memory.
    journal_[journalHead_] = {size, alignment, source};
    journalHead_ = (journalHead_ + 1) % journal_.size();
    journalCount_ = std::min(journalCount_ + 1, journal_.size());
}

// ---- OS chunk allocation ----
ArenaAllocator::ArenaChunk
ArenaAllocator::osAllocChunk_(std::size_t usableBytes, bool guards, bool preferHuge, int numaNode, bool strictNuma)
{
    ArenaChunk c;
    if (usableBytes > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
        throw std::bad_alloc();
    c.size = std::max<std::size_t>(usableBytes, 4096);
    if (numaNode < -1 || numaNode > 1048575) throw std::invalid_argument("invalid NUMA node");
#if defined(__linux__) && defined(SYS_mbind)
    constexpr auto bits = sizeof(unsigned long) * 8;
    std::vector<unsigned long> mask;
    if (numaNode >= 0)
    {
        checkpoint(AllocationDomain::metadata);
        mask.resize(static_cast<std::size_t>(numaNode) / bits + 1, 0);
        mask[numaNode / bits] |= 1UL << (numaNode % bits);
    }
#endif
    c.numa_node = numaNode;
    c.numa_strict = strictNuma;
    c.prefer_huge = preferHuge;
    bool map = guards;
#if defined(__linux__)
    map = map || preferHuge || numaNode >= 0;
#endif
    if (map)
    {
#if defined(__unix__) || defined(__APPLE__)
        const long rawPage = ::sysconf(_SC_PAGESIZE);
        if (rawPage <= 0) throw std::bad_alloc();
        const auto page = static_cast<std::size_t>(rawPage);
        const auto limit = std::numeric_limits<std::size_t>::max();
        std::size_t granularity = page;
#if defined(__linux__)
        if (preferHuge) granularity = std::max(page, std::size_t{2 * 1024 * 1024});
#endif
        if (c.size > limit - granularity - 2 * page) throw std::bad_alloc();
        c.size = ((c.size + granularity - 1) / granularity) * granularity;
        if (c.size > limit - granularity - 2 * page) throw std::bad_alloc();
        c.mapping_size = c.size + granularity + 2 * page;
        checkpoint(AllocationDomain::backing);
        c.mapping_base = ::mmap(nullptr, c.mapping_size, PROT_NONE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (c.mapping_base == MAP_FAILED) throw std::bad_alloc();
        finalloc::testing::acquired();
        c.use_mmap = true;
        const auto start = reinterpret_cast<std::uintptr_t>(c.mapping_base) + page;
        c.base = reinterpret_cast<void *>(((start + granularity - 1) / granularity) * granularity);
        if (::mprotect(c.base, c.size, PROT_READ | PROT_WRITE) != 0)
        {
            ::munmap(c.mapping_base, c.mapping_size);
            finalloc::testing::released();
            throw std::bad_alloc();
        }
        c.use_mmap = true;
        c.guard_pages = guards;
#if defined(__linux__) && defined(MADV_HUGEPAGE)
        if (preferHuge) c.huge_page_advised = (::madvise(c.base, c.size, MADV_HUGEPAGE) == 0);
#endif
#else
        throw std::runtime_error("guard pages require POSIX mmap");
#endif
    }
    else
    {
        checkpoint(AllocationDomain::backing);
        c.base = std::malloc(c.size);
        if (!c.base) throw std::bad_alloc();
        finalloc::testing::acquired();
    }
    if (numaNode >= 0)
    {
#if defined(__linux__) && defined(SYS_mbind)
        c.numa_policy_applied = (::syscall(SYS_mbind, c.base, c.size,
            strictNuma ? MPOL_BIND : MPOL_PREFERRED, mask.data(),
            static_cast<unsigned long>(numaNode + 1), 0UL) == 0);
        if (!c.numa_policy_applied) c.numa_policy_error = errno;
#else
        c.numa_policy_error = ENOTSUP;
#endif
        if (strictNuma && !c.numa_policy_applied)
        {
            const int error = c.numa_policy_error;
            osFreeChunk_(c);
            throw std::system_error(error, std::generic_category(), "NUMA memory policy");
        }
    }
    return c;
}

void ArenaAllocator::osFreeChunk_(ArenaChunk &c)
{
    if (c.base) finalloc::testing::released();
#if defined(__unix__) || defined(__APPLE__)
    if (c.use_mmap) ::munmap(c.mapping_base, c.mapping_size);
    else
#endif
        std::free(c.base);
    c = {};
}

namespace
{
    static constexpr std::size_t BIN_COUNT = 6; // 64K, 256K, 1M, 4M, 16M, 64M
    static inline std::size_t class_bytes(std::size_t idx)
    {
        static const std::size_t sizes[BIN_COUNT] = {
            64 * 1024,
            256 * 1024,
            1 * 1024 * 1024,
            4 * 1024 * 1024,
            16 * 1024 * 1024,
            64 * 1024 * 1024};
        return sizes[(idx < BIN_COUNT) ? idx : BIN_COUNT - 1];
    }
    static inline std::size_t pick_index(std::size_t minBytes)
    {
        for (std::size_t i = 0; i < BIN_COUNT; ++i)
        {
            if (class_bytes(i) >= minBytes)
                return i;
        }
        return BIN_COUNT - 1;
    }
}

ArenaGroup::Chunk ArenaGroup::acquire(std::size_t minBytes, bool guards, bool preferHuge, int numaNode, bool strictNuma)
{
    std::lock_guard<std::mutex> lock(mtx_);
    const std::size_t idx = pick_index(minBytes);
    if (idx >= bins_.size())
    {
        checkpoint(AllocationDomain::metadata);
        bins_.resize(BIN_COUNT);
    }

    for (std::size_t i = idx; i < bins_.size(); ++i)
    {
        auto &vec = bins_[i].slabs;
        for (auto it = vec.begin(); it != vec.end(); ++it)
            if (it->size >= minBytes && it->guard_pages == guards && it->prefer_huge == preferHuge &&
                it->numa_node == numaNode && (!strictNuma || (it->numa_policy_applied && it->numa_strict)))
            {
                Chunk c = *it;
                vec.erase(it);
                c.offset = 0;
                return c;
            }
    }
    const std::size_t want = std::max(minBytes, class_bytes(idx));
    return ArenaAllocator::osAllocChunk_(want, guards, preferHuge, numaNode, strictNuma);
}

void ArenaGroup::release(Chunk &&chunk) noexcept
{
    try
    {
        if (!chunk.base || chunk.size == 0) return;
        std::lock_guard<std::mutex> lock(mtx_);
        const std::size_t idx = pick_index(chunk.size);
        if (idx >= bins_.size())
        {
            checkpoint(AllocationDomain::metadata);
            bins_.resize(BIN_COUNT);
        }
        chunk.offset = 0;
        if (bins_[idx].slabs.size() == bins_[idx].slabs.capacity()) checkpoint(AllocationDomain::metadata);
        bins_[idx].slabs.push_back(chunk);
        chunk = {};
    }
    catch (...) { ArenaAllocator::osFreeChunk_(chunk); }
}

ArenaGroup::~ArenaGroup()
{
    for (auto &bin : bins_)
        for (auto &chunk : bin.slabs)
            ArenaAllocator::osFreeChunk_(chunk);
}

ArenaAllocator::MemoryStats ArenaAllocator::memoryStats() const
{
    MemoryStats s;
    s.payload = totalBytes_;
    s.chunks = chunks_.size();
    for (const auto &c : chunks_)
    {
        s.reserved += c.size;
        s.consumed += c.offset;
        s.numa_bound_chunks += c.numa_policy_applied;
        s.numa_failed_chunks += c.numa_node >= 0 && !c.numa_policy_applied;
    }
    return s;
}

void ArenaAllocator::appendChunk_(std::size_t minBytes)
{
    auto chunk = newChunk_(minBytes);
    try
    {
        if (chunks_.size() == chunks_.capacity()) checkpoint(AllocationDomain::metadata);
        chunks_.push_back(chunk);
    }
    catch (...) { osFreeChunk_(chunk); throw; }
}

std::vector<ArenaAllocator::JournalEntry> ArenaAllocator::journalSnapshot() const
{
    std::vector<JournalEntry> entries;
    entries.reserve(journalCount_);
    for (std::size_t i = 0; i < journalCount_; ++i)
        entries.push_back(journal_[(journalHead_ + journal_.size() - journalCount_ + i) % journal_.size()]);
    return entries;
}
