#pragma once

#include "gpu_runtime_compat.hpp"
#include "stage0_exact_gpu.hpp"

#include <cstddef>
#include <cstdint>

namespace p30lower {

using p20::PerlinState;

// The production lower kernel launches 256 lanes even though only columns with
// a non-zero upper mask need lower-density work. This benchmark kernel first
// compacts those column indices, then evaluates dense batches of 32 or 64 lanes.
// Terrain and column-shape caches are required; that is the exact P28 production
// path and avoids changing any generator semantics.
template<int BLOCK_THREADS>
struct CompactScratch {
    PerlinState perlin;
    int activeIndices[stage0gpu::FULL_POINTS];
    double noise2[BLOCK_THREADS * stage0gpu::LOWER_YCOUNT];
    double blend[BLOCK_THREADS * stage0gpu::LOWER_YCOUNT];
    int activeCount;
    int highReentryCount;
};

template<int BLOCK_THREADS>
__global__ void stage0LowerCompactKernel(
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
        int coarseOffsetZ,
        int* activeColumnCounts
) {
    static_assert(BLOCK_THREADS == 32 || BLOCK_THREADS == 64,
                  "P30 benchmark supports 32- or 64-thread compact blocks");

    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= BLOCK_THREADS) return;

    __shared__ CompactScratch<BLOCK_THREADS> s;

    if (lane == 0) {
        s.activeCount = 0;
        s.highReentryCount = 0;
        highReentryCounts[seedIndex] = 0;
        if (activeColumnCounts != nullptr) activeColumnCounts[seedIndex] = 0;
    }

    // Make the topology output deterministic even for lanes that are not active.
    if (highestReentryY != nullptr) {
        for (int point = lane; point < stage0gpu::FULL_POINTS; point += BLOCK_THREADS) {
            highestReentryY[static_cast<std::size_t>(seedIndex) * stage0gpu::FULL_POINTS + point] = 0xFFu;
        }
    }
    __syncthreads();

    if (p20Counts[seedIndex] <= 0 || fullUpperCounts[seedIndex] < minUpperCount) {
        return;
    }

    // A serial 256-byte scan is tiny compared with the 40 Perlin-octave passes
    // below and gives deterministic compact ordering without wave-prefix logic.
    if (lane == 0) {
        const std::size_t base = static_cast<std::size_t>(seedIndex) * stage0gpu::FULL_POINTS;
        int active = 0;
        for (int point = 0; point < stage0gpu::FULL_POINTS; ++point) {
            if (upperMasks[base + point] != 0) s.activeIndices[active++] = point;
        }
        s.activeCount = active;
        if (activeColumnCounts != nullptr) activeColumnCounts[seedIndex] = active;
    }
    __syncthreads();

    if (terrainPermutationCache == nullptr || terrainOffsetCache == nullptr
            || columnShapeD5 == nullptr || columnShapeD7 == nullptr) {
        if (lane == 0) highReentryCounts[seedIndex] = -1;
        return;
    }

    for (int batchStart = 0; batchStart < s.activeCount; batchStart += BLOCK_THREADS) {
        const int remaining = s.activeCount - batchStart;
        const int batchCount = remaining < BLOCK_THREADS ? remaining : BLOCK_THREADS;
        const bool active = lane < batchCount;
        const int originalLane = active ? s.activeIndices[batchStart + lane] : 0;
        const std::size_t topologyIndex = static_cast<std::size_t>(seedIndex)
                * stage0gpu::FULL_POINTS + originalLane;
        const unsigned char upperMask = active ? upperMasks[topologyIndex] : 0;

        double coarseX = 0.0;
        double coarseZ = 0.0;
        double climateX = 0.0;
        double climateZ = 0.0;
        if (active) {
            stage0gpu::fullPointCoordinates(originalLane, coarseOffsetX, coarseOffsetZ,
                    coarseX, coarseZ, climateX, climateZ);
        }

        double noise3[stage0gpu::LOWER_YCOUNT];
        for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
            noise3[y] = 0.0;
            if (active) {
                const int compactIndex = lane * stage0gpu::LOWER_YCOUNT + y;
                s.noise2[compactIndex] = 0.0;
                s.blend[compactIndex] = 0.0;
            }
        }
        __syncthreads();

        double amplitude = 1.0;
        for (int octave = 0; octave < 16; ++octave) {
            terraincache::loadStateCooperative(
                    s.perlin, seedIndex, terraincache::NOISE2_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache,
                    lane, BLOCK_THREADS);
            if (active) {
                const double sx = 684.412 * amplitude;
                const double sy = 684.412 * amplitude;
                const double sz = 684.412 * amplitude;
                const double weight = 1.0 / amplitude;
                double values[stage0gpu::LOWER_YCOUNT];
                stage0gpu::perlin3Lower11(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
                for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
                    const int compactIndex = lane * stage0gpu::LOWER_YCOUNT + y;
                    const double value = values[y] * weight;
                    if (octave == 0) s.noise2[compactIndex] = value;
                    else s.noise2[compactIndex] += value;
                }
            }
            __syncthreads();
            amplitude /= 2.0;
        }

        amplitude = 1.0;
        for (int octave = 0; octave < 16; ++octave) {
            terraincache::loadStateCooperative(
                    s.perlin, seedIndex, terraincache::NOISE3_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache,
                    lane, BLOCK_THREADS);
            if (active) {
                const double sx = 684.412 * amplitude;
                const double sy = 684.412 * amplitude;
                const double sz = 684.412 * amplitude;
                const double weight = 1.0 / amplitude;
                double values[stage0gpu::LOWER_YCOUNT];
                stage0gpu::perlin3Lower11(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
                for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
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
            terraincache::loadStateCooperative(
                    s.perlin, seedIndex, terraincache::NOISE1_BASE + octave,
                    terrainPermutationCache, terrainOffsetCache,
                    lane, BLOCK_THREADS);
            if (active) {
                const double sx = (684.412 / 80.0) * amplitude;
                const double sy = (684.412 / 160.0) * amplitude;
                const double sz = (684.412 / 80.0) * amplitude;
                const double weight = 1.0 / amplitude;
                double values[stage0gpu::LOWER_YCOUNT];
                stage0gpu::perlin3Lower11(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
                for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
                    const int compactIndex = lane * stage0gpu::LOWER_YCOUNT + y;
                    const double value = values[y] * weight;
                    if (octave == 0) s.blend[compactIndex] = value;
                    else s.blend[compactIndex] += value;
                }
            }
            __syncthreads();
            amplitude /= 2.0;
        }

        if (active) {
            const double d5 = columnShapeD5[topologyIndex];
            const double d7 = columnShapeD7[topologyIndex];
            std::uint32_t positiveMask = static_cast<std::uint32_t>(upperMask)
                    << stage0gpu::UPPER_Y_FROM;

            for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
                const int compactIndex = lane * stage0gpu::LOWER_YCOUNT + y;
                double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
                if (d9 < 0.0) d9 *= 4.0;

                const double blend = (s.blend[compactIndex] / 10.0 + 1.0) / 2.0;
                double d8;
                if (blend < 0.0) {
                    d8 = s.noise2[compactIndex] / 512.0;
                } else if (blend > 1.0) {
                    d8 = noise3[y] / 512.0;
                } else {
                    const double d10 = s.noise2[compactIndex] / 512.0;
                    const double d11 = noise3[y] / 512.0;
                    d8 = d10 + (d11 - d10) * blend;
                }
                d8 -= d9;

                if (lowerDensities != nullptr) {
                    const std::size_t outIndex = (static_cast<std::size_t>(seedIndex)
                            * stage0gpu::FULL_POINTS + originalLane)
                            * stage0gpu::LOWER_YCOUNT + y;
                    lowerDensities[outIndex] = d8;
                }
                if (d8 > 0.0) positiveMask |= (1u << y);
            }

            const int highest = stage0gpu::highestReentryYIndex(positiveMask);
            if (highestReentryY != nullptr) {
                highestReentryY[topologyIndex] = highest < 0
                        ? 0xFFu
                        : static_cast<unsigned char>(highest);
            }
            if (highest >= stage0gpu::UPPER_Y_FROM) atomicAdd(&s.highReentryCount, 1);
        }
        __syncthreads();
    }

    if (lane == 0) highReentryCounts[seedIndex] = s.highReentryCount;
}

} // namespace p30lower
