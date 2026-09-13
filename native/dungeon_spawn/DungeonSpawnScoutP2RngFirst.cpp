#define main lava_spawn_origin_embedded_main
#include "CursedSpawnOriginGpuFinder.cpp"
#undef main

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace dungeon_spawn_p2 {
using namespace lava_spawn_origin_p2;

struct RngPrefilterResult {
    std::int64_t seed;
    std::uint64_t sequenceIndex;
    int pass;
};

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

__device__ __forceinline__ void firstDungeonRng(
        std::int64_t seed,
        int& lakeFree,
        int& dx, int& dy, int& dz, int& rx, int& rz) {
    p20::JavaRandom worldRandom;
    worldRandom.setSeed(seed);
    const std::int64_t oddX = javaOddLong(javaNextLong(worldRandom));
    const std::int64_t oddZ = javaOddLong(javaNextLong(worldRandom));
    std::uint64_t popSeed = 0ULL - static_cast<std::uint64_t>(oddX);
    popSeed += 0ULL - static_cast<std::uint64_t>(oddZ);
    popSeed ^= static_cast<std::uint64_t>(seed);
    p20::JavaRandom pr;
    pr.setSeed(static_cast<std::int64_t>(popSeed));

    lakeFree = 0;
    if (pr.nextInt(4) == 0) return;
    if (pr.nextInt(8) == 0) return;
    lakeFree = 1;

    dx = -16 + pr.nextInt(16) + 8;
    dy = pr.nextInt(128);
    dz = -16 + pr.nextInt(16) + 8;
    rx = pr.nextInt(2) + 2;
    rz = pr.nextInt(2) + 2;
}

__global__ void rngPrefilterKernel(
        const std::int64_t* seeds,
        int count,
        std::uint64_t baseIndex,
        RngPrefilterResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    RngPrefilterResult r{};
    r.seed = seeds[i];
    r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(i);
    r.pass = 0;

    int lakeFree = 0, dx = 0, dy = 0, dz = 0, rx = 0, rz = 0;
    firstDungeonRng(r.seed, lakeFree, dx, dy, dz, rx, rz);
    if (!lakeFree) { out[i] = r; return; }
    if (dy < 63 || dy > 123) { out[i] = r; return; }
    if (0 < dx - rx || 0 > dx + rx || 0 < dz - rz || 0 > dz + rz) { out[i] = r; return; }
    r.pass = 1;
    out[i] = r;
}

__device__ __forceinline__ bool postDungeonSolidAtOrigin(
        const double* density, std::size_t base, int y,
        int dungeonX, int dungeonY, int dungeonZ, int radiusX, int radiusZ) {
    if (y < 0) return true;
    if (y >= 128) return false;
    const bool originInsideXZ = (0 >= dungeonX - radiusX && 0 <= dungeonX + radiusX
                              && 0 >= dungeonZ - radiusZ && 0 <= dungeonZ + radiusZ);
    if (originInsideXZ) {
        if (y == dungeonY - 1) return true;
        if (y >= dungeonY && y <= dungeonY + 3) {
            if (dungeonX == 0 && dungeonZ == 0 && y == dungeonY) return true;
            return false;
        }
    }
    return solidAtWorldY(density, base, 0, 0, y);
}

__global__ void scoreTerrainSurvivorsKernel(
        const std::int64_t* seeds,
        const std::uint64_t* sequenceIndices,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        DungeonResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;

    DungeonResult r{};
    r.seed = seeds[i];
    r.sequenceIndex = sequenceIndices[i];
    r.qualified = 0;
    r.prepopSandY = -1;
    r.sandReason = 0;
    r.dungeonX = r.dungeonY = r.dungeonZ = -999;
    r.radiusX = r.radiusZ = 0;
    r.predictedFeetY = -1;
    r.spawnerAtOrigin = 0;

    int lakeFree = 0, dx = 0, dy = 0, dz = 0, rx = 0, rz = 0;
    firstDungeonRng(r.seed, lakeFree, dx, dy, dz, rx, rz);
    if (!lakeFree) { out[i] = r; return; }
    r.dungeonX = dx; r.dungeonY = dy; r.dungeonZ = dz;
    r.radiusX = rx; r.radiusZ = rz;
    r.spawnerAtOrigin = (dx == 0 && dz == 0) ? 1 : 0;

    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;
    const int originY = exactSpawnCheckYAtOrigin(density, base, 0, 0);
    r.prepopSandY = originY;

    const bool desert = betaBiomeIsDesert(originTemperature[i], originRainfall[i]);
    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool beachSand = originSandNoise[i] + sandJitter * 0.2 > 0.0;
    const int depth = static_cast<int>(originStoneNoise[i] / 3.0 + 3.0 + depthJitter * 0.25);
    const int reason = desert ? 2 : ((originY >= 60 && originY <= 65 && beachSand) ? 1 : 0);
    r.sandReason = reason;
    if (!(originY >= 63 && depth > 0 && reason != 0)) { out[i] = r; return; }

    int feet = 65;
    while (feet < 127) {
        const bool a = postDungeonSolidAtOrigin(density, base, feet, dx, dy, dz, rx, rz);
        const bool b = postDungeonSolidAtOrigin(density, base, feet + 1, dx, dy, dz, rx, rz);
        if (!a && !b) break;
        ++feet;
    }
    r.predictedFeetY = feet;
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
    RngPrefilterResult* dPrefilter = nullptr;
    DungeonResult* dResults = nullptr;
    std::uint64_t* dSequenceIndices = nullptr;
    allocateArray(dPrefilter, static_cast<std::size_t>(c.batch), "allocate RNG prefilter results");
    allocateArray(dResults, static_cast<std::size_t>(c.batch), "allocate dungeon results");
    allocateArray(dSequenceIndices, static_cast<std::size_t>(c.batch), "allocate sequence indices");

    std::vector<RngPrefilterResult> prefHost(static_cast<std::size_t>(c.batch));
    std::vector<DungeonResult> resultHost(static_cast<std::size_t>(c.batch));
    std::vector<std::int64_t> packedSeeds(static_cast<std::size_t>(c.batch));
    std::vector<std::uint64_t> packedSeq(static_cast<std::size_t>(c.batch));

    const std::filesystem::path csvPath = c.outputDir / ("candidates_" + std::to_string(c.startIndex) + ".csv");
    std::ofstream csv(csvPath, std::ios::trunc);
    if (!csv) throw std::runtime_error("cannot write " + csvPath.string());
    writeHeader(csv);

    std::uint64_t checked = 0;
    std::uint64_t rngSurvivors = 0;
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

            {
                const int threads = 256;
                const int blocks = (n + threads - 1) / threads;
                hipLaunchKernelGGL(rngPrefilterKernel, dim3(blocks), dim3(threads), 0, 0,
                    b.seeds, n, sequenceBase, dPrefilter);
                checkHip(hipGetLastError(), "launch RNG prefilter");
                checkHip(hipDeviceSynchronize(), "finish RNG prefilter");
                checkHip(hipMemcpy(prefHost.data(), dPrefilter, static_cast<std::size_t>(n) * sizeof(RngPrefilterResult), hipMemcpyDeviceToHost),
                    "copy RNG prefilter results");
            }

            int m = 0;
            for (int j = 0; j < n; ++j) {
                const auto& p = prefHost[static_cast<std::size_t>(j)];
                if (!p.pass) continue;
                packedSeeds[static_cast<std::size_t>(m)] = p.seed;
                packedSeq[static_cast<std::size_t>(m)] = p.sequenceIndex;
                ++m;
            }
            rngSurvivors += static_cast<std::uint64_t>(m);

            if (m > 0) {
                checkHip(hipMemcpy(b.seeds, packedSeeds.data(), static_cast<std::size_t>(m) * sizeof(std::int64_t), hipMemcpyHostToDevice), "upload compacted seeds");
                checkHip(hipMemcpy(dSequenceIndices, packedSeq.data(), static_cast<std::size_t>(m) * sizeof(std::uint64_t), hipMemcpyHostToDevice), "upload compacted sequence indices");

                launchTerrain(b, m, c.terrainThreads);
                const int threads = 256;
                const int blocks = (m + threads - 1) / threads;
                hipLaunchKernelGGL(scoreTerrainSurvivorsKernel, dim3(blocks), dim3(threads), 0, 0,
                    b.seeds, dSequenceIndices, b.noise1,
                    b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
                    m, dResults);
                checkHip(hipGetLastError(), "launch terrain survivor scoring");
                checkHip(hipDeviceSynchronize(), "finish terrain survivor scoring");
                checkHip(hipMemcpy(resultHost.data(), dResults, static_cast<std::size_t>(m) * sizeof(DungeonResult), hipMemcpyDeviceToHost),
                    "copy dungeon candidate results");

                for (int j = 0; j < m; ++j) {
                    const auto& r = resultHost[static_cast<std::size_t>(j)];
                    if (!r.qualified) continue;
                    ++hits;
                    csv << r.seed << ',' << r.sequenceIndex << ',' << r.prepopSandY << ',' << r.sandReason << ','
                        << r.dungeonX << ',' << r.dungeonY << ',' << r.dungeonZ << ',' << r.radiusX << ',' << r.radiusZ << ','
                        << r.predictedFeetY << ',' << r.spawnerAtOrigin << '\n';
                }
            }

            checked += static_cast<std::uint64_t>(n);
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() >= c.progressMs || checked == c.count) {
                csv.flush();
                const double sec = std::chrono::duration<double>(now - start).count();
                std::cout << "progress checked=" << checked << '/' << c.count
                          << " rate=" << static_cast<std::uint64_t>(checked / std::max(0.001, sec))
                          << " seeds/s rngSurvivors=" << rngSurvivors
                          << " candidates=" << hits << "\n";
                lastPrint = now;
            }
            if (c.singleSeedSet) break;
        }
    } catch (...) {
        if (dPrefilter) (void)hipFree(dPrefilter);
        if (dResults) (void)hipFree(dResults);
        if (dSequenceIndices) (void)hipFree(dSequenceIndices);
        freeBuffers(b);
        throw;
    }

    if (dPrefilter) checkHip(hipFree(dPrefilter), "free RNG prefilter results");
    if (dResults) checkHip(hipFree(dResults), "free dungeon results");
    if (dSequenceIndices) checkHip(hipFree(dSequenceIndices), "free sequence indices");
    freeBuffers(b);
    csv.flush();
    std::cout << "DONE checked=" << checked << " rngSurvivors=" << rngSurvivors
              << " candidates=" << hits << " file=" << csvPath.string() << "\n";
    std::cout << "P2 optimization: RNG/dungeon-position filter runs before any terrain generation.\n";
    return 0;
}
} // namespace dungeon_spawn_p2

int main(int argc, char** argv) {
    try {
        return dungeon_spawn_p2::runScout(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
