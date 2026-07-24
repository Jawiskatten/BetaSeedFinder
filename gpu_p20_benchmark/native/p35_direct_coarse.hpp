#pragma once

#include "coarse_exact_gpu.hpp"

namespace p35coarse {

using namespace coarsegpu;

// Exact direct-write equivalent of perlin3Full17CachedXYZ. The production
// helper materializes a 17-double temporary in every lane and the caller then
// multiplies/stores it. This version preserves the same operation order but
// commits each Y value immediately, reducing the live result set to one double.
__device__ __forceinline__ void accumulatePerlin3Full17CachedXYZ(
        const p20::PerlinState& p,
        const PerlinAxisCache& cache,
        int xIndex,
        int zIndex,
        double weight,
        bool firstOctave,
        double* output17
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
        const double value = p20::lerp(zf, r0, r1) * weight;
        if (firstOctave) output17[y] = value;
        else output17[y] += value;
    }
}

__global__ void generateCoarseSignsDirect23Kernel(
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
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            accumulatePerlin3Full17CachedXYZ(
                    perlin, axes, xIndex, zIndex, weight, octave == 0, noise2 + base);
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
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            accumulatePerlin3Full17CachedXYZ(
                    perlin, axes, xIndex, zIndex, weight, octave == 0, noise3 + base);
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


__global__ void generateCoarseSignsDirectAllKernel(
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
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            accumulatePerlin3Full17CachedXYZ(
                    perlin, axes, xIndex, zIndex, weight, octave == 0, noise2 + base);
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
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            accumulatePerlin3Full17CachedXYZ(
                    perlin, axes, xIndex, zIndex, weight, octave == 0, noise3 + base);
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
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            accumulatePerlin3Full17CachedXYZ(
                    perlin, axes, xIndex, zIndex, weight, octave == 0, noise1 + base);
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


} // namespace p35coarse
