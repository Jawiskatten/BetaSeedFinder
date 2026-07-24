#include "p20_cpu_core.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::uint32_t readLe32(std::istream& in) {
    unsigned char b[4]; in.read(reinterpret_cast<char*>(b), 4);
    return static_cast<std::uint32_t>(b[0])
         | (static_cast<std::uint32_t>(b[1]) << 8)
         | (static_cast<std::uint32_t>(b[2]) << 16)
         | (static_cast<std::uint32_t>(b[3]) << 24);
}
std::uint64_t readLe64(std::istream& in) {
    unsigned char b[8]; in.read(reinterpret_cast<char*>(b), 8);
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}
}

int main(int argc, char** argv) {
    try {
        const std::string path = argc > 1 ? argv[1] : "gpu_p20_reference.bin";
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open reference file: " + path);
        char magic[8]; in.read(magic, 8);
        if (std::string(magic, 8) != "P20REF01") throw std::runtime_error("bad reference magic");
        const auto version = readLe32(in);
        const auto records = readLe32(in);
        const auto densityCount = readLe32(in);
        const auto axisSize = readLe32(in);
        if (version != 1 || densityCount != p20::DENSITY_COUNT || axisSize != 8) {
            throw std::runtime_error("unsupported reference format");
        }

        std::uint64_t rawMismatches = 0;
        std::uint64_t decisionMismatches = 0;
        std::uint64_t countMismatches = 0;
        std::uint64_t firstMismatchRecord = UINT64_MAX;
        int firstMismatchDensity = -1;
        std::uint64_t firstExpected = 0, firstActual = 0;
        const auto started = std::chrono::steady_clock::now();

        for (std::uint32_t r = 0; r < records; ++r) {
            const std::int64_t seed = static_cast<std::int64_t>(readLe64(in));
            const int expectedCount = static_cast<int>(readLe32(in));
            std::uint64_t expectedBits[p20::DENSITY_COUNT];
            for (int i = 0; i < p20::DENSITY_COUNT; ++i) expectedBits[i] = readLe64(in);

            const p20::P20Output actual = p20::exactP20(seed);
            if (actual.upperPositiveColumns != expectedCount) {
                ++countMismatches;
                if ((actual.upperPositiveColumns == 0) != (expectedCount == 0)) ++decisionMismatches;
            }
            for (int i = 0; i < p20::DENSITY_COUNT; ++i) {
                const std::uint64_t bits = p20::doubleBits(actual.density[i]);
                if (bits != expectedBits[i]) {
                    ++rawMismatches;
                    if (firstMismatchRecord == UINT64_MAX) {
                        firstMismatchRecord = r;
                        firstMismatchDensity = i;
                        firstExpected = expectedBits[i];
                        firstActual = bits;
                    }
                }
            }
        }

        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "CPU C++ exactness validation\n"
                  << "  records:             " << records << "\n"
                  << "  raw density mismatch:" << rawMismatches << "\n"
                  << "  positive-count diff: " << countMismatches << "\n"
                  << "  P20 decision diff:   " << decisionMismatches << "\n"
                  << "  speed:                " << std::fixed << std::setprecision(1)
                  << (records / seconds) << " seeds/s\n";
        if (firstMismatchRecord != UINT64_MAX) {
            std::cout << "  first mismatch: record=" << firstMismatchRecord
                      << " densityIndex=" << firstMismatchDensity
                      << " expectedBits=0x" << std::hex << firstExpected
                      << " actualBits=0x" << firstActual << std::dec << "\n";
        }
        return (rawMismatches == 0 && decisionMismatches == 0) ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
