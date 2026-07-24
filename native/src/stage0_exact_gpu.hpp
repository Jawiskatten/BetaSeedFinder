#pragma once

#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"
#include "terrain_perlin_cache.hpp"
#include "climate_perlin_cache.hpp"

#include <cstdint>
#include <cstddef>

namespace stage0gpu {

using p20::JavaRandom;
using p20::PerlinState;

static constexpr int FULL_POINTS = 256;
static constexpr int FULL_SIZE = 16;
static constexpr int FULL_YCOUNT = 17;
static constexpr int UPPER_Y_FROM = 11;
static constexpr int UPPER_YCOUNT = 6;
static constexpr int LOWER_YCOUNT = 11;
static constexpr int P20_MINI_SIZE = 8;

__device__ __forceinline__ void prepareTerrainPerlin(
        JavaRandom& rng,
        PerlinState& perlin,
        int seedIndex,
        int stateNumber,
        const unsigned char* terrainPermutationCache,
        const double* terrainOffsetCache
) {
    if (terrainPermutationCache != nullptr) {
        terraincache::loadStateCooperative(
                perlin, seedIndex, stateNumber,
                terrainPermutationCache, terrainOffsetCache,
                threadIdx.x, blockDim.x);
    } else {
        if (threadIdx.x == 0) p20::initPerlin(rng, perlin);
        __syncthreads();
    }
}

__device__ __forceinline__ bool isP20AxisIndex(int i) {
    return i == 0 || i == 2 || i == 5 || i == 7
        || i == 8 || i == 10 || i == 13 || i == 15;
}

__device__ __forceinline__ void fullPointCoordinates(
        int lane,
        int coarseOffsetX,
        int coarseOffsetZ,
        double& coarseX,
        double& coarseZ,
        double& climateX,
        double& climateZ
) {
    const int ix = lane >> 4;
    const int iz = lane & 15;
    coarseX = -28.0 + static_cast<double>(ix * 4 + coarseOffsetX);
    coarseZ = -28.0 + static_cast<double>(iz * 4 + coarseOffsetZ);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
}

__device__ __forceinline__ void fullPointCoordinates(
        int lane,
        double& coarseX,
        double& coarseZ,
        double& climateX,
        double& climateZ
) {
    fullPointCoordinates(lane, 0, 0, coarseX, coarseZ, climateX, climateZ);
}

struct UpperScratch {
    JavaRandom rng;
    PerlinState perlin;
    double temp[ FULL_POINTS ];
    double rain[ FULL_POINTS ];
    double climateBlend[ FULL_POINTS ];
    double noise1[ FULL_POINTS * UPPER_YCOUNT ];
    double noise2[ FULL_POINTS * UPPER_YCOUNT ];
    double noise3[ FULL_POINTS * UPPER_YCOUNT ];
    double noise4[ FULL_POINTS ];
#if !defined(BSF_NVIDIA_CUDA)
    // HIP path keeps the established shared layout. CUDA stores this one value
    // per thread locally so UpperScratch fits the 48 KiB static limit on Turing.
    double noise5[ FULL_POINTS ];
#endif
    int p20Count;
    int fullUpperCount;
};

__device__ __forceinline__ void accumulateSimplexShared(
        UpperScratch& s,
        std::int64_t seed,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double octavePersistence,
        double climateX,
        double climateZ,
        double* out,
        int seedIndex = 0,
        int stateBase = 0,
        const unsigned char* climatePermutationCache = nullptr,
        const double* climateOffsetCache = nullptr,
        bool active = true
) {
    const int lane = threadIdx.x;
    if (climatePermutationCache == nullptr && lane == 0) s.rng.setSeed(seed);
    out[lane] = 0.0;
    __syncthreads();

    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < octaves; ++octave) {
        if (climatePermutationCache != nullptr) {
            climatecache::loadStateCooperative(
                    s.perlin, seedIndex, stateBase + octave,
                    climatePermutationCache, climateOffsetCache,
                    lane, blockDim.x);
        } else {
            if (lane == 0) p20::initPerlin(s.rng, s.perlin);
            __syncthreads();
        }
        const double scaleX = (startScaleX / 1.5) * d7;
        const double scaleZ = (startScaleZ / 1.5) * d7;
        const double weight = 0.55 / d6;
        if (active) out[lane] += p20::simplex2(s.perlin, climateX * scaleX, climateZ * scaleZ) * weight;
        __syncthreads();
        d7 *= octaveScale;
        d6 *= octavePersistence;
    }
}

__global__ void stage0UpperKernelFromSeeds(
        const std::int64_t* seeds,
        int count,
        int* p20Counts,
        int* fullUpperCounts,
        unsigned char* upperMasks,
        double* upperDensities,
        int skipP20Rejects,
        const unsigned char* terrainPermutationCache,
        const double* terrainOffsetCache,
        const unsigned char* climatePermutationCache,
        const double* climateOffsetCache,
        double* columnShapeD5,
        double* columnShapeD7,
        int reuseP20Columns,
        int coarseOffsetX,
        int coarseOffsetZ
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= FULL_POINTS) return;

    // Optimized production path: run the cheap 64-column P20 kernel first, then
    // make the expensive 256-column upper scan a no-op for P20 rejects. The
    // condition is uniform for the whole block, so rejected blocks return before
    // allocating any useful work. Exact raw validation passes skipP20Rejects=0.
    if (skipP20Rejects != 0 && p20Counts[seedIndex] <= 0) {
        if (lane == 0) fullUpperCounts[seedIndex] = 0;
        return;
    }

    __shared__ UpperScratch s;
#if defined(BSF_NVIDIA_CUDA)
    double noise5Local = 0.0;
#endif
    const std::int64_t seed = seeds[seedIndex];

    double coarseX, coarseZ, climateX, climateZ;
    fullPointCoordinates(lane, coarseOffsetX, coarseOffsetZ, coarseX, coarseZ, climateX, climateZ);
    const int ix = lane >> 4;
    const int iz = lane & 15;
    const bool reuseP20Lane = reuseP20Columns != 0
            && isP20AxisIndex(ix) && isP20AxisIndex(iz)
            && columnShapeD5 != nullptr && columnShapeD7 != nullptr;
    const std::size_t columnIndex = static_cast<std::size_t>(seedIndex) * FULL_POINTS + lane;

    const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
    const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
    const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);

    accumulateSimplexShared(s, tempSeed, 4,
            0.02500000037252903, 0.02500000037252903, 0.25, 0.5,
            climateX, climateZ, s.temp, seedIndex, climatecache::TEMP_BASE,
            climatePermutationCache, climateOffsetCache, !reuseP20Lane);
    accumulateSimplexShared(s, rainSeed, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333, 0.5,
            climateX, climateZ, s.rain, seedIndex, climatecache::RAIN_BASE,
            climatePermutationCache, climateOffsetCache, !reuseP20Lane);
    accumulateSimplexShared(s, blendSeed, 2,
            0.25, 0.25, 0.5882352941176471, 0.5,
            climateX, climateZ, s.climateBlend, seedIndex, climatecache::BLEND_BASE,
            climatePermutationCache, climateOffsetCache, !reuseP20Lane);

    if (!reuseP20Lane) {
        const double d0 = s.climateBlend[lane] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (s.temp[lane] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (s.rain[lane] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        s.temp[lane] = d3;
        s.rain[lane] = d4;
    }
    __syncthreads();

    if (terrainPermutationCache == nullptr && lane == 0) s.rng.setSeed(seed);
    for (int yi = 0; yi < UPPER_YCOUNT; ++yi) {
        const int idx = lane * UPPER_YCOUNT + yi;
        s.noise1[idx] = 0.0;
        s.noise2[idx] = 0.0;
        s.noise3[idx] = 0.0;
    }
    s.noise4[lane] = 0.0;
#if defined(BSF_NVIDIA_CUDA)
    noise5Local = 0.0;
#else
    s.noise5[lane] = 0.0;
#endif
    if (lane == 0) {
        s.p20Count = 0;
        s.fullUpperCount = 0;
    }
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareTerrainPerlin(
                s.rng, s.perlin, seedIndex, terraincache::NOISE2_BASE + octave,
                terrainPermutationCache, terrainOffsetCache);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        if (!reuseP20Lane) {
            double values[UPPER_YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < UPPER_YCOUNT; ++yi) {
                const int idx = lane * UPPER_YCOUNT + yi;
                const double value = values[yi] * weight;
                if (octave == 0) s.noise2[idx] = value;
                else s.noise2[idx] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareTerrainPerlin(
                s.rng, s.perlin, seedIndex, terraincache::NOISE3_BASE + octave,
                terrainPermutationCache, terrainOffsetCache);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        if (!reuseP20Lane) {
            double values[UPPER_YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < UPPER_YCOUNT; ++yi) {
                const int idx = lane * UPPER_YCOUNT + yi;
                const double value = values[yi] * weight;
                if (octave == 0) s.noise3[idx] = value;
                else s.noise3[idx] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        prepareTerrainPerlin(
                s.rng, s.perlin, seedIndex, terraincache::NOISE1_BASE + octave,
                terrainPermutationCache, terrainOffsetCache);
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        if (!reuseP20Lane) {
            double values[UPPER_YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < UPPER_YCOUNT; ++yi) {
                const int idx = lane * UPPER_YCOUNT + yi;
                const double value = values[yi] * weight;
                if (octave == 0) s.noise1[idx] = value;
                else s.noise1[idx] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    if (terrainPermutationCache == nullptr) {
        if (lane == 0) {
            for (int i = 0; i < 8; ++i) p20::consumePerlin(s.rng, s.perlin);
        }
        __syncthreads();
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        prepareTerrainPerlin(
                s.rng, s.perlin, seedIndex, terraincache::NOISE4_BASE + octave,
                terrainPermutationCache, terrainOffsetCache);
        const double sx = 1.121 * amplitude;
        const double sz = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        if (!reuseP20Lane) {
            const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
            if (octave == 0) s.noise4[lane] = value;
            else s.noise4[lane] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareTerrainPerlin(
                s.rng, s.perlin, seedIndex, terraincache::NOISE5_BASE + octave,
                terrainPermutationCache, terrainOffsetCache);
        const double sx = 200.0 * amplitude;
        const double sz = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        if (!reuseP20Lane) {
            const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
#if defined(BSF_NVIDIA_CUDA)
            if (octave == 0) noise5Local = value;
            else noise5Local += value;
#else
            if (octave == 0) s.noise5[lane] = value;
            else s.noise5[lane] += value;
#endif
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    double d5;
    double d7;
    unsigned char mask = 0;
    if (reuseP20Lane) {
        d5 = columnShapeD5[columnIndex];
        d7 = columnShapeD7[columnIndex];
        mask = upperMasks[columnIndex];
    } else {
        const double d2 = s.temp[lane];
        const double d3 = s.rain[lane] * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;

        d5 = (s.noise4[lane] + 256.0) / 512.0;
        d5 *= d4;
        if (d5 > 1.0) d5 = 1.0;

#if defined(BSF_NVIDIA_CUDA)
        double d6 = noise5Local / 8000.0;
#else
        double d6 = s.noise5[lane] / 8000.0;
#endif
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
        d6 = d6 * 17.0 / 16.0;
        d7 = 17.0 / 2.0 + d6 * 4.0;

        for (int yi = 0; yi < UPPER_YCOUNT; ++yi) {
            const int y = UPPER_Y_FROM + yi;
            const int idx = lane * UPPER_YCOUNT + yi;
            double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
            if (d9 < 0.0) d9 *= 4.0;

            const double blend = (s.noise1[idx] / 10.0 + 1.0) / 2.0;
            double d8;
            if (blend < 0.0) {
                d8 = s.noise2[idx] / 512.0;
            } else if (blend > 1.0) {
                d8 = s.noise3[idx] / 512.0;
            } else {
                const double d10 = s.noise2[idx] / 512.0;
                const double d11 = s.noise3[idx] / 512.0;
                d8 = d10 + (d11 - d10) * blend;
            }
            d8 -= d9;
            if (y > 13) {
                const double d13 = static_cast<double>(static_cast<float>(y - 13) / 3.0F);
                d8 = d8 * (1.0 - d13) + -10.0 * d13;
            }
            if (upperDensities != nullptr) {
                const std::size_t outIndex = columnIndex * UPPER_YCOUNT + yi;
                upperDensities[outIndex] = d8;
            }
            if (d8 > 0.0) mask |= static_cast<unsigned char>(1u << yi);
        }
        if (columnShapeD5 != nullptr) columnShapeD5[columnIndex] = d5;
        if (columnShapeD7 != nullptr) columnShapeD7[columnIndex] = d7;
    }

    upperMasks[columnIndex] = mask;
    if (mask != 0) {
        atomicAdd(&s.fullUpperCount, 1);
        if (skipP20Rejects == 0 && isP20AxisIndex(ix) && isP20AxisIndex(iz)) {
            atomicAdd(&s.p20Count, 1);
        }
    }
    __syncthreads();

    if (lane == 0) {
        if (skipP20Rejects == 0) p20Counts[seedIndex] = s.p20Count;
        fullUpperCounts[seedIndex] = s.fullUpperCount;
    }
}

__device__ __forceinline__ void perlin3Lower11(
        const PerlinState& p,
        double xCoord,
        double yScale,
        double zCoord,
        double out11[LOWER_YCOUNT]
) {
    double x = xCoord + p.a;
    double z = zCoord + p.c;
    const int fx = p20::javaFloor(x);
    const int fz = p20::javaFloor(z);
    const int xi = fx & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double z1 = z - 1.0;
    const double xf = p20::fade(x);
    const double zf = p20::fade(z);
    const int xp0 = p.perm[xi];
    const int xp1 = p.perm[xi + 1];

    int yIndex[FULL_YCOUNT];
    int yCellStart[FULL_YCOUNT];
    double yFrac[FULL_YCOUNT];
    double yFracMinus1[FULL_YCOUNT];
    double yFade[FULL_YCOUNT];
    int previousYi = -2147483647;
    int cellStart = 0;
    for (int y = 0; y < FULL_YCOUNT; ++y) {
        double v = static_cast<double>(y) * yScale + p.b;
        const int floor = p20::javaFloor(v);
        const int yi = floor & 255;
        if (y == 0 || yi != previousYi) {
            cellStart = y;
            previousYi = yi;
        }
        yIndex[y] = yi;
        yCellStart[y] = cellStart;
        v -= static_cast<double>(floor);
        yFrac[y] = v;
        yFracMinus1[y] = v - 1.0;
        yFade[y] = p20::fade(v);
    }

    int activeCellStart = -1;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int y = 0; y < LOWER_YCOUNT; ++y) {
        const int yi = yIndex[y];
        const int firstY = yCellStart[y];
        if (firstY != activeCellStart) {
            activeCellStart = firstY;
            const double yf = yFrac[firstY];
            const double yfm1 = yFracMinus1[firstY];
            const int p0 = xp0 + yi;
            const int p00 = p.perm[p0] + zi;
            const int p01 = p.perm[p0 + 1] + zi;
            const int p1 = xp1 + yi;
            const int p10 = p.perm[p1] + zi;
            const int p11 = p.perm[p1 + 1] + zi;
            q00 = p20::lerp(xf, p20::grad3(p.perm[p00], x, yf, z), p20::grad3(p.perm[p10], x1, yf, z));
            q01 = p20::lerp(xf, p20::grad3(p.perm[p01], x, yfm1, z), p20::grad3(p.perm[p11], x1, yfm1, z));
            q10 = p20::lerp(xf, p20::grad3(p.perm[p00 + 1], x, yf, z1), p20::grad3(p.perm[p10 + 1], x1, yf, z1));
            q11 = p20::lerp(xf, p20::grad3(p.perm[p01 + 1], x, yfm1, z1), p20::grad3(p.perm[p11 + 1], x1, yfm1, z1));
        }
        const double r0 = p20::lerp(yFade[y], q00, q01);
        const double r1 = p20::lerp(yFade[y], q10, q11);
        out11[y] = p20::lerp(zf, r0, r1);
    }
}

struct LowerScratch {
    JavaRandom rng;
    PerlinState perlin;
    double noise2[ FULL_POINTS * LOWER_YCOUNT ];
    double blend[ FULL_POINTS * LOWER_YCOUNT ];
    int highReentryCount;
};

__device__ __forceinline__ double accumulateSimplexLocal(
        LowerScratch& s,
        std::int64_t seed,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double octavePersistence,
        double climateX,
        double climateZ
) {
    const int lane = threadIdx.x;
    if (lane == 0) s.rng.setSeed(seed);
    double out = 0.0;
    __syncthreads();

    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < octaves; ++octave) {
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        const double scaleX = (startScaleX / 1.5) * d7;
        const double scaleZ = (startScaleZ / 1.5) * d7;
        const double weight = 0.55 / d6;
        out += p20::simplex2(s.perlin, climateX * scaleX, climateZ * scaleZ) * weight;
        __syncthreads();
        d7 *= octaveScale;
        d6 *= octavePersistence;
    }
    return out;
}

__device__ __forceinline__ int highestReentryYIndex(std::uint32_t positiveMask) {
    bool sawPositive = false;
    bool sawGapAfterPositive = false;
    int highest = -1;
    for (int y = 0; y < FULL_YCOUNT; ++y) {
        const bool positive = (positiveMask & (1u << y)) != 0;
        if (positive) {
            if (sawPositive && sawGapAfterPositive) highest = y;
            sawPositive = true;
        } else if (sawPositive) {
            sawGapAfterPositive = true;
        }
    }
    return highest;
}

__device__ __forceinline__ bool hasHighReentry(std::uint32_t positiveMask) {
    return highestReentryYIndex(positiveMask) >= UPPER_Y_FROM;
}

__global__ void stage0LowerReentryKernel(
        const std::int64_t* seeds,
        int count,
        const int* p20Counts,
        const int* fullUpperCounts,
        int minUpperCount,
        const unsigned char* upperMasks,
        int* highReentryCounts,
        unsigned char* highestReentryY,
        double* lowerDensities,
        const unsigned char* terrainPermutationCache,
        const double* terrainOffsetCache,
        const double* columnShapeD5,
        const double* columnShapeD7,
        int coarseOffsetX,
        int coarseOffsetZ
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= FULL_POINTS) return;

    if (p20Counts[seedIndex] <= 0 || fullUpperCounts[seedIndex] < minUpperCount) {
        return;
    }

    __shared__ LowerScratch s;
    if (lane == 0) s.highReentryCount = 0;
    __syncthreads();

    const std::size_t topologyIndex = static_cast<std::size_t>(seedIndex) * FULL_POINTS + lane;
    if (highestReentryY != nullptr) highestReentryY[topologyIndex] = 0xFFu;

    const unsigned char upperMask = upperMasks[topologyIndex];
    const bool candidate = upperMask != 0;
    const bool useShapeCache = columnShapeD5 != nullptr && columnShapeD7 != nullptr;
    const std::int64_t seed = seeds[seedIndex];

    double coarseX, coarseZ, climateX, climateZ;
    fullPointCoordinates(lane, coarseOffsetX, coarseOffsetZ, coarseX, coarseZ, climateX, climateZ);

    double temp = 0.0;
    double rain = 0.0;
    if (!useShapeCache) {
        const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
        const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
        const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);

        temp = accumulateSimplexLocal(s, tempSeed, 4,
                0.02500000037252903, 0.02500000037252903, 0.25, 0.5,
                climateX, climateZ);
        rain = accumulateSimplexLocal(s, rainSeed, 4,
                0.05000000074505806, 0.05000000074505806, 0.3333333333333333, 0.5,
                climateX, climateZ);
        const double climateBlend = accumulateSimplexLocal(s, blendSeed, 2,
                0.25, 0.25, 0.5882352941176471, 0.5,
                climateX, climateZ);

        const double d0 = climateBlend * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        temp = (temp * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        rain = (rain * 0.15 + 0.5) * d2 + d0 * d1;
        temp = 1.0 - (1.0 - temp) * (1.0 - temp);
        if (temp < 0.0) temp = 0.0;
        if (rain < 0.0) rain = 0.0;
        if (temp > 1.0) temp = 1.0;
        if (rain > 1.0) rain = 1.0;
    }

    if (terrainPermutationCache == nullptr && lane == 0) s.rng.setSeed(seed);
    double noise3[LOWER_YCOUNT];
    for (int y = 0; y < LOWER_YCOUNT; ++y) {
        const int idx = lane * LOWER_YCOUNT + y;
        if (candidate) {
            s.noise2[idx] = 0.0;
            s.blend[idx] = 0.0;
        }
        noise3[y] = 0.0;
    }
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareTerrainPerlin(
                s.rng, s.perlin, seedIndex, terraincache::NOISE2_BASE + octave,
                terrainPermutationCache, terrainOffsetCache);
        if (candidate) {
            const double sx = 684.412 * amplitude;
            const double sy = 684.412 * amplitude;
            const double sz = 684.412 * amplitude;
            const double weight = 1.0 / amplitude;
            double values[LOWER_YCOUNT];
            perlin3Lower11(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int y = 0; y < LOWER_YCOUNT; ++y) {
                const int idx = lane * LOWER_YCOUNT + y;
                const double value = values[y] * weight;
                if (octave == 0) s.noise2[idx] = value;
                else s.noise2[idx] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareTerrainPerlin(
                s.rng, s.perlin, seedIndex, terraincache::NOISE3_BASE + octave,
                terrainPermutationCache, terrainOffsetCache);
        if (candidate) {
            const double sx = 684.412 * amplitude;
            const double sy = 684.412 * amplitude;
            const double sz = 684.412 * amplitude;
            const double weight = 1.0 / amplitude;
            double values[LOWER_YCOUNT];
            perlin3Lower11(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int y = 0; y < LOWER_YCOUNT; ++y) {
                const double value = values[y] * weight;
                if (octave == 0) noise3[y] = value;
                else noise3[y] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        prepareTerrainPerlin(
                s.rng, s.perlin, seedIndex, terraincache::NOISE1_BASE + octave,
                terrainPermutationCache, terrainOffsetCache);
        if (candidate) {
            const double sx = (684.412 / 80.0) * amplitude;
            const double sy = (684.412 / 160.0) * amplitude;
            const double sz = (684.412 / 80.0) * amplitude;
            const double weight = 1.0 / amplitude;
            double values[LOWER_YCOUNT];
            perlin3Lower11(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int y = 0; y < LOWER_YCOUNT; ++y) {
                const int idx = lane * LOWER_YCOUNT + y;
                const double value = values[y] * weight;
                if (octave == 0) s.blend[idx] = value;
                else s.blend[idx] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    if (terrainPermutationCache == nullptr) {
        if (lane == 0) {
            for (int i = 0; i < 8; ++i) p20::consumePerlin(s.rng, s.perlin);
        }
        __syncthreads();
    }

    double noise4 = 0.0;
    double noise5 = 0.0;
    if (!useShapeCache) {
        amplitude = 1.0;
        for (int octave = 0; octave < 10; ++octave) {
            prepareTerrainPerlin(
                    s.rng, s.perlin, seedIndex, terraincache::NOISE4_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache);
            if (candidate) {
                const double value = p20::perlin2(
                        s.perlin,
                        coarseX * (1.121 * amplitude),
                        coarseZ * (1.121 * amplitude)
                ) * (1.0 / amplitude);
                if (octave == 0) noise4 = value;
                else noise4 += value;
            }
            __syncthreads();
            amplitude /= 2.0;
        }

        amplitude = 1.0;
        for (int octave = 0; octave < 16; ++octave) {
            prepareTerrainPerlin(
                    s.rng, s.perlin, seedIndex, terraincache::NOISE5_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache);
            if (candidate) {
                const double value = p20::perlin2(
                        s.perlin,
                        coarseX * (200.0 * amplitude),
                        coarseZ * (200.0 * amplitude)
                ) * (1.0 / amplitude);
                if (octave == 0) noise5 = value;
                else noise5 += value;
            }
            __syncthreads();
            amplitude /= 2.0;
        }
    }

    if (candidate) {
        double d5;
        double d7;
        if (useShapeCache) {
            d5 = columnShapeD5[topologyIndex];
            d7 = columnShapeD7[topologyIndex];
        } else {
            const double d2 = temp;
            const double d3 = rain * d2;
            double d4 = 1.0 - d3;
            d4 *= d4;
            d4 *= d4;
            d4 = 1.0 - d4;

            d5 = (noise4 + 256.0) / 512.0;
            d5 *= d4;
            if (d5 > 1.0) d5 = 1.0;

            double d6 = noise5 / 8000.0;
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
            d6 = d6 * 17.0 / 16.0;
            d7 = 17.0 / 2.0 + d6 * 4.0;
        }

        std::uint32_t positiveMask = static_cast<std::uint32_t>(upperMask) << UPPER_Y_FROM;
        for (int y = 0; y < LOWER_YCOUNT; ++y) {
            const int idx = lane * LOWER_YCOUNT + y;
            double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
            if (d9 < 0.0) d9 *= 4.0;

            const double blend = (s.blend[idx] / 10.0 + 1.0) / 2.0;
            double d8;
            if (blend < 0.0) {
                d8 = s.noise2[idx] / 512.0;
            } else if (blend > 1.0) {
                d8 = noise3[y] / 512.0;
            } else {
                const double d10 = s.noise2[idx] / 512.0;
                const double d11 = noise3[y] / 512.0;
                d8 = d10 + (d11 - d10) * blend;
            }
            d8 -= d9;
            if (lowerDensities != nullptr) {
                const std::size_t outIndex = (static_cast<std::size_t>(seedIndex) * FULL_POINTS + lane)
                        * LOWER_YCOUNT + y;
                lowerDensities[outIndex] = d8;
            }
            if (d8 > 0.0) positiveMask |= (1u << y);
        }

        const int highestReentryYIndexValue = highestReentryYIndex(positiveMask);
        if (highestReentryY != nullptr) {
            highestReentryY[topologyIndex] = highestReentryYIndexValue < 0
                    ? 0xFFu
                    : static_cast<unsigned char>(highestReentryYIndexValue);
        }
        if (highestReentryYIndexValue >= UPPER_Y_FROM) atomicAdd(&s.highReentryCount, 1);
    }
    __syncthreads();

    if (lane == 0) highReentryCounts[seedIndex] = s.highReentryCount;
}

} // namespace stage0gpu
