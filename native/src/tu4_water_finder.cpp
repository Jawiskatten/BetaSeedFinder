#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace tu4water {

// Xbox 360 TU4 / Classic-sized finite world used by this project:
// 54x54 chunks = 864x864 blocks, centered as [-432,+431] on X/Z.
static constexpr int WORLD_SIDE = 864;
static constexpr int WORLD_HALF = 432;
static constexpr int TOTAL_COLUMNS = WORLD_SIDE * WORLD_SIDE; // 746,496
static constexpr int COARSE_CELLS = WORLD_SIDE / 4;           // 216
static constexpr int COARSE_POINTS = COARSE_CELLS + 1;        // 217
static constexpr int COARSE_POINT_COUNT = COARSE_POINTS * COARSE_POINTS;
static constexpr int COARSE_CELL_COUNT = COARSE_CELLS * COARSE_CELLS;
static constexpr int SCOUT_THREADS = 64;
static constexpr int EXACT_THREADS = 256;
static constexpr int TERRAIN_STATE_COUNT = 66;

struct Options {
    int batch = 8192;
    int topExact = 2;
    double statusSeconds = 2.0;
    std::uint64_t sequence = 0;
    bool sequenceSpecified = false;
    std::uint64_t startAttempt = 0;
    std::uint64_t maxAttempts = 0;
    bool continueAfterHit = false;
    bool verifyOnly = false;
    std::int64_t verifySeed = 0;
    std::string logPath = "tu4_water_hits.csv";
};

struct ExactWaterResult {
    int landColumns = TOTAL_COLUMNS;
    int waterColumns = 0;
};

#define HIP_CHECK(call) do { \
    hipError_t _err = (call); \
    if (_err != hipSuccess) { \
        throw std::runtime_error(std::string(#call) + " failed: " + hipGetErrorString(_err)); \
    } \
} while (0)

P20_HD std::int64_t multipliedSeed(std::int64_t seed, std::uint64_t multiplier) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * multiplier);
}

// -------------------------------------------------------------------------
// Scout
// -------------------------------------------------------------------------
// The exact full 864x864 terrain scan is far too expensive for every seed.
// For every seed the scout samples the macro-height field at an 8x8 lattice
// covering the whole TU4 world. The terrain's noise6/noise5-in-this-project
// field controls the large-scale vertical center (d7), so low d7 is a strong
// proxy for ocean-heavy worlds. Only the best scout seeds in each batch receive
// the exact 746,496-column water/land count below.
//
// Reaching terrain octave 50 normally means constructing 50 discarded Perlin
// permutations. This exact RNG-only replay consumes the identical Java Random
// draws without building those permutations, copied from the proven P12 idea.

__device__ __constant__ unsigned char NEXTINT_REJECT_TAIL[257] = {
    0, 0, 0, 2, 0, 3, 2, 2, 0, 2, 8, 2, 8, 11, 2, 8,
    0, 9, 2, 3, 8, 2, 2, 6, 8, 23, 24, 11, 16, 8, 8, 2,
    0, 2, 26, 23, 20, 22, 22, 11, 8, 39, 2, 8, 24, 38, 6, 21,
    32, 44, 48, 26, 24, 21, 38, 13, 16, 41, 8, 55, 8, 59, 2, 2,
    0, 63, 2, 50, 60, 29, 58, 40, 56, 16, 22, 23, 60, 2, 50, 25,
    48, 65, 80, 80, 44, 43, 8, 8, 24, 67, 38, 37, 52, 2, 68, 3,
    32, 66, 44, 2, 48, 34, 26, 83, 24, 23, 74, 68, 92, 92, 68, 59,
    16, 8, 98, 98, 8, 11, 114, 9, 8, 90, 120, 80, 64, 23, 2, 8,
    0, 8, 128, 124, 68, 79, 50, 38, 128, 17, 98, 90, 128, 68, 40, 24,
    128, 8, 16, 44, 96, 139, 98, 2, 136, 128, 2, 33, 128, 125, 104, 74,
    128, 121, 146, 50, 80, 68, 80, 87, 128, 141, 128, 155, 8, 48, 8, 23,
    112, 173, 156, 63, 128, 98, 128, 59, 144, 133, 2, 145, 68, 65, 98, 169,
    128, 54, 66, 128, 44, 44, 2, 23, 48, 50, 34, 37, 128, 203, 186, 29,
    128, 79, 128, 131, 180, 182, 68, 8, 200, 2, 92, 89, 68, 128, 170, 115,
    128, 173, 8, 88, 212, 195, 98, 2, 8, 4, 128, 68, 232, 104, 128, 55,
    128, 128, 90, 65, 120, 93, 80, 193, 64, 80, 148, 187, 128, 167, 8, 128,
    0
};

__device__ __forceinline__ void advanceJava6(p20::JavaRandom& random) {
    random.state =
            (random.state * 0x45D73749A7F9ULL + 0x17617168255EULL) & p20::JAVA_MASK;
}

__device__ __forceinline__ void consumeNextIntOnlyExact(
        p20::JavaRandom& random,
        int bound
) {
    const unsigned int tail = static_cast<unsigned int>(NEXTINT_REJECT_TAIL[bound]);
    const unsigned int limit = 0x80000000u - tail;
    unsigned int bits;
    do {
        bits = random.nextBits(31);
    } while (bits >= limit);
}

__device__ __forceinline__ void consumePerlinRngOnly(p20::JavaRandom& random) {
    advanceJava6(random); // a,b,c: three nextDouble() calls = six LCG draws
    for (int i = 0; i < 256; ++i) {
        consumeNextIntOnlyExact(random, 256 - i);
    }
}

__device__ __forceinline__ double heightCenterFromNoise5(double noise5) {
    double d6 = noise5 / 8000.0;
    if (d6 < 0.0) d6 = -d6 * 0.3;
    d6 = d6 * 3.0 - 2.0;
    if (d6 < 0.0) {
        d6 /= 2.0;
        if (d6 < -1.0) d6 = -1.0;
        d6 /= 1.4;
        d6 /= 2.0;
    } else {
        if (d6 > 1.0) d6 = 1.0;
        d6 /= 8.0;
    }
    d6 *= 17.0 / 16.0;
    return 17.0 / 2.0 + d6 * 4.0;
}

__global__ void waterScoutKernel(
        std::uint64_t sequence,
        std::uint64_t startAttempt,
        int count,
        unsigned short* lowSampleCounts,
        double* heightSums
) {
    const int seedIndex = static_cast<int>(blockIdx.x);
    const int lane = static_cast<int>(threadIdx.x);
    if (seedIndex >= count || lane >= SCOUT_THREADS) return;

    __shared__ p20::PerlinState perlin;
    __shared__ int lowScratch[SCOUT_THREADS];
    __shared__ double sumScratch[SCOUT_THREADS];

    const std::uint64_t attempt = startAttempt + static_cast<std::uint64_t>(seedIndex);
    const std::int64_t seed = static_cast<std::int64_t>(
            p20::splitMixDeterministicSeed(sequence, attempt));

    // 8x8 sample cell centers over the 216x216 coarse-cell TU4 footprint.
    const int row = lane >> 3;
    const int col = lane & 7;
    const int qx = -108 + ((2 * col + 1) * COARSE_CELLS) / 16;
    const int qz = -108 + ((2 * row + 1) * COARSE_CELLS) / 16;
    const double coarseX = static_cast<double>(qx);
    const double coarseZ = static_cast<double>(qz);

    p20::JavaRandom rng;
    if (lane == 0) {
        rng.setSeed(seed);
        // Terrain state order: 16 noise2 + 16 noise3 + 8 noise1 + 8 skipped
        // states + 10 noise4 = 58 before noise5? The exact project ordering is
        // NOISE5_BASE=50 because the skipped 8 states are not materialized in
        // the 66-state cache. RNG stream, however, must consume them. To reach
        // the first noise5 octave we consume 16+16+8+8+10 = 58 Perlin streams.
        for (int i = 0; i < 58; ++i) consumePerlinRngOnly(rng);
    }
    __syncthreads();

    double noise5 = 0.0;
    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        if (lane == 0) p20::initPerlin(rng, perlin);
        __syncthreads();

        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        noise5 += p20::perlin2(perlin, coarseX * scale, coarseZ * scale) * weight;

        __syncthreads();
        amplitude /= 2.0;
    }

    const double d7 = heightCenterFromNoise5(noise5);
    // Sea-surface y=63 is coarse vertical coordinate 7+7/8 = 7.875.
    lowScratch[lane] = d7 < 7.875 ? 1 : 0;
    sumScratch[lane] = d7;
    __syncthreads();

    for (int stride = SCOUT_THREADS / 2; stride > 0; stride >>= 1) {
        if (lane < stride) {
            lowScratch[lane] += lowScratch[lane + stride];
            sumScratch[lane] += sumScratch[lane + stride];
        }
        __syncthreads();
    }

    if (lane == 0) {
        lowSampleCounts[seedIndex] = static_cast<unsigned short>(lowScratch[0]);
        heightSums[seedIndex] = sumScratch[0];
    }
}

// -------------------------------------------------------------------------
// Exact TU4 measurement
// -------------------------------------------------------------------------
// Exact terrain states follow the existing validated project ordering:
//   terrain[0..15]  = noise2 (16 octaves)
//   terrain[16..31] = noise3 (16)
//   terrain[32..39] = noise1 blend (8)
//   eight additional constructor Perlins are consumed but not stored
//   terrain[40..49] = noise4 shape (10)
//   terrain[50..65] = noise5 macro height (16)
// The exact evaluator computes the Beta density lattice only at y=7 and y=8,
// then reproduces the generator's vertical interpolation to block y=63 and its
// 4x4 horizontal interpolation. density>0 at y=63 is land; otherwise the base
// generator places water there. Thus record ranking is exactly minimum LAND
// footprint / maximum WATER footprint in the finite 864x864 world.

__device__ __forceinline__ double climateNoise4(
        const p20::PerlinState* states,
        double x,
        double z,
        double baseScale,
        double octaveScale
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 4; ++octave) {
        const double scale = (baseScale / 1.5) * d7;
        total += p20::simplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= octaveScale;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double climateNoise2(
        const p20::PerlinState* states,
        double x,
        double z
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 2; ++octave) {
        const double scale = (0.25 / 1.5) * d7;
        total += p20::simplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= 0.5882352941176471;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

__device__ __forceinline__ double densityAtY(
        double noise1,
        double noise2,
        double noise3,
        double d5,
        double d7,
        int y
) {
    double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
    if (d9 < 0.0) d9 *= 4.0;

    const double blend = (noise1 / 10.0 + 1.0) / 2.0;
    double d8;
    if (blend < 0.0) {
        d8 = noise2 / 512.0;
    } else if (blend > 1.0) {
        d8 = noise3 / 512.0;
    } else {
        const double d10 = noise2 / 512.0;
        const double d11 = noise3 / 512.0;
        d8 = d10 + (d11 - d10) * blend;
    }
    d8 -= d9;
    return d8;
}

__global__ void exactSeaDensityKernel(
        const p20::PerlinState* terrain,
        const p20::PerlinState* tempStates,
        const p20::PerlinState* rainStates,
        const p20::PerlinState* blendStates,
        double* seaDensity
) {
    const int idx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (idx >= COARSE_POINT_COUNT) return;

    const int ix = idx / COARSE_POINTS;
    const int iz = idx - ix * COARSE_POINTS;
    const double coarseX = static_cast<double>(-108 + ix);
    const double coarseZ = static_cast<double>(-108 + iz);
    const double climateX = coarseX * 4.0 + 2.0;
    const double climateZ = coarseZ * 4.0 + 2.0;

    const double tempRaw = climateNoise4(
            tempStates, climateX, climateZ,
            0.02500000037252903, 0.25);
    const double rainRaw = climateNoise4(
            rainStates, climateX, climateZ,
            0.05000000074505806, 0.3333333333333333);
    const double blendRaw = climateNoise2(blendStates, climateX, climateZ);

    const double climateBlend = blendRaw * 1.1 + 0.5;
    double temperature = (tempRaw * 0.15 + 0.7) * 0.99 + climateBlend * 0.01;
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + climateBlend * 0.002;
    temperature = 1.0 - (1.0 - temperature) * (1.0 - temperature);
    temperature = clamp01(temperature);
    rain = clamp01(rain);

    double n2y7 = 0.0, n2y8 = 0.0;
    double n3y7 = 0.0, n3y8 = 0.0;
    double n1y7 = 0.0, n1y8 = 0.0;
    double n4 = 0.0;
    double n5 = 0.0;

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[octave];
        n2y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n2y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[16 + octave];
        n3y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n3y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[32 + octave];
        n1y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n1y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        const double scale = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        n4 += p20::perlin2(
                terrain[40 + octave], coarseX * scale, coarseZ * scale) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        n5 += p20::perlin2(
                terrain[50 + octave], coarseX * scale, coarseZ * scale) * weight;
        amplitude /= 2.0;
    }

    const double d2 = temperature;
    const double d3 = rain * d2;
    double d4 = 1.0 - d3;
    d4 *= d4;
    d4 *= d4;
    d4 = 1.0 - d4;

    double d5 = (n4 + 256.0) / 512.0;
    d5 *= d4;
    if (d5 > 1.0) d5 = 1.0;

    double d6 = n5 / 8000.0;
    if (d6 < 0.0) d6 = -d6 * 0.3;
    d6 = d6 * 3.0 - 2.0;
    if (d6 < 0.0) {
        d6 /= 2.0;
        if (d6 < -1.0) d6 = -1.0;
        d6 /= 1.4;
        d6 /= 2.0;
        d5 = 0.0;
    } else {
        if (d6 > 1.0) d6 = 1.0;
        d6 /= 8.0;
    }
    if (d5 < 0.0) d5 = 0.0;
    d5 += 0.5;
    d6 *= 17.0 / 16.0;
    const double d7 = 17.0 / 2.0 + d6 * 4.0;

    const double density7 = densityAtY(n1y7, n2y7, n3y7, d5, d7, 7);
    const double density8 = densityAtY(n1y8, n2y8, n3y8, d5, d7, 8);

    // Beta's generator performs seven repeated += steps for y=63. Preserve
    // that operation order instead of algebraically collapsing the expression.
    const double step = (density8 - density7) * 0.125;
    double v = density7;
    for (int subY = 0; subY < 7; ++subY) v += step;
    seaDensity[idx] = v;
}

__global__ void countLandKernel(
        const double* seaDensity,
        int* landCount
) {
    const int cell = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (cell >= COARSE_CELL_COUNT) return;

    const int cx = cell / COARSE_CELLS;
    const int cz = cell - cx * COARSE_CELLS;
    const int row0 = cx * COARSE_POINTS;
    const int row1 = (cx + 1) * COARSE_POINTS;

    const double q00 = seaDensity[row0 + cz];
    const double q01 = seaDensity[row0 + cz + 1];
    const double q10 = seaDensity[row1 + cz];
    const double q11 = seaDensity[row1 + cz + 1];

    double x0 = q00;
    double x1 = q01;
    const double dx0 = (q10 - q00) * 0.25;
    const double dx1 = (q11 - q01) * 0.25;
    int localLand = 0;

    for (int subX = 0; subX < 4; ++subX) {
        double v = x0;
        const double dz = (x1 - x0) * 0.25;
        for (int subZ = 0; subZ < 4; ++subZ) {
            if (v > 0.0) ++localLand;
            v += dz;
        }
        x0 += dx0;
        x1 += dx1;
    }

    atomicAdd(landCount, localLand);
}

struct ExactWorkspace {
    p20::PerlinState* dTerrain = nullptr;
    p20::PerlinState* dTemp = nullptr;
    p20::PerlinState* dRain = nullptr;
    p20::PerlinState* dBlend = nullptr;
    double* dDensity = nullptr;
    int* dLand = nullptr;

    ExactWorkspace() {
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dTerrain),
                            TERRAIN_STATE_COUNT * sizeof(p20::PerlinState)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dTemp),
                            4 * sizeof(p20::PerlinState)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dRain),
                            4 * sizeof(p20::PerlinState)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dBlend),
                            2 * sizeof(p20::PerlinState)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dDensity),
                            COARSE_POINT_COUNT * sizeof(double)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dLand), sizeof(int)));
    }

    ~ExactWorkspace() {
        if (dTerrain) (void)hipFree(dTerrain);
        if (dTemp) (void)hipFree(dTemp);
        if (dRain) (void)hipFree(dRain);
        if (dBlend) (void)hipFree(dBlend);
        if (dDensity) (void)hipFree(dDensity);
        if (dLand) (void)hipFree(dLand);
    }
};

void buildExactStates(
        std::int64_t seed,
        std::vector<p20::PerlinState>& terrain,
        std::vector<p20::PerlinState>& temp,
        std::vector<p20::PerlinState>& rain,
        std::vector<p20::PerlinState>& blend
) {
    terrain.resize(TERRAIN_STATE_COUNT);
    temp.resize(4);
    rain.resize(4);
    blend.resize(2);

    p20::JavaRandom rng;
    p20::PerlinState scratch;

    rng.setSeed(multipliedSeed(seed, 9871ULL));
    for (int i = 0; i < 4; ++i) p20::initPerlin(rng, temp[i]);

    rng.setSeed(multipliedSeed(seed, 39811ULL));
    for (int i = 0; i < 4; ++i) p20::initPerlin(rng, rain[i]);

    rng.setSeed(multipliedSeed(seed, 543321ULL));
    for (int i = 0; i < 2; ++i) p20::initPerlin(rng, blend[i]);

    rng.setSeed(seed);
    for (int i = 0; i < 40; ++i) p20::initPerlin(rng, terrain[i]);
    for (int i = 0; i < 8; ++i) p20::initPerlin(rng, scratch); // legacy noise4 generator, unused here
    for (int i = 40; i < TERRAIN_STATE_COUNT; ++i) p20::initPerlin(rng, terrain[i]);
}

ExactWaterResult runExact(std::int64_t seed, ExactWorkspace& w) {
    std::vector<p20::PerlinState> terrain;
    std::vector<p20::PerlinState> temp;
    std::vector<p20::PerlinState> rain;
    std::vector<p20::PerlinState> blend;
    buildExactStates(seed, terrain, temp, rain, blend);

    HIP_CHECK(hipMemcpy(w.dTerrain, terrain.data(),
                        terrain.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(w.dTemp, temp.data(),
                        temp.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(w.dRain, rain.data(),
                        rain.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(w.dBlend, blend.data(),
                        blend.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice));

    const int pointBlocks = (COARSE_POINT_COUNT + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            exactSeaDensityKernel,
            dim3(pointBlocks), dim3(EXACT_THREADS), 0, 0,
            w.dTerrain, w.dTemp, w.dRain, w.dBlend, w.dDensity);
    HIP_CHECK(hipGetLastError());

    HIP_CHECK(hipMemset(w.dLand, 0, sizeof(int)));
    const int cellBlocks = (COARSE_CELL_COUNT + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            countLandKernel,
            dim3(cellBlocks), dim3(EXACT_THREADS), 0, 0,
            w.dDensity, w.dLand);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    int land = 0;
    HIP_CHECK(hipMemcpy(&land, w.dLand, sizeof(int), hipMemcpyDeviceToHost));
    ExactWaterResult result;
    result.landColumns = land;
    result.waterColumns = TOTAL_COLUMNS - land;
    return result;
}

std::uint64_t parseU64(const std::string& value, const char* name) {
    std::size_t used = 0;
    const unsigned long long parsed = std::stoull(value, &used, 0);
    if (used != value.size()) throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    return static_cast<std::uint64_t>(parsed);
}

std::int64_t parseI64(const std::string& value, const char* name) {
    std::size_t used = 0;
    const long long parsed = std::stoll(value, &used, 0);
    if (used != value.size()) throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    return static_cast<std::int64_t>(parsed);
}

int parseInt(const std::string& value, const char* name) {
    std::size_t used = 0;
    const long parsed = std::stol(value, &used, 0);
    if (used != value.size()) throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string("Out of range ") + name + ": " + value);
    }
    return static_cast<int>(parsed);
}

void printHelp() {
    std::cout
        << "TU4WaterFinder - Beta 1.7.3 / TU4 864x864 maximum-water search\n\n"
        << "Metric: minimize land columns at sea surface y=63 across all 746,496 columns.\n"
        << "Equivalently, maximize the base-generator water footprint.\n\n"
        << "Options:\n"
        << "  --batch N              Seeds per GPU scout batch (default 8192)\n"
        << "  --top-exact N          Exact-check N best scout seeds per batch (default 2)\n"
        << "  --sequence N           Reproducible SplitMix sequence key\n"
        << "  --start-attempt N      Resume attempt index (default 0)\n"
        << "  --max-attempts N       Stop after N attempts; 0 = unlimited\n"
        << "  --status-seconds X     Status interval (default 2.0)\n"
        << "  --log PATH             Record CSV (default tu4_water_hits.csv)\n"
        << "  --continue-after-hit   Continue even if an all-water world is found\n"
        << "  --verify-seed SEED     Exact-check one seed and exit\n"
        << "  --help                  Show this help\n";
}

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value for ") + name);
            return argv[++i];
        };
        if (a == "--batch") o.batch = parseInt(need("--batch"), "batch");
        else if (a == "--top-exact") o.topExact = parseInt(need("--top-exact"), "top-exact");
        else if (a == "--sequence") { o.sequence = parseU64(need("--sequence"), "sequence"); o.sequenceSpecified = true; }
        else if (a == "--start-attempt") o.startAttempt = parseU64(need("--start-attempt"), "start-attempt");
        else if (a == "--max-attempts") o.maxAttempts = parseU64(need("--max-attempts"), "max-attempts");
        else if (a == "--status-seconds") o.statusSeconds = std::stod(need("--status-seconds"));
        else if (a == "--log") o.logPath = need("--log");
        else if (a == "--continue-after-hit") o.continueAfterHit = true;
        else if (a == "--verify-seed") { o.verifySeed = parseI64(need("--verify-seed"), "verify-seed"); o.verifyOnly = true; }
        else if (a == "--help" || a == "-h") { printHelp(); std::exit(0); }
        else throw std::runtime_error("Unknown option: " + a);
    }
    if (o.batch <= 0) throw std::runtime_error("--batch must be > 0");
    if (o.topExact <= 0) throw std::runtime_error("--top-exact must be > 0");
    if (o.topExact > o.batch) o.topExact = o.batch;
    if (!(o.statusSeconds > 0.0)) throw std::runtime_error("--status-seconds must be > 0");
    return o;
}

void appendRecord(
        const Options& o,
        std::uint64_t attempt,
        std::int64_t seed,
        const ExactWaterResult& result,
        unsigned short scoutLow,
        double scoutSum
) {
    bool needsHeader = false;
    {
        std::ifstream check(o.logPath, std::ios::binary | std::ios::ate);
        needsHeader = !check.good() || check.tellg() == 0;
    }
    std::ofstream out(o.logPath, std::ios::app);
    if (!out) return;
    if (needsHeader) {
        out << "sequence,attempt,seed,land_columns,water_columns,water_percent,scout_low64,scout_d7_sum\n";
    }
    const double pct = 100.0 * static_cast<double>(result.waterColumns) / TOTAL_COLUMNS;
    out << o.sequence << ',' << attempt << ',' << seed << ','
        << result.landColumns << ',' << result.waterColumns << ','
        << std::fixed << std::setprecision(9) << pct << ','
        << scoutLow << ',' << std::setprecision(12) << scoutSum << '\n';
}

} // namespace tu4water

int main(int argc, char** argv) {
    using namespace tu4water;
    try {
        Options o = parseOptions(argc, argv);
        if (!o.sequenceSpecified) {
            o.sequence = static_cast<std::uint64_t>(
                    std::chrono::high_resolution_clock::now().time_since_epoch().count())
                    ^ 0x5455345741544552ULL;
        }

        int device = 0;
        HIP_CHECK(hipGetDevice(&device));
        hipDeviceProp_t prop{};
        HIP_CHECK(hipGetDeviceProperties(&prop, device));

        std::cout << "TU4 Water Finder | exact world=864x864 (54x54 chunks) | columns="
                  << TOTAL_COLUMNS << "\n";
        std::cout << "Objective: MIN land at y=63 / MAX water footprint.\n";
        std::cout << "Scout: 8x8 exact macro-height (noise5) lattice; exact terrain for top candidates.\n";
        std::cout << "GPU: " << prop.name << "\n";
        std::cout << "sequence=" << o.sequence
                  << " startAttempt=" << o.startAttempt
                  << " batch=" << o.batch
                  << " topExact=" << o.topExact << "\n";

        ExactWorkspace exactWorkspace;

        if (o.verifyOnly) {
            const ExactWaterResult r = runExact(o.verifySeed, exactWorkspace);
            const double pct = 100.0 * static_cast<double>(r.waterColumns) / TOTAL_COLUMNS;
            std::cout << "[VERIFY] seed=" << o.verifySeed
                      << " landColumns=" << r.landColumns
                      << " waterColumns=" << r.waterColumns << '/' << TOTAL_COLUMNS
                      << " waterPercent=" << std::fixed << std::setprecision(6) << pct << "%\n";
            return 0;
        }

        unsigned short* dLow = nullptr;
        double* dSum = nullptr;
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dLow),
                            static_cast<std::size_t>(o.batch) * sizeof(unsigned short)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSum),
                            static_cast<std::size_t>(o.batch) * sizeof(double)));
        std::vector<unsigned short> hLow(static_cast<std::size_t>(o.batch));
        std::vector<double> hSum(static_cast<std::size_t>(o.batch));
        std::vector<int> indices(static_cast<std::size_t>(o.batch));

        std::uint64_t nextAttempt = o.startAttempt;
        std::uint64_t checked = 0;
        int bestLand = TOTAL_COLUMNS + 1;
        std::int64_t bestSeed = 0;
        int bestWater = -1;
        unsigned short globalScoutLow = 0;
        double globalScoutSum = std::numeric_limits<double>::infinity();
        bool stop = false;

        const auto started = std::chrono::steady_clock::now();
        auto lastStatus = started;

        while (!stop) {
            int count = o.batch;
            if (o.maxAttempts != 0) {
                const std::uint64_t remaining = o.maxAttempts - checked;
                if (remaining == 0) break;
                if (remaining < static_cast<std::uint64_t>(count)) count = static_cast<int>(remaining);
            }

            hipLaunchKernelGGL(
                    waterScoutKernel,
                    dim3(count), dim3(SCOUT_THREADS), 0, 0,
                    o.sequence, nextAttempt, count, dLow, dSum);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipMemcpy(hLow.data(), dLow,
                                static_cast<std::size_t>(count) * sizeof(unsigned short),
                                hipMemcpyDeviceToHost));
            HIP_CHECK(hipMemcpy(hSum.data(), dSum,
                                static_cast<std::size_t>(count) * sizeof(double),
                                hipMemcpyDeviceToHost));

            indices.resize(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) indices[static_cast<std::size_t>(i)] = i;
            const int exactN = std::min(o.topExact, count);
            auto betterScout = [&](int a, int b) {
                if (hLow[a] != hLow[b]) return hLow[a] > hLow[b];
                return hSum[a] < hSum[b];
            };
            if (exactN < count) {
                std::partial_sort(indices.begin(), indices.begin() + exactN, indices.end(), betterScout);
            } else {
                std::sort(indices.begin(), indices.end(), betterScout);
            }

            const int batchBest = indices[0];
            if (hLow[batchBest] > globalScoutLow ||
                (hLow[batchBest] == globalScoutLow && hSum[batchBest] < globalScoutSum)) {
                globalScoutLow = hLow[batchBest];
                globalScoutSum = hSum[batchBest];
                const std::uint64_t a = nextAttempt + static_cast<std::uint64_t>(batchBest);
                const std::int64_t s = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(o.sequence, a));
                std::cout << "[SCOUT RECORD] seed=" << s
                          << " lowHeightSamples=" << globalScoutLow << "/64"
                          << " d7Sum=" << std::fixed << std::setprecision(3) << globalScoutSum << "\n";
            }

            for (int rank = 0; rank < exactN; ++rank) {
                const int idx = indices[static_cast<std::size_t>(rank)];
                const std::uint64_t attempt = nextAttempt + static_cast<std::uint64_t>(idx);
                const std::int64_t seed = static_cast<std::int64_t>(
                        p20::splitMixDeterministicSeed(o.sequence, attempt));
                const ExactWaterResult r = runExact(seed, exactWorkspace);
                if (r.landColumns < bestLand) {
                    bestLand = r.landColumns;
                    bestWater = r.waterColumns;
                    bestSeed = seed;
                    const double pct = 100.0 * static_cast<double>(r.waterColumns) / TOTAL_COLUMNS;
                    std::cout << "[RECORD] seed=" << seed
                              << " landColumns=" << r.landColumns
                              << " waterColumns=" << r.waterColumns << '/' << TOTAL_COLUMNS
                              << " waterPercent=" << std::fixed << std::setprecision(6) << pct << "%"
                              << " scoutLow=" << hLow[idx] << "/64\n";
                    appendRecord(o, attempt, seed, r, hLow[idx], hSum[idx]);
                    if (r.landColumns == 0) {
                        std::cout << "[JACKPOT] Entire 864x864 TU4 terrain footprint is water at sea surface.\n";
                        if (!o.continueAfterHit) stop = true;
                    }
                }
            }

            nextAttempt += static_cast<std::uint64_t>(count);
            checked += static_cast<std::uint64_t>(count);

            const auto now = std::chrono::steady_clock::now();
            const double sinceStatus = std::chrono::duration<double>(now - lastStatus).count();
            if (sinceStatus >= o.statusSeconds || stop) {
                const double elapsed = std::chrono::duration<double>(now - started).count();
                const double rate = elapsed > 0.0 ? static_cast<double>(checked) / elapsed : 0.0;
                std::cout << "checked=" << checked
                          << " rate=" << std::fixed << std::setprecision(1) << rate << " seeds/s"
                          << " nextAttempt=" << nextAttempt
                          << " scoutBest=" << globalScoutLow << "/64";
                if (bestLand <= TOTAL_COLUMNS) {
                    const double pct = 100.0 * static_cast<double>(bestWater) / TOTAL_COLUMNS;
                    std::cout << " bestLand=" << bestLand
                              << " bestWater=" << bestWater
                              << " waterPercent=" << std::setprecision(6) << pct << "%"
                              << " bestSeed=" << bestSeed;
                }
                std::cout << " elapsed=" << std::setprecision(1) << elapsed << "s\n";
                lastStatus = now;
            }
        }

        (void)hipFree(dLow);
        (void)hipFree(dSum);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
