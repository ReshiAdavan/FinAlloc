#include "allocators/arenaAllocator.hpp"
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>
#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
static void faults(unsigned char *p)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0)
    {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        *static_cast<volatile unsigned char *>(p) = 1;
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
}
#endif
int main()
{
#if defined(__unix__) || defined(__APPLE__)
    {
    auto c = ArenaAllocator::osAllocChunk_(4097, true, false);
    assert(c.guard_pages && c.use_mmap && c.size >= 4097);
    auto *p = static_cast<unsigned char *>(c.base);
    std::memset(p, 0, c.size);
    faults(p - 1);
    faults(p + c.size);
    ArenaAllocator::osFreeChunk_(c);
    assert(!c.base && !c.mapping_base);
    }
#endif
    ArenaOptions opts;
    opts.use_canaries = true;
    opts.canary_size = 16;
    ArenaAllocator a(opts);
    auto *p = static_cast<unsigned char *>(a.allocate(37, 4096));
    std::memset(p, 42, 37);
    a.validate();
    for (int side : {-1, 37})
    {
        p[side] ^= 1;
        bool caught = false;
        try { a.validate(); } catch (const std::runtime_error &) { caught = true; }
        assert(caught);
        caught = false;
        try { a.reset(); } catch (const std::runtime_error &) { caught = true; }
        assert(caught);
        p[side] ^= 1;
    }
    ArenaAllocator moved(std::move(a));
    p[37] ^= 1;
    bool caught = false;
    try { moved.validate(); } catch (const std::runtime_error &) { caught = true; }
    assert(caught);
    p[37] ^= 1;
    moved.reset();
    moved.validate();
    moved.allocate(128 * 1024 * 1024); // above normal growth cap
    for (auto size : {std::numeric_limits<std::size_t>::max(),
                      std::numeric_limits<std::size_t>::max() - 100})
    {
        caught = false;
        try { moved.allocate(size); } catch (const std::bad_alloc &) { caught = true; }
        assert(caught);
    }
    ArenaGroup group;
    auto plain = group.acquire(4096, false, false);
    group.release(std::move(plain));
    assert(!plain.base);
#if defined(__unix__) || defined(__APPLE__)
    auto guarded = group.acquire(4096, true, false);
    assert(guarded.guard_pages);
    void *old = guarded.base;
    group.release(std::move(guarded));
    guarded = group.acquire(4096, true, false);
    assert(old == guarded.base);
    group.release(std::move(guarded));
#endif
    auto big = group.acquire(70 * 1024 * 1024, false, false);
    group.release(std::move(big));
    big = group.acquire(80 * 1024 * 1024, false, false);
    assert(big.size >= 80 * 1024 * 1024);
    group.release(std::move(big));
}
