#include "../tests/support/allocatorSequence.hpp"
#include <iostream>
extern "C" int LLVMFuzzerTestOneInput(const unsigned char *data, std::size_t size)
{
    try { finalloc::sequence::run({data, size}); }
    catch (const std::exception &e)
    {
        std::cerr << "allocator model failure: " << e.what() << '\n';
        std::abort();
    }
    return 0;
}
