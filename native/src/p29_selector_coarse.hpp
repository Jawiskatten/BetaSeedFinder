#pragma once

#include "coarse_exact_gpu.hpp"

#include <cstddef>
#include <cstdint>

namespace p29coarse {

static constexpr std::uint32_t FULL_Y_MASK = (1u << coarsecore::Y_LEVELS) - 1u;
static constexpr int SELECTOR_STAT_COUNT = 6;
enum SelectorStat {
    CELL_NOISE2_ONLY = 0,
    CELL_NOISE3_ONLY = 1,
    CELL_BOTH = 2,
    COLUMN_NOISE2_ONLY = 3,
    COLUMN_NOISE3_ONLY = 4,
    COLUMN_BOTH = 5
};

// Exact masked form of coarsegpu::perlin3Full17CachedXYZ.
//
// A mask bit says that the final terrain blend will read this Y value from the
// current noise map. Whole inactive Y lattice groups skip their gradient work.
// For active groups, the corner gradients are built from the same first Y in
// the group as the production kernel, preserving the exact operation order.
__device__ __forceinline__ void perlin3Masked17CachedXYZ(
        const p20::PerlinState& p,
        const coarsegpu::PerlinAxisCache& cache,
        int xIndex,
        int zIndex,
        std::uint32_t mask,
        double out17[coarsecore::Y_LEVELS]
) {
    if (mask == 0u) return;
    if (mask == FULL_Y_MASK) {
        coarsegpu::perlin3Full17CachedXYZ(p, cache, xIndex, zIndex, out17);
        return;
    }

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
    bool groupActive = false;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
        const int yi = cache.yIndex[y];
        const double yf = cache.yFrac[y];
        const double yfm1 = cache.yFracMinus1[y];
        const double yfade = cache.yFade[y];
        if (y == 0 || yi != previousYi) {
            previousYi = yi;
            int groupEnd = y + 1;
            while (groupEnd < coarsecore::Y_LEVELS && cache.yIndex[groupEnd] == yi) ++groupEnd;
            const std::uint32_t groupWidth = static_cast<std::uint32_t>(groupEnd - y);
            const std::uint32_t groupMask = ((1u << groupWidth) - 1u) << y;
            groupActive = (mask & groupMask) != 0u;
            if (groupActive) {
                const int p0 = xp0 + yi;
                const int p00 = p.perm[p0] + zi;
                const int p01 = p.perm[p0 + 1] + zi;
                const int p1 = xp1 + yi;
                const int p10 = p.perm[p1] + zi;
                const int p11 = p.perm[p1 + 1] + zi;
                q00 = p20::lerp(xf,
                        coarsegpu::grad3BranchlessExact(p.perm[p00], x, yf, z),
                        coarsegpu::grad3BranchlessExact(p.perm[p10], x1, yf, z));
                q01 = p20::lerp(xf,
                        coarsegpu::grad3BranchlessExact(p.perm[p01], x, yfm1, z),
                        coarsegpu::grad3BranchlessExact(p.perm[p11], x1, yfm1, z));
                q10 = p20::lerp(xf,
                        coarsegpu::grad3BranchlessExact(p.perm[p00 + 1], x, yf, z1),
                        coarsegpu::grad3BranchlessExact(p.perm[p10 + 1], x1, yf, z1));
                q11 = p20::lerp(xf,
                        coarsegpu::grad3BranchlessExact(p.perm[p01 + 1], x, yfm1, z1),
                        coarsegpu::grad3BranchlessExact(p.perm[p11 + 1], x1, yfm1, z1));
            }
        }
        if (!groupActive || (mask & (1u << y)) == 0u) continue;
        const double r0 = p20::lerp(yfade, q00, q01);
        const double r1 = p20::lerp(yfade, q10, q11);
        out17[y] = p20::lerp(zf, r0, r1);
    }
}

// P29 research kernel. It requires the exact P28 terrain and climate caches.
// noise1 is evaluated first because it is the final selector between noise2
// and noise3. Per-cell masks then prevent evaluation of values that cannot be
// read by the final density formula.
__global__ void generateCoarseSignsSelectorFirstKernel(
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
        const double* climateOffsetCache,
        std::uint32_t* noise2Masks,
        std::uint32_t* noise3Masks,
        unsigned int* selectorStats
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count) return;
    if (cacheSeedIndices == nullptr || terrainPermutationCache == nullptr
            || terrainOffsetCache == nullptr || climatePermutationCache == nullptr
            || climateOffsetCache == nullptr || noise2Masks == nullptr || noise3Masks == nullptr) return;

    __shared__ p20::PerlinState perlin;
    __shared__ coarsegpu::PerlinAxisCache axes;
    __shared__ unsigned int blockStats[SELECTOR_STAT_COUNT];

    const int cacheSeedIndex = cacheSeedIndices[seedIndex];
    const std::size_t colBase = static_cast<std::size_t>(seedIndex) * coarsecore::COLUMNS;
    const std::size_t cellBase = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;

    const int climateOctaves[3] = {4, 4, 2};
    const double startScaleX[3] = {0.02500000037252903, 0.05000000074505806, 0.25};
    const double startScaleZ[3] = {0.02500000037252903, 0.05000000074505806, 0.25};
    const double octaveScale[3] = {0.25, 0.3333333333333333, 0.5882352941176471};
    double* climateOut[3] = {temp, rain, climateBlend};

    for (int kind = 0; kind < 3; ++kind) {
        double d6 = 1.0;
        double d7 = 1.0;
        for (int octave = 0; octave < climateOctaves[kind]; ++octave) {
            const int stateBase = kind == 0 ? climatecache::TEMP_BASE
                    : (kind == 1 ? climatecache::RAIN_BASE : climatecache::BLEND_BASE);
            climatecache::loadStateCooperative(
                    perlin, cacheSeedIndex, stateBase + octave,
                    climatePermutationCache, climateOffsetCache, lane, blockDim.x);
            const double sx = (startScaleX[kind] / 1.5) * d7;
            const double sz = (startScaleZ[kind] / 1.5) * d7;
            const double weight = 0.55 / d6;
            for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
                double coarseX, coarseZ, climateX, climateZ;
                coarsegpu::coordinates(column, coarseOffsetX, coarseOffsetZ,
                        coarseX, coarseZ, climateX, climateZ);
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

    // Selector noise first. Cached state lookup makes the generator order
    // independent while retaining exact per-map arithmetic.
    double amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        terraincache::loadStateCooperative(
                perlin, cacheSeedIndex, terraincache::NOISE1_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        coarsegpu::preparePerlinAxes3D(perlin, sx, sy, sz,
                coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            coarsegpu::perlin3Full17CachedXYZ(perlin, axes, xIndex, zIndex, values);
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

    if (selectorStats != nullptr && lane < SELECTOR_STAT_COUNT) blockStats[lane] = 0u;
    __syncthreads();

    unsigned int localStats[SELECTOR_STAT_COUNT] = {};
    for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
        const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
        std::uint32_t need2 = 0u;
        std::uint32_t need3 = 0u;
        for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
            const double selector = noise1[base + y];
            if (selector < -10.0) {
                need2 |= 1u << y;
                if (selectorStats != nullptr) ++localStats[CELL_NOISE2_ONLY];
            } else if (selector > 10.0) {
                need3 |= 1u << y;
                if (selectorStats != nullptr) ++localStats[CELL_NOISE3_ONLY];
            } else {
                need2 |= 1u << y;
                need3 |= 1u << y;
                if (selectorStats != nullptr) ++localStats[CELL_BOTH];
            }
        }
        noise2Masks[colBase + column] = need2;
        noise3Masks[colBase + column] = need3;
        if (selectorStats != nullptr) {
            if (need3 == 0u) ++localStats[COLUMN_NOISE2_ONLY];
            else if (need2 == 0u) ++localStats[COLUMN_NOISE3_ONLY];
            else ++localStats[COLUMN_BOTH];
        }
    }

    if (selectorStats != nullptr) {
        for (int stat = 0; stat < SELECTOR_STAT_COUNT; ++stat) {
            atomicAdd(&blockStats[stat], localStats[stat]);
        }
    }
    __syncthreads();
    if (selectorStats != nullptr && lane < SELECTOR_STAT_COUNT) {
        selectorStats[static_cast<std::size_t>(seedIndex) * SELECTOR_STAT_COUNT + lane] = blockStats[lane];
    }
    __syncthreads();

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        terraincache::loadStateCooperative(
                perlin, cacheSeedIndex, terraincache::NOISE2_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        coarsegpu::preparePerlinAxes3D(perlin, sx, sy, sz,
                coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const std::uint32_t mask = noise2Masks[colBase + column];
            if (mask == 0u) continue;
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            perlin3Masked17CachedXYZ(perlin, axes, xIndex, zIndex, mask, values);
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                if ((mask & (1u << y)) == 0u) continue;
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
        terraincache::loadStateCooperative(
                perlin, cacheSeedIndex, terraincache::NOISE3_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        coarsegpu::preparePerlinAxes3D(perlin, sx, sy, sz,
                coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const std::uint32_t mask = noise3Masks[colBase + column];
            if (mask == 0u) continue;
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            double values[coarsecore::Y_LEVELS];
            perlin3Masked17CachedXYZ(perlin, axes, xIndex, zIndex, mask, values);
            const std::size_t base = cellBase + static_cast<std::size_t>(column) * coarsecore::Y_LEVELS;
            for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
                if ((mask & (1u << y)) == 0u) continue;
                const double value = values[y] * weight;
                if (octave == 0) noise3[base + y] = value;
                else noise3[base + y] += value;
            }
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        terraincache::loadStateCooperative(
                perlin, cacheSeedIndex, terraincache::NOISE4_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        const double scale = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        coarsegpu::preparePerlinAxes(perlin, scale, scale,
                coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            const double value = coarsegpu::perlin2CachedXZ(perlin, axes, xIndex, zIndex) * weight;
            if (octave == 0) noise4[colBase + column] = value;
            else noise4[colBase + column] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        terraincache::loadStateCooperative(
                perlin, cacheSeedIndex, terraincache::NOISE5_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, lane, blockDim.x);
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        coarsegpu::preparePerlinAxes(perlin, scale, scale,
                coarseOffsetX, coarseOffsetZ, axes);
        for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
            const int xIndex = column / coarsecore::SIZE;
            const int zIndex = column - xIndex * coarsecore::SIZE;
            const double value = coarsegpu::perlin2CachedXZ(perlin, axes, xIndex, zIndex) * weight;
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

} // namespace p29coarse
