#pragma once

#include "gpu_runtime_compat.hpp"
#include "stage0_exact_gpu.hpp"
#include "terrain_perlin_cache.hpp"
#include "climate_perlin_cache.hpp"

#include <cstddef>
#include <cstdint>

namespace p40center {

using p20::PerlinState;

static constexpr int LANES_PER_CENTER = 64;
static constexpr int YCOUNT = 6;

__device__ __forceinline__ int axisValue(int i) {
    switch (i) {
        case 0: return 0;
        case 1: return 2;
        case 2: return 5;
        case 3: return 7;
        case 4: return 8;
        case 5: return 10;
        case 6: return 13;
        default: return 15;
    }
}

template<int CENTERS>
struct GroupP20Scratch {
    static constexpr int THREADS = CENTERS * LANES_PER_CENTER;
    static constexpr int DENSITY_COUNT = THREADS * YCOUNT;

    PerlinState perlin;
    double tempRaw[THREADS];
    double rainRaw[THREADS];
    double climateBlend[THREADS];
    double temperature[THREADS];
    double rain[THREADS];
    double noise1[DENSITY_COUNT];
    double noise2[DENSITY_COUNT];
    double noise3[DENSITY_COUNT];
    double noise4[THREADS];
    double noise5[THREADS];
    int positiveCount[CENTERS];
};

static_assert(sizeof(GroupP20Scratch<2>) <= 64 * 1024, "P40 pair scratch exceeds 64 KiB LDS");
static_assert(sizeof(GroupP20Scratch<4>) <= 64 * 1024, "P40 group4 scratch exceeds 64 KiB LDS");

__device__ __forceinline__ void pointCoordinates(
        int lane,
        int coarseOffsetX,
        int coarseOffsetZ,
        double& coarseX,
        double& coarseZ,
        double& climateX,
        double& climateZ
) {
    const int ix = lane >> 3;
    const int iz = lane & 7;
    const int gx = axisValue(ix);
    const int gz = axisValue(iz);
    coarseX = -28.0 + static_cast<double>(gx * 4 + coarseOffsetX);
    coarseZ = -28.0 + static_cast<double>(gz * 4 + coarseOffsetZ);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
}

template<int CENTERS>
__device__ __forceinline__ void accumulateClimateCached(
        GroupP20Scratch<CENTERS>& s,
        int seedIndex,
        int stateBase,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double climateX,
        double climateZ,
        double* out,
        bool active,
        const unsigned char* climatePermutationCache,
        const double* climateOffsetCache
) {
    constexpr int THREADS = GroupP20Scratch<CENTERS>::THREADS;
    const int tid = threadIdx.x;
    out[tid] = 0.0;
    __syncthreads();

    double amplitudeWeight = 1.0;
    double frequency = 1.0;
    for (int octave = 0; octave < octaves; ++octave) {
        climatecache::loadStateCooperative(
                s.perlin, seedIndex, stateBase + octave,
                climatePermutationCache, climateOffsetCache,
                tid, THREADS);
        if (active) {
            const double scaleX = (startScaleX / 1.5) * frequency;
            const double scaleZ = (startScaleZ / 1.5) * frequency;
            const double weight = 0.55 / amplitudeWeight;
            out[tid] += p20::simplex2(s.perlin, climateX * scaleX, climateZ * scaleZ) * weight;
        }
        __syncthreads();
        frequency *= octaveScale;
        amplitudeWeight *= 0.5;
    }
}

// Exact cached same-stage fusion for P20. One workgroup handles multiple search
// centers from the same world. Each Perlin state is reconstructed once, then all
// center groups evaluate their own real coordinates before the next octave.
// Center zero still uses the original cache-building kernel; this kernel is for
// cached centers only.
template<int CENTERS>
__global__ void p20CachedCenterGroupKernel(
        const std::int64_t* seeds,
        int count,
        int centerBase,
        int activeCenters,
        const int* centerOffsetX,
        const int* centerOffsetZ,
        int* positiveCountsAll,
        unsigned char* upperMasksAll,
        double* columnShapeD5All,
        double* columnShapeD7All,
        const unsigned char* terrainPermutationCache,
        const double* terrainOffsetCache,
        const unsigned char* climatePermutationCache,
        const double* climateOffsetCache
) {
    constexpr int THREADS = GroupP20Scratch<CENTERS>::THREADS;
    const int seedIndex = blockIdx.x;
    const int tid = threadIdx.x;
    if (seedIndex >= count || tid >= THREADS) return;

    const int localCenter = tid / LANES_PER_CENTER;
    const int lane = tid - localCenter * LANES_PER_CENTER;
    const bool active = localCenter < activeCenters;
    const int centerIndex = centerBase + localCenter;

    __shared__ GroupP20Scratch<CENTERS> s;

    double coarseX = 0.0, coarseZ = 0.0, climateX = 0.0, climateZ = 0.0;
    if (active) {
        pointCoordinates(lane, centerOffsetX[centerIndex], centerOffsetZ[centerIndex],
                coarseX, coarseZ, climateX, climateZ);
    }

    accumulateClimateCached(s, seedIndex, climatecache::TEMP_BASE, 4,
            0.02500000037252903, 0.02500000037252903, 0.25,
            climateX, climateZ, s.tempRaw, active,
            climatePermutationCache, climateOffsetCache);
    accumulateClimateCached(s, seedIndex, climatecache::RAIN_BASE, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333,
            climateX, climateZ, s.rainRaw, active,
            climatePermutationCache, climateOffsetCache);
    accumulateClimateCached(s, seedIndex, climatecache::BLEND_BASE, 2,
            0.25, 0.25, 0.5882352941176471,
            climateX, climateZ, s.climateBlend, active,
            climatePermutationCache, climateOffsetCache);

    if (active) {
        const double d0 = s.climateBlend[tid] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (s.tempRaw[tid] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (s.rainRaw[tid] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        s.temperature[tid] = d3;
        s.rain[tid] = d4;
    } else {
        s.temperature[tid] = 0.0;
        s.rain[tid] = 0.0;
    }

    for (int yi = 0; yi < YCOUNT; ++yi) {
        const int idx = tid * YCOUNT + yi;
        s.noise1[idx] = 0.0;
        s.noise2[idx] = 0.0;
        s.noise3[idx] = 0.0;
    }
    s.noise4[tid] = 0.0;
    s.noise5[tid] = 0.0;
    if (lane == 0 && localCenter < CENTERS) s.positiveCount[localCenter] = 0;
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        terraincache::loadStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE2_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, tid, THREADS);
        if (active) {
            const double sx = 684.412 * amplitude;
            const double sy = 684.412 * amplitude;
            const double sz = 684.412 * amplitude;
            const double weight = 1.0 / amplitude;
            double values[YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = tid * YCOUNT + yi;
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
        terraincache::loadStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE3_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, tid, THREADS);
        if (active) {
            const double sx = 684.412 * amplitude;
            const double sy = 684.412 * amplitude;
            const double sz = 684.412 * amplitude;
            const double weight = 1.0 / amplitude;
            double values[YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = tid * YCOUNT + yi;
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
        terraincache::loadStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE1_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, tid, THREADS);
        if (active) {
            const double sx = (684.412 / 80.0) * amplitude;
            const double sy = (684.412 / 160.0) * amplitude;
            const double sz = (684.412 / 80.0) * amplitude;
            const double weight = 1.0 / amplitude;
            double values[YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = tid * YCOUNT + yi;
                const double value = values[yi] * weight;
                if (octave == 0) s.noise1[idx] = value;
                else s.noise1[idx] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        terraincache::loadStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE4_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, tid, THREADS);
        if (active) {
            const double value = p20::perlin2(
                    s.perlin, coarseX * (1.121 * amplitude), coarseZ * (1.121 * amplitude))
                    * (1.0 / amplitude);
            if (octave == 0) s.noise4[tid] = value;
            else s.noise4[tid] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        terraincache::loadStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE5_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, tid, THREADS);
        if (active) {
            const double value = p20::perlin2(
                    s.perlin, coarseX * (200.0 * amplitude), coarseZ * (200.0 * amplitude))
                    * (1.0 / amplitude);
            if (octave == 0) s.noise5[tid] = value;
            else s.noise5[tid] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    if (active) {
        const double d2 = s.temperature[tid];
        const double d3 = s.rain[tid] * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;

        double d5 = (s.noise4[tid] + 256.0) / 512.0;
        d5 *= d4;
        if (d5 > 1.0) d5 = 1.0;

        double d6 = s.noise5[tid] / 8000.0;
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

        const int fullLane = axisValue(lane >> 3) * 16 + axisValue(lane & 7);
        const std::size_t centerSeedIndex = static_cast<std::size_t>(centerIndex) * count + seedIndex;
        const std::size_t fullColumnIndex = centerSeedIndex * stage0gpu::FULL_POINTS + fullLane;
        unsigned char upperMask = 0;
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int y = 11 + yi;
            const int idx = tid * YCOUNT + yi;
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
            if (d8 > 0.0) upperMask |= static_cast<unsigned char>(1u << yi);
        }

        upperMasksAll[fullColumnIndex] = upperMask;
        columnShapeD5All[fullColumnIndex] = d5;
        columnShapeD7All[fullColumnIndex] = d7;
        if (upperMask != 0) atomicAdd(&s.positiveCount[localCenter], 1);
    }
    __syncthreads();

    if (active && lane == 0) {
        positiveCountsAll[static_cast<std::size_t>(centerIndex) * count + seedIndex] =
                s.positiveCount[localCenter];
    }
}

} // namespace p40center
