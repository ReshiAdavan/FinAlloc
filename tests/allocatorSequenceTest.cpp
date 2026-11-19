#include "support/allocatorSequence.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
int main(int argc, char **argv)
{
    std::uint64_t seed = 42;
    std::size_t cases = 100, start = 0;
    std::string inputFile;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg.rfind("--seed=",0) == 0) seed = std::stoull(arg.substr(7));
        else if (arg.rfind("--cases=",0) == 0) cases = std::stoull(arg.substr(8));
        else if (arg.rfind("--start=",0) == 0) start = std::stoull(arg.substr(8));
        else if (arg.rfind("--input=",0) == 0) inputFile = arg.substr(8);
        else { std::cerr << "unknown argument: " << arg << '\n'; return 2; }
    }
    if (!inputFile.empty())
    {
        std::ifstream file(inputFile, std::ios::binary);
        if (!file) { std::cerr << "cannot open corpus input\n"; return 2; }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
        finalloc::sequence::run(bytes);
        return 0;
    }
    for (std::size_t c = start; c - start < cases; ++c)
    {
        std::mt19937_64 rng(seed ^ (c * 0x9E3779B97F4A7C15ULL));
        std::vector<unsigned char> bytes(2048);
        for (auto &b : bytes) b = static_cast<unsigned char>(rng());
        try { finalloc::sequence::run(bytes); }
        catch (const std::exception &e)
        { std::cerr << "seed=" << seed << " case=" << c << ": " << e.what() << '\n'; return 1; }
    }
    std::cout << "sequence tests passed seed=" << seed << " start=" << start << " cases=" << cases << '\n';
}
