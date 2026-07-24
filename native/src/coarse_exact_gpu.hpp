#pragma once

#include "gpu_runtime_compat.hpp"
#include "coarse_exact_core.hpp"

#include <cstddef>
#include <cstdint>

namespace coarsegpu {

static constexpr int THREADS = 256;

struct PerlinAxisCache {
    int xPerm0[coarsecore::SIZE];
    int xPerm1[coarsecore::SIZE];
    int zIndex[coarsecore::SIZE];
    double xFrac[coarsecore::SIZE];
    double xFracMinus1[coarsecore::SIZE];
    double xFade[coarsecore::SIZE];
    double zFrac[coarsecore::SIZE];
    double zFracMinus1[coarsecore::SIZE];
    double zFade[coarsecore::SIZE];

    // The 3D Perlin grids always use the same 17 Y coordinates for every X/Z
    // column in an octave. Cache those exact Java-style floor/fraction/fade
    // results once per octave instead of recalculating them 3,721 times.
    int yIndex[coarsecore::Y_LEVELS];
    double yFrac[coarsecore::Y_LEVELS];
    double yFracMinus1[coarsecore::Y_LEVELS];
    double yFade[coarsecore::Y_LEVELS];
};

__device__ __forceinline__ void preparePerlinAxes(
        const p20::PerlinState& p,
        double scaleX,
        double scaleZ,
        int coarseOffsetX,
        int coarseOffsetZ,
        PerlinAxisCache& cache
) {
    const int lane = threadIdx.x;
    if (lane < coarsecore::SIZE) {
        const double coarseX = static_cast<double>(coarsecore::FROM_COARSE + lane + coarseOffsetX);
        const double coarseZ = static_cast<double>(coarsecore::FROM_COARSE + lane + coarseOffsetZ);

        double x = coarseX * scaleX + p.a;
        const int fx = p20::javaFloor(x);
        const int xi = fx & 255;
        x -= static_cast<double>(fx);
        cache.xPerm0[lane] = p.perm[xi];
        cache.xPerm1[lane] = p.perm[xi + 1];
        cache.xFrac[lane] = x;
        cache.xFracMinus1[lane] = x - 1.0;
        cache.xFade[lane] = p20::fade(x);

        double z = coarseZ * scaleZ + p.c;
        const int fz = p20::javaFloor(z);
        z -= static_cast<double>(fz);
        cache.zIndex[lane] = fz & 255;
        cache.zFrac[lane] = z;
        cache.zFracMinus1[lane] = z - 1.0;
        cache.zFade[lane] = p20::fade(z);
    }
    __syncthreads();
}

__device__ __forceinline__ void preparePerlinAxes(
        const p20::PerlinState& p,
        double scaleX,
        double scaleZ,
        PerlinAxisCache& cache
) {
    preparePerlinAxes(p, scaleX, scaleZ, 0, 0, cache);
}

__device__ __forceinline__ void preparePerlinAxes3D(
        const p20::PerlinState& p,
        double scaleX,
        double scaleY,
        double scaleZ,
        int coarseOffsetX,
        int coarseOffsetZ,
        PerlinAxisCache& cache
) {
    const int lane = threadIdx.x;

    if (lane < coarsecore::SIZE) {
        const double coarseX = static_cast<double>(coarsecore::FROM_COARSE + lane + coarseOffsetX);
        const double coarseZ = static_cast<double>(coarsecore::FROM_COARSE + lane + coarseOffsetZ);

        double x = coarseX * scaleX + p.a;
        const int fx = p20::javaFloor(x);
        const int xi = fx & 255;
        x -= static_cast<double>(fx);
        cache.xPerm0[lane] = p.perm[xi];
        cache.xPerm1[lane] = p.perm[xi + 1];
        cache.xFrac[lane] = x;
        cache.xFracMinus1[lane] = x - 1.0;
        cache.xFade[lane] = p20::fade(x);

        double z = coarseZ * scaleZ + p.c;
        const int fz = p20::javaFloor(z);
        z -= static_cast<double>(fz);
        cache.zIndex[lane] = fz & 255;
        cache.zFrac[lane] = z;
        cache.zFracMinus1[lane] = z - 1.0;
        cache.zFade[lane] = p20::fade(z);
    }

    if (lane < coarsecore::Y_LEVELS) {
        double y = static_cast<double>(lane) * scaleY + p.b;
        const int fy = p20::javaFloor(y);
        y -= static_cast<double>(fy);
        cache.yIndex[lane] = fy & 255;
        cache.yFrac[lane] = y;
        cache.yFracMinus1[lane] = y - 1.0;
        cache.yFade[lane] = p20::fade(y);
    }

    __syncthreads();
}

__device__ __forceinline__ void preparePerlinAxes3D(
        const p20::PerlinState& p,
        double scaleX,
        double scaleY,
        double scaleZ,
        PerlinAxisCache& cache
) {
    preparePerlinAxes3D(p, scaleX, scaleY, scaleZ, 0, 0, cache);
}

// Exact branchless form of the classic improved-Perlin grad3 dispatch.
//
// The old 16-case switch is mathematically exact, but the hash is effectively
// random across lanes. On a wavefront that can force many divergent switch
// paths. This form preserves the same operand order/signs for all 16 cases and
// lets the compiler lower the choices to predicated selects instead.
__device__ __forceinline__ double grad3BranchlessExact(
        int hash,
        double x,
        double y,
        double z
) {
    const int h = hash & 15;
    const double u = (h < 8) ? x : y;
    const double v = (h < 4) ? y : ((h == 12 || h == 14) ? x : z);
    const double su = ((h & 1) == 0) ? u : -u;
    const double sv = ((h & 2) == 0) ? v : -v;
    return su + sv;
}

__device__ __forceinline__ void perlin3Full17CachedXYZ(
        const p20::PerlinState& p,
        const PerlinAxisCache& cache,
        int xIndex,
        int zIndex,
        double out17[coarsecore::Y_LEVELS]
) {
    const double x = cache.xFrac[xIndex];
    const double x1 = cache.xFracMinus1[xIndex];
    const double xf = cache.xFade[xIndex];
    const double z = cache.zFrac[zIndex];
    const double z1 = cache.zFracMinus1[zIndex];
    const double zf = cache.zFade[zIndex];
    const int zi = cache.zIndex[zIndex];
    const int xp0 = cache.xPerm0[xIndex];
    const int xp1 = cache.xPerm1[xIndex];

    int previousYi = -2147483647;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
        const int yi = cache.yIndex[y];
        const double yf = cache.yFrac[y];
        const double yfm1 = cache.yFracMinus1[y];
        const double yfade = cache.yFade[y];
        if (y == 0 || yi != previousYi) {
            previousYi = yi;
            const int p0 = xp0 + yi;
            const int p00 = p.perm[p0] + zi;
            const int p01 = p.perm[p0 + 1] + zi;
            const int p1 = xp1 + yi;
            const int p10 = p.perm[p1] + zi;
            const int p11 = p.perm[p1 + 1] + zi;
            q00 = p20::lerp(xf, grad3BranchlessExact(p.perm[p00], x, yf, z), grad3BranchlessExact(p.perm[p10], x1, yf, z));
            q01 = p20::lerp(xf, grad3BranchlessExact(p.perm[p01], x, yfm1, z), grad3BranchlessExact(p.perm[p11], x1, yfm1, z));
            q10 = p20::lerp(xf, grad3BranchlessExact(p.perm[p00 + 1], x, yf, z1), grad3BranchlessExact(p.perm[p10 + 1], x1, yf, z1));
            q11 = p20::lerp(xf, grad3BranchlessExact(p.perm[p01 + 1], x, yfm1, z1), grad3BranchlessExact(p.perm[p11 + 1], x1, yfm1, z1));
        }
        const double r0 = p20::lerp(yfade, q00, q01);
        const double r1 = p20::lerp(yfade, q10, q11);
        out17[y] = p20::lerp(zf, r0, r1);
    }
}

__device__ __forceinline__ double perlin2CachedXZ(
        const p20::PerlinState& p,
        const PerlinAxisCache& cache,
        int xIndex,
        int zIndex
) {
    const double x = cache.xFrac[xIndex];
    const double x1 = cache.xFracMinus1[xIndex];
    const double xf = cache.xFade[xIndex];
    const double z = cache.zFrac[zIndex];
    const double z1 = cache.zFracMinus1[zIndex];
    const double zf = cache.zFade[zIndex];
    const int zi = cache.zIndex[zIndex];
    const int xp0 = cache.xPerm0[xIndex];
    const int xp1 = cache.xPerm1[xIndex];
    const int p00 = p.perm[xp0] + zi;
    const int p10 = p.perm[xp1] + zi;

    const double q0 = p20::lerp(xf,
            p20::grad2Legacy(p.perm[p00], x, z),
            p20::grad3(p.perm[p10], x1, 0.0, z));
    const double q1 = p20::lerp(xf,
            p20::grad3(p.perm[p00 + 1], x, 0.0, z1),
            p20::grad3(p.perm[p10 + 1], x1, 0.0, z1));
    return p20::lerp(zf, q0, q1);
}

struct Buffers {
    int capacity = 0;
    std::int64_t* seeds = nullptr;
    int* cacheSeedIndices = nullptr;
    double* temp = nullptr;
    double* rain = nullptr;
    double* climateBlend = nullptr;
    double* noise1 = nullptr;
    double* noise2 = nullptr;
    double* noise3 = nullptr;
    double* noise4 = nullptr;
    double* noise5 = nullptr;
    unsigned char* signs = nullptr;
    int* labels = nullptr;
    int* queue = nullptr;
    int* columnSeen = nullptr;
    int* columnMinY = nullptr;
    int* componentColumns = nullptr;
    int* scores = nullptr;
};

__device__ __forceinline__ void coordinates(
        int column,
        int coarseOffsetX,
        int coarseOffsetZ,
        double& coarseX,
        double& coarseZ,
        double& climateX,
        double& climateZ
) {
    const int x = column / coarsecore::SIZE;
    const int z = column - x * coarsecore::SIZE;
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
}

__device__ __forceinline__ void coordinates(
        int column,
        double& coarseX,
        double& coarseZ,
        double& climateX,
        double& climateZ
) {
    coordinates(column, 0, 0, coarseX, coarseZ, climateX, climateZ);
}

static constexpr int COARSE_PROFILE_PHASES = 6;
enum CoarseProfilePhase {
    COARSE_PROFILE_CLIMATE = 0,
    COARSE_PROFILE_NOISE2 = 1,
    COARSE_PROFILE_NOISE3 = 2,
    COARSE_PROFILE_NOISE1 = 3,
    COARSE_PROFILE_NOISE45 = 4,
    COARSE_PROFILE_TERRAIN = 5
};

// Optional deep profiler for the two 16-octave density-noise phases.
// The production kernel never touches these counters.
static constexpr int COARSE_NOISE_DETAIL_OCTAVES = 16;
static constexpr int COARSE_NOISE_DETAIL_SERIES = 4;
static constexpr int COARSE_NOISE_DETAIL_VALUES = COARSE_NOISE_DETAIL_OCTAVES * COARSE_NOISE_DETAIL_SERIES;
enum CoarseNoiseDetailSeries {
    COARSE_DETAIL_NOISE2_SETUP = 0,
    COARSE_DETAIL_NOISE2_EVAL = 1,
    COARSE_DETAIL_NOISE3_SETUP = 2,
    COARSE_DETAIL_NOISE3_EVAL = 3
};

__device__ __forceinline__ std::size_t coarseNoiseDetailIndex(int seedIndex, int series, int octave) {
    return static_cast<std::size_t>(seedIndex) * COARSE_NOISE_DETAIL_VALUES
            + static_cast<std::size_t>(series) * COARSE_NOISE_DETAIL_OCTAVES
            + static_cast<std::size_t>(octave);
}

__global__ void generateCoarseSignsKernel(
        const std::int64_t* seeds,
        int count,
        double* temp,
        double* rain,
        double* climateBlend,
        double* noise1,
        double* noise2,
        double* noise3,
        double* noise4,
        double* noise5,
        unsigned char* signs,
        int coarseOffsetX,
        int coarseOffsetZ,
        const int* cacheSeedIndices,
        const unsigned char* terrainPermutationCache,
        const double* terrainOffsetCache,
        const unsigned char* climatePermutationCache,
        const double* climateOffsetCache
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count) return;

    __shared__ p20::JavaRandom rng;
    __shared__ p20::PerlinState perlin;
    __shared__ PerlinAxisCache axes;
    const std::int64_t seed = seeds[seedIndex];
    const int cacheSeedIndex = cacheSeedIndices != nullptr ? cacheSeedIndices[seedIndex] : seedIndex;
    const std::size_t colBase = static_cast<std::size_t>(seedIndex) * coarsecore::COLUMNS;
    const std::size_t cellBase = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;

    const std::int64_t climateSeeds[3] = {
            static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL),
            static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL),
            static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL)
    };
    const int climateOctaves[3] = {4, 4, 2};
    const double startScaleX[3] = {0.02500000037252903, 0.05000000074505806, 0.25};
    const double startScaleZ[3] = {0.02500000037252903, 0.05000000074505806, 0.25};
    const double octaveScale[3] = {0.25, 0.3333333333333333, 0.5882352941176471};
    double* climateOut[3] = {temp, rain, climateBlend};

    for (int kind = 0; kind < 3; ++kind) {
        if (climatePermutationCache == nullptr && lane == 0) rng.setSeed(climateSeeds[kind]);
        __syncthreads();
        double d6 = 1.0;
        double d7 = 1.0;
        for (int octave = 0; octave < climateOctaves[kind]; ++octave) {
            if (climatePermutationCache != nullptr) {
                const int stateBase = kind == 0 ? climatecache::TEMP_BASE
                        : (kind == 1 ? climatecache::RAIN_BASE : climatecache::BLEND_BASE);
                climatecache::loadStateCooperative(
                        perlin, cacheSeedIndex, stateBase + octave,
                        climatePermutationCache, climateOffsetCache, lane, blockDim.x);
            } else {
                if (lane == 0) p20::initPerlin(rng, perlin);
                __syncthreads();
            }
            const double sx = (startScaleX[kind] / 1.5) * d7;
            const double sz = (startScaleZ[kind] / 1.5) * d7;
            const double weight = 0.55 / d6;
            for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
                double coarseX, coarseZ, climateX, climateZ;
                coordinates(column, coarseOffsetX, coarseOffsetZ, coarseX, coarseZ, climateX, climateZ);
                const double value = p20::simplex2(perlin, climateX * sx, climateZ * sz) * weight;
                if (octave == 0) climateOut[kind][colBase + column] = value;
                else climateOut[kind][colBase + column] += value;
            }
            __syncthreads();
            d7 *= octaveScale[kind];
            d6 *= 0.5;
        }
    }

    for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
        const std::size_t c = colBase + column;
        const double d0 = climateBlend[c] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (temp[c] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (rain[c] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        temp[c] = d3;
        rain[c] = d4;
    }
    __syncthreads();

    if (terrainPermutationCache == nullptr && lane == 0) rng.setSeed(seed);
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        if (terrainPermutationCache != nullptr) {
            terraincache::loadStateCooperative(
                    perlin, cacheSeedIndex, terraincache::NOISE2_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        } else {
            if (lane == 0) p20::initPerlin(rng, perlin);
            __syncthreads();
        }
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes3D(perlin, sx, sy, sz, coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            perlin3Full17CachedXYZ(perlin, axes, xIndex, zIndex, values);
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                const double value = values[y] * weight;
                if (octave == 0) noise2[base + y] = value;
                else noise2[base + y] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        if (terrainPermutationCache != nullptr) {
            terraincache::loadStateCooperative(
                    perlin, cacheSeedIndex, terraincache::NOISE3_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        } else {
            if (lane == 0) p20::initPerlin(rng, perlin);
            __syncthreads();
        }
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes3D(perlin, sx, sy, sz, coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            perlin3Full17CachedXYZ(perlin, axes, xIndex, zIndex, values);
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                const double value = values[y] * weight;
                if (octave == 0) noise3[base + y] = value;
                else noise3[base + y] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        if (terrainPermutationCache != nullptr) {
            terraincache::loadStateCooperative(
                    perlin, cacheSeedIndex, terraincache::NOISE1_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        } else {
            if (lane == 0) p20::initPerlin(rng, perlin);
            __syncthreads();
        }
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes3D(perlin, sx, sy, sz, coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            perlin3Full17CachedXYZ(perlin, axes, xIndex, zIndex, values);
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                const double value = values[y] * weight;
                if (octave == 0) noise1[base + y] = value;
                else noise1[base + y] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    if (terrainPermutationCache == nullptr && lane == 0) {
        for (int i = 0; i < 8; ++i) p20::consumePerlin(rng, perlin);
    }
    __syncthreads();

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        if (terrainPermutationCache != nullptr) {
            terraincache::loadStateCooperative(
                    perlin, cacheSeedIndex, terraincache::NOISE4_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        } else {
            if (lane == 0) p20::initPerlin(rng, perlin);
            __syncthreads();
        }
        const double scale = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes(perlin, scale, scale, coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            const double value = perlin2CachedXZ(perlin, axes, xIndex, zIndex) * weight;
            if (octave == 0) noise4[colBase + column] = value;
            else noise4[colBase + column] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        if (terrainPermutationCache != nullptr) {
            terraincache::loadStateCooperative(
                    perlin, cacheSeedIndex, terraincache::NOISE5_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        } else {
            if (lane == 0) p20::initPerlin(rng, perlin);
            __syncthreads();
        }
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes(perlin, scale, scale, coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            const double value = perlin2CachedXZ(perlin, axes, xIndex, zIndex) * weight;
            if (octave == 0) noise5[colBase + column] = value;
            else noise5[colBase + column] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
        const std::size_t c = colBase + column;
        const double d2 = temp[c];
        const double d3 = rain[c] * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;
        double d5 = (noise4[c] + 256.0) / 512.0;
        d5 *= d4;
        if (d5 > 1.0) d5 = 1.0;
        double d6 = noise5[c] / 8000.0;
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
        d6 = d6 * static_cast<double>(coarsecore::Y_LEVELS) / 16.0;
        const double d7 = static_cast<double>(coarsecore::Y_LEVELS) / 2.0 + d6 * 4.0;
        const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
        for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
            double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
            if (d9 < 0.0) d9 *= 4.0;
            const double blend = (noise1[base + y] / 10.0 + 1.0) / 2.0;
            double d8;
            if (blend < 0.0) d8 = noise2[base + y] / 512.0;
            else if (blend > 1.0) d8 = noise3[base + y] / 512.0;
            else {
                const double d10 = noise2[base + y] / 512.0;
                const double d11 = noise3[base + y] / 512.0;
                d8 = d10 + (d11 - d10) * blend;
            }
            d8 -= d9;
            if (y > coarsecore::Y_LEVELS - 4) {
                const double d13 = static_cast<double>(static_cast<float>(y - (coarsecore::Y_LEVELS - 4)) / 3.0F);
                d8 = d8 * (1.0 - d13) + -10.0 * d13;
            }
            signs[base + y] = d8 > 0.0 ? 1 : 0;
        }
    }
}

__global__ void generateCoarseSignsProfileKernel(
        const std::int64_t* seeds,
        int count,
        double* temp,
        double* rain,
        double* climateBlend,
        double* noise1,
        double* noise2,
        double* noise3,
        double* noise4,
        double* noise5,
        unsigned char* signs,
        unsigned long long* phaseTicks,
        unsigned long long* noiseDetailTicks
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count) return;

    __shared__ p20::JavaRandom rng;
    __shared__ p20::PerlinState perlin;
    __shared__ PerlinAxisCache axes;
    const std::int64_t seed = seeds[seedIndex];
    const std::size_t colBase = static_cast<std::size_t>(seedIndex) * coarsecore::COLUMNS;
    const std::size_t cellBase = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;

    unsigned long long phaseStart = 0;
    __syncthreads();
    if (lane == 0) phaseStart = wall_clock64();
    __syncthreads();

    const std::int64_t climateSeeds[3] = {
            static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL),
            static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL),
            static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL)
    };
    const int climateOctaves[3] = {4, 4, 2};
    const double startScaleX[3] = {0.02500000037252903, 0.05000000074505806, 0.25};
    const double startScaleZ[3] = {0.02500000037252903, 0.05000000074505806, 0.25};
    const double octaveScale[3] = {0.25, 0.3333333333333333, 0.5882352941176471};
    double* climateOut[3] = {temp, rain, climateBlend};

    for (int kind = 0; kind < 3; ++kind) {
        if (lane == 0) rng.setSeed(climateSeeds[kind]);
        __syncthreads();
        double d6 = 1.0;
        double d7 = 1.0;
        for (int octave = 0; octave < climateOctaves[kind]; ++octave) {
            if (lane == 0) p20::initPerlin(rng, perlin);
            __syncthreads();
            const double sx = (startScaleX[kind] / 1.5) * d7;
            const double sz = (startScaleZ[kind] / 1.5) * d7;
            const double weight = 0.55 / d6;
            for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
                double coarseX, coarseZ, climateX, climateZ;
                coordinates(column, coarseX, coarseZ, climateX, climateZ);
                const double value = p20::simplex2(perlin, climateX * sx, climateZ * sz) * weight;
                if (octave == 0) climateOut[kind][colBase + column] = value;
                else climateOut[kind][colBase + column] += value;
            }
            __syncthreads();
            d7 *= octaveScale[kind];
            d6 *= 0.5;
        }
    }

    for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
        const std::size_t c = colBase + column;
        const double d0 = climateBlend[c] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (temp[c] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (rain[c] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        temp[c] = d3;
        rain[c] = d4;
    }
    __syncthreads();

    __syncthreads();
    if (lane == 0) {
        const unsigned long long phaseEnd = wall_clock64();
        phaseTicks[static_cast<std::size_t>(seedIndex) * COARSE_PROFILE_PHASES + COARSE_PROFILE_CLIMATE] = phaseEnd - phaseStart;
        phaseStart = phaseEnd;
    }
    __syncthreads();

    if (lane == 0) rng.setSeed(seed);
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0;
        unsigned long long detailAxisEnd = 0;
        if (noiseDetailTicks != nullptr) {
            if (lane == 0) detailStart = wall_clock64();
            __syncthreads();
        }
        if (lane == 0) p20::initPerlin(rng, perlin);
        __syncthreads();
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes3D(perlin, sx, sy, sz, axes);

        if (noiseDetailTicks != nullptr) {
            if (lane == 0) {
                detailAxisEnd = wall_clock64();
                noiseDetailTicks[coarseNoiseDetailIndex(seedIndex, COARSE_DETAIL_NOISE2_SETUP, octave)] = detailAxisEnd - detailStart;
            }
            __syncthreads();
        }
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            perlin3Full17CachedXYZ(perlin, axes, xIndex, zIndex, values);
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                const double value = values[y] * weight;
                if (octave == 0) noise2[base + y] = value;
                else noise2[base + y] += value;
            }
        }
        __syncthreads();

        if (noiseDetailTicks != nullptr) {
            if (lane == 0) {
                const unsigned long long detailEnd = wall_clock64();
                noiseDetailTicks[coarseNoiseDetailIndex(seedIndex, COARSE_DETAIL_NOISE2_EVAL, octave)] = detailEnd - detailAxisEnd;
            }
            __syncthreads();
        }
        amplitude /= 2.0;
    }

    __syncthreads();
    if (lane == 0) {
        const unsigned long long phaseEnd = wall_clock64();
        phaseTicks[static_cast<std::size_t>(seedIndex) * COARSE_PROFILE_PHASES + COARSE_PROFILE_NOISE2] = phaseEnd - phaseStart;
        phaseStart = phaseEnd;
    }
    __syncthreads();

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0;
        unsigned long long detailAxisEnd = 0;
        if (noiseDetailTicks != nullptr) {
            if (lane == 0) detailStart = wall_clock64();
            __syncthreads();
        }
        if (lane == 0) p20::initPerlin(rng, perlin);
        __syncthreads();
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes3D(perlin, sx, sy, sz, axes);

        if (noiseDetailTicks != nullptr) {
            if (lane == 0) {
                detailAxisEnd = wall_clock64();
                noiseDetailTicks[coarseNoiseDetailIndex(seedIndex, COARSE_DETAIL_NOISE3_SETUP, octave)] = detailAxisEnd - detailStart;
            }
            __syncthreads();
        }
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            perlin3Full17CachedXYZ(perlin, axes, xIndex, zIndex, values);
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                const double value = values[y] * weight;
                if (octave == 0) noise3[base + y] = value;
                else noise3[base + y] += value;
            }
        }
        __syncthreads();

        if (noiseDetailTicks != nullptr) {
            if (lane == 0) {
                const unsigned long long detailEnd = wall_clock64();
                noiseDetailTicks[coarseNoiseDetailIndex(seedIndex, COARSE_DETAIL_NOISE3_EVAL, octave)] = detailEnd - detailAxisEnd;
            }
            __syncthreads();
        }
        amplitude /= 2.0;
    }

    __syncthreads();
    if (lane == 0) {
        const unsigned long long phaseEnd = wall_clock64();
        phaseTicks[static_cast<std::size_t>(seedIndex) * COARSE_PROFILE_PHASES + COARSE_PROFILE_NOISE3] = phaseEnd - phaseStart;
        phaseStart = phaseEnd;
    }
    __syncthreads();

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        if (lane == 0) p20::initPerlin(rng, perlin);
        __syncthreads();
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes3D(perlin, sx, sy, sz, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            perlin3Full17CachedXYZ(perlin, axes, xIndex, zIndex, values);
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                const double value = values[y] * weight;
                if (octave == 0) noise1[base + y] = value;
                else noise1[base + y] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    if (lane == 0) {
        for (int i = 0; i < 8; ++i) p20::consumePerlin(rng, perlin);
    }
    __syncthreads();

    __syncthreads();
    if (lane == 0) {
        const unsigned long long phaseEnd = wall_clock64();
        phaseTicks[static_cast<std::size_t>(seedIndex) * COARSE_PROFILE_PHASES + COARSE_PROFILE_NOISE1] = phaseEnd - phaseStart;
        phaseStart = phaseEnd;
    }
    __syncthreads();

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        if (lane == 0) p20::initPerlin(rng, perlin);
        __syncthreads();
        const double scale = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes(perlin, scale, scale, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            const double value = perlin2CachedXZ(perlin, axes, xIndex, zIndex) * weight;
            if (octave == 0) noise4[colBase + column] = value;
            else noise4[colBase + column] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        if (lane == 0) p20::initPerlin(rng, perlin);
        __syncthreads();
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        preparePerlinAxes(perlin, scale, scale, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            const double value = perlin2CachedXZ(perlin, axes, xIndex, zIndex) * weight;
            if (octave == 0) noise5[colBase + column] = value;
            else noise5[colBase + column] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    __syncthreads();
    if (lane == 0) {
        const unsigned long long phaseEnd = wall_clock64();
        phaseTicks[static_cast<std::size_t>(seedIndex) * COARSE_PROFILE_PHASES + COARSE_PROFILE_NOISE45] = phaseEnd - phaseStart;
        phaseStart = phaseEnd;
    }
    __syncthreads();

    for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
        const std::size_t c = colBase + column;
        const double d2 = temp[c];
        const double d3 = rain[c] * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;
        double d5 = (noise4[c] + 256.0) / 512.0;
        d5 *= d4;
        if (d5 > 1.0) d5 = 1.0;
        double d6 = noise5[c] / 8000.0;
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
        d6 = d6 * static_cast<double>(coarsecore::Y_LEVELS) / 16.0;
        const double d7 = static_cast<double>(coarsecore::Y_LEVELS) / 2.0 + d6 * 4.0;
        const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
        for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
            double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
            if (d9 < 0.0) d9 *= 4.0;
            const double blend = (noise1[base + y] / 10.0 + 1.0) / 2.0;
            double d8;
            if (blend < 0.0) d8 = noise2[base + y] / 512.0;
            else if (blend > 1.0) d8 = noise3[base + y] / 512.0;
            else {
                const double d10 = noise2[base + y] / 512.0;
                const double d11 = noise3[base + y] / 512.0;
                d8 = d10 + (d11 - d10) * blend;
            }
            d8 -= d9;
            if (y > coarsecore::Y_LEVELS - 4) {
                const double d13 = static_cast<double>(static_cast<float>(y - (coarsecore::Y_LEVELS - 4)) / 3.0F);
                d8 = d8 * (1.0 - d13) + -10.0 * d13;
            }
            signs[base + y] = d8 > 0.0 ? 1 : 0;
        }
    }
    __syncthreads();
    if (lane == 0) {
        const unsigned long long phaseEnd = wall_clock64();
        phaseTicks[static_cast<std::size_t>(seedIndex) * COARSE_PROFILE_PHASES + COARSE_PROFILE_TERRAIN] = phaseEnd - phaseStart;
    }
}

__global__ void scoreCoarseSignsKernel(
        const unsigned char* signs,
        int count,
        int* labels,
        int* queue,
        int* columnSeen,
        int* columnMinY,
        int* componentColumns,
        int* scores
) {
    const int seedIndex = blockIdx.x;
    if (seedIndex >= count || threadIdx.x != 0) return;

    const std::size_t cellBase = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;
    const std::size_t colBase = static_cast<std::size_t>(seedIndex) * coarsecore::COLUMNS;
    int* seedLabels = labels + cellBase;
    int* seedQueue = queue + cellBase;
    int* seedColumnSeen = columnSeen + colBase;
    int* seedColumnMinY = columnMinY + colBase;
    int* seedComponentColumns = componentColumns + colBase;
    const unsigned char* seedSigns = signs + cellBase;

    for (int i = 0; i < coarsecore::CELLS; ++i) seedLabels[i] = 0;
    for (int i = 0; i < coarsecore::COLUMNS; ++i) seedColumnSeen[i] = 0;

    int best = 0;
    int nextId = 1;
    for (int x = 0; x < coarsecore::SIZE; ++x) {
        for (int z = 0; z < coarsecore::SIZE; ++z) {
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                const int start = coarsecore::index3(x, y, z);
                if (seedLabels[start] != 0 || seedSigns[start] == 0) continue;

                const int id = nextId++;
                int head = 0;
                int tail = 0;
                seedQueue[tail++] = start;
                seedLabels[start] = id;
                int cells = 0;
                int maxY = 0;
                int columnCount = 0;
                bool touchesBottom = false;
                bool touchesSide = false;

                while (head < tail) {
                    const int idx = seedQueue[head++];
                    const int cy = idx % coarsecore::Y_LEVELS;
                    const int tmp = idx / coarsecore::Y_LEVELS;
                    const int cz = tmp % coarsecore::SIZE;
                    const int cx = tmp / coarsecore::SIZE;
                    ++cells;
                    if (cy > maxY) maxY = cy;
                    if (cy == 0) touchesBottom = true;
                    if (cx == 0 || cz == 0 || cx == coarsecore::SIZE - 1 || cz == coarsecore::SIZE - 1) touchesSide = true;

                    const int col = cx * coarsecore::SIZE + cz;
                    if (seedColumnSeen[col] != id) {
                        seedColumnSeen[col] = id;
                        seedComponentColumns[columnCount++] = col;
                        seedColumnMinY[col] = cy;
                    } else if (cy < seedColumnMinY[col]) {
                        seedColumnMinY[col] = cy;
                    }

#define ENQUEUE_COARSE(NI) do { const int ni_ = (NI); if (seedLabels[ni_] == 0 && seedSigns[ni_] != 0) { seedLabels[ni_] = id; seedQueue[tail++] = ni_; } } while (0)
                    if (cx + 1 < coarsecore::SIZE) ENQUEUE_COARSE(coarsecore::index3(cx + 1, cy, cz));
                    if (cx > 0) ENQUEUE_COARSE(coarsecore::index3(cx - 1, cy, cz));
                    if (cy + 1 < coarsecore::Y_LEVELS) ENQUEUE_COARSE(coarsecore::index3(cx, cy + 1, cz));
                    if (cy > 0) ENQUEUE_COARSE(coarsecore::index3(cx, cy - 1, cz));
                    if (cz + 1 < coarsecore::SIZE) ENQUEUE_COARSE(coarsecore::index3(cx, cy, cz + 1));
                    if (cz > 0) ENQUEUE_COARSE(coarsecore::index3(cx, cy, cz - 1));
#undef ENQUEUE_COARSE
                }

                if (cells <= best || maxY < coarsecore::MIN_INTERESTING_Y || touchesBottom || touchesSide) continue;

                bool reentry = false;
                for (int i = 0; i < columnCount && !reentry; ++i) {
                    const int col = seedComponentColumns[i];
                    const int cx = col / coarsecore::SIZE;
                    const int cz = col - cx * coarsecore::SIZE;
                    const int minY = seedColumnMinY[col];
                    if (minY <= 0 || minY >= coarsecore::Y_LEVELS) continue;
                    for (int lowerY = minY - 1; lowerY >= 0; --lowerY) {
                        const int idx = coarsecore::index3(cx, lowerY, cz);
                        if (seedSigns[idx] != 0 && seedLabels[idx] != id) {
                            if (minY - lowerY - 1 >= 1) reentry = true;
                            break;
                        }
                    }
                }
                if (reentry) best = cells;
            }
        }
    }
    scores[seedIndex] = best;
}

} // namespace coarsegpu
