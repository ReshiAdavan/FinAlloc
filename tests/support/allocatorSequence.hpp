#pragma once
#include "allocators/arenaAllocator.hpp"
#include "allocators/sizeClassPool.hpp"
#include "allocators/threadCachedPool.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace finalloc::sequence
{
inline void require(bool condition, const char *message)
{ if (!condition) throw std::runtime_error(message); }
struct Block { void *p; std::size_t size, alignment, owner; unsigned char pattern; };
inline void run(std::span<const unsigned char> input)
{
    ArenaOptions opts;
    opts.initial_chunk_size = opts.max_chunk_size = 4096;
    opts.use_canaries = true; opts.canary_size = 8;
    ArenaAllocator arena(opts);
    PoolOptions debug;
    debug.poison_on_free = debug.verify_poison_on_alloc = true;
    debug.quarantine_size = input.empty() ? 0 : input[0] % 5;
    PoolAllocator pool(128, 32, debug);
    LockFreePoolAllocator shared(128, 32, debug);
    ThreadCachedPool cached(128, 32, 4, 8);
    SizeClassPool<> classes(128, 8);
    std::vector<Block> live;
    live.reserve(64);
    auto validate = [&] {
        std::size_t counts[5] = {};
        for (std::size_t i = 0; i < live.size(); ++i)
        {
            const auto &b = live[i];
            ++counts[b.owner];
            const auto address = reinterpret_cast<std::uintptr_t>(b.p);
            require(address % b.alignment == 0, "misaligned live allocation");
            auto *p = static_cast<const unsigned char *>(b.p);
            for (std::size_t j = 0; j < b.size; ++j) require(p[j] == b.pattern, "live payload corrupted");
            for (std::size_t j = 0; j < i; ++j)
            {
                auto other = reinterpret_cast<std::uintptr_t>(live[j].p);
                require(address + b.size <= other || other + live[j].size <= address, "overlapping live ranges");
            }
        }
        require(pool.used() == counts[1] && pool.getStats().in_use == counts[1], "pool usage mismatch");
        require(shared.used() == counts[2] && shared.getStats().in_use == counts[2], "shared usage mismatch");
        require(cached.used() == counts[3], "cache usage mismatch");
        arena.validate();
    };
    auto retire = [&](const Block &b) {
        switch (b.owner)
        {
        case 1: pool.deallocate(b.p); break;
        case 2: shared.deallocate(b.p); break;
        case 3: cached.deallocate(b.p); break;
        case 4: classes.deallocate(b.p, b.size); break;
        default: break; // arena retains until reset/release
        }
    };
    const auto bound = std::min(input.size(), std::size_t{8192});
    for (std::size_t i = 0; i + 3 < bound; i += 4)
    {
        validate();
        const auto op = input[i] % 12;
        if (op < 5 && live.size() < 64)
        {
            auto owner = std::size_t(op);
            std::size_t size = input[i + 1] % 128 + 1;
            std::size_t alignment = owner == 0 ? std::size_t{1} << (input[i + 2] % 6 + 3) : alignof(std::max_align_t);
            void *p = nullptr;
            switch (owner)
            {
            case 0: p = arena.allocate(size, alignment); break;
            case 1: p = pool.allocate(); break;
            case 2: p = shared.allocate(); break;
            case 3: p = cached.allocate(); break;
            case 4: p = classes.allocate(size); break;
            }
            // Fixed pool/quarantine exhaustion is an expected result.
            if (p)
            {
                auto pattern = static_cast<unsigned char>(input[i + 3] | 1);
                // Check ranges before writing so an overlap cannot mask corruption.
                auto address = reinterpret_cast<std::uintptr_t>(p);
                for (const auto &b : live)
                {
                    auto other = reinterpret_cast<std::uintptr_t>(b.p);
                    require(address + size <= other || other + b.size <= address, "new allocation overlaps live block");
                }
                std::memset(p, pattern, size);
                live.push_back({p, size, alignment, owner, pattern});
            }
        }
        else if (op == 5 && !live.empty())
        {
            auto index = input[i + 1] % live.size();
            retire(live[index]); live.erase(live.begin() + index);
        }
        else if (op == 6 || op == 9)
        {
            std::erase_if(live, [](const auto &b) { return b.owner == 0; });
            if (op == 6) arena.reset(); else arena.release();
        }
        else if (op == 7)
        {
            ArenaAllocator moved(std::move(arena));
            arena = std::move(moved);
        }
        else if (op == 8) cached.drainCurrentThread();
        else if (op == 10)
        {
            struct Throws { Throws() { throw std::runtime_error("constructor"); } };
            const auto before = pool.used();
            try { pool.construct<Throws>(); } catch (const std::runtime_error &) {}
            require(pool.used() == before, "throwing constructor consumed a pool slot");
        }
        else if (op == 11)
        {
            bool threw = false;
            try { arena.allocate(std::numeric_limits<std::size_t>::max()); }
            catch (const std::bad_alloc &) { threw = true; }
            require(threw, "oversized request did not fail");
        }
    }
    validate();
    for (const auto &b : live) retire(b);
    live.clear(); cached.drainCurrentThread(); arena.reset();
    validate();
    require(cached.backingStats().in_use == 0, "drained cache retains backing slots");
}
}
