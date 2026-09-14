#define main highest_pillar_spawn_embedded_main
#include "HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cactus_elevator_p1 {
using namespace highest_pillar_spawn_p1;

struct BridgeResult {
    std::int64_t seed;
    std::uint64_t sequenceIndex;
    int qualified;
    int spawnSurfaceY;
    int desert;
    int sandReason;
    int preFeetY;
    int feetH2;
    int feetH3;
    int liftH2;
    int liftH3;
    int requiredHeight;
    int overheadBottom;
};

struct BridgeConfig {
    std::filesystem::path outputDir;
    std::uint64_t count = 10000000;
    std::uint64_t startIndex = 0;
    int batch = 8192;
    int terrainThreads = 64;
    int minLift = 8;
    std::uint64_t randomKey = 0;
    bool randomKeySet = false;
    SeedMode seedMode = SeedMode::Unique48;
    int progressMs = 1000;
    bool singleSeedSet = false;
    std::int64_t singleSeed = 0;
};

__device__ __forceinline__ bool cactusHorizontalClearAt(
        const double* density, std::size_t base, int y) {
    return !terrainSolidAtBlock(density, base, -1, y, 0)
        && !terrainSolidAtBlock(density, base,  1, y, 0)
        && !terrainSolidAtBlock(density, base,  0, y,-1)
        && !terrainSolidAtBlock(density, base,  0, y, 1);
}

__device__ __forceinline__ bool cactusOccupiesY(int y, int height) {
    return y >= 64 && y < 64 + height;
}

__device__ __forceinline__ bool collidesWithSyntheticCactus(
        const double* density, std::size_t base, int feetY, int cactusHeight) {
    if (terrainSolidAtBlock(density, base, 0, feetY, 0)
            || terrainSolidAtBlock(density, base, 0, feetY + 1, 0)) return true;
    // A cactus collision box is inset horizontally but still intersects the
    // player's X/Z 0.2..0.8 footprint. Vertically it intersects when the
    // cactus block occupies feetY or feetY+1.
    return cactusOccupiesY(feetY, cactusHeight)
        || cactusOccupiesY(feetY + 1, cactusHeight);
}

__device__ __forceinline__ int playerFeetWithSyntheticCactus(
        const double* density, std::size_t base, int cactusHeight) {
    int feet = 65;
    while (feet < 128 && collidesWithSyntheticCactus(density, base, feet, cactusHeight)) ++feet;
    return feet;
}

__device__ __forceinline__ int firstTerrainSolidAbove(
        const double* density, std::size_t base, int fromY) {
    for (int y = fromY; y < 128; ++y) {
        if (terrainSolidAtBlock(density, base, 0, y, 0)) return y;
    }
    return -1;
}

__global__ void scoreCactusBridgeKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        std::uint64_t baseIndex,
        int minLift,
        BridgeResult* out) {
    const int seedIndex = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (seedIndex >= count) return;
    const std::size_t base = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;
    const int center = -coarsecore::FROM_COARSE;

    BridgeResult r{};
    r.seed = seeds[seedIndex];
    r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(seedIndex);
    r.overheadBottom = -1;

    const int spawnY = exactSpawnCheckYAtOrigin(density, base, center, center);
    r.spawnSurfaceY = spawnY;
    const bool desert = betaBiomeIsDesert(originTemperature[seedIndex], originRainfall[seedIndex]);
    r.desert = desert ? 1 : 0;

    // We deliberately target the cleanest possible bug geometry: the spawn
    // selector approves sand at Y63 because Y64 is air, while the player is
    // later created with feet at Y65. A population cactus can occupy Y64+ and
    // bridge the otherwise-safe gap into an overhead terrain mass.
    if (spawnY != 63 || !desert) {
        out[seedIndex] = r;
        return;
    }

    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool beachSand = originSandNoise[seedIndex] + sandJitter * 0.2 > 0.0;
    const int depth = static_cast<int>(originStoneNoise[seedIndex] / 3.0 + 3.0 + depthJitter * 0.25);
    const int reason = desert ? 2 : ((spawnY >= 60 && spawnY <= 65 && beachSand) ? 1 : 0);
    r.sandReason = reason;
    if (depth <= 0 || reason == 0) {
        out[seedIndex] = r;
        return;
    }

    const int preFeet = actualPlayerFeetY(density, base);
    r.preFeetY = preFeet;
    if (preFeet != 65) {
        out[seedIndex] = r;
        return;
    }

    // Cactus placement itself requires no solid horizontal neighbor at each
    // cactus block. Two blocks can bridge into terrain beginning at Y67; three
    // blocks can bridge one block farther, into terrain beginning at Y68.
    const bool clear2 = cactusHorizontalClearAt(density, base, 64)
                     && cactusHorizontalClearAt(density, base, 65);
    const bool clear3 = clear2 && cactusHorizontalClearAt(density, base, 66);

    const int feet2 = clear2 ? playerFeetWithSyntheticCactus(density, base, 2) : 65;
    const int feet3 = clear3 ? playerFeetWithSyntheticCactus(density, base, 3) : 65;
    r.feetH2 = feet2;
    r.feetH3 = feet3;
    r.liftH2 = feet2 - 65;
    r.liftH3 = feet3 - 65;
    r.overheadBottom = firstTerrainSolidAbove(density, base, 67);

    const int bestLift = r.liftH3 > r.liftH2 ? r.liftH3 : r.liftH2;
    if (bestLift >= minLift) {
        r.qualified = 1;
        r.requiredHeight = r.liftH2 >= minLift ? 2 : 3;
    }
    out[seedIndex] = r;
}

static std::uint64_t parseU64Bridge(const std::string& value, const char* name) {
    std::size_t used = 0;
    const std::uint64_t out = std::stoull(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}
static std::int64_t parseI64Bridge(const std::string& value, const char* name) {
    std::size_t used = 0;
    const std::int64_t out = std::stoll(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}
static int parseIntBridge(const std::string& value, const char* name) {
    std::size_t used = 0;
    const int out = std::stoi(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}

static BridgeConfig parseBridgeArgs(int argc, char** argv) {
    BridgeConfig c;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (++i >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[i];
        };
        if (arg == "--output") c.outputDir = value("--output");
        else if (arg == "--count") c.count = parseU64Bridge(value("--count"), "count");
        else if (arg == "--start-index") c.startIndex = parseU64Bridge(value("--start-index"), "start-index");
        else if (arg == "--batch") c.batch = parseIntBridge(value("--batch"), "batch");
        else if (arg == "--terrain-threads") c.terrainThreads = parseIntBridge(value("--terrain-threads"), "terrain-threads");
        else if (arg == "--min-lift") c.minLift = parseIntBridge(value("--min-lift"), "min-lift");
        else if (arg == "--random-key") { c.randomKey = parseU64Bridge(value("--random-key"), "random-key"); c.randomKeySet = true; }
        else if (arg == "--seed-mode") {
            const std::string mode = value("--seed-mode");
            if (mode == "unique48") c.seedMode = SeedMode::Unique48;
            else if (mode == "splitmix64") c.seedMode = SeedMode::SplitMix64;
            else throw std::invalid_argument("--seed-mode must be unique48 or splitmix64");
        }
        else if (arg == "--progress-ms") c.progressMs = parseIntBridge(value("--progress-ms"), "progress-ms");
        else if (arg == "--seed") { c.singleSeed = parseI64Bridge(value("--seed"), "seed"); c.singleSeedSet = true; c.count = 1; c.batch = 1; }
        else throw std::invalid_argument("unknown argument: " + arg);
    }
    if (c.outputDir.empty()) throw std::invalid_argument("--output is required");
    if (c.batch < 1 || c.batch > 32768) throw std::invalid_argument("--batch must be 1..32768");
    if (c.terrainThreads != 64 && c.terrainThreads != 128 && c.terrainThreads != 256) throw std::invalid_argument("--terrain-threads must be 64, 128, or 256");
    if (c.minLift < 1 || c.minLift > 62) throw std::invalid_argument("--min-lift must be 1..62");
    if (c.progressMs < 100 || c.progressMs > 60000) throw std::invalid_argument("--progress-ms must be 100..60000");
    if (c.seedMode == SeedMode::Unique48 && !c.singleSeedSet) {
        if (c.startIndex >= JAVA_SEED_PERIOD || c.count > JAVA_SEED_PERIOD - c.startIndex) throw std::invalid_argument("unique48 sequence range must stay within 0..2^48");
    }
    if (!c.randomKeySet) {
        std::random_device rd;
        const std::uint64_t now = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        c.randomKey = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd()) ^ now;
    }
    return c;
}

static void launchBridgeBatch(DeviceBuffers& b, BridgeResult* dResults, const BridgeConfig& c,
                              std::uint64_t baseIndex, int count, std::vector<BridgeResult>& host) {
    if (c.singleSeedSet) {
        checkHip(hipMemcpy(b.seeds, &c.singleSeed, sizeof(c.singleSeed), hipMemcpyHostToDevice), "copy single seed");
    } else {
        const int threads = 256;
        const int blocks = (count + threads - 1) / threads;
        hipLaunchKernelGGL(generateRandomSeedsKernel, dim3(blocks), dim3(threads), 0, 0,
            b.seeds, count, c.randomKey, baseIndex, static_cast<int>(c.seedMode));
        checkHip(hipGetLastError(), "generate cactus-elevator seeds");
    }
    launchTerrain(b, count, c.terrainThreads);
    const int threads = 128;
    const int blocks = (count + threads - 1) / threads;
    hipLaunchKernelGGL(scoreCactusBridgeKernel, dim3(blocks), dim3(threads), 0, 0,
        b.seeds, b.noise1, b.originSandNoise, b.originStoneNoise,
        b.originTemperature, b.originRainfall, count, baseIndex, c.minLift, dResults);
    checkHip(hipGetLastError(), "launch cactus bridge scoring");
    checkHip(hipDeviceSynchronize(), "finish cactus bridge batch");
    host.resize(static_cast<std::size_t>(count));
    checkHip(hipMemcpy(host.data(), dResults, static_cast<std::size_t>(count) * sizeof(BridgeResult), hipMemcpyDeviceToHost), "copy cactus bridge results");
}

static int runBridge(const BridgeConfig& c) {
    printDevice();
    std::filesystem::create_directories(c.outputDir);
    const std::filesystem::path csvPath = c.outputDir / ("candidates_" + std::to_string(c.startIndex) + ".csv");
    std::ofstream csv(csvPath, std::ios::trunc);
    if (!csv) throw std::runtime_error("cannot write " + csvPath.string());
    csv << "seed,sequence_index,spawn_surface_y,desert,sand_reason,pre_feet_y,predicted_feet_h2,predicted_feet_h3,predicted_lift_h2,predicted_lift_h3,predicted_required_cactus_height,overhead_bottom\n";

    DeviceBuffers b = allocateBuffers(c.batch);
    BridgeResult* dResults = nullptr;
    allocateArray(dResults, static_cast<std::size_t>(c.batch), "allocate cactus bridge results");
    std::vector<BridgeResult> host;

    std::uint64_t checked = 0;
    std::uint64_t hits = 0;
    int bestLift = -1;
    std::int64_t bestSeed = 0;
    const auto start = std::chrono::steady_clock::now();
    auto lastPrint = start;

    try {
        while (checked < c.count) {
            const int n = static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint64_t>(c.batch), c.count - checked));
            launchBridgeBatch(b, dResults, c, c.startIndex + checked, n, host);
            for (const auto& r : host) {
                if (!r.qualified) continue;
                ++hits;
                const int lift = r.liftH3 > r.liftH2 ? r.liftH3 : r.liftH2;
                if (lift > bestLift) {
                    bestLift = lift;
                    bestSeed = r.seed;
                    std::cout << "NEW GPU CACTUS-BRIDGE RECORD seed=" << r.seed
                              << " predictedLift=" << lift
                              << " h2=" << r.liftH2 << " h3=" << r.liftH3
                              << " overheadBottom=" << r.overheadBottom << "\n";
                }
                csv << r.seed << ',' << r.sequenceIndex << ',' << r.spawnSurfaceY << ',' << r.desert << ',' << r.sandReason << ','
                    << r.preFeetY << ',' << r.feetH2 << ',' << r.feetH3 << ',' << r.liftH2 << ',' << r.liftH3 << ','
                    << r.requiredHeight << ',' << r.overheadBottom << '\n';
            }
            checked += static_cast<std::uint64_t>(n);
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() >= c.progressMs || checked == c.count) {
                csv.flush();
                const double sec = std::chrono::duration<double>(now - start).count();
                std::cout << "progress checked=" << checked << '/' << c.count
                          << " rate=" << static_cast<std::uint64_t>(checked / std::max(0.001, sec))
                          << " seeds/s candidates=" << hits;
                if (bestLift >= 0) std::cout << " bestPredictedLift=" << bestLift << " seed=" << bestSeed;
                else std::cout << " bestPredictedLift=NONE";
                std::cout << "\n";
                lastPrint = now;
            }
            if (c.singleSeedSet) break;
        }
    } catch (...) {
        if (dResults) (void)hipFree(dResults);
        freeBuffers(b);
        throw;
    }

    if (dResults) checkHip(hipFree(dResults), "free cactus bridge results");
    freeBuffers(b);
    csv.flush();
    std::cout << "DONE checked=" << checked << " candidates=" << hits
              << " bestPredictedLift=" << (bestLift >= 0 ? std::to_string(bestLift) : std::string("NONE"))
              << " file=" << csvPath.string() << "\n";
    std::cout << "Target: desert spawn sand at Y63 + no initial collision + a hypothetical 2/3-high cactus can bridge the player into overhead terrain. Exact population is verified in Java.\n";
    return 0;
}

} // namespace cactus_elevator_p1

int main(int argc, char** argv) {
    try {
        return cactus_elevator_p1::runBridge(cactus_elevator_p1::parseBridgeArgs(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "CactusElevator P1 ERROR: " << e.what() << '\n';
        return 1;
    }
}
