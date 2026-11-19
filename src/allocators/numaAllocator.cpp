#include "allocators/numaAllocator.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

std::vector<int> NumaAllocator::discoverNodes()
{
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
    {
        if (line.rfind("Mems_allowed_list:", 0) != 0) continue;
        std::istringstream list(line.substr(line.find(':') + 1));
        std::vector<int> nodes;
        std::string range;
        while (std::getline(list, range, ','))
        {
            auto dash = range.find('-');
            int first = std::stoi(range);
            int last = dash == std::string::npos ? first : std::stoi(range.substr(dash + 1));
            if (first < 0 || last < first || last > 1048575)
                throw std::runtime_error("invalid kernel NUMA node list");
            for (int i = first; i <= last; ++i) nodes.push_back(i);
        }
        if (!nodes.empty()) return nodes;
    }
#endif
    return {0};
}
NumaThreadLocation NumaAllocator::currentLocation()
{
#if defined(__linux__) && defined(SYS_getcpu)
    unsigned int cpu = 0, node = 0;
    if (::syscall(SYS_getcpu, &cpu, &node, nullptr) == 0)
        return {static_cast<int>(cpu), static_cast<int>(node)};
#endif
    return {-1, 0};
}
NumaAllocator::NumaAllocator(NumaOptions options) : options_(std::move(options))
{
    if (options_.nodes.empty()) options_.nodes = discoverNodes();
    std::sort(options_.nodes.begin(), options_.nodes.end());
    if (options_.nodes.front() < 0 || options_.nodes.back() > 1048575 ||
        std::adjacent_find(options_.nodes.begin(), options_.nodes.end()) != options_.nodes.end())
        throw std::invalid_argument("invalid or duplicate NUMA nodes");
    for (int id : options_.nodes) nodes_.push_back({id, arena_(id), 0, 0});
}
std::unique_ptr<ArenaAllocator> NumaAllocator::arena_(int node) const
{
    auto opts = options_.arena;
    opts.numa_node = node;
    return std::make_unique<ArenaAllocator>(opts);
}
NumaAllocator::Node &NumaAllocator::find_(int node)
{
    for (auto &n : nodes_) if (n.id == node) return n;
    throw std::invalid_argument("node is not configured");
}
void *NumaAllocator::allocate(std::size_t bytes, std::size_t alignment, int preferredNode)
{
    if (preferredNode < -1) throw std::invalid_argument("invalid preferred node");
    auto location = options_.location_provider ? options_.location_provider() : currentLocation();
    std::lock_guard<std::mutex> lock(mutex_);
    registry_[std::this_thread::get_id()] = location;
    Node *chosen = nullptr;
    if (preferredNode >= 0) chosen = &find_(preferredNode);
    else
    {
        for (auto &n : nodes_) if (n.id == location.node) chosen = &n;
        auto leastUsed = [&]() -> Node * {
            return &*std::min_element(nodes_.begin(), nodes_.end(), [](const Node &a, const Node &b) {
                return a.arena->memoryStats().payload < b.arena->memoryStats().payload;
            });
        };
        if (!chosen) chosen = leastUsed();
        const auto used = chosen->arena->memoryStats().payload;
        if (options_.spillover_bytes && (used >= options_.spillover_bytes ||
            bytes > options_.spillover_bytes - used)) chosen = leastUsed();
    }
    auto *p = chosen->arena->allocate(bytes, alignment);
    ++chosen->allocations;
    chosen->remote += chosen->id != location.node;
    return p;
}
void NumaAllocator::resetNode(int node)
{
    std::lock_guard<std::mutex> lock(mutex_);
    find_(node).arena->reset();
}
void NumaAllocator::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &n : nodes_) n.arena->validate();
    for (auto &n : nodes_) n.arena->reset();
}
std::size_t NumaAllocator::rebalanceIdle(int targetNode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto &target = find_(targetNode);
    // Allocate first so a strict-policy failure leaves existing arenas intact.
    if (target.arena->chunkCount() == 0) target.arena = arena_(targetNode);
    std::size_t count = 0;
    for (auto &n : nodes_)
        if (n.id != targetNode && n.arena->memoryStats().payload == 0 && n.arena->chunkCount())
        {
            n.arena->release();
            ++count;
        }
    return count;
}
std::vector<NumaNodeStats> NumaAllocator::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NumaNodeStats> result;
    for (const auto &n : nodes_) result.push_back({n.id, n.arena->memoryStats(), n.allocations, n.remote});
    return result;
}
std::map<std::thread::id, NumaThreadLocation> NumaAllocator::threadRegistry() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_;
}
void NumaAllocator::writeProfile(std::ostream &out) const
{
    out << "node,payload,overhead,idle,reserved,chunks,bound_chunks,policy_failures,allocations,remote_allocations\n";
    for (const auto &s : stats())
        out << s.node << ',' << s.memory.payload << ',' << s.memory.consumed - s.memory.payload
            << ',' << s.memory.reserved - s.memory.consumed << ',' << s.memory.reserved
            << ',' << s.memory.chunks << ',' << s.memory.numa_bound_chunks << ',' << s.memory.numa_failed_chunks
            << ',' << s.allocations << ',' << s.remote_allocations << '\n';
}
void NumaAllocator::prefetch(const void *address)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(address, 0, 3);
#else
    (void)address;
#endif
}
