#define main highest_pillar_spawn_p1_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace floating_island_spawn_p4_scout {
using namespace highest_pillar_spawn_p1;

struct ScoutHit {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    int spawnSurfaceY = -1;
    int firstUpperY = -1;
    int airGap = 0;
    int playerFeetY = -1;
    int supportY = -1;
    int sandReason = 0;
};

struct Config {
    std::filesystem::path candidateOut;
    std::uint64_t count = 1000000;
    std::uint64_t startIndex = 0;
    std::uint64_t randomKey = 0;
    SeedMode seedMode = SeedMode::Unique48;
    int batch = 16384;
    int terrainThreads = 32;
    int progressMs = 1000;
    int yieldMs = 0;
    bool selfTest = false;
};

static std::uint64_t parseU64(const std::string& value, const char* name) {
    std::size_t used = 0;
    const auto out = std::stoull(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}
static int parseInt(const std::string& value, const char* name) {
    std::size_t used = 0;
    const int out = std::stoi(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}

static Config parseArgsP4(int argc, char** argv) {
    Config c;
    bool randomKeySet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (++i >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[i];
        };
        if (arg == "--candidate-out") c.candidateOut = value("--candidate-out");
        else if (arg == "--count") c.count = parseU64(value("--count"), "count");
        else if (arg == "--start-index") c.startIndex = parseU64(value("--start-index"), "start-index");
        else if (arg == "--random-key") { c.randomKey = parseU64(value("--random-key"), "random-key"); randomKeySet = true; }
        else if (arg == "--seed-mode") {
            const auto mode = value("--seed-mode");
            if (mode == "unique48") c.seedMode = SeedMode::Unique48;
            else if (mode == "splitmix64") c.seedMode = SeedMode::SplitMix64;
            else throw std::invalid_argument("--seed-mode must be unique48 or splitmix64");
        }
        else if (arg == "--batch") c.batch = parseInt(value("--batch"), "batch");
        else if (arg == "--terrain-threads") c.terrainThreads = parseInt(value("--terrain-threads"), "terrain-threads");
        else if (arg == "--progress-ms") c.progressMs = parseInt(value("--progress-ms"), "progress-ms");
        else if (arg == "--yield-ms") c.yieldMs = parseInt(value("--yield-ms"), "yield-ms");
        else if (arg == "--self-test") c.selfTest = true;
        else throw std::invalid_argument("unknown argument: " + arg);
    }
    if (!c.selfTest && c.candidateOut.empty()) throw std::invalid_argument("--candidate-out is required");
    if (!c.selfTest && !randomKeySet) throw std::invalid_argument("--random-key is required");
    if (c.batch < 1 || c.batch > 1048576) throw std::invalid_argument("--batch must be 1..1048576");
    if (c.terrainThreads != 32 && c.terrainThreads != 64) throw std::invalid_argument("--terrain-threads must be 32 or 64");
    if (c.progressMs < 100 || c.progressMs > 60000) throw std::invalid_argument("--progress-ms must be 100..60000");
    if (c.yieldMs < 0 || c.yieldMs > 50) throw std::invalid_argument("--yield-ms must be 0..50");
    if (c.seedMode == SeedMode::Unique48 && c.startIndex + c.count > JAVA_SEED_PERIOD) {
        throw std::invalid_argument("unique48 sequence range exceeds 2^48");
    }
    return c;
}

__device__ __forceinline__ bool originSolid(const double* density, std::size_t base, int worldY) {
    return solidAtWorldY(density, base, 0, 0, worldY);
}

__device__ __forceinline__ int originPlayerFeetY(const double* density, std::size_t base) {
    int feetY = 65;
    while (feetY < 128 && (originSolid(density, base, feetY) || originSolid(density, base, feetY + 1))) ++feetY;
    return feetY;
}

__device__ __forceinline__ int originHighestSolidAtOrBelow(const double* density, std::size_t base, int startY) {
    int y = startY > 127 ? 127 : startY;
    for (; y >= 0; --y) if (originSolid(density, base, y)) return y;
    return -1;
}

__global__ void compactScoutHitsKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        std::uint64_t baseIndex,
        unsigned int* hitCount,
        ScoutHit* hits,
        unsigned int hitCapacity) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;

    const int spawnSurfaceY = exactSpawnCheckYAtOrigin(density, base, 0, 0);
    const bool desert = betaBiomeIsDesert(originTemperature[i], originRainfall[i]);

    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool beachSand = originSandNoise[i] + sandJitter * 0.2 > 0.0;
    const int surfaceDepth = static_cast<int>(originStoneNoise[i] / 3.0 + 3.0 + depthJitter * 0.25);
    const bool beachBand = spawnSurfaceY >= 60 && spawnSurfaceY <= 65;
    const int sandReason = desert ? 2 : ((beachBand && beachSand) ? 1 : 0);
    if (spawnSurfaceY < 63 || surfaceDepth <= 0 || sandReason == 0) return;

    const int feetY = originPlayerFeetY(density, base);
    if (feetY <= 65) return;

    const int supportY = originHighestSolidAtOrBelow(density, base, feetY - 1);
    if (supportY < 0 || supportY != feetY - 1) return;

    int firstUpperY = -1;
    for (int y = spawnSurfaceY + 1; y <= supportY; ++y) {
        if (originSolid(density, base, y)) { firstUpperY = y; break; }
    }
    if (firstUpperY < 0) return;
    const int airGap = firstUpperY - spawnSurfaceY - 1;

    // With a sand surface at Y>=63 and the initial player feet at Y=65,
    // only one- or two-block air gaps can still place upper terrain inside
    // the 1.8-block-tall initial player AABB. Keep the exact collision result
    // as the real proof and use this as a final cheap hard gate.
    if (airGap < 1 || airGap > 2) return;

    const unsigned int slot = atomicAdd(hitCount, 1u);
    if (slot >= hitCapacity) return;
    ScoutHit h;
    h.seed = seeds[i];
    h.sequenceIndex = baseIndex + static_cast<std::uint64_t>(i);
    h.spawnSurfaceY = spawnSurfaceY;
    h.firstUpperY = firstUpperY;
    h.airGap = airGap;
    h.playerFeetY = feetY;
    h.supportY = supportY;
    h.sandReason = sandReason;
    hits[slot] = h;
}

static void launchOneBatch(DeviceBuffers& b, const Config& c, std::uint64_t baseIndex, int count,
                           unsigned int* dHitCount, ScoutHit* dHits, std::vector<ScoutHit>& hostHits) {
    const int seedThreads = 256;
    const int seedBlocks = (count + seedThreads - 1) / seedThreads;
    hipLaunchKernelGGL(generateRandomSeedsKernel, dim3(seedBlocks), dim3(seedThreads), 0, 0,
        b.seeds, count, c.randomKey, baseIndex, static_cast<int>(c.seedMode));
    checkHip(hipGetLastError(), "generate P4 scout seeds");

    launchTerrain(b, count, c.terrainThreads);
    checkHip(hipMemset(dHitCount, 0, sizeof(unsigned int)), "reset P4 scout hit counter");

    const int gateThreads = 256;
    const int gateBlocks = (count + gateThreads - 1) / gateThreads;
    hipLaunchKernelGGL(compactScoutHitsKernel, dim3(gateBlocks), dim3(gateThreads), 0, 0,
        b.seeds, b.noise1,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        count, baseIndex, dHitCount, dHits, static_cast<unsigned int>(b.capacity));
    checkHip(hipGetLastError(), "launch P4 compact origin scout");
    checkHip(hipDeviceSynchronize(), "finish P4 compact origin scout");

    unsigned int hitCount = 0;
    checkHip(hipMemcpy(&hitCount, dHitCount, sizeof(hitCount), hipMemcpyDeviceToHost), "copy P4 hit count");
    if (hitCount > static_cast<unsigned int>(b.capacity)) throw std::runtime_error("P4 scout candidate buffer overflow");
    hostHits.resize(hitCount);
    if (hitCount) {
        checkHip(hipMemcpy(hostHits.data(), dHits, static_cast<std::size_t>(hitCount) * sizeof(ScoutHit), hipMemcpyDeviceToHost),
                 "copy P4 compact scout hits");
        std::sort(hostHits.begin(), hostHits.end(), [](const ScoutHit& a, const ScoutHit& b) {
            return a.sequenceIndex < b.sequenceIndex;
        });
    }
}

static void writeHeader(std::ofstream& f) {
    f << "seed,sequence_index,spawn_surface_y,first_upper_y,air_gap,player_feet_y,support_y,sand_reason\n";
}
static void writeHit(std::ofstream& f, const ScoutHit& h) {
    f << h.seed << ',' << h.sequenceIndex << ',' << h.spawnSurfaceY << ',' << h.firstUpperY << ','
      << h.airGap << ',' << h.playerFeetY << ',' << h.supportY << ',' << h.sandReason << '\n';
}

static ScoutHit runExplicitSeed(DeviceBuffers& b, std::int64_t seed, int terrainThreads,
                                unsigned int* dHitCount, ScoutHit* dHits) {
    checkHip(hipMemcpy(b.seeds, &seed, sizeof(seed), hipMemcpyHostToDevice), "copy P4 self-test seed");
    launchTerrain(b, 1, terrainThreads);
    checkHip(hipMemset(dHitCount, 0, sizeof(unsigned int)), "reset P4 self-test hit counter");
    hipLaunchKernelGGL(compactScoutHitsKernel, dim3(1), dim3(1), 0, 0,
        b.seeds, b.noise1,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        1, 0, dHitCount, dHits, 1u);
    checkHip(hipGetLastError(), "launch P4 self-test scout");
    checkHip(hipDeviceSynchronize(), "finish P4 self-test scout");
    unsigned int n = 0;
    checkHip(hipMemcpy(&n, dHitCount, sizeof(n), hipMemcpyDeviceToHost), "copy P4 self-test count");
    if (n != 1) throw std::runtime_error("known floating-pillar seed failed P4 origin-only scout");
    ScoutHit h;
    checkHip(hipMemcpy(&h, dHits, sizeof(h), hipMemcpyDeviceToHost), "copy P4 self-test hit");
    return h;
}

static int selfTest() {
    printDevice();
    DeviceBuffers b = allocateBuffers(1);
    unsigned int* dCount = nullptr;
    ScoutHit* dHits = nullptr;
    allocateArray(dCount, 1, "allocate P4 self-test counter");
    allocateArray(dHits, 1, "allocate P4 self-test hit");
    try {
        const auto h = runExplicitSeed(b, 6430576860599818994LL, 32, dCount, dHits);
        if (h.spawnSurfaceY != 63 || h.firstUpperY != 65 || h.airGap != 1 || h.playerFeetY != 74 || h.supportY != 73) {
            throw std::runtime_error("P4 origin-only scout metadata disagrees with verified P3 pillar seed");
        }
        std::cout << "P4 SCOUT SELFTEST OK seed=" << h.seed
                  << " spawnSurfaceY=" << h.spawnSurfaceY
                  << " firstUpperY=" << h.firstUpperY
                  << " airGap=" << h.airGap
                  << " playerFeetY=" << h.playerFeetY
                  << " supportY=" << h.supportY << '\n';
    } catch (...) {
        if (dHits) (void)hipFree(dHits);
        if (dCount) (void)hipFree(dCount);
        freeBuffers(b);
        throw;
    }
    checkHip(hipFree(dHits), "free P4 self-test hits");
    checkHip(hipFree(dCount), "free P4 self-test counter");
    freeBuffers(b);
    return 0;
}

static int run(const Config& c) {
    if (c.selfTest) return selfTest();
    printDevice();
    std::filesystem::create_directories(c.candidateOut.parent_path());
    std::ofstream out(c.candidateOut, std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write candidate file: " + c.candidateOut.string());
    writeHeader(out);

    DeviceBuffers b = allocateBuffers(c.batch);
    unsigned int* dHitCount = nullptr;
    ScoutHit* dHits = nullptr;
    allocateArray(dHitCount, 1, "allocate P4 scout hit counter");
    allocateArray(dHits, static_cast<std::size_t>(c.batch), "allocate P4 scout hit buffer");

    std::vector<ScoutHit> hits;
    std::uint64_t processed = 0;
    std::uint64_t totalHits = 0;
    const auto start = std::chrono::steady_clock::now();
    auto lastProgress = start;

    try {
        while (processed < c.count) {
            const int n = static_cast<int>(std::min<std::uint64_t>(c.count - processed, static_cast<std::uint64_t>(c.batch)));
            launchOneBatch(b, c, c.startIndex + processed, n, dHitCount, dHits, hits);
            for (const auto& h : hits) writeHit(out, h);
            totalHits += hits.size();
            processed += static_cast<std::uint64_t>(n);
            if (c.yieldMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(c.yieldMs));

            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgress).count() >= c.progressMs || processed == c.count) {
                const double seconds = std::chrono::duration<double>(now - start).count();
                const double rate = seconds > 0.0 ? static_cast<double>(processed) / seconds : 0.0;
                std::cout << "P4 scout progress checked=" << processed << '/' << c.count
                          << " rate=" << std::fixed << std::setprecision(1) << rate << " seeds/s"
                          << " candidates=" << totalHits << '\n';
                lastProgress = now;
            }
        }
    } catch (...) {
        if (dHits) (void)hipFree(dHits);
        if (dHitCount) (void)hipFree(dHitCount);
        freeBuffers(b);
        throw;
    }

    out.flush();
    checkHip(hipFree(dHits), "free P4 scout hits");
    checkHip(hipFree(dHitCount), "free P4 scout hit counter");
    freeBuffers(b);
    std::cout << "P4 scout candidates=" << totalHits << " file=" << c.candidateOut.string() << '\n';
    return 0;
}

} // namespace floating_island_spawn_p4_scout

int main(int argc, char** argv) {
    try {
        const auto c = floating_island_spawn_p4_scout::parseArgsP4(argc, argv);
        return floating_island_spawn_p4_scout::run(c);
    } catch (const std::exception& e) {
        std::cerr << "FloatingIslandSpawn P4 scout ERROR: " << e.what() << '\n';
        return 1;
    }
}
