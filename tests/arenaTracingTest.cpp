#include "allocators/arenaAllocator.hpp"
#include <cassert>
#include <string>
struct Tracked
{
    int &count;
    explicit Tracked(int &c) : count(c) { ++count; }
    ~Tracked() { --count; }
};
int main()
{
    ArenaOptions opts;
    opts.initial_chunk_size = 4096;
    opts.max_chunk_size = 4096;
    opts.journaling = true;
    opts.journal_threshold_bytes = 64;
    ArenaAllocator a(opts);
    a.allocate(32);
    assert(a.journalSnapshot().empty());
    for (int i = 0; i < 1100; ++i) a.allocate(64 + i, 64);
    auto entries = a.journalSnapshot();
    assert(entries.size() == 1024);
    assert(entries.front().size == 64 + 76);
    assert(entries.back().size == 64 + 1099);
    assert(entries.back().alignment == 64);
    assert(std::string(entries.back().source.file_name()).find("arenaTracingTest.cpp") != std::string::npos);
    assert(entries.back().source.line() != 0);
    auto chunks = a.chunkCount();
    a.reset();
    for (int i = 0; i < 1100; ++i) a.allocate(64 + i, 64);
    assert(a.chunkCount() == chunks); // reuse every retained chunk in order
    ArenaAllocator b(std::move(a));
    assert(b.journalSnapshot().size() == 1024);
    assert(a.journalSnapshot().empty());
    a.allocate(128); // moved-from object remains reusable
    assert(a.journalSnapshot().size() == 1);
    ArenaAllocator c(opts);
    c = std::move(b);
    assert(c.journalSnapshot().size() == 1024 && b.journalSnapshot().empty());
    int live = 0;
    auto *obj = c.construct<Tracked>(live);
    assert(live == 1);
    c.destroy(obj);
    assert(live == 0);
    c.release();
    c.allocate(64);
    assert(c.bytesRemaining() > 0);
}
