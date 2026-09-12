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

namespace lava_spawn_origin_p2 {

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
    int qualified;             // spawn-valid sand at 0,0, then lava at player feet Y=65
    int score;
    int originY;               // pre-population sand surface selected by the spawn check
    int originBiome;           // 1=desert, 0=other (shore sand is still accepted)
    int sandReason;            // 2=desert top, 1=beach-noise replacement
    int lakeAttemptY;
    int lakeBaseX;
    int lakeBaseY;
    int lakeBaseZ;
    int originLocalX;
    int originLocalY;
    int originLocalZ;
    int lavaBodyBlocks;
    int lavaColumnsR1;
    int lavaColumnsR2;
    int lavaColumnsR4;
    int lakeLavaBlocks;
    int boundaryRejected;
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

__device__ __forceinline__ double nodeDensity(const double* density, std::size_t base, int x, int y, int z) {
    if (x < 0 || z < 0 || x >= coarsecore::SIZE || z >= coarsecore::SIZE) return -10.0;
    if (y < 0) return 10.0;
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

__device__ __forceinline__ int floorDivPositiveStep(int value, int step) {
    return value >= 0 ? value / step : -((-value + step - 1) / step);
}

// Exact trilinear density used by Beta's 4x8x4 terrain fill for one block.
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

enum RawBlockClass : int { RAW_AIR = 0, RAW_SOLID = 1, RAW_WATER = 2 };

__device__ __forceinline__ int rawBlockAt(
        const double* density, std::size_t base, int x, int y, int z) {
    if (terrainSolidAtBlock(density, base, x, y, z)) return RAW_SOLID;
    return y < 64 ? RAW_WATER : RAW_AIR;
}

__device__ __forceinline__ std::int64_t javaNextLong(p20::JavaRandom& random) {
    const std::int32_t hi = static_cast<std::int32_t>(random.nextBits(32));
    const std::int32_t lo = static_cast<std::int32_t>(random.nextBits(32));
    std::uint64_t bits = static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32;
    bits += static_cast<std::uint64_t>(static_cast<std::int64_t>(lo));
    return static_cast<std::int64_t>(bits);
}

__device__ __forceinline__ std::int64_t javaOddLong(std::int64_t value) {
    return (value / 2) * 2 + 1;
}

__device__ __forceinline__ int lakeIndex(int x, int y, int z) {
    return (x * 16 + z) * 8 + y;
}

__device__ __forceinline__ bool lakeBoundary(const unsigned char* mask, int x, int y, int z) {
    if (mask[lakeIndex(x, y, z)]) return false;
    return (x < 15 && mask[lakeIndex(x + 1, y, z)])
        || (x > 0 && mask[lakeIndex(x - 1, y, z)])
        || (z < 15 && mask[lakeIndex(x, y, z + 1)])
        || (z > 0 && mask[lakeIndex(x, y, z - 1)])
        || (y < 7 && mask[lakeIndex(x, y + 1, z)])
        || (y > 0 && mask[lakeIndex(x, y - 1, z)]);
}

__device__ __forceinline__ void consumeLakeShape(p20::JavaRandom& random) {
    const int ellipsoids = random.nextInt(4) + 4;
    for (int i = 0; i < ellipsoids * 6; ++i) (void)random.nextDouble();
}

__global__ void scoreLavaSpawnOriginKernel(
        const std::int64_t* seeds,
        const double* density,
        const unsigned char*,
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

    __shared__ unsigned char lakeMask[2048];
    __shared__ int possibleShared;
    __shared__ int originYShared;
    __shared__ int originBiomeShared;
    __shared__ int sandReasonShared;
    __shared__ int attemptYShared;
    __shared__ int lakeBaseXShared;
    __shared__ int lakeBaseYShared;
    __shared__ int lakeBaseZShared;
    __shared__ int localXShared;
    __shared__ int localYShared;
    __shared__ int localZShared;
    for (int i = tid; i < 2048; i += blockDim.x) lakeMask[i] = 0;
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

        const bool spawnSand = originY >= 63 && depth > 0 && reason != 0;
        originYShared = originY;
        originBiomeShared = desert ? 1 : 0;
        sandReasonShared = reason;
        possibleShared = 0;
        attemptYShared = lakeBaseXShared = lakeBaseYShared = lakeBaseZShared = 0;
        localXShared = localYShared = localZShared = -1;

        if (spawnSand) {
            // Only population chunk (-1,-1) can place a lake interior across X/Z 0.
            p20::JavaRandom worldRandom;
            worldRandom.setSeed(seeds[seedIndex]);
            const std::int64_t oddX = javaOddLong(javaNextLong(worldRandom));
            const std::int64_t oddZ = javaOddLong(javaNextLong(worldRandom));
            std::uint64_t populationSeed = 0ULL - static_cast<std::uint64_t>(oddX);
            populationSeed += 0ULL - static_cast<std::uint64_t>(oddZ);
            populationSeed ^= static_cast<std::uint64_t>(seeds[seedIndex]);
            p20::JavaRandom populationRandom;
            populationRandom.setSeed(static_cast<std::int64_t>(populationSeed));

            // Reject the 1/4 of populations with an earlier water-lake attempt;
            // that keeps the lava validation independent of an earlier lake edit.
            const bool noWaterLake = populationRandom.nextInt(4) != 0;
            if (noWaterLake && populationRandom.nextInt(8) == 0) {
                const int randomX = populationRandom.nextInt(16);
                const int yBound = populationRandom.nextInt(120) + 8;
                const int attemptY = populationRandom.nextInt(yBound);
                const int randomZ = populationRandom.nextInt(16);
                const bool allowedHeight = attemptY < 64 || populationRandom.nextInt(10) == 0;
                if (allowedHeight) {
                    const int baseX = -16 + randomX;
                    const int baseZ = -16 + randomZ;
                    int descendedY = attemptY;
                    while (descendedY > 0 && rawBlockAt(density, base, baseX, descendedY, baseZ) == RAW_AIR) {
                        --descendedY;
                    }
                    const int baseY = descendedY - 4;
                    const int localX = -baseX;
                    const int localY = 65 - baseY; // player bounding-box feet are at world Y=65
                    const int localZ = -baseZ;

                    const int ellipsoids = populationRandom.nextInt(4) + 4;
                    for (int e = 0; e < ellipsoids; ++e) {
                        const double sx = populationRandom.nextDouble() * 6.0 + 3.0;
                        const double sy = populationRandom.nextDouble() * 4.0 + 2.0;
                        const double sz = populationRandom.nextDouble() * 6.0 + 3.0;
                        const double cx = populationRandom.nextDouble() * (16.0 - sx - 2.0) + 1.0 + sx / 2.0;
                        const double cy = populationRandom.nextDouble() * (8.0 - sy - 4.0) + 2.0 + sy / 2.0;
                        const double cz = populationRandom.nextDouble() * (16.0 - sz - 2.0) + 1.0 + sz / 2.0;
                        for (int x = 1; x < 15; ++x) for (int z = 1; z < 15; ++z) for (int y = 1; y < 7; ++y) {
                            const double dx = (static_cast<double>(x) - cx) / (sx / 2.0);
                            const double dy = (static_cast<double>(y) - cy) / (sy / 2.0);
                            const double dz = (static_cast<double>(z) - cz) / (sz / 2.0);
                            if (dx * dx + dy * dy + dz * dz < 1.0) lakeMask[lakeIndex(x, y, z)] = 1;
                        }
                    }
                    const bool coversFeet = localX >= 0 && localX < 16 && localZ >= 0 && localZ < 16
                        && localY >= 0 && localY < 4 && lakeMask[lakeIndex(localX, localY, localZ)] != 0;
                    attemptYShared = attemptY;
                    lakeBaseXShared = baseX; lakeBaseYShared = baseY; lakeBaseZShared = baseZ;
                    localXShared = localX; localYShared = localY; localZShared = localZ;
                    possibleShared = coversFeet ? 1 : 0;
                }
            }
        }
    }
    __syncthreads();

    int localBoundaryRejects = 0;
    int localLavaBlocks = 0;
    if (possibleShared) {
        for (int index = tid; index < 2048; index += blockDim.x) {
            const int y = index & 7;
            const int column = index >> 3;
            const int z = column & 15;
            const int x = column >> 4;
            if (lakeMask[index] && y < 4) ++localLavaBlocks;
            if (lakeBoundary(lakeMask, x, y, z)) {
                const int block = rawBlockAt(density, base,
                    lakeBaseXShared + x, lakeBaseYShared + y, lakeBaseZShared + z);
                if ((y >= 4 && block == RAW_WATER) || (y < 4 && block != RAW_SOLID)) {
                    ++localBoundaryRejects;
                }
            }
        }
    }
    __shared__ int rejectParts[SCORE_THREADS];
    __shared__ int lavaParts[SCORE_THREADS];
    rejectParts[tid] = localBoundaryRejects;
    lavaParts[tid] = localLavaBlocks;
    __syncthreads();
    for (int stride = SCORE_THREADS / 2; stride > 0; stride >>= 1) {
        if (tid < stride) {
            rejectParts[tid] += rejectParts[tid + stride];
            lavaParts[tid] += lavaParts[tid + stride];
        }
        __syncthreads();
    }

    if (tid == 0) {
        const int lx = localXShared;
        const int ly = localYShared;
        const int lz = localZShared;
        int lavaBody = 0;
        if (possibleShared) {
            for (int bodyY = 65; bodyY <= 66; ++bodyY) {
                const int localBodyY = bodyY - lakeBaseYShared;
                if (localBodyY >= 0 && localBodyY < 4 &&
                    lakeMask[lakeIndex(lx, localBodyY, lz)]) ++lavaBody;
            }
        }
        bool bodyClear = possibleShared != 0;
        for (int bodyY = 65; bodyClear && bodyY <= 66; ++bodyY) {
            const int localBodyY = bodyY - lakeBaseYShared;
            const bool edited = localBodyY >= 0 && localBodyY < 8 &&
                lakeMask[lakeIndex(lx, localBodyY, lz)] != 0;
            if (!edited && rawBlockAt(density, base, 0, bodyY, 0) == RAW_SOLID) bodyClear = false;
        }
        int lavaR1 = 0, lavaR2 = 0, lavaR4 = 0;
        if (possibleShared) {
            for (int dx = -4; dx <= 4; ++dx) for (int dz = -4; dz <= 4; ++dz) {
                const int x = lx + dx;
                const int z = lz + dz;
                if (x < 0 || x >= 16 || z < 0 || z >= 16 || ly < 0 || ly >= 4) continue;
                if (!lakeMask[lakeIndex(x, ly, z)]) continue;
                const int cheb = (dx < 0 ? -dx : dx) > (dz < 0 ? -dz : dz)
                    ? (dx < 0 ? -dx : dx) : (dz < 0 ? -dz : dz);
                if (cheb <= 1) ++lavaR1;
                if (cheb <= 2) ++lavaR2;
                ++lavaR4;
            }
        }
        const bool qualified = possibleShared && rejectParts[0] == 0 && bodyClear && lavaBody > 0;
        DeviceResult r{};
        r.seed = seeds[seedIndex];
        r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(seedIndex);
        r.qualified = qualified ? 1 : 0;
        r.originY = originYShared;
        r.originBiome = originBiomeShared;
        r.sandReason = sandReasonShared;
        r.lakeAttemptY = attemptYShared;
        r.lakeBaseX = lakeBaseXShared; r.lakeBaseY = lakeBaseYShared; r.lakeBaseZ = lakeBaseZShared;
        r.originLocalX = lx; r.originLocalY = ly; r.originLocalZ = lz;
        r.lavaBodyBlocks = lavaBody;
        r.lavaColumnsR1 = lavaR1; r.lavaColumnsR2 = lavaR2; r.lavaColumnsR4 = lavaR4;
        r.lakeLavaBlocks = lavaParts[0];
        r.boundaryRejected = rejectParts[0];
        r.score = qualified ? lavaBody * 1000000 + lavaR1 * 10000 + lavaR2 * 1000
            + lavaR4 * 100 + lavaParts[0] : NO_SCORE;
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
#error "LavaSpawnOrigin P2 requires the current modern coarse GPU API"
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
    hipLaunchKernelGGL(scoreLavaSpawnOriginKernel, dim3(count), dim3(SCORE_THREADS), 0, 0,
        b.seeds, b.noise1, b.signs,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        count, baseIndex, b.results);
    checkHip(hipGetLastError(), "launch literal lava spawn scoring");
    checkHip(hipDeviceSynchronize(), "finish literal lava spawn batch");
    host.resize(static_cast<std::size_t>(count));
    checkHip(hipMemcpy(host.data(), b.results, static_cast<std::size_t>(count) * sizeof(DeviceResult), hipMemcpyDeviceToHost),
             "copy cursed origin spawn results");
}

using MetricFn = int (*)(const DeviceResult&);
static int metricOverall(const DeviceResult& r) { return r.score; }
static int metricImmersion(const DeviceResult& r) { return r.lavaBodyBlocks * 1000000 + r.score / 100; }
static int metricLavaRing(const DeviceResult& r) {
    return r.lavaColumnsR1 * 1000000 + r.lavaColumnsR2 * 10000 + r.lavaColumnsR4 * 100 + r.lakeLavaBlocks;
}
static int metricLakeSize(const DeviceResult& r) { return r.lakeLavaBlocks; }
static int metricHighest(const DeviceResult& r) { return r.originY; }

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
        if (fields.size() != 21) continue;
        try {
            DeviceResult r{};
            r.seed = std::stoll(fields[1]);
            r.sequenceIndex = std::stoull(fields[2]);
            r.qualified = std::stoi(fields[3]);
            r.score = std::stoi(fields[4]);
            r.originY = std::stoi(fields[5]);
            r.originBiome = std::stoi(fields[6]);
            r.sandReason = std::stoi(fields[7]);
            r.lakeAttemptY = std::stoi(fields[8]);
            r.lakeBaseX = std::stoi(fields[9]);
            r.lakeBaseY = std::stoi(fields[10]);
            r.lakeBaseZ = std::stoi(fields[11]);
            r.originLocalX = std::stoi(fields[12]);
            r.originLocalY = std::stoi(fields[13]);
            r.originLocalZ = std::stoi(fields[14]);
            r.lavaBodyBlocks = std::stoi(fields[15]);
            r.lavaColumnsR1 = std::stoi(fields[16]);
            r.lavaColumnsR2 = std::stoi(fields[17]);
            r.lavaColumnsR4 = std::stoi(fields[18]);
            r.lakeLavaBlocks = std::stoi(fields[19]);
            r.boundaryRejected = std::stoi(fields[20]);
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
    f << "rank,seed,sequence_index,qualified,score,prepop_sand_y,origin_biome_desert,sand_reason,lake_attempt_y,lake_base_x,lake_base_y,lake_base_z,origin_local_x,origin_local_y,origin_local_z,lava_body_blocks,lava_columns_r1,lava_columns_r2,lava_columns_r4,lake_lava_blocks,boundary_rejected\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        f << (i + 1) << ',' << r.seed << ',' << r.sequenceIndex << ',' << r.qualified << ',' << r.score << ','
          << r.originY << ',' << r.originBiome << ',' << r.sandReason << ',' << r.lakeAttemptY << ','
          << r.lakeBaseX << ',' << r.lakeBaseY << ',' << r.lakeBaseZ << ',' << r.originLocalX << ','
          << r.originLocalY << ',' << r.originLocalZ << ',' << r.lavaBodyBlocks << ',' << r.lavaColumnsR1 << ','
          << r.lavaColumnsR2 << ',' << r.lavaColumnsR4 << ',' << r.lakeLavaBlocks << ',' << r.boundaryRejected << '\n';
    }
}

static void saveState(const Config& c, std::uint64_t completed, const std::vector<Board>& boards) {
    std::filesystem::create_directories(c.outputDir);
    for (const auto& b : boards) writeCsv(c.outputDir / ("top_" + b.name + ".csv"), b.rows);
    std::ofstream ck(c.outputDir / "checkpoint.txt", std::ios::trunc);
    ck << "VERSION=LavaSpawnOriginP2\n"
       << "SPAWN_X=0\nSPAWN_Y=64\nSPAWN_Z=0\nPLAYER_FEET_Y=65\n"
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
           << "BEST_PREPOP_SAND_Y=" << r.originY << "\nBEST_LAVA_BODY_BLOCKS=" << r.lavaBodyBlocks
           << "\nBEST_LAVA_R1=" << r.lavaColumnsR1 << "\nBEST_LAVA_R2=" << r.lavaColumnsR2
           << "\nBEST_LAKE_LAVA_BLOCKS=" << r.lakeLavaBlocks << "\n";
    }
}

static const char* sandReasonName(int code) {
    if (code == 2) return "DESERT_TOP";
    if (code == 1) return "BEACH_NOISE";
    return "NONE";
}

static void printResult(const char* prefix, const DeviceResult& r) {
    std::cout << prefix << " seed=" << r.seed << " score=" << r.score
              << " spawnXZ=(0,0) playerFeetY=65 prepopSandY=" << r.originY
              << " sand=" << sandReasonName(r.sandReason)
              << " lavaBody=" << r.lavaBodyBlocks << "/2"
              << " lavaR1=" << r.lavaColumnsR1 << "/9"
              << " lavaR2=" << r.lavaColumnsR2 << "/25"
              << " lavaR4=" << r.lavaColumnsR4 << "/81"
              << " lakeLavaBlocks=" << r.lakeLavaBlocks
              << " lakeBase=(" << r.lakeBaseX << ',' << r.lakeBaseY << ',' << r.lakeBaseZ << ")\n";
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
        std::cerr << "LavaSpawnOrigin P2 self-test FAILED: repeated fixed-key batch changed.\n";
        return 2;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].sequenceIndex != i) {
            std::cerr << "LavaSpawnOrigin P2 self-test FAILED: sequence index mismatch.\n";
            return 3;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if ((static_cast<std::uint64_t>(a[i].seed) & JAVA_SEED_MASK) ==
                (static_cast<std::uint64_t>(a[j].seed) & JAVA_SEED_MASK)) {
                std::cerr << "LavaSpawnOrigin P2 self-test FAILED: unique48 collision.\n";
                return 4;
            }
        }
    }
    for (const auto& r : a) {
        if (r.lavaBodyBlocks < 0 || r.lavaBodyBlocks > 2 ||
            r.lavaColumnsR1 < 0 || r.lavaColumnsR1 > 9 ||
            r.lavaColumnsR2 < 0 || r.lavaColumnsR2 > 25 ||
            r.lavaColumnsR4 < 0 || r.lavaColumnsR4 > 81) {
            std::cerr << "LavaSpawnOrigin P2 self-test FAILED: invalid lava metrics.\n";
            return 6;
        }
        if (r.qualified && (r.originY < 63 || r.sandReason == 0 || r.lavaBodyBlocks < 1 ||
                            r.boundaryRejected != 0 || r.score == NO_SCORE)) return 7;
    }
    std::cout << "LavaSpawnOrigin P2 deterministic GPU self-test PASS\n";
    printResult("sample", a.front());
    return 0;
}

static int run(const Config& c) {
    printDevice();
    std::cout << "LavaSpawnOrigin P2 | Beta 1.7.3 literal lava at fixed player spawn\n"
              << "Compile radius: " << p14config::CHUNK_RADIUS << " chunks"
              << " | lattice=" << coarsecore::SIZE << 'x' << coarsecore::SIZE
              << " | storedY=" << coarsecore::Y_LEVELS << " | mode=" << p14config::MODE_NAME << "\n"
              << "Hard gate: sand selects spawn X=0,Z=0 before population; chunk (-1,-1)'s lava lake then covers player feet Y=65\n"
              << "Ranking: deeper player immersion and more lava surrounding the spawn block\n"
              << "Seed mode: " << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64")
              << " | randomKey=" << c.randomKey << "\n";

    if (c.selfTest) return runSelfTest(c);
    std::filesystem::create_directories(c.outputDir);
    {
        std::ofstream info(c.outputDir / "run_config.txt", std::ios::trunc);
        info << "LavaSpawnOriginP2\nSPAWN_X=0\nSPAWN_Y=64\nSPAWN_Z=0\nPLAYER_FEET_Y=65\nRADIUS=" << p14config::CHUNK_RADIUS << "\nMODE=" << p14config::MODE_NAME
             << "\nCOUNT=" << c.count << "\nSTART_INDEX=" << c.startIndex << "\nBATCH=" << c.batch
             << "\nTERRAIN_THREADS=" << c.terrainThreads << "\nTOP=" << c.top << "\nRANDOM_KEY=" << c.randomKey
             << "\nSEED_MODE=" << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64") << "\n";
        if (c.singleSeedSet) info << "SINGLE_SEED=" << c.singleSeed << "\n";
    }

    DeviceBuffers b = allocateBuffers(c.batch);
    std::vector<DeviceResult> host;
    std::vector<Board> boards = {
        {"overall", metricOverall, {}}, {"immersion", metricImmersion, {}},
        {"lava_ring", metricLavaRing, {}}, {"lake_size", metricLakeSize, {}},
        {"highest_prepop_sand", metricHighest, {}}
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
                    printResult("NEW LITERAL LAVA SPAWN", r);
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
                std::cout << " literalLavaSpawnHits=" << qualified;
                if (!boards[0].rows.empty()) {
                    const auto& best = boards[0].rows.front();
                    std::cout << " best=" << best.score << " seed=" << best.seed
                              << " playerFeet=(0,65,0)";
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
    if (!boards[0].rows.empty()) printResult("FINAL LITERAL LAVA SPAWN BEST", boards[0].rows.front());
    std::cout << "Results: " << c.outputDir.string() << "\n";
    return 0;
}

} // namespace lava_spawn_origin_p2

int main(int argc, char** argv) {
    try {
        const auto c = lava_spawn_origin_p2::parseArgs(argc, argv);
        return lava_spawn_origin_p2::run(c);
    } catch (const std::exception& e) {
        std::cerr << "LavaSpawnOrigin P2 ERROR: " << e.what() << "\n";
        return 1;
    }
}
