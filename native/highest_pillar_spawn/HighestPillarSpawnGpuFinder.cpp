#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"
#include "terrain_perlin_cache.hpp"
#include "climate_perlin_cache.hpp"
#include "coarse_exact_gpu.hpp"
#include "skyblock_p14_config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace highest_pillar_spawn_p1 {

static constexpr int SCORE_THREADS = 128;
static constexpr std::uint64_t JAVA_SEED_PERIOD = 1ULL << 48;
static constexpr std::uint64_t JAVA_SEED_MASK = JAVA_SEED_PERIOD - 1ULL;

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
    int spawnSand;
    int qualified;
    std::int64_t score;
    int spawnSurfaceY;
    int playerFeetY;
    int supportY;
    int originBiome;
    int sandReason;
    int isolatedR1;
    int pillarDepth;
    int neighborMaxY;
    int dropR1;
    int dropR2Sample;
    int dropR4Sample;
    int clearR2AtTop;
    int clearR4AtTop;
};

struct DeviceBuffers {
    int capacity = 0;
    std::int64_t* seeds = nullptr;
    double* temp = nullptr;
    double* rain = nullptr;
    double* climateBlend = nullptr;
    double* noise1 = nullptr;
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
        allocateArray(b.results, seeds, "allocate results");
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

__device__ __forceinline__ double nodeDensity(const double* density, std::size_t base, int x, int y, int z) {
    if (x < 0 || z < 0 || x >= coarsecore::SIZE || z >= coarsecore::SIZE) return -10.0;
    if (y < 0) return 10.0;
    if (y >= coarsecore::Y_LEVELS) return -10.0;
    return density[base + static_cast<std::size_t>(coarsecore::index3(x, y, z))];
}

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

__device__ __forceinline__ int exactSpawnCheckYAtOrigin(
        const double* density, std::size_t base, int x, int z) {
    int y = 63;
    while (y + 1 < 128 && solidAtWorldY(density, base, x, z, y + 1)) ++y;
    return solidAtWorldY(density, base, x, z, y) ? y : -1;
}

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

__device__ __forceinline__ int floorDivPositiveStep(int value, int step) {
    return value >= 0 ? value / step : -((-value + step - 1) / step);
}

__device__ __forceinline__ bool terrainSolidAtBlock(
        const double* density, std::size_t base, int worldX, int worldY, int worldZ) {
    if (worldY < coarsecore::Y_BASE * 8) return true;
    if (worldY >= 128) return false;
    const int coarseX = floorDivPositiveStep(worldX, 4);
    const int coarseZ = floorDivPositiveStep(worldZ, 4);
    const int ix = coarseX - coarsecore::FROM_COARSE;
    const int iz = coarseZ - coarsecore::FROM_COARSE;
    const int iy = (worldY >> 3) - coarsecore::Y_BASE;
    const double fx = static_cast<double>(worldX - coarseX * 4) * 0.25;
    const double fz = static_cast<double>(worldZ - coarseZ * 4) * 0.25;
    const double fy = static_cast<double>(worldY & 7) * 0.125;
    const double d000 = nodeDensity(density, base, ix,     iy,     iz);
    const double d001 = nodeDensity(density, base, ix,     iy,     iz + 1);
    const double d100 = nodeDensity(density, base, ix + 1, iy,     iz);
    const double d101 = nodeDensity(density, base, ix + 1, iy,     iz + 1);
    const double d010 = nodeDensity(density, base, ix,     iy + 1, iz);
    const double d011 = nodeDensity(density, base, ix,     iy + 1, iz + 1);
    const double d110 = nodeDensity(density, base, ix + 1, iy + 1, iz);
    const double d111 = nodeDensity(density, base, ix + 1, iy + 1, iz + 1);
    const double a0 = d000 + (d100 - d000) * fx;
    const double a1 = d001 + (d101 - d001) * fx;
    const double b0 = d010 + (d110 - d010) * fx;
    const double b1 = d011 + (d111 - d011) * fx;
    const double low = a0 + (a1 - a0) * fz;
    const double high = b0 + (b1 - b0) * fz;
    return low + (high - low) * fy > 0.0;
}

__device__ __forceinline__ bool playerCollidesAtFeetY(
        const double* density, std::size_t base, int feetY) {
    // At X/Z 0.5 the 0.6-wide player AABB only touches column (0,0).
    // For integer feet Y and 1.8-block height, only blocks feetY and feetY+1 can intersect it.
    return terrainSolidAtBlock(density, base, 0, feetY, 0)
        || terrainSolidAtBlock(density, base, 0, feetY + 1, 0);
}

__device__ __forceinline__ int actualPlayerFeetY(
        const double* density, std::size_t base) {
    int feetY = 65;
    while (feetY < 128 && playerCollidesAtFeetY(density, base, feetY)) ++feetY;
    return feetY;
}

__device__ __forceinline__ int highestSolidAtOrBelow(
        const double* density, std::size_t base, int x, int z, int startY) {
    int y = startY > 127 ? 127 : startY;
    for (; y >= 0; --y) {
        if (terrainSolidAtBlock(density, base, x, y, z)) return y;
    }
    return -1;
}

__device__ __forceinline__ bool adjacentEightAirAt(
        const double* density, std::size_t base, int y) {
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            if (terrainSolidAtBlock(density, base, dx, y, dz)) return false;
        }
    }
    return true;
}

__device__ __forceinline__ int exactNeighborMaxY(
        const double* density, std::size_t base, int supportY) {
    int best = -1;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int y = highestSolidAtOrBelow(density, base, dx, dz, supportY);
            if (y > best) best = y;
        }
    }
    return best;
}

__device__ __forceinline__ int sampleRingMaxY(
        const double* density, std::size_t base, int supportY, int r) {
    const int points[8][2] = {
        { r, 0}, {-r, 0}, {0, r}, {0,-r},
        { r, r}, { r,-r}, {-r, r}, {-r,-r}
    };
    int best = -1;
    for (int i = 0; i < 8; ++i) {
        const int y = highestSolidAtOrBelow(density, base, points[i][0], points[i][1], supportY);
        if (y > best) best = y;
    }
    return best;
}

__device__ __forceinline__ int clearCountAtTop(
        const double* density, std::size_t base, int supportY, int r) {
    int clear = 0;
    for (int dz = -r; dz <= r; ++dz) {
        for (int dx = -r; dx <= r; ++dx) {
            if (dx == 0 && dz == 0) continue;
            if (!terrainSolidAtBlock(density, base, dx, supportY, dz)) ++clear;
        }
    }
    return clear;
}

__device__ __forceinline__ int oneByOnePillarDepth(
        const double* density, std::size_t base, int supportY) {
    int depth = 0;
    const int minY = supportY > 63 ? supportY - 63 : 0;
    for (int y = supportY; y >= minY; --y) {
        if (!terrainSolidAtBlock(density, base, 0, y, 0)) break;
        if (!adjacentEightAirAt(density, base, y)) break;
        ++depth;
    }
    return depth;
}

__global__ void scoreHighestPillarKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        std::uint64_t baseIndex,
        DeviceResult* out) {
    const int seedIndex = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (seedIndex >= count) return;
    const std::size_t base = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;
    const int center = -coarsecore::FROM_COARSE;

    DeviceResult r{};
    r.seed = seeds[seedIndex];
    r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(seedIndex);
    r.neighborMaxY = -1;
    r.dropR1 = r.dropR2Sample = r.dropR4Sample = -999;

    const int spawnSurfaceY = exactSpawnCheckYAtOrigin(density, base, center, center);
    r.spawnSurfaceY = spawnSurfaceY;
    const bool desert = betaBiomeIsDesert(originTemperature[seedIndex], originRainfall[seedIndex]);
    r.originBiome = desert ? 1 : 0;

    // For chunk (0,0), world X/Z 0 is the first replacement column. These are
    // exactly its sand/gravel/depth random draws in Beta 1.7.3.
    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool beachSand = originSandNoise[seedIndex] + sandJitter * 0.2 > 0.0;
    const int depth = static_cast<int>(originStoneNoise[seedIndex] / 3.0 + 3.0 + depthJitter * 0.25);
    const bool beachBand = spawnSurfaceY >= 60 && spawnSurfaceY <= 65;
    const int reason = desert ? 2 : ((beachBand && beachSand) ? 1 : 0);
    r.sandReason = reason;
    const bool spawnSand = spawnSurfaceY >= 63 && depth > 0 && reason != 0;
    r.spawnSand = spawnSand ? 1 : 0;
    if (!spawnSand) {
        out[seedIndex] = r;
        return;
    }

    const int feetY = actualPlayerFeetY(density, base);
    const int supportY = feetY > 0 ? highestSolidAtOrBelow(density, base, 0, 0, feetY - 1) : -1;
    r.playerFeetY = feetY;
    r.supportY = supportY;
    if (supportY < 0 || supportY != feetY - 1) {
        out[seedIndex] = r;
        return;
    }

    const bool isolated = adjacentEightAirAt(density, base, supportY);
    r.isolatedR1 = isolated ? 1 : 0;
    r.clearR2AtTop = clearCountAtTop(density, base, supportY, 2);
    r.clearR4AtTop = clearCountAtTop(density, base, supportY, 4);
    r.pillarDepth = isolated ? oneByOnePillarDepth(density, base, supportY) : 0;

    const int n1 = exactNeighborMaxY(density, base, supportY);
    const int n2 = sampleRingMaxY(density, base, supportY, 2);
    const int n4 = sampleRingMaxY(density, base, supportY, 4);
    r.neighborMaxY = n1;
    r.dropR1 = supportY - n1;
    r.dropR2Sample = supportY - n2;
    r.dropR4Sample = supportY - n4;

    r.qualified = isolated ? 1 : 0;
    if (r.qualified) {
        // Lexicographic intent encoded in one signed 64-bit score:
        // actual player feet height first; then true 1x1 shaft depth; then local drop.
        r.score = static_cast<std::int64_t>(feetY) * 1000000000LL
                + static_cast<std::int64_t>(r.pillarDepth) * 1000000LL
                + static_cast<std::int64_t>(std::max(0, std::min(999, r.dropR1))) * 1000LL
                + static_cast<std::int64_t>(std::max(0, std::min(999, r.dropR2Sample)));
    }
    out[seedIndex] = r;
}

struct Config {
    std::filesystem::path outputDir;
    std::uint64_t count = 10000000;
    std::uint64_t startIndex = 0;
    int batch = 8192;
    int terrainThreads = 64;
    int top = 250;
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
#error "HighestPillarSpawn P1 requires the current modern coarse GPU API"
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
    const int blocks = (count + SCORE_THREADS - 1) / SCORE_THREADS;
    hipLaunchKernelGGL(scoreHighestPillarKernel, dim3(blocks), dim3(SCORE_THREADS), 0, 0,
        b.seeds, b.noise1,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        count, baseIndex, b.results);
    checkHip(hipGetLastError(), "launch highest pillar spawn scoring");
    checkHip(hipDeviceSynchronize(), "finish highest pillar spawn batch");
    host.resize(static_cast<std::size_t>(count));
    checkHip(hipMemcpy(host.data(), b.results, static_cast<std::size_t>(count) * sizeof(DeviceResult), hipMemcpyDeviceToHost),
             "copy highest pillar spawn results");
}

using Metric = std::int64_t (*)(const DeviceResult&);
static std::int64_t metricOverall(const DeviceResult& r) { return r.score; }
static std::int64_t metricHighest(const DeviceResult& r) { return r.playerFeetY; }
static std::int64_t metricDepth(const DeviceResult& r) { return r.pillarDepth; }
static std::int64_t metricDrop(const DeviceResult& r) { return r.dropR1; }

struct Board {
    std::string name;
    Metric metric;
    std::vector<DeviceResult> rows;
};

static void insertTop(Board& b, const DeviceResult& r, int limit) {
    if (!r.qualified) return;
    const std::int64_t value = b.metric(r);
    auto it = std::lower_bound(b.rows.begin(), b.rows.end(), value,
        [&](const DeviceResult& a, std::int64_t v) {
            const auto av = b.metric(a);
            if (av != v) return av > v;
            return a.score > r.score;
        });
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
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) fields.push_back(field);
        if (fields.size() != 19) continue;
        try {
            DeviceResult r{};
            r.seed = std::stoll(fields[1]);
            r.sequenceIndex = std::stoull(fields[2]);
            r.spawnSand = std::stoi(fields[3]);
            r.qualified = std::stoi(fields[4]);
            r.score = std::stoll(fields[5]);
            r.spawnSurfaceY = std::stoi(fields[6]);
            r.playerFeetY = std::stoi(fields[7]);
            r.supportY = std::stoi(fields[8]);
            r.originBiome = std::stoi(fields[9]);
            r.sandReason = std::stoi(fields[10]);
            r.isolatedR1 = std::stoi(fields[11]);
            r.pillarDepth = std::stoi(fields[12]);
            r.neighborMaxY = std::stoi(fields[13]);
            r.dropR1 = std::stoi(fields[14]);
            r.dropR2Sample = std::stoi(fields[15]);
            r.dropR4Sample = std::stoi(fields[16]);
            r.clearR2AtTop = std::stoi(fields[17]);
            r.clearR4AtTop = std::stoi(fields[18]);
            rows.push_back(r);
        } catch (...) {}
    }
    return rows;
}

static void writeCsv(const std::filesystem::path& path, const std::vector<DeviceResult>& rows) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f << "rank,seed,sequence_index,spawn_sand,qualified,score,spawn_surface_y,player_feet_y,support_y,origin_biome_desert,sand_reason,isolated_r1,pillar_depth,neighbor_max_y,drop_r1,drop_r2_sample,drop_r4_sample,clear_r2_at_top,clear_r4_at_top\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        f << (i + 1) << ',' << r.seed << ',' << r.sequenceIndex << ',' << r.spawnSand << ',' << r.qualified << ','
          << r.score << ',' << r.spawnSurfaceY << ',' << r.playerFeetY << ',' << r.supportY << ',' << r.originBiome << ','
          << r.sandReason << ',' << r.isolatedR1 << ',' << r.pillarDepth << ',' << r.neighborMaxY << ',' << r.dropR1 << ','
          << r.dropR2Sample << ',' << r.dropR4Sample << ',' << r.clearR2AtTop << ',' << r.clearR4AtTop << '\n';
    }
}

static void saveState(const Config& c, std::uint64_t completed, const std::vector<Board>& boards) {
    std::filesystem::create_directories(c.outputDir);
    for (const auto& b : boards) writeCsv(c.outputDir / ("top_" + b.name + ".csv"), b.rows);
    std::ofstream ck(c.outputDir / "checkpoint.txt", std::ios::trunc);
    ck << "VERSION=HighestPillarSpawnP1\n"
       << "SPAWN_X=0\nSAVED_SPAWN_Y=64\nSPAWN_Z=0\n"
       << "RADIUS=" << p14config::CHUNK_RADIUS << "\n"
       << "START_INDEX=" << c.startIndex << "\n"
       << "COMPLETED=" << completed << "\n"
       << "NEXT_INDEX=" << (c.startIndex + completed) << "\n"
       << "COUNT=" << c.count << "\n"
       << "RANDOM_KEY=" << c.randomKey << "\n"
       << "SEED_MODE=" << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64") << "\n";
    if (!boards.empty() && !boards[0].rows.empty()) {
        const auto& r = boards[0].rows.front();
        ck << "BEST_SEED=" << r.seed << "\nBEST_SCORE=" << r.score
           << "\nBEST_PLAYER_FEET_Y=" << r.playerFeetY
           << "\nBEST_SUPPORT_Y=" << r.supportY
           << "\nBEST_PILLAR_DEPTH=" << r.pillarDepth
           << "\nBEST_DROP_R1=" << r.dropR1 << "\n";
    }
}

static const char* sandReasonName(int code) {
    if (code == 2) return "DESERT_TOP";
    if (code == 1) return "BEACH_NOISE";
    return "NONE";
}

static void printResult(const char* prefix, const DeviceResult& r) {
    std::cout << prefix << " seed=" << r.seed
              << " playerFeetY=" << r.playerFeetY
              << " supportY=" << r.supportY
              << " spawnSurfaceY=" << r.spawnSurfaceY
              << " pillarDepth=" << r.pillarDepth
              << " dropR1=" << r.dropR1
              << " dropR2Sample=" << r.dropR2Sample
              << " dropR4Sample=" << r.dropR4Sample
              << " clearR2=" << r.clearR2AtTop << "/24"
              << " clearR4=" << r.clearR4AtTop << "/80"
              << " sand=" << sandReasonName(r.sandReason)
              << " score=" << r.score << "\n";
}

static int runSelfTest(Config c) {
    printDevice();
    c.singleSeedSet = true;
    c.singleSeed = 8734788222889465725LL;
    c.count = 1;
    c.batch = 1;
    DeviceBuffers b = allocateBuffers(1);
    std::vector<DeviceResult> host;
    try {
        launchBatch(b, c, 0, 1, host);
    } catch (...) {
        freeBuffers(b);
        throw;
    }
    freeBuffers(b);
    const auto& r = host.front();
    if (!r.spawnSand || r.spawnSurfaceY != 86 || r.playerFeetY != 87) {
        std::ostringstream msg;
        msg << "known-seed self-test mismatch: spawnSand=" << r.spawnSand
            << " spawnSurfaceY=" << r.spawnSurfaceY << " playerFeetY=" << r.playerFeetY
            << " (expected 1,86,87 before population edits)";
        throw std::runtime_error(msg.str());
    }
    printResult("SELFTEST OK", r);
    return 0;
}

static int run(Config c) {
    if (c.selfTest) return runSelfTest(c);
    printDevice();
    std::cout << "HighestPillarSpawn P1 | exact origin sand gate + player collision push + 1x1 top isolation\n";
    std::cout << "Hard gate: Beta can select X=0,Z=0 as spawn, and the final support block has all 8 adjacent blocks air at that Y.\n";
    std::cout << "Ranking: actual player feet Y first, then consecutive 1x1 pillar depth, then local drop.\n";
    std::cout << "Seed mode: " << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64")
              << " | randomKey=" << c.randomKey << "\n";

    std::vector<Board> boards = {
        {"overall", metricOverall, {}},
        {"highest", metricHighest, {}},
        {"pillar_depth", metricDepth, {}},
        {"drop_r1", metricDrop, {}}
    };
    std::uint64_t completed = 0;
    if (c.resumeExisting) {
        for (auto& b : boards) {
            b.rows = readCsv(c.outputDir / ("top_" + b.name + ".csv"));
            if (static_cast<int>(b.rows.size()) > c.top) b.rows.resize(static_cast<std::size_t>(c.top));
        }
        std::ifstream ck(c.outputDir / "checkpoint.txt");
        std::string line;
        while (std::getline(ck, line)) {
            if (line.rfind("COMPLETED=", 0) == 0) completed = std::stoull(line.substr(10));
            else if (line.rfind("RANDOM_KEY=", 0) == 0) c.randomKey = std::stoull(line.substr(11));
        }
        if (completed > c.count) completed = c.count;
        std::cout << "Resuming at completed=" << completed << "\n";
    }

    DeviceBuffers b = allocateBuffers(c.batch);
    std::vector<DeviceResult> host;
    auto start = std::chrono::steady_clock::now();
    auto lastProgress = start;
    auto lastCheckpoint = start;
    std::int64_t bestPrintedScore = boards[0].rows.empty() ? -1 : boards[0].rows.front().score;
    std::uint64_t qualifiedCount = 0;

    try {
        while (completed < c.count) {
            const std::uint64_t left = c.count - completed;
            const int batchCount = static_cast<int>(std::min<std::uint64_t>(left, static_cast<std::uint64_t>(c.batch)));
            launchBatch(b, c, c.startIndex + completed, batchCount, host);
            for (const auto& r : host) {
                if (!r.qualified) continue;
                ++qualifiedCount;
                for (auto& board : boards) insertTop(board, r, c.top);
                if (r.score > bestPrintedScore) {
                    bestPrintedScore = r.score;
                    printResult("NEW HIGHEST PILLAR SPAWN", r);
                }
            }
            completed += static_cast<std::uint64_t>(batchCount);
            const auto now = std::chrono::steady_clock::now();
            const auto progressElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgress).count();
            const auto checkpointElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheckpoint).count();
            if (progressElapsed >= c.progressMs || completed == c.count) {
                const double seconds = std::chrono::duration<double>(now - start).count();
                const double rate = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
                std::cout << "progress checked=" << completed << '/' << c.count
                          << " rate=" << std::fixed << std::setprecision(1) << rate << " seeds/s"
                          << " pillarHits=" << qualifiedCount;
                if (!boards[0].rows.empty()) {
                    const auto& best = boards[0].rows.front();
                    std::cout << " bestFeetY=" << best.playerFeetY
                              << " bestDepth=" << best.pillarDepth
                              << " bestSeed=" << best.seed;
                }
                std::cout << "\n";
                lastProgress = now;
            }
            if (checkpointElapsed >= c.checkpointMs || completed == c.count) {
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
    if (!boards[0].rows.empty()) printResult("FINAL HIGHEST PILLAR SPAWN BEST", boards[0].rows.front());
    std::cout << "Results: " << c.outputDir.string() << "\n";
    return 0;
}

} // namespace highest_pillar_spawn_p1

int main(int argc, char** argv) {
    try {
        const auto c = highest_pillar_spawn_p1::parseArgs(argc, argv);
        return highest_pillar_spawn_p1::run(c);
    } catch (const std::exception& e) {
        std::cerr << "HighestPillarSpawn P1 ERROR: " << e.what() << "\n";
        return 1;
    }
}
