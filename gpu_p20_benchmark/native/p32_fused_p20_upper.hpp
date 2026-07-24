#pragma once

#include "gpu_runtime_compat.hpp"
#include "stage0_exact_gpu.hpp"

#include <cstddef>
#include <cstdint>

namespace p32fused {

using p20::PerlinState;

static constexpr int P20_POINTS = 64;
static constexpr int UPPER_POINTS = 192;
static constexpr int YCOUNT = stage0gpu::UPPER_YCOUNT;

template<int BLOCK_THREADS>
struct FusedScratch {
    PerlinState perlin;
    double temp[UPPER_POINTS];
    double rain[UPPER_POINTS];
    double climateBlend[UPPER_POINTS];
    double noise1[UPPER_POINTS * YCOUNT];
    double noise2[UPPER_POINTS * YCOUNT];
    double noise3[UPPER_POINTS * YCOUNT];
    double noise4[UPPER_POINTS];
    double noise5[UPPER_POINTS];
    int p20Count;
    int fullUpperCount;
};

__device__ __forceinline__ int p20AxisValue(int i) {
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

__device__ __forceinline__ int p20FullLane(int compact) {
    return p20AxisValue(compact >> 3) * stage0gpu::FULL_SIZE
         + p20AxisValue(compact & 7);
}

__device__ __forceinline__ void p20Coordinates(
        int compact,
        int coarseOffsetX,
        int coarseOffsetZ,
        double& coarseX,
        double& coarseZ,
        double& climateX,
        double& climateZ
) {
    const int gx = p20AxisValue(compact >> 3);
    const int gz = p20AxisValue(compact & 7);
    coarseX = -28.0 + static_cast<double>(gx * 4 + coarseOffsetX);
    coarseZ = -28.0 + static_cast<double>(gz * 4 + coarseOffsetZ);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
}

__device__ __forceinline__ int nonP20AxisValue(int compact) {
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

__device__ __forceinline__ int fullLaneFromUpperCompact(int compactLane) {
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

template<int BLOCK_THREADS>
__device__ __forceinline__ void clearWorking(FusedScratch<BLOCK_THREADS>& s, int points) {
    const int lane = threadIdx.x;
    for (int point = lane; point < points; point += BLOCK_THREADS) {
        s.temp[point] = 0.0;
        s.rain[point] = 0.0;
        s.climateBlend[point] = 0.0;
        s.noise4[point] = 0.0;
        s.noise5[point] = 0.0;
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = point * YCOUNT + yi;
            s.noise1[idx] = 0.0;
            s.noise2[idx] = 0.0;
            s.noise3[idx] = 0.0;
        }
    }
    __syncthreads();
}

template<int BLOCK_THREADS, bool P20_PHASE>
__device__ __forceinline__ void accumulateClimate(
        FusedScratch<BLOCK_THREADS>& s,
        int seedIndex,
        int stateBase,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double* out,
        int coarseOffsetX,
        int coarseOffsetZ,
        const unsigned char* climatePermutationCache,
        const double* climateOffsetCache
) {
    const int lane = threadIdx.x;
    const int points = P20_PHASE ? P20_POINTS : UPPER_POINTS;
    double amplitudeWeight = 1.0;
    double frequency = 1.0;
    for (int octave = 0; octave < octaves; ++octave) {
        climatecache::loadStateCooperative(
                s.perlin, seedIndex, stateBase + octave,
                climatePermutationCache, climateOffsetCache,
                lane, BLOCK_THREADS);
        const double scaleX = (startScaleX / 1.5) * frequency;
        const double scaleZ = (startScaleZ / 1.5) * frequency;
        const double weight = 0.55 / amplitudeWeight;
        for (int point = lane; point < points; point += BLOCK_THREADS) {
            double coarseX, coarseZ, climateX, climateZ;
            if constexpr (P20_PHASE) {
                p20Coordinates(point, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            } else {
                const int fullLane = fullLaneFromUpperCompact(point);
                stage0gpu::fullPointCoordinates(fullLane, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            }
            out[point] += p20::simplex2(
                    s.perlin, climateX * scaleX, climateZ * scaleZ) * weight;
        }
        __syncthreads();
        frequency *= octaveScale;
        amplitudeWeight *= 0.5;
    }
}

template<int BLOCK_THREADS, bool P20_PHASE>
__device__ __forceinline__ void evaluateCachedPhase(
        FusedScratch<BLOCK_THREADS>& s,
        int seedIndex,
        int coarseOffsetX,
        int coarseOffsetZ,
        const unsigned char* terrainPermutationCache,
        const double* terrainOffsetCache,
        const unsigned char* climatePermutationCache,
        const double* climateOffsetCache
) {
    const int lane = threadIdx.x;
    const int points = P20_PHASE ? P20_POINTS : UPPER_POINTS;
    clearWorking(s, points);

    accumulateClimate<BLOCK_THREADS, P20_PHASE>(s, seedIndex,
            climatecache::TEMP_BASE, 4,
            0.02500000037252903, 0.02500000037252903, 0.25,
            s.temp, coarseOffsetX, coarseOffsetZ,
            climatePermutationCache, climateOffsetCache);
    accumulateClimate<BLOCK_THREADS, P20_PHASE>(s, seedIndex,
            climatecache::RAIN_BASE, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333,
            s.rain, coarseOffsetX, coarseOffsetZ,
            climatePermutationCache, climateOffsetCache);
    accumulateClimate<BLOCK_THREADS, P20_PHASE>(s, seedIndex,
            climatecache::BLEND_BASE, 2,
            0.25, 0.25, 0.5882352941176471,
            s.climateBlend, coarseOffsetX, coarseOffsetZ,
            climatePermutationCache, climateOffsetCache);

    for (int point = lane; point < points; point += BLOCK_THREADS) {
        const double d0 = s.climateBlend[point] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (s.temp[point] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (s.rain[point] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        s.temp[point] = d3;
        s.rain[point] = d4;
    }
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        terraincache::loadStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE2_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, BLOCK_THREADS);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int point = lane; point < points; point += BLOCK_THREADS) {
            double coarseX, coarseZ, climateX, climateZ;
            if constexpr (P20_PHASE) {
                p20Coordinates(point, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            } else {
                const int fullLane = fullLaneFromUpperCompact(point);
                stage0gpu::fullPointCoordinates(fullLane, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            }
            double values[YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = point * YCOUNT + yi;
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
                terrainPermutationCache, terrainOffsetCache, lane, BLOCK_THREADS);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int point = lane; point < points; point += BLOCK_THREADS) {
            double coarseX, coarseZ, climateX, climateZ;
            if constexpr (P20_PHASE) {
                p20Coordinates(point, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            } else {
                const int fullLane = fullLaneFromUpperCompact(point);
                stage0gpu::fullPointCoordinates(fullLane, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            }
            double values[YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = point * YCOUNT + yi;
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
                terrainPermutationCache, terrainOffsetCache, lane, BLOCK_THREADS);
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        for (int point = lane; point < points; point += BLOCK_THREADS) {
            double coarseX, coarseZ, climateX, climateZ;
            if constexpr (P20_PHASE) {
                p20Coordinates(point, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            } else {
                const int fullLane = fullLaneFromUpperCompact(point);
                stage0gpu::fullPointCoordinates(fullLane, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            }
            double values[YCOUNT];
            p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = point * YCOUNT + yi;
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
                terrainPermutationCache, terrainOffsetCache, lane, BLOCK_THREADS);
        for (int point = lane; point < points; point += BLOCK_THREADS) {
            double coarseX, coarseZ, climateX, climateZ;
            if constexpr (P20_PHASE) {
                p20Coordinates(point, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            } else {
                const int fullLane = fullLaneFromUpperCompact(point);
                stage0gpu::fullPointCoordinates(fullLane, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            }
            const double value = p20::perlin2(
                    s.perlin, coarseX * (1.121 * amplitude), coarseZ * (1.121 * amplitude))
                    * (1.0 / amplitude);
            if (octave == 0) s.noise4[point] = value;
            else s.noise4[point] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        terraincache::loadStateCooperative(
                s.perlin, seedIndex, terraincache::NOISE5_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, BLOCK_THREADS);
        for (int point = lane; point < points; point += BLOCK_THREADS) {
            double coarseX, coarseZ, climateX, climateZ;
            if constexpr (P20_PHASE) {
                p20Coordinates(point, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            } else {
                const int fullLane = fullLaneFromUpperCompact(point);
                stage0gpu::fullPointCoordinates(fullLane, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
            }
            const double value = p20::perlin2(
                    s.perlin, coarseX * (200.0 * amplitude), coarseZ * (200.0 * amplitude))
                    * (1.0 / amplitude);
            if (octave == 0) s.noise5[point] = value;
            else s.noise5[point] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }
}

template<int BLOCK_THREADS>
__global__ void fusedCachedP20UpperKernel(
        const std::int64_t* seeds,
        int count,
        int* p20Counts,
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
    static_assert(BLOCK_THREADS == 64 || BLOCK_THREADS == 128 || BLOCK_THREADS == 192,
                  "P32 supports 64, 128, or 192 threads");
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= BLOCK_THREADS) return;
    (void)seeds;

    __shared__ FusedScratch<BLOCK_THREADS> s;
    if (lane == 0) s.p20Count = 0;
    __syncthreads();

    evaluateCachedPhase<BLOCK_THREADS, true>(s, seedIndex,
            coarseOffsetX, coarseOffsetZ,
            terrainPermutationCache, terrainOffsetCache,
            climatePermutationCache, climateOffsetCache);

    for (int point = lane; point < P20_POINTS; point += BLOCK_THREADS) {
        const double d2 = s.temp[point];
        const double d3 = s.rain[point] * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;
        double d5 = (s.noise4[point] + 256.0) / 512.0;
        d5 *= d4;
        if (d5 > 1.0) d5 = 1.0;
        double d6 = s.noise5[point] / 8000.0;
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
            const int idx = point * YCOUNT + yi;
            double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
            if (d9 < 0.0) d9 *= 4.0;
            const double blend = (s.noise1[idx] / 10.0 + 1.0) / 2.0;
            double d8;
            if (blend < 0.0) d8 = s.noise2[idx] / 512.0;
            else if (blend > 1.0) d8 = s.noise3[idx] / 512.0;
            else {
                const double d10 = s.noise2[idx] / 512.0;
                const double d11 = s.noise3[idx] / 512.0;
                d8 = d10 + (d11 - d10) * blend;
            }
            d8 -= d9;
            if (y > 13) {
                const double d13 = static_cast<double>(static_cast<float>(y - 13) / 3.0F);
                d8 = d8 * (1.0 - d13) + -10.0 * d13;
            }
            if (d8 > 0.0) mask |= static_cast<unsigned char>(1u << yi);
        }
        const int fullLane = p20FullLane(point);
        const std::size_t columnIndex = static_cast<std::size_t>(seedIndex)
                * stage0gpu::FULL_POINTS + fullLane;
        upperMasks[columnIndex] = mask;
        columnShapeD5[columnIndex] = d5;
        columnShapeD7[columnIndex] = d7;
        if (mask != 0) atomicAdd(&s.p20Count, 1);
    }
    __syncthreads();

    if (lane == 0) p20Counts[seedIndex] = s.p20Count;
    __syncthreads();
    if (s.p20Count <= 0) {
        if (lane == 0) fullUpperCounts[seedIndex] = 0;
        return;
    }

    if (lane == 0) s.fullUpperCount = s.p20Count;
    __syncthreads();
    evaluateCachedPhase<BLOCK_THREADS, false>(s, seedIndex,
            coarseOffsetX, coarseOffsetZ,
            terrainPermutationCache, terrainOffsetCache,
            climatePermutationCache, climateOffsetCache);

    for (int point = lane; point < UPPER_POINTS; point += BLOCK_THREADS) {
        const double d2 = s.temp[point];
        const double d3 = s.rain[point] * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;
        double d5 = (s.noise4[point] + 256.0) / 512.0;
        d5 *= d4;
        if (d5 > 1.0) d5 = 1.0;
        double d6 = s.noise5[point] / 8000.0;
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
        const int fullLane = fullLaneFromUpperCompact(point);
        const std::size_t columnIndex = static_cast<std::size_t>(seedIndex)
                * stage0gpu::FULL_POINTS + fullLane;
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int y = stage0gpu::UPPER_Y_FROM + yi;
            const int idx = point * YCOUNT + yi;
            double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
            if (d9 < 0.0) d9 *= 4.0;
            const double blend = (s.noise1[idx] / 10.0 + 1.0) / 2.0;
            double d8;
            if (blend < 0.0) d8 = s.noise2[idx] / 512.0;
            else if (blend > 1.0) d8 = s.noise3[idx] / 512.0;
            else {
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
    }
    __syncthreads();
    if (lane == 0) fullUpperCounts[seedIndex] = s.fullUpperCount;
}

} // namespace p32fused
