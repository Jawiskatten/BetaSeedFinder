#define main highest_pillar_spawn_p1_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace floating_island_spawn_p6_diag {
using namespace highest_pillar_spawn_p1;

static int floorDiv4(int value) {
    return value >= 0 ? value / 4 : -((-value + 3) / 4);
}

static double nodeDensityHost(const double* density, int x, int y, int z) {
    if (x < 0 || z < 0 || x >= coarsecore::SIZE || z >= coarsecore::SIZE) return -10.0;
    if (y < 0) return 10.0;
    if (y >= coarsecore::Y_LEVELS) return -10.0;
    return density[coarsecore::index3(x, y, z)];
}

static bool solidAtBlockHost(const double* density, int worldX, int worldY, int worldZ) {
    if (worldY < coarsecore::Y_BASE * 8) return true;
    if (worldY >= 128) return false;
    const int coarseX = floorDiv4(worldX);
    const int coarseZ = floorDiv4(worldZ);
    const int ix = coarseX - coarsecore::FROM_COARSE;
    const int iz = coarseZ - coarsecore::FROM_COARSE;
    const int iy = (worldY >> 3) - coarsecore::Y_BASE;
    const double fx = static_cast<double>(worldX - coarseX * 4) * 0.25;
    const double fz = static_cast<double>(worldZ - coarseZ * 4) * 0.25;
    const double fy = static_cast<double>(worldY & 7) * 0.125;
    const double d000 = nodeDensityHost(density, ix,     iy,     iz);
    const double d001 = nodeDensityHost(density, ix,     iy,     iz + 1);
    const double d100 = nodeDensityHost(density, ix + 1, iy,     iz);
    const double d101 = nodeDensityHost(density, ix + 1, iy,     iz + 1);
    const double d010 = nodeDensityHost(density, ix,     iy + 1, iz);
    const double d011 = nodeDensityHost(density, ix,     iy + 1, iz + 1);
    const double d110 = nodeDensityHost(density, ix + 1, iy + 1, iz);
    const double d111 = nodeDensityHost(density, ix + 1, iy + 1, iz + 1);
    const double a0 = d000 + (d100 - d000) * fx;
    const double a1 = d001 + (d101 - d001) * fx;
    const double b0 = d010 + (d110 - d010) * fx;
    const double b1 = d011 + (d111 - d011) * fx;
    const double low = a0 + (a1 - a0) * fz;
    const double high = b0 + (b1 - b0) * fz;
    return low + (high - low) * fy > 0.0;
}

static int firstUncoveredSurfaceY(const double* density) {
    int y = 63;
    while (y + 1 < 128 && solidAtBlockHost(density, 0, y + 1, 0)) ++y;
    return solidAtBlockHost(density, 0, y, 0) ? y : -1;
}

static int firstSolidAbove(const double* density, int y0) {
    for (int y = y0 + 1; y < 128; ++y) if (solidAtBlockHost(density, 0, y, 0)) return y;
    return -1;
}

static int highestSolidAtOrBelowHost(const double* density, int y0) {
    for (int y = std::min(127, y0); y >= 0; --y) if (solidAtBlockHost(density, 0, y, 0)) return y;
    return -1;
}

// OLD project approximation: incorrectly treated Entity.posY=65 as the feet
// coordinate and tested only the two blocks at posY and posY+1.
static bool legacyCollision(const double* density, int posY) {
    return solidAtBlockHost(density, 0, posY, 0)
        || solidAtBlockHost(density, 0, posY + 1, 0);
}

static int legacyClearPosY(const double* density) {
    int y = 65;
    while (y < 128 && legacyCollision(density, y)) ++y;
    return y;
}

// Vanilla Beta 1.7.3 EntityPlayer has width=0.6, height=1.8 and yOffset=1.62.
// The constructor places Entity.posY at worldSpawnY+1 = 65. Entity.setPosition
// therefore gives an AABB [posY-1.62, posY+0.18]. At integer posY the full
// blocks that overlap that AABB are exactly posY-2, posY-1 and posY.
static bool vanillaInitialAabbCollision(const double* density, int posY) {
    return solidAtBlockHost(density, 0, posY - 2, 0)
        || solidAtBlockHost(density, 0, posY - 1, 0)
        || solidAtBlockHost(density, 0, posY, 0);
}

static int vanillaClearPosY(const double* density) {
    int posY = 65;
    while (posY < 130 && vanillaInitialAabbCollision(density, posY)) ++posY;
    return posY;
}

// Once preparePlayerToSpawn has found a collision-free integer posY, normal
// gravity can land the player on the highest full block whose top is at/below
// the current AABB bottom. For integer posY and yOffset=1.62 that is <= posY-3.
static int vanillaLandingSupportY(const double* density, int clearPosY) {
    return highestSolidAtOrBelowHost(density, clearPosY - 3);
}

static std::string runs(const double* density, int fromY, int toY) {
    std::ostringstream out;
    bool first = true;
    int y = fromY;
    while (y <= toY) {
        if (!solidAtBlockHost(density, 0, y, 0)) { ++y; continue; }
        const int start = y;
        while (y + 1 <= toY && solidAtBlockHost(density, 0, y + 1, 0)) ++y;
        if (!first) out << ',';
        first = false;
        if (start == y) out << start;
        else out << start << '-' << y;
        ++y;
    }
    return first ? std::string("none") : out.str();
}

static void diagnoseOne(std::int64_t seed, const double* density) {
    const int surface = firstUncoveredSurfaceY(density);
    const int upper = surface >= 0 ? firstSolidAbove(density, surface) : -1;
    const int gap = (surface >= 0 && upper >= 0) ? upper - surface - 1 : -1;

    const int legacyPos = legacyClearPosY(density);
    const int legacySupport = highestSolidAtOrBelowHost(density, legacyPos - 1);
    const int vanillaPos = vanillaClearPosY(density);
    const int vanillaSupport = vanillaLandingSupportY(density, vanillaPos);
    const int vanillaFeet = vanillaSupport >= 0 ? vanillaSupport + 1 : -1;
    const bool legacyUpper = upper >= 0 && legacySupport >= upper;
    const bool vanillaUpper = upper >= 0 && vanillaSupport >= upper;

    std::cout << "\nseed=" << seed << '\n';
    std::cout << "raw_origin_solid_runs_y56_110=" << runs(density, 56, 110) << '\n';
    std::cout << "raw_spawn_surface_y=" << surface
              << " first_raw_upper_y=" << upper
              << " raw_air_gap=" << gap << '\n';
    std::cout << "legacy_project_model clearPosY=" << legacyPos
              << " supportY=" << legacySupport
              << " landsOnRawUpper=" << (legacyUpper ? "YES" : "NO") << '\n';
    std::cout << std::fixed << std::setprecision(2)
              << "vanilla_AABB_model clearEntityPosY=" << vanillaPos
              << " clearBBoxY=[" << (static_cast<double>(vanillaPos) - 1.62)
              << ',' << (static_cast<double>(vanillaPos) + 0.18) << ']'
              << " landingSupportY=" << vanillaSupport
              << " landingFeetY=" << vanillaFeet
              << " landsOnRawUpper=" << (vanillaUpper ? "YES" : "NO") << '\n';
}

static int run(const std::vector<std::int64_t>& seeds) {
    if (seeds.empty()) throw std::invalid_argument("at least one seed is required");
    printDevice();
    DeviceBuffers b = allocateBuffers(static_cast<int>(seeds.size()));
    try {
        checkHip(hipMemcpy(b.seeds, seeds.data(), seeds.size() * sizeof(std::int64_t), hipMemcpyHostToDevice),
                 "copy P6 diagnostic seeds");
        launchTerrain(b, static_cast<int>(seeds.size()), 64);
        std::vector<double> density(seeds.size() * static_cast<std::size_t>(coarsecore::CELLS));
        checkHip(hipMemcpy(density.data(), b.noise1, density.size() * sizeof(double), hipMemcpyDeviceToHost),
                 "copy P6 diagnostic density");
        for (std::size_t i = 0; i < seeds.size(); ++i) {
            diagnoseOne(seeds[i], density.data() + i * static_cast<std::size_t>(coarsecore::CELLS));
        }
    } catch (...) {
        freeBuffers(b);
        throw;
    }
    freeBuffers(b);
    std::cout << "\nIMPORTANT: this diagnostic is exact for the project's generated base terrain,"
                 " but it intentionally does NOT apply Beta MapGenCaves yet. Vanilla generates caves"
                 " before getFirstUncoveredBlock/canCoordinateBeSpawn is queried. If the vanilla-AABB"
                 " prediction still disagrees with the real game, cave carving or the resulting spawn"
                 " coordinate is the next thing to model.\n";
    return 0;
}

} // namespace floating_island_spawn_p6_diag

int main(int argc, char** argv) {
    try {
        std::vector<std::int64_t> seeds;
        for (int i = 1; i < argc; ++i) seeds.push_back(std::stoll(argv[i]));
        if (seeds.empty()) {
            seeds.push_back(-3405360075020439777LL);
            seeds.push_back(6430576860599818994LL);
        }
        return floating_island_spawn_p6_diag::run(seeds);
    } catch (const std::exception& e) {
        std::cerr << "P6 spawn diagnostic ERROR: " << e.what() << '\n';
        return 1;
    }
}
