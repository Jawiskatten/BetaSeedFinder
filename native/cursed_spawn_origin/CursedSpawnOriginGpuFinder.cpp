#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"
#include "terrain_perlin_cache.hpp"
#include "climate_perlin_cache.hpp"
#include "coarse_exact_gpu.hpp"
#include "skyblock_p14_config.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cursed_spawn_origin_p1 {

static constexpr int SCORE_THREADS = 128;
static constexpr std::uint64_t JAVA_SEED_PERIOD = 1ULL << 48;
static constexpr std::uint64_t JAVA_SEED_MASK = JAVA_SEED_PERIOD - 1ULL;
static constexpr int NO_SCORE = -0x3fffffff;

static_assert(coarsecore::Y_BASE == p14config::Y_BASE, "generated Y base mismatch");
static_assert(coarsecore::Y_LEVELS == p14config::STORED_Y_LEVELS, "generated Y-level mismatch");
static_assert(coarsecore::FULL_Y_LEVELS == 17, "Beta 1.7.3 terrain formula must keep 17 full Y levels");

enum class SeedMode : int { SplitMix64 = 0, Unique48 = 1 };

[[noreturn]] static void failHip(const char* operation, hipError_t error) {
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(error));
}
static void checkHip(hipError_t error, const char* operation) {
    if (error != hipSuccess) failHip(operation, error);
}
template <typename T>
static void allocateArray(T*& pointer, std::size_t count, const char* label) {
    checkHip(hipMalloc(reinterpret_cast<void**>(&pointer), count * sizeof(T)), label);
}

struct DeviceResult {
    std::int64_t seed;
    std::uint64_t sequenceIndex;
    int qualified;             // exact unpopulated surface at X=0,Z=0 is sand
    int score;
    int originY;
    int originBiome;           // 1=desert, 0=other (shore sand is still accepted)
    int sandReason;            // 2=desert top, 1=beach-noise replacement
    int safeColumnsR8;
    int safeColumnsR16;
    int waterColumnsR16;
    int immediateWater;
    int maxDropR8;
    int maxDropR16;
    int cliffEdgesR16;
    int overhangColumnsR16;
    int floatingColumnsR16;
    int floatingNodesR16;
    int roofNodes;
    int caveGapColumnsR16;
    int isolationScore;
    int cliffScore;
    int overhangScore;
    int waterPrisonScore;
    int highSpawnScore;
    int combinationCount;
};

struct DeviceBuffers {
    int capacity = 0;
    std::int64_t* seeds = nullptr;
    double* temp = nullptr;
    double* rain = nullptr;
    double* climateBlend = nullptr;
    double* noise1 = nullptr; // generated header writes final coarse density here
    double* noise2 = nullptr;
    double* noise3 = nullptr;
    double* noise4 = nullptr;
    double* noise5 = nullptr;
    double* originSandNoise = nullptr;
    double* originStoneNoise = nullptr;
    double* originTemperature = nullptr;
    double* originRainfall = nullptr;
    unsigned char* signs = nullptr;
    DeviceResult* results = nullptr;
};

static DeviceBuffers allocateBuffers(int capacity) {
    DeviceBuffers b;
    b.capacity = capacity;
    const std::size_t seeds = static_cast<std::size_t>(capacity);
    const std::size_t columns = seeds * coarsecore::COLUMNS;
    const std::size_t cells = seeds * coarsecore::CELLS;
    try {
        allocateArray(b.seeds, seeds, "allocate seeds");
        allocateArray(b.temp, columns, "allocate temperature");
        allocateArray(b.rain, columns, "allocate rainfall");
        allocateArray(b.climateBlend, columns, "allocate climate blend");
        allocateArray(b.noise1, cells, "allocate noise1/final density");
        allocateArray(b.noise2, cells, "allocate noise2");
        allocateArray(b.noise3, cells, "allocate noise3");
        allocateArray(b.noise4, columns, "allocate noise4");
        allocateArray(b.noise5, columns, "allocate noise5");
        allocateArray(b.originSandNoise, seeds, "allocate origin sand noise");
        allocateArray(b.originStoneNoise, seeds, "allocate origin stone noise");
        allocateArray(b.originTemperature, seeds, "allocate origin temperature");
        allocateArray(b.originRainfall, seeds, "allocate origin rainfall");
        allocateArray(b.signs, cells, "allocate coarse signs");
        allocateArray(b.results, seeds, "allocate crazy-spawn results");
    } catch (...) {
        if (b.seeds) (void)hipFree(b.seeds);
        if (b.temp) (void)hipFree(b.temp);
        if (b.rain) (void)hipFree(b.rain);
        if (b.climateBlend) (void)hipFree(b.climateBlend);
        if (b.noise1) (void)hipFree(b.noise1);
        if (b.noise2) (void)hipFree(b.noise2);
        if (b.noise3) (void)hipFree(b.noise3);
        if (b.noise4) (void)hipFree(b.noise4);
        if (b.noise5) (void)hipFree(b.noise5);
        if (b.originSandNoise) (void)hipFree(b.originSandNoise);
        if (b.originStoneNoise) (void)hipFree(b.originStoneNoise);
        if (b.originTemperature) (void)hipFree(b.originTemperature);
        if (b.originRainfall) (void)hipFree(b.originRainfall);
        if (b.signs) (void)hipFree(b.signs);
        if (b.results) (void)hipFree(b.results);
        throw;
    }
    return b;
}

static void freeBuffers(DeviceBuffers& b) {
    if (b.seeds) checkHip(hipFree(b.seeds), "free seeds");
    if (b.temp) checkHip(hipFree(b.temp), "free temperature");
    if (b.rain) checkHip(hipFree(b.rain), "free rainfall");
    if (b.climateBlend) checkHip(hipFree(b.climateBlend), "free climate blend");
    if (b.noise1) checkHip(hipFree(b.noise1), "free noise1");
    if (b.noise2) checkHip(hipFree(b.noise2), "free noise2");
    if (b.noise3) checkHip(hipFree(b.noise3), "free noise3");
    if (b.noise4) checkHip(hipFree(b.noise4), "free noise4");
    if (b.noise5) checkHip(hipFree(b.noise5), "free noise5");
    if (b.originSandNoise) checkHip(hipFree(b.originSandNoise), "free origin sand noise");
    if (b.originStoneNoise) checkHip(hipFree(b.originStoneNoise), "free origin stone noise");
    if (b.originTemperature) checkHip(hipFree(b.originTemperature), "free origin temperature");
    if (b.originRainfall) checkHip(hipFree(b.originRainfall), "free origin rainfall");
    if (b.signs) checkHip(hipFree(b.signs), "free signs");
    if (b.results) checkHip(hipFree(b.results), "free results");
    b = DeviceBuffers{};
}

__device__ __forceinline__ std::uint64_t splitMix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

__device__ __forceinline__ std::uint64_t permute48(std::uint64_t x) {
    x &= JAVA_SEED_MASK;
    x ^= x >> 24;
    x = (x * 0xD6E8FEB86659ULL) & JAVA_SEED_MASK;
    x ^= x >> 23;
    x = (x * 0xA5A3564E27F5ULL) & JAVA_SEED_MASK;
    x ^= x >> 24;
    return x & JAVA_SEED_MASK;
}

__global__ void generateRandomSeedsKernel(
        std::int64_t* seeds, int count, std::uint64_t randomKey,
        std::uint64_t baseIndex, int seedMode) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    const std::uint64_t sequence = baseIndex + static_cast<std::uint64_t>(i);
    if (seedMode == static_cast<int>(SeedMode::Unique48)) {
        const std::uint64_t low = permute48(sequence + (randomKey & JAVA_SEED_MASK));
        const std::uint64_t high = splitMix64(randomKey ^ sequence) & ~JAVA_SEED_MASK;
        seeds[i] = static_cast<std::int64_t>(high | low);
    } else {
        seeds[i] = static_cast<std::int64_t>(splitMix64(randomKey + sequence));
    }
}

struct Partial {
    int safeR8;
    int safeR16;
    int waterR16;
    int immediateWater;
    int maxDropR8;
    int maxDropR16;
    int cliffEdgesR16;
    int overhangR16;
    int floatingR16;
    int floatingNodesR16;
    int caveGapR16;
};

__device__ __forceinline__ bool nodeSolid(const unsigned char* signs, std::size_t base, int x, int y, int z) {
    if (x < 0 || z < 0 || x >= coarsecore::SIZE || z >= coarsecore::SIZE) return false;
    if (y < 0) return true;
    if (y >= coarsecore::Y_LEVELS) return false;
    return signs[base + static_cast<std::size_t>(coarsecore::index3(x, y, z))] != 0;
}

__device__ __forceinline__ double nodeDensity(const double* density, std::size_t base, int x, int y, int z) {
    if (y >= coarsecore::Y_LEVELS) return -10.0; // P14/P17 full mode's exact implicit top
    return density[base + static_cast<std::size_t>(coarsecore::index3(x, y, z))];
}

// Exact block-level vertical interpolation at a 4-block lattice corner.
// X=0,Z=0 is always one of these corners, so the origin height is not rounded.
__device__ __forceinline__ bool solidAtWorldY(
        const double* density, std::size_t base, int x, int z, int worldY) {
    if (worldY < coarsecore::Y_BASE * 8) return true;
    if (worldY >= 128) return false;
    const int coarseWorldY = worldY >> 3;
    const int localY = coarseWorldY - coarsecore::Y_BASE;
    if (localY < 0) return true;
    if (localY >= coarsecore::Y_LEVELS) return false;
    const int inCell = worldY & 7;
    const double d0 = nodeDensity(density, base, x, localY, z);
    const double d1 = nodeDensity(density, base, x, localY + 1, z);
    const double d = d0 + (d1 - d0) * (static_cast<double>(inCell) * 0.125);
    return d > 0.0;
}

__device__ __forceinline__ int exactSurfaceYAtNode(
        const double* density, std::size_t base, int x, int z) {
    for (int worldY = 127; worldY >= coarsecore::Y_BASE * 8; --worldY) {
        if (solidAtWorldY(density, base, x, z, worldY)) return worldY;
    }
    return coarsecore::Y_BASE * 8 - 1;
}

// Beta's overworld spawn test starts at Y=63 and walks upward only while the
// next block is non-air. It intentionally does not jump an air gap to a higher
// floating mass.
__device__ __forceinline__ int exactSpawnCheckYAtOrigin(
        const double* density, std::size_t base, int x, int z) {
    int y = 63;
    while (y + 1 < 128 && solidAtWorldY(density, base, x, z, y + 1)) ++y;
    return solidAtWorldY(density, base, x, z, y) ? y : -1;
}

// Exact Beta 1.7.3 64x64 biome lookup for the only native sand-top biome.
__device__ __forceinline__ bool betaBiomeIsDesert(double temperature, double rainfall) {
    int ti = static_cast<int>(temperature * 63.0);
    int ri = static_cast<int>(rainfall * 63.0);
    if (ti < 0) ti = 0; else if (ti > 63) ti = 63;
    if (ri < 0) ri = 0; else if (ri > 63) ri = 63;
    const float f = static_cast<float>(ti) / 63.0f;
    float wet = static_cast<float>(ri) / 63.0f;
    wet *= f;
    return wet < 0.2f && f >= 0.95f;
}

__device__ __forceinline__ Partial emptyPartial() {
    return Partial{};
}

__device__ __forceinline__ void mergePartial(Partial& a, const Partial& b) {
    a.safeR8 += b.safeR8;
    a.safeR16 += b.safeR16;
    a.waterR16 += b.waterR16;
    a.immediateWater += b.immediateWater;
    if (b.maxDropR8 > a.maxDropR8) a.maxDropR8 = b.maxDropR8;
    if (b.maxDropR16 > a.maxDropR16) a.maxDropR16 = b.maxDropR16;
    a.cliffEdgesR16 += b.cliffEdgesR16;
    a.overhangR16 += b.overhangR16;
    a.floatingR16 += b.floatingR16;
    a.floatingNodesR16 += b.floatingNodesR16;
    a.caveGapR16 += b.caveGapR16;
}

__global__ void scoreCursedSpawnOriginKernel(
        const std::int64_t* seeds,
        const double* density,
        const unsigned char* signs,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        std::uint64_t baseIndex,
        DeviceResult* out) {
    const int seedIndex = static_cast<int>(blockIdx.x);
    if (seedIndex >= count) return;
    const int tid = static_cast<int>(threadIdx.x);
    const std::size_t base = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;

    Partial p = emptyPartial();
    __shared__ short surfaceMap[coarsecore::COLUMNS];
    __shared__ int originYShared;
    __shared__ int originBiomeShared;
    __shared__ int sandReasonShared;
    __shared__ int qualifiedShared;
    __shared__ int roofNodesShared;
    for (int column = tid; column < coarsecore::COLUMNS; column += blockDim.x) {
        const int x = column / coarsecore::SIZE;
        const int z = column - x * coarsecore::SIZE;
        surfaceMap[column] = static_cast<short>(exactSurfaceYAtNode(density, base, x, z));
    }
    __syncthreads();

    const int center = -coarsecore::FROM_COARSE;
    if (tid == 0) {
        const int originY = exactSpawnCheckYAtOrigin(density, base, center, center);
        const bool desert = betaBiomeIsDesert(
            originTemperature[seedIndex], originRainfall[seedIndex]);

        // Chunk (0,0) is seeded with zero. X=0,Z=0 is the first surface column,
        // so these are exactly the three random draws used by Beta's replacement pass.
        p20::JavaRandom surfaceRandom;
        surfaceRandom.setSeed(0);
        const double sandJitter = surfaceRandom.nextDouble();
        (void)surfaceRandom.nextDouble(); // gravel jitter
        const double depthJitter = surfaceRandom.nextDouble();
        const bool beachSand = originSandNoise[seedIndex] + sandJitter * 0.2 > 0.0;
        const int depth = static_cast<int>(originStoneNoise[seedIndex] / 3.0 + 3.0 + depthJitter * 0.25);
        const bool beachBand = originY >= 60 && originY <= 65;
        const int reason = desert ? 2 : ((beachBand && beachSand) ? 1 : 0);

        originYShared = originY;
        originBiomeShared = desert ? 1 : 0;
        sandReasonShared = reason;
        qualifiedShared = (originY >= 63 && depth > 0 && reason != 0) ? 1 : 0;

        int roofNodes = 0;
        bool sawAir = false;
        for (int y = 64; y < 128; ++y) {
            const bool solid = solidAtWorldY(density, base, center, center, y);
            if (!solid) sawAir = true;
            else if (sawAir) ++roofNodes;
        }
        roofNodesShared = roofNodes;
    }
    __syncthreads();

    // Score only the first 16 blocks around the exact origin. On this P1 scout the
    // horizontal scene is sampled on Beta's native 4-block density lattice.
    static constexpr int GRID_RADIUS = 4;
    static constexpr int GRID_WIDTH = GRID_RADIUS * 2 + 1;
    static constexpr int TOTAL_LOCAL_COLUMNS = GRID_WIDTH * GRID_WIDTH;
    for (int linear = tid; linear < TOTAL_LOCAL_COLUMNS; linear += blockDim.x) {
        const int gx = linear / GRID_WIDTH - GRID_RADIUS;
        const int gz = linear % GRID_WIDTH - GRID_RADIUS;
        const int x = center + gx;
        const int z = center + gz;
        if (x < 1 || z < 1 || x >= coarsecore::SIZE - 1 || z >= coarsecore::SIZE - 1) continue;

        const int surface = static_cast<int>(surfaceMap[x * coarsecore::SIZE + z]);
        int delta = surface - originYShared;
        if (delta < 0) delta = -delta;
        const int cheb = (gx < 0 ? -gx : gx) > (gz < 0 ? -gz : gz)
                ? (gx < 0 ? -gx : gx) : (gz < 0 ? -gz : gz);
        const bool safe = surface >= 63 && delta <= 2;
        if (safe) {
            ++p.safeR16;
            if (cheb <= 2) ++p.safeR8;
        }
        const bool water = surface < 63;
        if (water) {
            ++p.waterR16;
            if (cheb <= 1 && cheb > 0) ++p.immediateWater;
        }
        int drop = originYShared - surface;
        if (drop < 0) drop = 0;
        if (drop > p.maxDropR16) p.maxDropR16 = drop;
        if (cheb <= 2 && drop > p.maxDropR8) p.maxDropR8 = drop;

        const int sx = static_cast<int>(surfaceMap[(x + 1) * coarsecore::SIZE + z]);
        const int sz = static_cast<int>(surfaceMap[x * coarsecore::SIZE + (z + 1)]);
        int edgeX = surface - sx; if (edgeX < 0) edgeX = -edgeX;
        int edgeZ = surface - sz; if (edgeZ < 0) edgeZ = -edgeZ;
        if (edgeX >= 8) ++p.cliffEdgesR16;
        if (edgeZ >= 8) ++p.cliffEdgesR16;

        bool previous = nodeSolid(signs, base, x, 0, z);
        int runs = previous ? 1 : 0;
        int detachedNodes = 0;
        bool detached = false;
        bool gapBelowTop = false;
        for (int y = 1; y < coarsecore::Y_LEVELS; ++y) {
            const bool current = nodeSolid(signs, base, x, y, z);
            if (current && !previous) {
                ++runs;
                detached = true;
                gapBelowTop = true;
            } else if (!current) {
                detached = false;
            }
            if (current && detached) ++detachedNodes;
            previous = current;
        }
        if (runs >= 2) ++p.overhangR16;
        if (!nodeSolid(signs, base, x, 0, z) && runs > 0) ++p.floatingR16;
        p.floatingNodesR16 += detachedNodes;
        if (gapBelowTop) ++p.caveGapR16;
    }

    __shared__ Partial shared[SCORE_THREADS];
    shared[tid] = p;
    __syncthreads();
    for (int stride = SCORE_THREADS / 2; stride > 0; stride >>= 1) {
        if (tid < stride) mergePartial(shared[tid], shared[tid + stride]);
        __syncthreads();
    }

    if (tid == 0) {
        const Partial q = shared[0];
        DeviceResult r{};
        r.seed = seeds[seedIndex];
        r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(seedIndex);
        r.qualified = qualifiedShared;
        r.originY = originYShared;
        r.originBiome = originBiomeShared;
        r.sandReason = sandReasonShared;
        r.safeColumnsR8 = q.safeR8;
        r.safeColumnsR16 = q.safeR16;
        r.waterColumnsR16 = q.waterR16;
        r.immediateWater = q.immediateWater;
        r.maxDropR8 = q.maxDropR8;
        r.maxDropR16 = q.maxDropR16;
        r.cliffEdgesR16 = q.cliffEdgesR16;
        r.overhangColumnsR16 = q.overhangR16;
        r.floatingColumnsR16 = q.floatingR16;
        r.floatingNodesR16 = q.floatingNodesR16;
        r.roofNodes = roofNodesShared;
        r.caveGapColumnsR16 = q.caveGapR16;
        r.isolationScore = (81 - q.safeR16) * 100 + (25 - q.safeR8) * 80;
        r.cliffScore = q.maxDropR8 * 220 + q.maxDropR16 * 90 + q.cliffEdgesR16 * 35;
        r.overhangScore = q.overhangR16 * 260 + q.floatingR16 * 220
                + q.floatingNodesR16 * 45 + roofNodesShared * 320 + q.caveGapR16 * 70;
        r.waterPrisonScore = q.waterR16 * 120 + q.immediateWater * 550;
        r.highSpawnScore = originYShared > 63 ? (originYShared - 63) * 180 : 0;
        int combo = 0;
        if (q.safeR8 <= 9) ++combo;
        if (q.waterR16 >= 18 || q.immediateWater >= 3) ++combo;
        if (q.maxDropR8 >= 12) ++combo;
        if (q.overhangR16 >= 2 || roofNodesShared > 0) ++combo;
        if (originYShared >= 85) ++combo;
        r.combinationCount = combo;
        long long total = static_cast<long long>(r.isolationScore) * 4
                + static_cast<long long>(r.waterPrisonScore) * 5
                + static_cast<long long>(r.cliffScore) * 3
                + static_cast<long long>(r.overhangScore) * 2
                + r.highSpawnScore;
        total = total * (100 + combo * 18) / 100;
        if (!qualifiedShared) total = NO_SCORE;
        if (total > 2000000000LL) total = 2000000000LL;
        r.score = static_cast<int>(total);
        out[seedIndex] = r;
    }
}

struct Config {
    std::filesystem::path outputDir;
    std::uint64_t count = 10'000'000ULL;
    std::uint64_t startIndex = 0;
    int batch = 4096;
    int terrainThreads = 64;
    int top = 100;
    std::uint64_t randomKey = 0;
    bool randomKeySet = false;
    SeedMode seedMode = SeedMode::Unique48;
    int progressMs = 1000;
    int checkpointMs = 5000;
    bool selfTest = false;
    bool singleSeedSet = false;
    std::int64_t singleSeed = 0;
    bool resumeExisting = false;
};

static std::uint64_t parseU64(const std::string& value, const char* name) {
    std::size_t used = 0;
    const std::uint64_t out = std::stoull(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}
static std::int64_t parseI64(const std::string& value, const char* name) {
    std::size_t used = 0;
    const std::int64_t out = std::stoll(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}
static int parseInt(const std::string& value, const char* name) {
    std::size_t used = 0;
    const int out = std::stoi(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}

static Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (++i >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[i];
        };
        if (arg == "--output") c.outputDir = value("--output");
        else if (arg == "--count") c.count = parseU64(value("--count"), "count");
        else if (arg == "--start-index") c.startIndex = parseU64(value("--start-index"), "start-index");
        else if (arg == "--batch") c.batch = parseInt(value("--batch"), "batch");
        else if (arg == "--terrain-threads") c.terrainThreads = parseInt(value("--terrain-threads"), "terrain-threads");
        else if (arg == "--top") c.top = parseInt(value("--top"), "top");
        else if (arg == "--random-key") { c.randomKey = parseU64(value("--random-key"), "random-key"); c.randomKeySet = true; }
        else if (arg == "--seed-mode") {
            const std::string mode = value("--seed-mode");
            if (mode == "unique48") c.seedMode = SeedMode::Unique48;
            else if (mode == "splitmix64") c.seedMode = SeedMode::SplitMix64;
            else throw std::invalid_argument("--seed-mode must be unique48 or splitmix64");
        }
        else if (arg == "--progress-ms") c.progressMs = parseInt(value("--progress-ms"), "progress-ms");
        else if (arg == "--checkpoint-ms") c.checkpointMs = parseInt(value("--checkpoint-ms"), "checkpoint-ms");
        else if (arg == "--seed") { c.singleSeed = parseI64(value("--seed"), "seed"); c.singleSeedSet = true; c.count = 1; c.batch = 1; }
        else if (arg == "--resume-existing") c.resumeExisting = true;
        else if (arg == "--self-test") c.selfTest = true;
        else throw std::invalid_argument("unknown argument: " + arg);
    }
    if (!c.selfTest && c.outputDir.empty()) throw std::invalid_argument("--output is required");
    if (c.batch < 1 || c.batch > 32768) throw std::invalid_argument("--batch must be 1..32768");
    if (c.terrainThreads != 64 && c.terrainThreads != 128 && c.terrainThreads != 256) {
        throw std::invalid_argument("--terrain-threads must be 64, 128, or 256");
    }
    if (c.top < 1 || c.top > 10000) throw std::invalid_argument("--top must be 1..10000");
    if (c.progressMs < 100 || c.progressMs > 60000) throw std::invalid_argument("--progress-ms must be 100..60000");
    if (c.checkpointMs < 1000 || c.checkpointMs > 600000) throw std::invalid_argument("--checkpoint-ms must be 1000..600000");
    if (c.seedMode == SeedMode::Unique48 && !c.singleSeedSet) {
        if (c.startIndex >= JAVA_SEED_PERIOD || c.count > JAVA_SEED_PERIOD - c.startIndex) {
            throw std::invalid_argument("unique48 sequence range must stay within 0..2^48");
        }
    }
    if (!c.randomKeySet) {
        std::random_device rd;
        const std::uint64_t now = static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        c.randomKey = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd()) ^ now;
    }
    return c;
}

static void printDevice() {
    int device = 0;
    checkHip(hipGetDevice(&device), "get GPU device");
    hipDeviceProp_t prop{};
    checkHip(hipGetDeviceProperties(&prop, device), "get GPU properties");
    std::cout << "GPU: " << prop.name;
#if !defined(BSF_NVIDIA_CUDA) && !defined(__CUDACC__)
    std::cout << " | architecture=" << prop.gcnArchName;
#endif
    std::cout << "\n";
}

static void launchTerrain(DeviceBuffers& b, int count, int terrainThreads) {
#if defined(SKYBLOCK_COARSE_API_MODERN)
    hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel,
        dim3(count), dim3(terrainThreads), 0, 0,
        b.seeds, count,
        b.temp, b.rain, b.climateBlend,
        b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
        b.signs,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        0, 0,
        nullptr, nullptr, nullptr, nullptr, nullptr);
#elif defined(SKYBLOCK_COARSE_API_LEGACY)
#error "CursedSpawnOrigin P1 requires the current modern coarse GPU API"
#else
#error "Define SKYBLOCK_COARSE_API_MODERN or SKYBLOCK_COARSE_API_LEGACY"
#endif
    checkHip(hipGetLastError(), "launch exact Beta terrain generation");
}

static void launchBatch(DeviceBuffers& b, const Config& c, std::uint64_t baseIndex, int count,
                        std::vector<DeviceResult>& host) {
    if (c.singleSeedSet) {
        checkHip(hipMemcpy(b.seeds, &c.singleSeed, sizeof(c.singleSeed), hipMemcpyHostToDevice), "copy single seed");
    } else {
        const int threads = 256;
        const int blocks = (count + threads - 1) / threads;
        hipLaunchKernelGGL(generateRandomSeedsKernel, dim3(blocks), dim3(threads), 0, 0,
            b.seeds, count, c.randomKey, baseIndex, static_cast<int>(c.seedMode));
        checkHip(hipGetLastError(), "generate random seeds");
    }
    launchTerrain(b, count, c.terrainThreads);
    hipLaunchKernelGGL(scoreCursedSpawnOriginKernel, dim3(count), dim3(SCORE_THREADS), 0, 0,
        b.seeds, b.noise1, b.signs,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        count, baseIndex, b.results);
    checkHip(hipGetLastError(), "launch cursed origin spawn scoring");
    checkHip(hipDeviceSynchronize(), "finish cursed origin spawn batch");
    host.resize(static_cast<std::size_t>(count));
    checkHip(hipMemcpy(host.data(), b.results, static_cast<std::size_t>(count) * sizeof(DeviceResult), hipMemcpyDeviceToHost),
             "copy cursed origin spawn results");
}

using MetricFn = int (*)(const DeviceResult&);
static int metricOverall(const DeviceResult& r) { return r.score; }
static int metricIsolation(const DeviceResult& r) { return r.isolationScore; }
static int metricTinySafeArea(const DeviceResult& r) {
    return (81 - r.safeColumnsR16) * 10000 + (25 - r.safeColumnsR8) * 100 + r.waterColumnsR16;
}
static int metricWaterPrison(const DeviceResult& r) { return r.waterPrisonScore; }
static int metricCliff(const DeviceResult& r) { return r.cliffScore; }
static int metricOverhang(const DeviceResult& r) { return r.overhangScore; }
static int metricHighest(const DeviceResult& r) { return r.originY; }
static int metricCombo(const DeviceResult& r) { return r.combinationCount * 1000000 + r.score / 1000; }

struct Board {
    std::string name;
    MetricFn metric;
    std::vector<DeviceResult> rows;
};

static void insertTop(Board& b, const DeviceResult& r, int limit) {
    if (!r.qualified) return;
    const int value = b.metric(r);
    auto it = std::lower_bound(b.rows.begin(), b.rows.end(), value,
        [&](const DeviceResult& a, int v) { return b.metric(a) > v; });
    if (static_cast<int>(b.rows.size()) < limit || it != b.rows.end()) {
        b.rows.insert(it, r);
        if (static_cast<int>(b.rows.size()) > limit) b.rows.pop_back();
    }
}

static std::vector<DeviceResult> readCsv(const std::filesystem::path& path) {
    std::vector<DeviceResult> rows;
    std::ifstream f(path);
    if (!f) return rows;
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) fields.push_back(field);
        if (fields.size() != 26) continue;
        try {
            DeviceResult r{};
            r.seed = std::stoll(fields[1]);
            r.sequenceIndex = std::stoull(fields[2]);
            r.qualified = std::stoi(fields[3]);
            r.score = std::stoi(fields[4]);
            r.originY = std::stoi(fields[5]);
            r.originBiome = std::stoi(fields[6]);
            r.sandReason = std::stoi(fields[7]);
            r.safeColumnsR8 = std::stoi(fields[8]);
            r.safeColumnsR16 = std::stoi(fields[9]);
            r.waterColumnsR16 = std::stoi(fields[10]);
            r.immediateWater = std::stoi(fields[11]);
            r.maxDropR8 = std::stoi(fields[12]);
            r.maxDropR16 = std::stoi(fields[13]);
            r.cliffEdgesR16 = std::stoi(fields[14]);
            r.overhangColumnsR16 = std::stoi(fields[15]);
            r.floatingColumnsR16 = std::stoi(fields[16]);
            r.floatingNodesR16 = std::stoi(fields[17]);
            r.roofNodes = std::stoi(fields[18]);
            r.caveGapColumnsR16 = std::stoi(fields[19]);
            r.isolationScore = std::stoi(fields[20]);
            r.cliffScore = std::stoi(fields[21]);
            r.overhangScore = std::stoi(fields[22]);
            r.waterPrisonScore = std::stoi(fields[23]);
            r.highSpawnScore = std::stoi(fields[24]);
            r.combinationCount = std::stoi(fields[25]);
            rows.push_back(r);
        } catch (...) {
            // Ignore a partially-written or malformed trailing row from an interrupted run.
        }
    }
    return rows;
}

static void writeCsv(const std::filesystem::path& path, const std::vector<DeviceResult>& rows) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f << "rank,seed,sequence_index,qualified,score,origin_y,origin_biome_desert,sand_reason,safe_columns_r8,safe_columns_r16,water_columns_r16,immediate_water,max_drop_r8,max_drop_r16,cliff_edges_r16,overhang_columns_r16,floating_columns_r16,floating_nodes_r16,roof_nodes,cave_gap_columns_r16,isolation_score,cliff_score,overhang_score,water_prison_score,high_spawn_score,combination_count\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        f << (i + 1) << ',' << r.seed << ',' << r.sequenceIndex << ',' << r.qualified << ',' << r.score << ','
          << r.originY << ',' << r.originBiome << ',' << r.sandReason << ',' << r.safeColumnsR8 << ','
          << r.safeColumnsR16 << ',' << r.waterColumnsR16 << ',' << r.immediateWater << ',' << r.maxDropR8 << ','
          << r.maxDropR16 << ',' << r.cliffEdgesR16 << ',' << r.overhangColumnsR16 << ',' << r.floatingColumnsR16 << ','
          << r.floatingNodesR16 << ',' << r.roofNodes << ',' << r.caveGapColumnsR16 << ',' << r.isolationScore << ','
          << r.cliffScore << ',' << r.overhangScore << ',' << r.waterPrisonScore << ',' << r.highSpawnScore << ','
          << r.combinationCount << '\n';
    }
}

static void saveState(const Config& c, std::uint64_t completed, const std::vector<Board>& boards) {
    std::filesystem::create_directories(c.outputDir);
    for (const auto& b : boards) writeCsv(c.outputDir / ("top_" + b.name + ".csv"), b.rows);
    std::ofstream ck(c.outputDir / "checkpoint.txt", std::ios::trunc);
    ck << "VERSION=CursedSpawnOriginP1\n"
       << "SPAWN_X=0\nSPAWN_Z=0\n"
       << "RADIUS=" << p14config::CHUNK_RADIUS << "\n"
       << "START_INDEX=" << c.startIndex << "\n"
       << "COMPLETED=" << completed << "\n"
       << "NEXT_INDEX=" << (c.startIndex + completed) << "\n"
       << "COUNT=" << c.count << "\n"
       << "RANDOM_KEY=" << c.randomKey << "\n"
       << "SEED_MODE=" << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64") << "\n";
    if (!boards.empty() && !boards[0].rows.empty()) {
        const auto& r = boards[0].rows.front();
        ck << "BEST_SEED=" << r.seed << "\nBEST_SCORE=" << r.score << "\n"
           << "BEST_ORIGIN_Y=" << r.originY << "\nBEST_SAFE_R8=" << r.safeColumnsR8
           << "\nBEST_SAFE_R16=" << r.safeColumnsR16 << "\nBEST_WATER_R16=" << r.waterColumnsR16
           << "\nBEST_MAX_DROP_R8=" << r.maxDropR8 << "\n";
    }
}

static const char* sandReasonName(int code) {
    if (code == 2) return "DESERT_TOP";
    if (code == 1) return "BEACH_NOISE";
    return "NONE";
}

static void printResult(const char* prefix, const DeviceResult& r) {
    std::cout << prefix << " seed=" << r.seed << " score=" << r.score
              << " spawn=(0," << r.originY << ",0)"
              << " sand=" << sandReasonName(r.sandReason)
              << " safeR8=" << r.safeColumnsR8 << "/25"
              << " safeR16=" << r.safeColumnsR16 << "/81"
              << " waterR16=" << r.waterColumnsR16 << "/81"
              << " immediateWater=" << r.immediateWater << "/8"
              << " dropR8=" << r.maxDropR8 << " dropR16=" << r.maxDropR16
              << " overhangR16=" << r.overhangColumnsR16
              << " floatingR16=" << r.floatingColumnsR16
              << " roofNodes=" << r.roofNodes
              << " combo=" << r.combinationCount << "\n";
}

static int runSelfTest(const Config& original) {
    Config c = original;
    c.randomKey = 0x1234FEDCBA987654ULL;
    c.randomKeySet = true;
    c.seedMode = SeedMode::Unique48;
    c.singleSeedSet = false;
    c.batch = 4;
    DeviceBuffers b = allocateBuffers(4);
    std::vector<DeviceResult> a, d;
    launchBatch(b, c, 0, 4, a);
    launchBatch(b, c, 0, 4, d);
    freeBuffers(b);
    if (a.size() != d.size() || std::memcmp(a.data(), d.data(), a.size() * sizeof(DeviceResult)) != 0) {
        std::cerr << "CursedSpawnOrigin P1 self-test FAILED: repeated fixed-key batch changed.\n";
        return 2;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].sequenceIndex != i) {
            std::cerr << "CursedSpawnOrigin P1 self-test FAILED: sequence index mismatch.\n";
            return 3;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if ((static_cast<std::uint64_t>(a[i].seed) & JAVA_SEED_MASK) ==
                (static_cast<std::uint64_t>(a[j].seed) & JAVA_SEED_MASK)) {
                std::cerr << "CursedSpawnOrigin P1 self-test FAILED: unique48 collision.\n";
                return 4;
            }
        }
    }
    for (const auto& r : a) {
        if (r.safeColumnsR8 < 0 || r.safeColumnsR8 > 25 ||
            r.safeColumnsR16 < 0 || r.safeColumnsR16 > 81 ||
            r.waterColumnsR16 < 0 || r.waterColumnsR16 > 81 ||
            r.immediateWater < 0 || r.immediateWater > 8) {
            std::cerr << "CursedSpawnOrigin P1 self-test FAILED: invalid fixed-origin metrics.\n";
            return 6;
        }
        if (r.qualified && (r.originY < 63 || r.sandReason == 0 || r.score == NO_SCORE)) return 7;
    }
    std::cout << "CursedSpawnOrigin P1 deterministic GPU self-test PASS\n";
    printResult("sample", a.front());
    return 0;
}

static int run(const Config& c) {
    printDevice();
    std::cout << "CursedSpawnOrigin P1 | Beta 1.7.3 exact sand gate at X=0,Z=0\n"
              << "Compile radius: " << p14config::CHUNK_RADIUS << " chunks"
              << " | lattice=" << coarsecore::SIZE << 'x' << coarsecore::SIZE
              << " | storedY=" << coarsecore::Y_LEVELS << " | mode=" << p14config::MODE_NAME << "\n"
              << "Hard gate: final unpopulated surface block at (0,0) is sand and spawn-valid at Y>=63\n"
              << "Score scene: fixed 0,0 only; tiny safe area + water prison + cliff + overhead/floating terrain\n"
              << "Seed mode: " << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64")
              << " | randomKey=" << c.randomKey << "\n";

    if (c.selfTest) return runSelfTest(c);
    std::filesystem::create_directories(c.outputDir);
    {
        std::ofstream info(c.outputDir / "run_config.txt", std::ios::trunc);
        info << "CursedSpawnOriginP1\nSPAWN_X=0\nSPAWN_Z=0\nRADIUS=" << p14config::CHUNK_RADIUS << "\nMODE=" << p14config::MODE_NAME
             << "\nCOUNT=" << c.count << "\nSTART_INDEX=" << c.startIndex << "\nBATCH=" << c.batch
             << "\nTERRAIN_THREADS=" << c.terrainThreads << "\nTOP=" << c.top << "\nRANDOM_KEY=" << c.randomKey
             << "\nSEED_MODE=" << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64") << "\n";
        if (c.singleSeedSet) info << "SINGLE_SEED=" << c.singleSeed << "\n";
    }

    DeviceBuffers b = allocateBuffers(c.batch);
    std::vector<DeviceResult> host;
    std::vector<Board> boards = {
        {"overall", metricOverall, {}}, {"tiny_safe_area", metricTinySafeArea, {}},
        {"isolation", metricIsolation, {}}, {"water_prison", metricWaterPrison, {}},
        {"cliff", metricCliff, {}}, {"overhang", metricOverhang, {}},
        {"highest", metricHighest, {}}, {"combo", metricCombo, {}}
    };
    if (c.resumeExisting) {
        for (auto& board : boards) {
            auto oldRows = readCsv(c.outputDir / ("top_" + board.name + ".csv"));
            for (const auto& r : oldRows) insertTop(board, r, c.top);
        }
        if (!boards[0].rows.empty()) {
            std::cout << "Loaded " << boards[0].rows.size() << " prior overall leaders from the existing run.\n";
        }
    }

    const auto start = std::chrono::steady_clock::now();
    auto lastProgress = start;
    auto lastCheckpoint = start;
    std::uint64_t completed = 0;
    std::uint64_t qualified = 0;
    int previousBest = (!boards[0].rows.empty() ? boards[0].rows.front().score : NO_SCORE);

    try {
        while (completed < c.count) {
            const int n = static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint64_t>(c.batch), c.count - completed));
            launchBatch(b, c, c.startIndex + completed, n, host);
            for (const auto& r : host) {
                if (r.qualified) ++qualified;
                for (auto& board : boards) insertTop(board, r, c.top);
                if (r.qualified && r.score > previousBest) {
                    previousBest = r.score;
                    printResult("NEW CURSED ORIGIN BEST", r);
                }
            }
            completed += static_cast<std::uint64_t>(n);
            const auto now = std::chrono::steady_clock::now();
            const auto progressElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgress).count();
            if (progressElapsed >= c.progressMs || completed == c.count) {
                const double seconds = std::chrono::duration<double>(now - start).count();
                const double rate = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
                std::cout << "progress checked=" << completed << '/' << c.count << " rate=" << std::fixed << std::setprecision(1)
                          << rate << " seeds/s";
                std::cout << " qualified0_0=" << qualified;
                if (!boards[0].rows.empty()) {
                    const auto& best = boards[0].rows.front();
                    std::cout << " best=" << best.score << " seed=" << best.seed
                              << " spawn=(0," << best.originY << ",0)";
                }
                std::cout << "\n" << std::flush;
                lastProgress = now;
            }
            const auto ckElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheckpoint).count();
            if (ckElapsed >= c.checkpointMs || completed == c.count) {
                saveState(c, completed, boards);
                lastCheckpoint = now;
            }
        }
    } catch (...) {
        try { saveState(c, completed, boards); } catch (...) {}
        freeBuffers(b);
        throw;
    }
    freeBuffers(b);
    saveState(c, completed, boards);
    if (!boards[0].rows.empty()) printResult("FINAL CURSED ORIGIN BEST", boards[0].rows.front());
    std::cout << "Results: " << c.outputDir.string() << "\n";
    return 0;
}

} // namespace cursed_spawn_origin_p1

int main(int argc, char** argv) {
    try {
        const auto c = cursed_spawn_origin_p1::parseArgs(argc, argv);
        return cursed_spawn_origin_p1::run(c);
    } catch (const std::exception& e) {
        std::cerr << "CursedSpawnOrigin P1 ERROR: " << e.what() << "\n";
        return 1;
    }
}
