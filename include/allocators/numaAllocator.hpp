#pragma once
#include "arenaAllocator.hpp"
#include <functional>
#include <map>
#include <ostream>
#include <thread>

struct NumaThreadLocation
{
    int cpu = -1;
    int node = 0;
};
struct NumaOptions
{
    ArenaOptions arena;
    // Empty: discover process-allowed memory nodes on Linux; node 0 elsewhere.
    std::vector<int> nodes;
    // Local arena first; spill to the least used node at this payload budget.
    // Zero disables spillover. This is a routing threshold, not a memory limit.
    std::size_t spillover_bytes = 0;
    // Override CPU/node discovery for tests or an application's own registry.
    std::function<NumaThreadLocation()> location_provider;
};
struct NumaNodeStats
{
    int node;
    ArenaAllocator::MemoryStats memory;
    std::uint64_t allocations = 0, remote_allocations = 0;
};

// Methods synchronize allocation/metadata. Users must synchronize reset/rebalance
// with all accesses to pointers allocated from the affected nodes.
class NumaAllocator
{
public:
    explicit NumaAllocator(NumaOptions options = {});
    void *allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t),
                   int preferredNode = -1);
    void resetNode(int node);
    void reset();
    // Reclaim reset/idle arenas on other nodes and provision the target node.
    // Live allocations never move; returns the count of reclaimed idle arenas.
    std::size_t rebalanceIdle(int targetNode);
    std::vector<NumaNodeStats> stats() const;
    std::map<std::thread::id, NumaThreadLocation> threadRegistry() const;
    void writeProfile(std::ostream &out) const;
    static std::vector<int> discoverNodes();
    static NumaThreadLocation currentLocation();
    static void prefetch(const void *address);
private:
    struct Node
    {
        int id;
        std::unique_ptr<ArenaAllocator> arena;
        std::uint64_t allocations = 0, remote = 0;
    };
    Node &find_(int node);
    std::unique_ptr<ArenaAllocator> arena_(int node) const;
    NumaOptions options_;
    std::vector<Node> nodes_;
    mutable std::mutex mutex_;
    // One latest observation per thread ID, refreshed at each allocation.
    std::map<std::thread::id, NumaThreadLocation> registry_;
};
