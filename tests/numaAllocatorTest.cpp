#include "allocators/numaAllocator.hpp"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <system_error>
#include <thread>
#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
int main()
{
    std::atomic<int> current{0};
    NumaOptions opts;
    opts.nodes = {0, 2}; // sparse synthetic topology, independent of hardware
    opts.arena.initial_chunk_size = 4096;
    opts.arena.use_canaries = true;
    opts.arena.canary_size = 16;
    opts.spillover_bytes = 64;
    opts.location_provider = [&] { return NumaThreadLocation{current.load() * 4, current.load()}; };
    NumaAllocator a(opts);
    auto *p = static_cast<char *>(a.allocate(64, 256));
    assert(reinterpret_cast<std::uintptr_t>(p) % 256 == 0);
    std::memset(p, 0x42, 64);
    a.allocate(32); // spill from full node 0 to node 2
    auto stats = a.stats();
    assert(stats[0].memory.payload == 64 && stats[1].memory.payload == 32);
    assert(stats[1].remote_allocations == 1);
    current = 2;
    a.allocate(8, 16, 2);
    auto registry = a.threadRegistry();
    assert(registry.at(std::this_thread::get_id()).node == 2);
    assert(registry.at(std::this_thread::get_id()).cpu == 8);
    assert(a.rebalanceIdle(2) == 0); // do not free live node 0
    assert(p[0] == 0x42);
    a.resetNode(0);
    assert(a.rebalanceIdle(2) == 1);
    assert(a.stats()[0].memory.reserved == 0);
    a.allocate(16, 16, 0); // recreate reclaimed arena with its own policy
    assert(a.stats()[0].memory.payload == 16);
    bool caught = false;
    try { a.allocate(8, 16, 1); } catch (const std::invalid_argument &) { caught = true; }
    assert(caught);
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            for (int j = 0; j < 500; ++j) std::memset(a.allocate(32), 0x55, 32);
        });
    for (auto &t : threads) t.join();
    std::uint64_t total = 0;
    for (const auto &s : a.stats())
    {
        total += s.allocations;
        assert(s.memory.reserved >= s.memory.consumed);
        assert(s.memory.consumed >= s.memory.payload);
        assert(s.memory.numa_bound_chunks + s.memory.numa_failed_chunks == s.memory.chunks);
    }
    assert(total == 2004);
    std::ostringstream profile;
    a.writeProfile(profile);
    assert(profile.str().find("remote_allocations") != std::string::npos);
    a.reset();
    for (auto &s : a.stats()) assert(s.memory.payload == 0);
    assert(a.rebalanceIdle(0) == 1);
    NumaAllocator native;
    auto *live = native.allocate(4096);
    std::memset(live, 1, 4096);
    NumaAllocator::prefetch(live);
    assert(!NumaAllocator::discoverNodes().empty());
    std::cout << "Native NUMA location cpu=" << NumaAllocator::currentLocation().cpu
              << " node=" << NumaAllocator::currentLocation().node << '\n';
    native.writeProfile(std::cout);
    try
    {
        int node = NumaAllocator::discoverNodes().front();
        auto c = ArenaAllocator::osAllocChunk_(4096, false, false, node, true);
        assert(c.numa_policy_applied && c.numa_strict);
        std::memset(c.base, 1, c.size);
#if defined(__linux__) && defined(SYS_get_mempolicy)
        int mode = -1;
        unsigned long mask[16384] = {};
        if (::syscall(SYS_get_mempolicy, &mode, mask, 1048576UL, c.base, MPOL_F_ADDR) == 0)
        {
            assert(mode == MPOL_BIND);
            assert(mask[node / (8 * sizeof(unsigned long))] & (1UL << (node % (8 * sizeof(unsigned long)))));
            int residentNode = -1;
            if (::syscall(SYS_get_mempolicy, &residentNode, nullptr, 0UL, c.base,
                          MPOL_F_ADDR | MPOL_F_NODE) == 0)
                assert(residentNode == node);
            std::cout << "Kernel NUMA binding policy verified\n";
        }
#endif
        ArenaAllocator::osFreeChunk_(c);
    }
    catch (const std::system_error &e)
    {
        std::cout << "Strict NUMA unavailable on this host: " << e.what() << '\n';
    }
    opts.nodes = {0, 0};
    caught = false;
    try { NumaAllocator invalid(opts); } catch (const std::invalid_argument &) { caught = true; }
    assert(caught);
}
