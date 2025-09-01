#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>
#include <utility>
#include <new>

// ================================================================
// Options
// ================================================================
struct ArenaOptions {
    // Growth strategy
    std::size_t initial_chunk_size = 1 << 20;  // 1 MiB
    double      growth_factor      = 2.0;      // next = ceil(prev * growth_factor)
    std::size_t max_chunk_size     = 1 << 26;  // 64 MiB cap

    // Guard/hugepage knobs (no-ops in this portable impl)
    bool guard_pages = false;
    bool prefer_huge = false;

    // Canary/redzones for corruption detection
    bool        use_canaries   = false;       // enable canaries at alloc time
    std::size_t canary_size    = 0;           // bytes before/after payload
    std::uint8_t canary_byte   = 0xCA;

    // Journaling/tracing (ring buffer)
    bool        journaling             = false;
    std::size_t journal_threshold_bytes = 0;  // only record allocations >= threshold
};

// forward decl
class ArenaGroup;

// ================================================================
// ArenaAllocator
// ================================================================
class ArenaAllocator {
public:
    struct ArenaChunk {
        void*       base       = nullptr;  // usable base
        std::size_t size       = 0;        // usable size (bytes)
        std::size_t offset     = 0;        // current offset (bytes)
        bool        use_mmap   = false;    // reserved for future (no-op)
        bool        guard_pages= false;    // reserved for future (no-op)

        std::size_t usableSize() const { return size; }
    };

    explicit ArenaAllocator(const ArenaOptions& opts);
    ~ArenaAllocator();

    // move only
    ArenaAllocator(ArenaAllocator&&) noexcept;
    ArenaAllocator& operator=(ArenaAllocator&&) noexcept;
    ArenaAllocator(const ArenaAllocator&)            = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    // Raw allocate
    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));

    // Placement-construct helpers
    template <typename T, typename... Args>
    T* construct(Args&&... args) {
        void* p = allocate(sizeof(T), alignof(T));
        return p ? new (p) T(std::forward<Args>(args)...) : nullptr;
    }
    template <typename T>
    void destroy(T* /*ptr*/) {
        // Arenas don't free individual objects (optional dtor call is a policy choice).
    }

    // Reset offsets but keep slabs
    void reset();

    // Release all slabs to OS or ArenaGroup
    void release();

    // Expose stats
    std::size_t chunkCount() const { return chunks_.size(); }
    std::size_t bytesRemaining() const;
    const ArenaOptions& options() const { return opts_; }

    // (Public so ArenaGroup or tests can call; portable malloc/free in this impl)
    static ArenaChunk osAllocChunk_(std::size_t usableBytes, bool /*guards*/, bool /*preferHuge*/);
    static void       osFreeChunk_(ArenaChunk& c);

    // For advanced use: attach a recycler
    void attachGroup(ArenaGroup* g) { group_ = g; }

private:
    // Header written before each allocation (aligned to max_align_t)
    struct BlockHeader {
        std::uint32_t magic = 0xABCD1234u;
        std::uint32_t reserved = 0;
        std::size_t   payload_size = 0;
        std::size_t   alignment    = 0;
        std::size_t   pre_canary   = 0;
        std::size_t   post_canary  = 0;
    };

    // Journal
    struct JournalEntry {
        std::size_t size;
        std::size_t alignment;
        std::uintptr_t retaddr; // best-effort return address capture (optional)
    };

    // helpers
    void*        allocateSlow_(std::size_t size, std::size_t alignment);
    bool         tryAllocFromChunk_(ArenaChunk& c, std::size_t user, std::size_t align, void** out);
    static std::size_t alignUp_(std::size_t n, std::size_t a) {
        return (n + a - 1) & ~(a - 1);
    }
    ArenaChunk   newChunk_(std::size_t minBytes);

    // canaries
    void writeCanaries_(unsigned char* user, std::size_t size, std::size_t pre, std::size_t post);
    void maybeJournal_(std::size_t size, std::size_t alignment);

    // state
    ArenaOptions            opts_{};
    std::vector<ArenaChunk> chunks_;
    std::size_t             nextChunkBytes_ = 0;
    std::size_t             totalBytes_     = 0;
    ArenaGroup*             group_          = nullptr;

    // journaling
    bool                    journalOn_      = false;
    std::vector<JournalEntry> journal_;
    std::size_t             journalHead_    = 0;
};

// ================================================================
// ThreadLocalArena
// ================================================================
class ThreadLocalArena {
public:
    static ArenaAllocator& instance() {
        if (!tls_) tls_ = std::make_unique<ArenaAllocator>(ArenaOptions{});
        return *tls_;
    }
    static ArenaAllocator& withOptions(const ArenaOptions& opts) {
        tls_ = std::make_unique<ArenaAllocator>(opts);
        return *tls_;
    }
    static void reset()   { if (tls_) tls_->reset(); }
    static void release() { if (tls_) tls_->release(); }

private:
    static thread_local std::unique_ptr<ArenaAllocator> tls_;
};

// ================================================================
// ArenaGroup — slab recycler
// ================================================================
class ArenaGroup {
public:
    using Chunk = ArenaAllocator::ArenaChunk;

    Chunk acquire(std::size_t minBytes, bool guards, bool preferHuge);
    void  release(Chunk&& chunk);

private:
    // simple size-class bins (power-of-two-ish)
    struct Bin { std::vector<Chunk> slabs; };

    std::vector<Bin> bins_;
    std::mutex       mtx_;
};
