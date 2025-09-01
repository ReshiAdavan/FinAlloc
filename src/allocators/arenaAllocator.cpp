#include "allocators/arenaAllocator.hpp"

#include <cassert>
#include <cstring>
#include <algorithm>
#include <iostream>

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
    // start with one chunk
    ArenaChunk first = newChunk_(0);
    chunks_.push_back(std::move(first));
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
    o.nextChunkBytes_ = 0;
    o.totalBytes_ = 0;
    o.group_ = nullptr;
    o.journalHead_ = 0;
    return *this;
}

void *ArenaAllocator::allocate(std::size_t bytes, std::size_t alignment)
{
    if (bytes == 0)
        bytes = 1;

    // normalize alignment to power-of-two and >= max_align_t
    const std::size_t kMinAlign = alignof(std::max_align_t);
    if (alignment < kMinAlign)
        alignment = kMinAlign;
    if ((alignment & (alignment - 1)) != 0)
        alignment = next_pow2(alignment);

    // try current chunk
    void *out = nullptr;
    if (!chunks_.empty() && tryAllocFromChunk_(chunks_.back(), bytes, alignment, &out))
    {
        totalBytes_ += bytes;
        maybeJournal_(bytes, alignment);
        return out;
    }
    // slow path: get a new chunk and retry
    return allocateSlow_(bytes, alignment);
}

void ArenaAllocator::reset()
{
    for (auto &c : chunks_)
        c.offset = 0;
    totalBytes_ = 0;
    // keep chunks; journaling left intact
}

void ArenaAllocator::release()
{
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
    if (chunks_.empty())
        return 0;
    const auto &c = chunks_.back();
    return (c.size > c.offset) ? (c.size - c.offset) : 0;
}

// ---- private: slow path ----
void *ArenaAllocator::allocateSlow_(std::size_t size, std::size_t alignment)
{
    // Worst-case within a fresh chunk:
    // [header aligned to max_align] + pre_canary + alignment slack + user + post_canary
    const std::size_t header = ArenaAllocator::alignUp_(sizeof(BlockHeader), alignof(std::max_align_t));
    const std::size_t pre = opts_.use_canaries ? opts_.canary_size : 0;
    const std::size_t post = opts_.use_canaries ? opts_.canary_size : 0;
    const std::size_t worst = header + pre + alignment + size + post;

    // choose next chunk size: geometric growth bounded, at least 'worst'
    std::size_t want = std::max(nextChunkBytes_, worst);
    want = std::clamp(want, std::max(opts_.initial_chunk_size, worst), opts_.max_chunk_size);

    // acquire new chunk
    chunks_.push_back(newChunk_(want));

    // advance growth for next time
    const double g = (opts_.growth_factor > 1.0) ? opts_.growth_factor : 2.0;
    std::size_t next = static_cast<std::size_t>(static_cast<double>(want) * g);
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
        chunks_.push_back(newChunk_(worst));
        bool ok = tryAllocFromChunk_(chunks_.back(), size, alignment, &out);
        if (!ok)
        {
            std::cerr << "[Arena] allocateSlow_: could not satisfy allocation\n";
            std::abort();
        }
    }
    totalBytes_ += size;
    maybeJournal_(size, alignment);
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

    if (end > base + c.size)
        return false;

    // write header
    auto *hdr = reinterpret_cast<BlockHeader *>(hdrAddr);
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
        // guard/huge flags are harmless no-ops in this portable osAlloc
        return group_->acquire(want, opts_.guard_pages, opts_.prefer_huge);
    }
    return osAllocChunk_(want, opts_.guard_pages, opts_.prefer_huge);
}

// ---- private: canaries and journaling ----
void ArenaAllocator::writeCanaries_(unsigned char *user, std::size_t size, std::size_t pre, std::size_t post)
{
    if (pre > 0)
        std::memset(user - pre, opts_.canary_byte, pre);
    if (post > 0)
        std::memset(user + size, opts_.canary_byte, post);
}

void ArenaAllocator::maybeJournal_(std::size_t size, std::size_t alignment)
{
    if (!journalOn_)
        return;
    if (size < opts_.journal_threshold_bytes)
        return;

    if (journal_.empty())
    {
        // lazy init a small ring buffer
        journal_.resize(1024);
        journalHead_ = 0;
    }
    JournalEntry e;
    e.size = size;
    e.alignment = alignment;
    e.retaddr = 0; // portable fallback (no backtrace in this build)
    journal_[journalHead_] = e;
    journalHead_ = (journalHead_ + 1) % journal_.size();
}

// ---- public static: portable OS chunk alloc/free ----
ArenaAllocator::ArenaChunk
ArenaAllocator::osAllocChunk_(std::size_t usableBytes, bool /*guards*/, bool /*preferHuge*/)
{
    ArenaChunk c;
    c.size = std::max<std::size_t>(usableBytes, std::size_t{4096});
    c.base = std::malloc(c.size);
    if (!c.base)
        throw std::bad_alloc();
    c.offset = 0;
    c.use_mmap = false;
    c.guard_pages = false;
    return c;
}

void ArenaAllocator::osFreeChunk_(ArenaAllocator::ArenaChunk &c)
{
    if (c.base)
        std::free(c.base);
    c.base = nullptr;
    c.size = 0;
    c.offset = 0;
    c.use_mmap = false;
    c.guard_pages = false;
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

ArenaGroup::Chunk ArenaGroup::acquire(std::size_t minBytes, bool guards, bool preferHuge)
{
    std::lock_guard<std::mutex> lock(mtx_);
    const std::size_t idx = pick_index(minBytes);
    if (idx >= bins_.size())
        bins_.resize(BIN_COUNT);

    auto &vec = bins_[idx].slabs;
    if (!vec.empty())
    {
        Chunk c = std::move(vec.back());
        vec.pop_back();
        c.offset = 0;
        return c;
    }
    const std::size_t want = std::max(minBytes, class_bytes(idx));
    return ArenaAllocator::osAllocChunk_(want, guards, preferHuge);
}

void ArenaGroup::release(Chunk &&chunk)
{
    if (!chunk.base || chunk.size == 0)
        return;
    std::lock_guard<std::mutex> lock(mtx_);
    const std::size_t idx = pick_index(chunk.size);
    if (idx >= bins_.size())
        bins_.resize(BIN_COUNT);
    chunk.offset = 0;
    bins_[idx].slabs.emplace_back(std::move(chunk));
}
