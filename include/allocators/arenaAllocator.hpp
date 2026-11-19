#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>
#include <utility>
#include <new>
#include <source_location>

struct ArenaOptions
{
    std::size_t initial_chunk_size = 1 << 20; // 1 MiB
    double growth_factor = 2.0;               // next = ceil(prev * growth_factor)
    std::size_t max_chunk_size = 1 << 26;     // 64 MiB cap

    bool guard_pages = false;
    bool prefer_huge = false;
    int numa_node = -1; // Linux preferred node; -1 uses the process policy
    bool strict_numa = false;

    bool use_canaries = false;
    std::size_t canary_size = 0;
    std::uint8_t canary_byte = 0xCA;

    bool journaling = false;
    std::size_t journal_threshold_bytes = 0;
};

class ArenaGroup;

class ArenaAllocator
{
public:
    struct ArenaChunk
    {
        void *base = nullptr;
        std::size_t size = 0;
        std::size_t offset = 0;
        bool use_mmap = false;
        bool guard_pages = false;
        bool prefer_huge = false;
        bool huge_page_advised = false; // advice accepted, not a physical residency guarantee
        int numa_node = -1;
        bool numa_strict = false;
        bool numa_policy_applied = false;
        int numa_policy_error = 0;
        void *mapping_base = nullptr;
        std::size_t mapping_size = 0;
        std::size_t usableSize() const { return size; }
    };

    explicit ArenaAllocator(const ArenaOptions &opts);
    ~ArenaAllocator();

    ArenaAllocator(ArenaAllocator &&) noexcept;
    ArenaAllocator &operator=(ArenaAllocator &&) noexcept;
    ArenaAllocator(const ArenaAllocator &) = delete;
    ArenaAllocator &operator=(const ArenaAllocator &) = delete;

    void *allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t),
                   std::source_location source = std::source_location::current());
    struct JournalEntry
    {
        std::size_t size;
        std::size_t alignment;
        std::source_location source;
    };
    std::vector<JournalEntry> journalSnapshot() const;

    template <typename T, typename... Args>
    T *construct(Args &&...args)
    {
        void *p = allocate(sizeof(T), alignof(T));
        return p ? new (p) T(std::forward<Args>(args)...) : nullptr;
    }
    template <typename T>
    void destroy(T *ptr) { if (ptr) ptr->~T(); }
    void validate() const; // throws std::runtime_error on red-zone corruption
    void reset();
    void release() noexcept;

    std::size_t chunkCount() const { return chunks_.size(); }
    std::size_t bytesRemaining() const;
    struct MemoryStats
    {
        std::size_t reserved = 0, consumed = 0, payload = 0, chunks = 0;
        std::size_t numa_bound_chunks = 0, numa_failed_chunks = 0;
    };
    MemoryStats memoryStats() const;
    const ArenaOptions &options() const { return opts_; }

    static ArenaChunk osAllocChunk_(std::size_t usableBytes, bool guards, bool preferHuge, int numaNode = -1, bool strictNuma = false);
    static void osFreeChunk_(ArenaChunk &c);

    void attachGroup(ArenaGroup *g) { group_ = g; }

private:
    struct BlockHeader
    {
        std::uint32_t magic = 0xABCD1234u;
        std::uint32_t reserved = 0;
        std::size_t payload_size = 0;
        std::size_t alignment = 0;
        std::size_t pre_canary = 0;
        std::size_t post_canary = 0;
    };

    // helpers
    void *allocateSlow_(std::size_t size, std::size_t alignment, std::source_location source);
    bool tryAllocFromChunk_(ArenaChunk &c, std::size_t user, std::size_t align, void **out);
    static std::size_t alignUp_(std::size_t n, std::size_t a)
    {
        return (n + a - 1) & ~(a - 1);
    }
    void appendChunk_(std::size_t minBytes);
    ArenaChunk newChunk_(std::size_t minBytes);

    // canaries
    void writeCanaries_(unsigned char *user, std::size_t size, std::size_t pre, std::size_t post);
    void maybeJournal_(std::size_t size, std::size_t alignment, std::source_location source);

    struct CanaryRecord
    {
        unsigned char *user;
        std::size_t size;
    };
    std::vector<CanaryRecord> canaries_;

    // state
    ArenaOptions opts_{};
    std::vector<ArenaChunk> chunks_;
    std::size_t activeChunk_ = 0;
    std::size_t nextChunkBytes_ = 0;
    std::size_t totalBytes_ = 0;
    ArenaGroup *group_ = nullptr;

    // journaling
    bool journalOn_ = false;
    std::vector<JournalEntry> journal_;
    std::size_t journalHead_ = 0;
    std::size_t journalCount_ = 0;
};

class ThreadLocalArena
{
public:
    static ArenaAllocator &instance()
    {
        if (!tls_)
            tls_ = std::make_unique<ArenaAllocator>(ArenaOptions{});
        return *tls_;
    }
    static ArenaAllocator &withOptions(const ArenaOptions &opts)
    {
        tls_ = std::make_unique<ArenaAllocator>(opts);
        return *tls_;
    }
    static void reset()
    {
        if (tls_)
            tls_->reset();
    }
    static void release()
    {
        if (tls_)
            tls_->release();
    }

private:
    static thread_local std::unique_ptr<ArenaAllocator> tls_;
};

class ArenaGroup
{
public:
    using Chunk = ArenaAllocator::ArenaChunk;

    ~ArenaGroup();
    Chunk acquire(std::size_t minBytes, bool guards, bool preferHuge, int numaNode = -1, bool strictNuma = false);
    void release(Chunk &&chunk) noexcept;

private:
    // simple size-class bins (power-of-two-ish)
    struct Bin
    {
        std::vector<Chunk> slabs;
    };

    std::vector<Bin> bins_;
    std::mutex mtx_;
};
