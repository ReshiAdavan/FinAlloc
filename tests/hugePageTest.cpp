#include "allocators/arenaAllocator.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
int main()
{
    for (bool guards : {false, true})
    {
        auto c = ArenaAllocator::osAllocChunk_(3 * 1024 * 1024, guards, true);
        assert(c.base && c.size >= 3 * 1024 * 1024);
        assert(c.prefer_huge && c.guard_pages == guards);
#if defined(__linux__)
        assert(c.use_mmap);
        assert(reinterpret_cast<std::uintptr_t>(c.base) % (2 * 1024 * 1024) == 0);
        if (c.huge_page_advised)
        {
            // Verify the kernel recorded huge-page advice on this VMA.
            std::ifstream smaps("/proc/self/smaps");
            std::string line;
            bool ours = false, found = false;
            while (std::getline(smaps, line))
            {
                unsigned long long lo, hi;
                if (std::sscanf(line.c_str(), "%llx-%llx", &lo, &hi) == 2)
                {
                    auto address = reinterpret_cast<std::uintptr_t>(c.base);
                    ours = address >= lo && address < hi;
                }
                if (ours && line.rfind("VmFlags:", 0) == 0)
                    found = line.find(" hg") != std::string::npos;
            }
            assert(found);
        }
        std::cout << "Linux THP advice accepted=" << c.huge_page_advised << '\n';
#else
        assert(!c.huge_page_advised);
#endif
        std::memset(c.base, 0x42, c.size);
        ArenaAllocator::osFreeChunk_(c);
    }
    ArenaGroup g;
    auto plain = g.acquire(4096, false, false);
    g.release(std::move(plain));
    auto huge = g.acquire(4096, false, true);
    assert(huge.prefer_huge);
    void *old = huge.base;
    g.release(std::move(huge));
    huge = g.acquire(4096, false, true);
    assert(huge.base == old && huge.prefer_huge);
    g.release(std::move(huge));
    ArenaOptions o;
    o.prefer_huge = true;
    o.guard_pages = true;
    ArenaAllocator a(o);
    std::memset(a.allocate(4 * 1024 * 1024, 4096), 0x55, 4 * 1024 * 1024);
    a.reset();
    a.allocate(64);
}
