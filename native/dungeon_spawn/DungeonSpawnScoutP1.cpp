#define main lava_spawn_origin_embedded_main
#include "CursedSpawnOriginGpuFinder.cpp"
#undef main

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace dungeon_spawn_p1 {
using namespace lava_spawn_origin_p2;

struct DungeonResult {
    std::int64_t seed;
    std::uint64_t sequenceIndex;
    int qualified;
    int prepopSandY;
    int sandReason;
    int dungeonX;
    int dungeonY;
    int dungeonZ;
    int radiusX;
    int radiusZ;
    int predictedFeetY;
    int spawnerAtOrigin;
};

__device__ __forceinline__ bool postDungeonSolidAtOrigin(
        const double* density, std::size_t base, int y,
        int dungeonX, int dungeonY, int dungeonZ, int radiusX, int radiusZ) {
    if (y < 0) return true;
    if (y >= 128) return false;
    const bool originInsideXZ = (0 >= dungeonX - radiusX && 0 <= dungeonX + radiusX
                              && 0 >= dungeonZ - radiusZ && 0 <= dungeonZ + radiusZ);
    if (originInsideXZ) {
        // Dungeon floor is rebuilt at y-1. The room interior is air y..y+3,
        // except that a center spawner occupies (dungeonX,dungeonY,dungeonZ).
        if (y == dungeonY - 1) return true;
        if (y >= dungeonY && y <= dungeonY + 3) {
            if (dungeonX == 0 && dungeonZ == 0 && y == dungeonY) return true;
            return false;
        }
    }
    return solidAtWorldY(density, base, 0, 0, y);
}

__global__ void scoreDungeonCandidatesKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        std::uint64_t baseIndex,
        DungeonResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;

    DungeonResult r{};
    r.seed = seeds[i];
    r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(i);
    r.qualified = 0;
    r.prepopSandY = -1;
    r.sandReason = 0;
    r.dungeonX = r.dungeonY = r.dungeonZ = -999;
    r.radiusX = r.radiusZ = 0;
    r.predictedFeetY = -1;
    r.spawnerAtOrigin = 0;

    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;
    // This P1 build deliberately uses a vanilla chunk-local 5x17x5 lattice,
    // so x=z=0 are the owning chunk's exact origin nodes.
    const int originY = exactSpawnCheckYAtOrigin(density, base, 0, 0);
    r.prepopSandY = originY;

    const bool desert = betaBiomeIsDesert(originTemperature[i], originRainfall[i]);
    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0); // chunk (0,0) surface RNG seed
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble(); // gravel jitter
    const double depthJitter = surfaceRandom.nextDouble();
    const bool beachSand = originSandNoise[i] + sandJitter * 0.2 > 0.0;
    const int depth = static_cast<int>(originStoneNoise[i] / 3.0 + 3.0 + depthJitter * 0.25);
    const int reason = desert ? 2 : ((originY >= 60 && originY <= 65 && beachSand) ? 1 : 0);
    r.sandReason = reason;
    if (!(originY >= 63 && depth > 0 && reason != 0)) {
        out[i] = r;
        return;
    }

    // Population chunk (-1,-1) is the only one whose dungeon centers (-8..7)
    // can put the origin inside a normal 5x5 or 7x7 dungeon interior.
    p20::JavaRandom worldRandom;
    worldRandom.setSeed(seeds[i]);
    const std::int64_t oddX = javaOddLong(javaNextLong(worldRandom));
    const std::int64_t oddZ = javaOddLong(javaNextLong(worldRandom));
    std::uint64_t popSeed = 0ULL - static_cast<std::uint64_t>(oddX);
    popSeed += 0ULL - static_cast<std::uint64_t>(oddZ);
    popSeed ^= static_cast<std::uint64_t>(seeds[i]);
    p20::JavaRandom pr;
    pr.setSeed(static_cast<std::int64_t>(popSeed));

    // P1 intentionally searches the lake-free population subset. This keeps the
    // RNG state for the first dungeon attempt exact without emulating WorldGenLakes.
    // Coverage is 3/4 * 7/8 = 65.625% before the dungeon constraints.
    if (pr.nextInt(4) == 0) { out[i] = r; return; }
    if (pr.nextInt(8) == 0) { out[i] = r; return; }

    const int dx = -16 + pr.nextInt(16) + 8;
    const int dy = pr.nextInt(128);
    const int dz = -16 + pr.nextInt(16) + 8;
    const int rx = pr.nextInt(2) + 2;
    const int rz = pr.nextInt(2) + 2;
    r.dungeonX = dx; r.dungeonY = dy; r.dungeonZ = dz;
    r.radiusX = rx; r.radiusZ = rz;
    r.spawnerAtOrigin = (dx == 0 && dz == 0) ? 1 : 0;

    // y=62 or lower cannot leave a 1.8-block player inside the room when the
    // player starts with feet at y=65. y>123 cannot have Beta's required solid
    // ceiling at dungeonY+4 in the 128-block world.
    if (dy < 63 || dy > 123) { out[i] = r; return; }
    if (0 < dx - rx || 0 > dx + rx || 0 < dz - rz || 0 > dz + rz) {
        out[i] = r;
        return;
    }

    // Cheap but strong prediction: if the dungeon succeeds, rebuild its floor,
    // clear its room, place the center spawner, then run the exact integer-Y
    // preparePlayerToSpawn collision rule on the origin column. Caves are not
    // included here; the Java verifier is authoritative for them and population.
    int feet = 65;
    while (feet < 127) {
        const bool a = postDungeonSolidAtOrigin(density, base, feet, dx, dy, dz, rx, rz);
        const bool b = postDungeonSolidAtOrigin(density, base, feet + 1, dx, dy, dz, rx, rz);
        if (!a && !b) break;
        ++feet;
    }
    r.predictedFeetY = feet;

    // Interior feet can be at dungeonY..dungeonY+2. If the spawner is exactly at
    // the origin, standing on it normally gives dungeonY+1, which is still inside.
    if (feet < dy || feet > dy + 2) { out[i] = r; return; }

    r.qualified = 1;
    out[i] = r;
}

static void writeHeader(std::ofstream& f) {
    f << "seed,sequence_index,prepop_sand_y,sand_reason,dungeon_x,dungeon_y,dungeon_z,radius_x,radius_z,predicted_feet_y,spawner_at_origin\n";
}

static int runScout(int argc, char** argv) {
    Config c = parseArgs(argc, argv);
    if (c.outputDir.empty()) throw std::invalid_argument("--output is required");
    std::filesystem::create_directories(c.outputDir);
    printDevice();

    DeviceBuffers b = allocateBuffers(c.batch);
    DungeonResult* dResults = nullptr;
    allocateArray(dResults, static_cast<std::size_t>(c.batch), "allocate dungeon results");
    std::vector<DungeonResult> host;
    host.resize(static_cast<std::size_t>(c.batch));

    const std::filesystem::path csvPath = c.outputDir / ("candidates_" + std::to_string(c.startIndex) + ".csv");
    std::ofstream csv(csvPath, std::ios::trunc);
    if (!csv) throw std::runtime_error("cannot write " + csvPath.string());
    writeHeader(csv);

    std::uint64_t checked = 0;
    std::uint64_t hits = 0;
    const auto start = std::chrono::steady_clock::now();
    auto lastPrint = start;

    try {
        while (checked < c.count) {
            const int n = static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint64_t>(c.batch), c.count - checked));
            const std::uint64_t sequenceBase = c.startIndex + checked;

            if (c.singleSeedSet) {
                checkHip(hipMemcpy(b.seeds, &c.singleSeed, sizeof(c.singleSeed), hipMemcpyHostToDevice), "copy single seed");
            } else {
                const int threads = 256;
                const int blocks = (n + threads - 1) / threads;
                hipLaunchKernelGGL(generateRandomSeedsKernel, dim3(blocks), dim3(threads), 0, 0,
                    b.seeds, n, c.randomKey, sequenceBase, static_cast<int>(c.seedMode));
                checkHip(hipGetLastError(), "generate random seeds");
            }

            launchTerrain(b, n, c.terrainThreads);
            const int threads = 256;
            const int blocks = (n + threads - 1) / threads;
            hipLaunchKernelGGL(scoreDungeonCandidatesKernel, dim3(blocks), dim3(threads), 0, 0,
                b.seeds, b.noise1,
                b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
                n, sequenceBase, dResults);
            checkHip(hipGetLastError(), "launch dungeon spawn scout");
            checkHip(hipDeviceSynchronize(), "finish dungeon spawn scout batch");
            checkHip(hipMemcpy(host.data(), dResults, static_cast<std::size_t>(n) * sizeof(DungeonResult), hipMemcpyDeviceToHost),
                     "copy dungeon candidate results");

            for (int j = 0; j < n; ++j) {
                const auto& r = host[static_cast<std::size_t>(j)];
                if (!r.qualified) continue;
                ++hits;
                csv << r.seed << ',' << r.sequenceIndex << ',' << r.prepopSandY << ',' << r.sandReason << ','
                    << r.dungeonX << ',' << r.dungeonY << ',' << r.dungeonZ << ',' << r.radiusX << ',' << r.radiusZ << ','
                    << r.predictedFeetY << ',' << r.spawnerAtOrigin << '\n';
            }
            csv.flush();
            checked += static_cast<std::uint64_t>(n);

            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() >= c.progressMs || checked == c.count) {
                const double sec = std::chrono::duration<double>(now - start).count();
                std::cout << "progress checked=" << checked << '/' << c.count
                          << " rate=" << static_cast<std::uint64_t>(checked / std::max(0.001, sec))
                          << " seeds/s candidates=" << hits << "\n";
                lastPrint = now;
            }
            if (c.singleSeedSet) break;
        }
    } catch (...) {
        if (dResults) (void)hipFree(dResults);
        freeBuffers(b);
        throw;
    }

    if (dResults) checkHip(hipFree(dResults), "free dungeon results");
    freeBuffers(b);
    std::cout << "DONE checked=" << checked << " candidates=" << hits << " file=" << csvPath.string() << "\n";
    std::cout << "P1 coverage note: first dungeon attempt in population chunk (-1,-1), only when both earlier lake chances miss.\n";
    return 0;
}
} // namespace dungeon_spawn_p1

int main(int argc, char** argv) {
    try {
        return dungeon_spawn_p1::runScout(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
