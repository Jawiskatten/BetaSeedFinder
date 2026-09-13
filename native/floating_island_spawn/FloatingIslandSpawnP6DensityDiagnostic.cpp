#define main highest_pillar_spawn_p1_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace p6_density_diag {
using namespace highest_pillar_spawn_p1;

static int floorDiv4(int value) {
    return value >= 0 ? value / 4 : -((-value + 3) / 4);
}

static double nodeDensity(const double* density, int x, int y, int z) {
    if (x < 0 || z < 0 || x >= coarsecore::SIZE || z >= coarsecore::SIZE) return -10.0;
    if (y < 0) return 10.0;
    if (y >= coarsecore::Y_LEVELS) return -10.0;
    return density[coarsecore::index3(x, y, z)];
}

static double blockDensity(const double* density, int worldX, int worldY, int worldZ) {
    if (worldY < coarsecore::Y_BASE * 8) return 10.0;
    if (worldY >= 128) return -10.0;
    const int coarseX = floorDiv4(worldX);
    const int coarseZ = floorDiv4(worldZ);
    const int ix = coarseX - coarsecore::FROM_COARSE;
    const int iz = coarseZ - coarsecore::FROM_COARSE;
    const int iy = (worldY >> 3) - coarsecore::Y_BASE;
    const double fx = static_cast<double>(worldX - coarseX * 4) * 0.25;
    const double fz = static_cast<double>(worldZ - coarseZ * 4) * 0.25;
    const double fy = static_cast<double>(worldY & 7) * 0.125;
    const double d000 = nodeDensity(density, ix,     iy,     iz);
    const double d001 = nodeDensity(density, ix,     iy,     iz + 1);
    const double d100 = nodeDensity(density, ix + 1, iy,     iz);
    const double d101 = nodeDensity(density, ix + 1, iy,     iz + 1);
    const double d010 = nodeDensity(density, ix,     iy + 1, iz);
    const double d011 = nodeDensity(density, ix,     iy + 1, iz + 1);
    const double d110 = nodeDensity(density, ix + 1, iy + 1, iz);
    const double d111 = nodeDensity(density, ix + 1, iy + 1, iz + 1);
    const double a0 = d000 + (d100 - d000) * fx;
    const double a1 = d001 + (d101 - d001) * fx;
    const double b0 = d010 + (d110 - d010) * fx;
    const double b1 = d011 + (d111 - d010) * 0.0; // overwritten below; keeps compiler from fusing unrelated expressions
    (void)b1;
    const double realB1 = d011 + (d111 - d011) * fx;
    const double low = a0 + (a1 - a0) * fz;
    const double high = b0 + (realB1 - b0) * fz;
    return low + (high - low) * fy;
}

static void diagnose(std::int64_t seed, const double* density) {
    std::cout << "\nseed=" << seed << "\n";
    std::cout << "origin block-density values (project GPU lattice):\n";
    std::cout << std::scientific << std::setprecision(17);
    for (int y = 76; y <= 85; ++y) {
        const double d = blockDensity(density, 0, y, 0);
        std::cout << "  y=" << y << " density=" << d << " solid=" << (d > 0.0 ? "YES" : "NO") << "\n";
    }

    std::cout << "origin vertical coarse nodes used around the disputed boundary:\n";
    const int ix = -coarsecore::FROM_COARSE;
    const int iz = -coarsecore::FROM_COARSE;
    for (int worldNodeY = 72; worldNodeY <= 96; worldNodeY += 8) {
        const int iy = (worldNodeY >> 3) - coarsecore::Y_BASE;
        const double d = nodeDensity(density, ix, iy, iz);
        std::cout << "  nodeY=" << worldNodeY << " density=" << d << "\n";
    }

    const double d81 = blockDensity(density, 0, 81, 0);
    std::cout << std::defaultfloat;
    if (d81 > 0.0 && d81 < 1e-6) {
        std::cout << "SMOKING_GUN: y81 is only barely positive in the project model. A Java-vs-GPU floating-point difference can flip this block to air.\n";
    } else if (d81 > 0.0) {
        std::cout << "y81 is materially positive, so a later worldgen/population stage is more likely than a tiny floating-point sign flip.\n";
    } else {
        std::cout << "y81 is already air in this build; the earlier diagnostic and current worker disagree and need reconciliation.\n";
    }
}

static int run(const std::vector<std::int64_t>& seeds) {
    printDevice();
    DeviceBuffers b = allocateBuffers(static_cast<int>(seeds.size()));
    try {
        checkHip(hipMemcpy(b.seeds, seeds.data(), seeds.size() * sizeof(std::int64_t), hipMemcpyHostToDevice), "copy P6 density diagnostic seeds");
        launchTerrain(b, static_cast<int>(seeds.size()), 64);
        std::vector<double> density(seeds.size() * static_cast<std::size_t>(coarsecore::CELLS));
        checkHip(hipMemcpy(density.data(), b.noise1, density.size() * sizeof(double), hipMemcpyDeviceToHost), "copy P6 density lattice");
        for (std::size_t i = 0; i < seeds.size(); ++i) diagnose(seeds[i], density.data() + i * static_cast<std::size_t>(coarsecore::CELLS));
    } catch (...) {
        freeBuffers(b);
        throw;
    }
    freeBuffers(b);
    return 0;
}

} // namespace p6_density_diag

int main(int argc, char** argv) {
    try {
        std::vector<std::int64_t> seeds;
        for (int i = 1; i < argc; ++i) seeds.push_back(std::stoll(argv[i]));
        if (seeds.empty()) seeds.push_back(-3405360075020439777LL);
        return p6_density_diag::run(seeds);
    } catch (const std::exception& e) {
        std::cerr << "P6 density diagnostic ERROR: " << e.what() << '\n';
        return 1;
    }
}
