#pragma once

#include "gpu_runtime_compat.hpp"
#include "stage0_exact_gpu.hpp"

#include <cstddef>
#include <cstdint>

namespace p34perm {

#if defined(__HIPCC__) || defined(__CUDACC__)
#define P34_HD __host__ __device__ __forceinline__
#else
#define P34_HD inline
#endif

struct CompactPerlinState {
    unsigned char perm[256];
    double a;
    double b;
    double c;
};

P34_HD int permAt(const CompactPerlinState& state, int index) {
    return static_cast<int>(state.perm[index & 255]);
}

__device__ __forceinline__ void loadCompactStateCooperative(
        CompactPerlinState& state,
        int seedIndex,
        int stateNumber,
        int stateCount,
        const unsigned char* permutationCache,
        const double* offsetCache,
        int lane,
        int laneCount
) {
    const std::size_t stateLinear = static_cast<std::size_t>(seedIndex) * stateCount
            + static_cast<std::size_t>(stateNumber);
    if (lane == 0) {
        const std::size_t offsetBase = stateLinear * 3;
        state.a = offsetCache[offsetBase];
        state.b = offsetCache[offsetBase + 1];
        state.c = offsetCache[offsetBase + 2];
    }
    const std::size_t permBase = stateLinear * 256;
    for (int i = lane; i < 256; i += laneCount) {
        state.perm[i] = permutationCache[permBase + static_cast<std::size_t>(i)];
    }
    __syncthreads();
}

__device__ __forceinline__ void loadTerrainStateCooperative(
        CompactPerlinState& state,
        int seedIndex,
        int stateNumber,
        const unsigned char* permutationCache,
        const double* offsetCache,
        int lane,
        int laneCount
) {
    loadCompactStateCooperative(state, seedIndex, stateNumber, terraincache::STATE_COUNT,
            permutationCache, offsetCache, lane, laneCount);
}

__device__ __forceinline__ void loadClimateStateCooperative(
        CompactPerlinState& state,
        int seedIndex,
        int stateNumber,
        const unsigned char* permutationCache,
        const double* offsetCache,
        int lane,
        int laneCount
) {
    loadCompactStateCooperative(state, seedIndex, stateNumber, climatecache::STATE_COUNT,
            permutationCache, offsetCache, lane, laneCount);
}

P34_HD void buildUpper6YAxisCache(
        const CompactPerlinState& state,
        double yScale,
        p20::Upper6YAxisCache& cache
) {
    int previousYi = -2147483647;
    int cellStart = 0;
    for (int y = 0; y < 17; ++y) {
        double v = static_cast<double>(y) * yScale + state.b;
        const int floor = p20::javaFloor(v);
        const int yi = floor & 255;
        if (y == 0 || yi != previousYi) {
            cellStart = y;
            previousYi = yi;
        }
        cache.yIndex[y] = yi;
        cache.yCellStart[y] = cellStart;
        v -= static_cast<double>(floor);
        cache.yFrac[y] = v;
        cache.yFracMinus1[y] = v - 1.0;
        cache.yFade[y] = p20::fade(v);
    }
}

P34_HD void perlin3Upper6SharedY(
        const CompactPerlinState& state,
        double xCoord,
        double zCoord,
        const p20::Upper6YAxisCache& cache,
        double out6[6]
) {
    double x = xCoord + state.a;
    double z = zCoord + state.c;
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
    const int xp0 = permAt(state, xi);
    const int xp1 = permAt(state, xi + 1);

    int activeCellStart = -1;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int k = 0; k < 6; ++k) {
        const int y = 11 + k;
        const int yi = cache.yIndex[y];
        const int firstY = cache.yCellStart[y];
        if (firstY != activeCellStart) {
            activeCellStart = firstY;
            const double yf = cache.yFrac[firstY];
            const double yfm1 = cache.yFracMinus1[firstY];
            const int p0 = xp0 + yi;
            const int p00 = permAt(state, p0) + zi;
            const int p01 = permAt(state, p0 + 1) + zi;
            const int p1 = xp1 + yi;
            const int p10 = permAt(state, p1) + zi;
            const int p11 = permAt(state, p1 + 1) + zi;
            q00 = p20::lerp(xf, p20::grad3(permAt(state, p00), x, yf, z), p20::grad3(permAt(state, p10), x1, yf, z));
            q01 = p20::lerp(xf, p20::grad3(permAt(state, p01), x, yfm1, z), p20::grad3(permAt(state, p11), x1, yfm1, z));
            q10 = p20::lerp(xf, p20::grad3(permAt(state, p00 + 1), x, yf, z1), p20::grad3(permAt(state, p10 + 1), x1, yf, z1));
            q11 = p20::lerp(xf, p20::grad3(permAt(state, p01 + 1), x, yfm1, z1), p20::grad3(permAt(state, p11 + 1), x1, yfm1, z1));
        }
        const double r0 = p20::lerp(cache.yFade[y], q00, q01);
        const double r1 = p20::lerp(cache.yFade[y], q10, q11);
        out6[k] = p20::lerp(zf, r0, r1);
    }
}

P34_HD double perlin2(const CompactPerlinState& state, double xCoord, double zCoord) {
    double x = xCoord + state.a;
    double z = zCoord + state.c;
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

    const int xp0 = permAt(state, xi);
    const int xp1 = permAt(state, xi + 1);
    const int p00 = permAt(state, xp0) + zi;
    const int p10 = permAt(state, xp1) + zi;

    const double q0 = p20::lerp(xf,
            p20::grad2Legacy(permAt(state, p00), x, z),
            p20::grad3(permAt(state, p10), x1, 0.0, z));
    const double q1 = p20::lerp(xf,
            p20::grad3(permAt(state, p00 + 1), x, 0.0, z1),
            p20::grad3(permAt(state, p10 + 1), x1, 0.0, z1));
    return p20::lerp(zf, q0, q1);
}

P34_HD double simplex2(const CompactPerlinState& state, double xCoord, double zCoord) {
    constexpr double F = 0.3660254037844386;
    constexpr double G = 0.21132486540518713;
    const double x = xCoord + state.a;
    const double z = zCoord + state.b;
    const double skew = (x + z) * F;
    const int i = p20::simplexFastFloor(x + skew);
    const int j = p20::simplexFastFloor(z + skew);
    const double unskew = static_cast<double>(i + j) * G;
    const double x0 = x - (static_cast<double>(i) - unskew);
    const double z0 = z - (static_cast<double>(j) - unskew);
    const int i1 = x0 > z0 ? 1 : 0;
    const int j1 = x0 > z0 ? 0 : 1;
    const double x1 = x0 - static_cast<double>(i1) + G;
    const double z1 = z0 - static_cast<double>(j1) + G;
    const double x2 = x0 - 1.0 + 2.0 * G;
    const double z2 = z0 - 1.0 + 2.0 * G;
    const int ii = i & 255;
    const int jj = j & 255;
    const int g0 = permAt(state, ii + permAt(state, jj)) % 12;
    const int g1 = permAt(state, ii + i1 + permAt(state, jj + j1)) % 12;
    const int g2 = permAt(state, ii + 1 + permAt(state, jj + 1)) % 12;

    double t0 = 0.5 - x0 * x0 - z0 * z0;
    double n0;
    if (t0 < 0.0) n0 = 0.0;
    else { t0 *= t0; n0 = t0 * t0 * p20::simplexGrad2(g0, x0, z0); }
    double t1 = 0.5 - x1 * x1 - z1 * z1;
    double n1;
    if (t1 < 0.0) n1 = 0.0;
    else { t1 *= t1; n1 = t1 * t1 * p20::simplexGrad2(g1, x1, z1); }
    double t2 = 0.5 - x2 * x2 - z2 * z2;
    double n2;
    if (t2 < 0.0) n2 = 0.0;
    else { t2 *= t2; n2 = t2 * t2 * p20::simplexGrad2(g2, x2, z2); }
    return 70.0 * (n0 + n1 + n2);
}

static constexpr int THREADS = 192;
static constexpr int YCOUNT = stage0gpu::UPPER_YCOUNT;

struct CompactPermUpperScratch {
    CompactPerlinState perlin;
    double temp[THREADS];
    double rain[THREADS];
    double climateBlend[THREADS];
    double noise1[THREADS * YCOUNT];
    double noise2[THREADS * YCOUNT];
    double noise3[THREADS * YCOUNT];
    double noise4[THREADS];
    double noise5[THREADS];
    int fullUpperCount;
    p20::Upper6YAxisCache yAxis;
};

__device__ __forceinline__ int nonP20AxisValue(int compact) {
    // Full 0..15 axis with the P20 positions {0,2,5,7,8,10,13,15} removed.
    switch (compact) {
        case 0: return 1;
        case 1: return 3;
        case 2: return 4;
        case 3: return 6;
        case 4: return 9;
        case 5: return 11;
        case 6: return 12;
        default: return 14;
    }
}

__device__ __forceinline__ int fullLaneFromCompact(int compactLane) {
    int remaining = compactLane;
    for (int ix = 0; ix < stage0gpu::FULL_SIZE; ++ix) {
        const bool p20X = stage0gpu::isP20AxisIndex(ix);
        const int columns = p20X ? 8 : 16;
        if (remaining < columns) {
            const int iz = p20X ? nonP20AxisValue(remaining) : remaining;
            return ix * stage0gpu::FULL_SIZE + iz;
        }
        remaining -= columns;
    }
    return stage0gpu::FULL_POINTS - 1;
}

__device__ __forceinline__ void accumulateClimate(
        CompactPermUpperScratch& s,
        int seedIndex,
        int stateBase,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double climateX,
        double climateZ,
        double* out,
        const unsigned char* climatePermutationCache,
        const double* climateOffsetCache
) {
    const int lane = threadIdx.x;
    out[lane] = 0.0;
    __syncthreads();

    double amplitudeWeight = 1.0;
    double frequency = 1.0;
    for (int octave = 0; octave < octaves; ++octave) {
        loadClimateStateCooperative(
                s.perlin, seedIndex, stateBase + octave,
                climatePermutationCache, climateOffsetCache,
                lane, THREADS);
        const double scaleX = (startScaleX / 1.5) * frequency;
        const double scaleZ = (startScaleZ / 1.5) * frequency;
        const double weight = 0.55 / amplitudeWeight;
        out[lane] += simplex2(s.perlin, climateX * scaleX, climateZ * scaleZ) * weight;
        __syncthreads();
        frequency *= octaveScale;
        amplitudeWeight *= 0.5;
    }
}

// P33 benchmark-only exact shared-Y upper stage. P20 has already evaluated and persisted the
// 64 sparse columns. This kernel maps 192 dense lanes to the remaining columns,
// avoiding the 64 idle lanes carried by the 256-thread production kernel.
__global__ void stage0UpperCompact192SharedYCompactPermKernel(
        const std::int64_t* seeds,
        int count,
        const int* p20Counts,
        int* fullUpperCounts,
        unsigned char* upperMasks,
        double* upperDensities,
        const unsigned char* terrainPermutationCache,
        const double* terrainOffsetCache,
        const unsigned char* climatePermutationCache,
        const double* climateOffsetCache,
        double* columnShapeD5,
        double* columnShapeD7,
        int coarseOffsetX,
        int coarseOffsetZ
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= THREADS) return;

    if (p20Counts[seedIndex] <= 0) {
        if (lane == 0) fullUpperCounts[seedIndex] = 0;
        return;
    }

    __shared__ CompactPermUpperScratch s;
    const int fullLane = fullLaneFromCompact(lane);
    const std::size_t columnIndex = static_cast<std::size_t>(seedIndex)
            * stage0gpu::FULL_POINTS + fullLane;

    double coarseX, coarseZ, climateX, climateZ;
    stage0gpu::fullPointCoordinates(fullLane, coarseOffsetX, coarseOffsetZ,
            coarseX, coarseZ, climateX, climateZ);

    accumulateClimate(s, seedIndex, climatecache::TEMP_BASE, 4,
            0.02500000037252903, 0.02500000037252903, 0.25,
            climateX, climateZ, s.temp,
            climatePermutationCache, climateOffsetCache);
    accumulateClimate(s, seedIndex, climatecache::RAIN_BASE, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333,
            climateX, climateZ, s.rain,
            climatePermutationCache, climateOffsetCache);
    accumulateClimate(s, seedIndex, climatecache::BLEND_BASE, 2,
            0.25, 0.25, 0.5882352941176471,
            climateX, climateZ, s.climateBlend,
            climatePermutationCache, climateOffsetCache);

    {
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

    for (int yi = 0; yi < YCOUNT; ++yi) {
        const int idx = lane * YCOUNT + yi;
        s.noise1[idx] = 0.0;
        s.noise2[idx] = 0.0;
        s.noise3[idx] = 0.0;
    }
    s.noise4[lane] = 0.0;
    s.noise5[lane] = 0.0;
    if (lane == 0) s.fullUpperCount = p20Counts[seedIndex];
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        loadTerrainStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE2_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, THREADS);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        if (lane == 0) buildUpper6YAxisCache(s.perlin, sy, s.yAxis);
        __syncthreads();
        double values[YCOUNT];
        perlin3Upper6SharedY(s.perlin, coarseX * sx, coarseZ * sz, s.yAxis, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise2[idx] = value;
            else s.noise2[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        loadTerrainStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE3_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, THREADS);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        if (lane == 0) buildUpper6YAxisCache(s.perlin, sy, s.yAxis);
        __syncthreads();
        double values[YCOUNT];
        perlin3Upper6SharedY(s.perlin, coarseX * sx, coarseZ * sz, s.yAxis, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise3[idx] = value;
            else s.noise3[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        loadTerrainStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE1_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, THREADS);
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        if (lane == 0) buildUpper6YAxisCache(s.perlin, sy, s.yAxis);
        __syncthreads();
        double values[YCOUNT];
        perlin3Upper6SharedY(s.perlin, coarseX * sx, coarseZ * sz, s.yAxis, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise1[idx] = value;
            else s.noise1[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        loadTerrainStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE4_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, THREADS);
        const double value = perlin2(
                s.perlin, coarseX * (1.121 * amplitude), coarseZ * (1.121 * amplitude))
                * (1.0 / amplitude);
        if (octave == 0) s.noise4[lane] = value;
        else s.noise4[lane] += value;
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        loadTerrainStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE5_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, THREADS);
        const double value = perlin2(
                s.perlin, coarseX * (200.0 * amplitude), coarseZ * (200.0 * amplitude))
                * (1.0 / amplitude);
        if (octave == 0) s.noise5[lane] = value;
        else s.noise5[lane] += value;
        __syncthreads();
        amplitude /= 2.0;
    }

    const double d2 = s.temp[lane];
    const double d3 = s.rain[lane] * d2;
    double d4 = 1.0 - d3;
    d4 *= d4;
    d4 *= d4;
    d4 = 1.0 - d4;

    double d5 = (s.noise4[lane] + 256.0) / 512.0;
    d5 *= d4;
    if (d5 > 1.0) d5 = 1.0;

    double d6 = s.noise5[lane] / 8000.0;
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
    const double d7 = 17.0 / 2.0 + d6 * 4.0;

    unsigned char mask = 0;
    for (int yi = 0; yi < YCOUNT; ++yi) {
        const int y = stage0gpu::UPPER_Y_FROM + yi;
        const int idx = lane * YCOUNT + yi;
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
            upperDensities[columnIndex * YCOUNT + yi] = d8;
        }
        if (d8 > 0.0) mask |= static_cast<unsigned char>(1u << yi);
    }

    upperMasks[columnIndex] = mask;
    columnShapeD5[columnIndex] = d5;
    columnShapeD7[columnIndex] = d7;
    if (mask != 0) atomicAdd(&s.fullUpperCount, 1);
    __syncthreads();

    if (lane == 0) fullUpperCounts[seedIndex] = s.fullUpperCount;
}

} // namespace p34perm
