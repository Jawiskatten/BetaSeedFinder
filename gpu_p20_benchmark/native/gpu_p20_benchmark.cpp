#include "gpu_runtime_compat.hpp"

#include "p20_exact_math.hpp"
#include "terrain_perlin_cache.hpp"
#include "climate_perlin_cache.hpp"
#include "stage0_exact_gpu.hpp"
#include "coarse_exact_gpu.hpp"
#include "p29_selector_coarse.hpp"
#include "p30_compact_lower.hpp"
#include "p31_compact_upper.hpp"
#include "p32_fused_p20_upper.hpp"
#include "p33_shared_y.hpp"
#include "p34_compact_perm.hpp"
#include "p35_direct_coarse.hpp"
#include "p36_parallel_coarse_score.hpp"
#include "p19_monster_gate.hpp"
#include "early_detail_profile.hpp"
#include "init_perlin_detail_profile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace p20gpu {

using p20::JavaRandom;
using p20::PerlinState;

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
static constexpr int POINTS = 64;
static constexpr int YCOUNT = 6;
static constexpr int DENSITY_COUNT = POINTS * YCOUNT;

struct SharedScratch {
    JavaRandom rng;
    PerlinState perlin;
    int shuffleJ[256];
    std::uint64_t shuffleBaseState;
    int shuffleRejected;
    double tempRaw[POINTS];
    double rainRaw[POINTS];
    double climateBlend[POINTS];
    double temperature[POINTS];
    double rain[POINTS];
    double noise1[DENSITY_COUNT];
    double noise2[DENSITY_COUNT];
    double noise3[DENSITY_COUNT];
    double noise4[POINTS];
    double noise5[POINTS];
    int positiveCount;
};

struct SharedYScratch : SharedScratch {
    p20::Upper6YAxisCache yAxis;
};


// Exact speculative fast path for the 256 nextInt(bound) calls inside terrain
// Perlin initialization. Java's 48-bit LCG can jump directly to draw N, so the
// 64 P20 lanes generate four candidate shuffle indices each in parallel. The
// overwhelmingly common one-draw case advances the RNG by exactly 256 draws and
// replays the exact dependent swaps serially. Any Java rejection restores the
// pre-shuffle RNG state and falls back to the original serial loop.
__device__ __forceinline__ void javaLcgJumpCoefficients(
        unsigned int draws,
        std::uint64_t& outMul,
        std::uint64_t& outAdd
) {
    std::uint64_t accMul = 1ULL;
    std::uint64_t accAdd = 0ULL;
    std::uint64_t curMul = p20::JAVA_MULT;
    std::uint64_t curAdd = p20::JAVA_ADD;
    while (draws != 0u) {
        if ((draws & 1u) != 0u) {
            accAdd = (curMul * accAdd + curAdd) & p20::JAVA_MASK;
            accMul = (curMul * accMul) & p20::JAVA_MASK;
        }
        const std::uint64_t oldMul = curMul;
        curAdd = (oldMul * curAdd + curAdd) & p20::JAVA_MASK;
        curMul = (oldMul * oldMul) & p20::JAVA_MASK;
        draws >>= 1u;
    }
    outMul = accMul;
    outAdd = accAdd;
}

static constexpr std::uint64_t JAVA_LCG_JUMP64_MUL = 0xAB768C7E6901ULL;
static constexpr std::uint64_t JAVA_LCG_JUMP64_ADD = 0xF77B98004E40ULL;
static constexpr std::uint64_t JAVA_LCG_JUMP256_MUL = 0x4FA0405FA401ULL;
static constexpr std::uint64_t JAVA_LCG_JUMP256_ADD = 0xA7A83E92B900ULL;

__device__ __forceinline__ std::uint64_t jumpJava64(std::uint64_t state) {
    return (JAVA_LCG_JUMP64_MUL * state + JAVA_LCG_JUMP64_ADD) & p20::JAVA_MASK;
}

__device__ __forceinline__ std::uint64_t jumpJava256(std::uint64_t state) {
    return (JAVA_LCG_JUMP256_MUL * state + JAVA_LCG_JUMP256_ADD) & p20::JAVA_MASK;
}

__device__ __forceinline__ void initTerrainPerlinParallel(
        SharedScratch& s,
        std::uint64_t laneJumpMul,
        std::uint64_t laneJumpAdd
) {
    const int lane = threadIdx.x;

    if (lane == 0) {
        s.perlin.a = s.rng.nextDouble() * 256.0;
        s.perlin.b = s.rng.nextDouble() * 256.0;
        s.perlin.c = s.rng.nextDouble() * 256.0;
        s.shuffleBaseState = s.rng.state;
        s.shuffleRejected = 0;
    }
    __syncthreads();

    std::uint64_t drawState = (laneJumpMul * s.shuffleBaseState + laneJumpAdd) & p20::JAVA_MASK;
    for (int i = lane; i < 256; i += POINTS) {
        s.perlin.perm[i] = i;

        const int bound = 256 - i;
        const std::uint32_t bits = static_cast<std::uint32_t>(drawState >> 17);
        int value;
        bool rejected = false;
        if ((bound & -bound) == bound) {
            value = static_cast<int>((static_cast<std::int64_t>(bound) * bits) >> 31);
        } else {
            value = static_cast<int>(bits % static_cast<std::uint32_t>(bound));
            const std::uint32_t wrapped = bits
                    - static_cast<std::uint32_t>(value)
                    + static_cast<std::uint32_t>(bound - 1);
            rejected = static_cast<std::int32_t>(wrapped) < 0;
        }
        s.shuffleJ[i] = value + i;
        if (rejected) atomicAdd(&s.shuffleRejected, 1);
        drawState = jumpJava64(drawState);
    }
    __syncthreads();

    if (lane == 0) {
        if (s.shuffleRejected != 0) {
            // Exact rare fallback: restore the state immediately after the three
            // offset doubles, then replay the original Java nextInt loop verbatim.
            s.rng.state = s.shuffleBaseState;
            for (int i = 0; i < 256; ++i) s.perlin.perm[i] = i;
            for (int i = 0; i < 256; ++i) {
                const int j = s.rng.nextInt(256 - i) + i;
                const int tmp = s.perlin.perm[i];
                s.perlin.perm[i] = s.perlin.perm[j];
                s.perlin.perm[j] = tmp;
                s.perlin.perm[i + 256] = s.perlin.perm[i];
            }
        } else {
            // The candidate draw for every bound was accepted. The j sequence is
            // therefore identical to 256 serial nextInt calls; only generation was
            // parallelized. Replay the dependent swaps in the original order.
            for (int i = 0; i < 256; ++i) {
                const int j = s.shuffleJ[i];
                const int tmp = s.perlin.perm[i];
                s.perlin.perm[i] = s.perlin.perm[j];
                s.perlin.perm[j] = tmp;
                s.perlin.perm[i + 256] = s.perlin.perm[i];
            }
            s.rng.state = jumpJava256(s.shuffleBaseState);
        }
    }
    __syncthreads();
}


// The eight skipped noise1 states only exist to advance Java RNG. They never feed
// terrain evaluation or the cross-stage cache, so preserve the exact RNG sequence
// without constructing or mutating a permutation that will immediately be thrown away.
__device__ __forceinline__ void consumeTerrainPerlinParallel(
        SharedScratch& s,
        std::uint64_t laneJumpMul,
        std::uint64_t laneJumpAdd
) {
    const int lane = threadIdx.x;
    if (lane == 0) {
        (void)s.rng.nextDouble();
        (void)s.rng.nextDouble();
        (void)s.rng.nextDouble();
        s.shuffleBaseState = s.rng.state;
        s.shuffleRejected = 0;
    }
    __syncthreads();

    std::uint64_t drawState = (laneJumpMul * s.shuffleBaseState + laneJumpAdd) & p20::JAVA_MASK;
    for (int i = lane; i < 256; i += POINTS) {
        const int bound = 256 - i;
        const std::uint32_t bits = static_cast<std::uint32_t>(drawState >> 17);
        if ((bound & -bound) != bound) {
            const int value = static_cast<int>(bits % static_cast<std::uint32_t>(bound));
            const std::uint32_t wrapped = bits
                    - static_cast<std::uint32_t>(value)
                    + static_cast<std::uint32_t>(bound - 1);
            if (static_cast<std::int32_t>(wrapped) < 0) atomicAdd(&s.shuffleRejected, 1);
        }
        drawState = jumpJava64(drawState);
    }
    __syncthreads();

    if (lane == 0) {
        if (s.shuffleRejected != 0) {
            s.rng.state = s.shuffleBaseState;
            for (int i = 0; i < 256; ++i) (void)s.rng.nextInt(256 - i);
        } else {
            s.rng.state = jumpJava256(s.shuffleBaseState);
        }
    }
    __syncthreads();
}

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

__device__ void accumulateSimplexGroup(
        SharedScratch& s,
        std::int64_t seed,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double octavePersistence,
        double climateX,
        double climateZ,
        double* out,
        int seedIndex,
        int stateBase,
        unsigned char* climatePermutationCache,
        double* climateOffsetCache,
        int loadClimateCache
) {
    const int lane = threadIdx.x;
    if (loadClimateCache == 0 && lane == 0) s.rng.setSeed(seed);
    out[lane] = 0.0;
    __syncthreads();

    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < octaves; ++octave) {
        if (loadClimateCache != 0) {
            climatecache::loadStateCooperative(
                    s.perlin, seedIndex, stateBase + octave,
                    climatePermutationCache, climateOffsetCache,
                    lane, POINTS);
        } else {
            if (lane == 0) p20::initPerlin(s.rng, s.perlin);
            __syncthreads();
            if (climatePermutationCache != nullptr) {
                climatecache::storeStateCooperative(
                        s.perlin, seedIndex, stateBase + octave,
                        climatePermutationCache, climateOffsetCache,
                        lane, POINTS);
            }
        }
        const double scaleX = (startScaleX / 1.5) * d7;
        const double scaleZ = (startScaleZ / 1.5) * d7;
        const double weight = 0.55 / d6;
        out[lane] += p20::simplex2(s.perlin, climateX * scaleX, climateZ * scaleZ) * weight;
        __syncthreads();
        d7 *= octaveScale;
        d6 *= octavePersistence;
    }
}

__device__ __forceinline__ void prepareP20TerrainPerlin(
        SharedScratch& s,
        int seedIndex,
        int stateNumber,
        unsigned char* terrainPermutationCache,
        double* terrainOffsetCache,
        int loadTerrainCache,
        std::uint64_t laneJumpMul,
        std::uint64_t laneJumpAdd
) {
    const int lane = threadIdx.x;
    if (loadTerrainCache != 0) {
        terraincache::loadStateCooperative(
                s.perlin, seedIndex, stateNumber,
                terrainPermutationCache, terrainOffsetCache,
                lane, POINTS);
        return;
    }

    initTerrainPerlinParallel(s, laneJumpMul, laneJumpAdd);
    if (terrainPermutationCache != nullptr) {
        terraincache::storeStateCooperative(
                s.perlin, seedIndex, stateNumber,
                terrainPermutationCache, terrainOffsetCache,
                lane, POINTS);
    }
}

__global__ void p20KernelFromSeeds(
        const std::int64_t* seeds,
        int count,
        int* positiveCounts,
        double* densities,
        unsigned char* terrainPermutationCache,
        double* terrainOffsetCache,
        unsigned char* climatePermutationCache,
        double* climateOffsetCache,
        unsigned char* p20UpperMasks,
        double* columnShapeD5,
        double* columnShapeD7,
        int coarseOffsetX,
        int coarseOffsetZ,
        int loadTerrainCache,
        int loadClimateCache
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= POINTS) return;

    __shared__ SharedScratch s;
    const std::int64_t seed = seeds[seedIndex];

    double coarseX, coarseZ, climateX, climateZ;
    pointCoordinates(lane, coarseOffsetX, coarseOffsetZ, coarseX, coarseZ, climateX, climateZ);

    const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
    const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
    const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);

    accumulateSimplexGroup(s, tempSeed, 4,
            0.02500000037252903, 0.02500000037252903, 0.25, 0.5,
            climateX, climateZ, s.tempRaw,
            seedIndex, climatecache::TEMP_BASE, climatePermutationCache, climateOffsetCache, loadClimateCache);
    accumulateSimplexGroup(s, rainSeed, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333, 0.5,
            climateX, climateZ, s.rainRaw,
            seedIndex, climatecache::RAIN_BASE, climatePermutationCache, climateOffsetCache, loadClimateCache);
    accumulateSimplexGroup(s, blendSeed, 2,
            0.25, 0.25, 0.5882352941176471, 0.5,
            climateX, climateZ, s.climateBlend,
            seedIndex, climatecache::BLEND_BASE, climatePermutationCache, climateOffsetCache, loadClimateCache);

    {
        const double d0 = s.climateBlend[lane] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (s.tempRaw[lane] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (s.rainRaw[lane] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        s.temperature[lane] = d3;
        s.rain[lane] = d4;
    }
    __syncthreads();

    std::uint64_t laneJumpMul;
    std::uint64_t laneJumpAdd;
    javaLcgJumpCoefficients(static_cast<unsigned int>(lane + 1), laneJumpMul, laneJumpAdd);

    if (lane == 0) s.rng.setSeed(seed);
    for (int yi = 0; yi < YCOUNT; ++yi) {
        const int idx = lane * YCOUNT + yi;
        s.noise1[idx] = 0.0;
        s.noise2[idx] = 0.0;
        s.noise3[idx] = 0.0;
    }
    s.noise4[lane] = 0.0;
    s.noise5[lane] = 0.0;
    if (lane == 0) s.positiveCount = 0;
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE2_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        double values[6];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise2[idx] = 0.0 + value;
            else s.noise2[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE3_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        double values[6];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise3[idx] = 0.0 + value;
            else s.noise3[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE1_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        double values[6];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise1[idx] = 0.0 + value;
            else s.noise1[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    if (loadTerrainCache == 0) {
        for (int i = 0; i < 8; ++i) {
            consumeTerrainPerlinParallel(s, laneJumpMul, laneJumpAdd);
        }
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE4_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = 1.121 * amplitude;
        const double sz = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
        if (octave == 0) s.noise4[lane] = 0.0 + value;
        else s.noise4[lane] += value;
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE5_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = 200.0 * amplitude;
        const double sz = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
        if (octave == 0) s.noise5[lane] = 0.0 + value;
        else s.noise5[lane] += value;
        __syncthreads();
        amplitude /= 2.0;
    }

    const double d2 = s.temperature[lane];
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

    const int fullLane = axisValue(lane >> 3) * 16 + axisValue(lane & 7);
    const std::size_t fullColumnIndex = static_cast<std::size_t>(seedIndex) * stage0gpu::FULL_POINTS + fullLane;
    unsigned char upperMask = 0;
    for (int yi = 0; yi < YCOUNT; ++yi) {
        const int y = 11 + yi;
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
        if (densities != nullptr) {
            const std::size_t outIndex = p20UpperMasks != nullptr
                    ? fullColumnIndex * YCOUNT + yi
                    : static_cast<std::size_t>(seedIndex) * DENSITY_COUNT + idx;
            densities[outIndex] = d8;
        }
        if (d8 > 0.0) upperMask |= static_cast<unsigned char>(1u << yi);
    }

    if (p20UpperMasks != nullptr || columnShapeD5 != nullptr || columnShapeD7 != nullptr) {
        if (p20UpperMasks != nullptr) p20UpperMasks[fullColumnIndex] = upperMask;
        if (columnShapeD5 != nullptr) columnShapeD5[fullColumnIndex] = d5;
        if (columnShapeD7 != nullptr) columnShapeD7[fullColumnIndex] = d7;
    }

    if (upperMask != 0) atomicAdd(&s.positiveCount, 1);
    __syncthreads();
    if (lane == 0) positiveCounts[seedIndex] = s.positiveCount;
}

// P33 benchmark-only P20 variant. The exact per-octave Y-axis setup is built once
// by lane zero and reused by all 64 X/Z lanes. Production remains p20KernelFromSeeds.
__global__ void p20KernelFromSeedsSharedY(
        const std::int64_t* seeds,
        int count,
        int* positiveCounts,
        double* densities,
        unsigned char* terrainPermutationCache,
        double* terrainOffsetCache,
        unsigned char* climatePermutationCache,
        double* climateOffsetCache,
        unsigned char* p20UpperMasks,
        double* columnShapeD5,
        double* columnShapeD7,
        int coarseOffsetX,
        int coarseOffsetZ,
        int loadTerrainCache,
        int loadClimateCache
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= POINTS) return;

    __shared__ SharedYScratch s;
    const std::int64_t seed = seeds[seedIndex];

    double coarseX, coarseZ, climateX, climateZ;
    pointCoordinates(lane, coarseOffsetX, coarseOffsetZ, coarseX, coarseZ, climateX, climateZ);

    const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
    const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
    const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);

    accumulateSimplexGroup(s, tempSeed, 4,
            0.02500000037252903, 0.02500000037252903, 0.25, 0.5,
            climateX, climateZ, s.tempRaw,
            seedIndex, climatecache::TEMP_BASE, climatePermutationCache, climateOffsetCache, loadClimateCache);
    accumulateSimplexGroup(s, rainSeed, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333, 0.5,
            climateX, climateZ, s.rainRaw,
            seedIndex, climatecache::RAIN_BASE, climatePermutationCache, climateOffsetCache, loadClimateCache);
    accumulateSimplexGroup(s, blendSeed, 2,
            0.25, 0.25, 0.5882352941176471, 0.5,
            climateX, climateZ, s.climateBlend,
            seedIndex, climatecache::BLEND_BASE, climatePermutationCache, climateOffsetCache, loadClimateCache);

    {
        const double d0 = s.climateBlend[lane] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (s.tempRaw[lane] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (s.rainRaw[lane] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        s.temperature[lane] = d3;
        s.rain[lane] = d4;
    }
    __syncthreads();

    std::uint64_t laneJumpMul;
    std::uint64_t laneJumpAdd;
    javaLcgJumpCoefficients(static_cast<unsigned int>(lane + 1), laneJumpMul, laneJumpAdd);

    if (lane == 0) s.rng.setSeed(seed);
    for (int yi = 0; yi < YCOUNT; ++yi) {
        const int idx = lane * YCOUNT + yi;
        s.noise1[idx] = 0.0;
        s.noise2[idx] = 0.0;
        s.noise3[idx] = 0.0;
    }
    s.noise4[lane] = 0.0;
    s.noise5[lane] = 0.0;
    if (lane == 0) s.positiveCount = 0;
    __syncthreads();

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE2_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        if (lane == 0) p20::buildUpper6YAxisCache(s.perlin, sy, s.yAxis);
        __syncthreads();
        double values[6];
        p20::perlin3Upper6SharedY(s.perlin, coarseX * sx, coarseZ * sz, s.yAxis, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise2[idx] = 0.0 + value;
            else s.noise2[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE3_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        if (lane == 0) p20::buildUpper6YAxisCache(s.perlin, sy, s.yAxis);
        __syncthreads();
        double values[6];
        p20::perlin3Upper6SharedY(s.perlin, coarseX * sx, coarseZ * sz, s.yAxis, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise3[idx] = 0.0 + value;
            else s.noise3[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE1_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        if (lane == 0) p20::buildUpper6YAxisCache(s.perlin, sy, s.yAxis);
        __syncthreads();
        double values[6];
        p20::perlin3Upper6SharedY(s.perlin, coarseX * sx, coarseZ * sz, s.yAxis, values);
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int idx = lane * YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise1[idx] = 0.0 + value;
            else s.noise1[idx] += value;
        }
        __syncthreads();
        amplitude /= 2.0;
    }

    if (loadTerrainCache == 0) {
        for (int i = 0; i < 8; ++i) {
            consumeTerrainPerlinParallel(s, laneJumpMul, laneJumpAdd);
        }
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE4_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = 1.121 * amplitude;
        const double sz = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
        if (octave == 0) s.noise4[lane] = 0.0 + value;
        else s.noise4[lane] += value;
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        prepareP20TerrainPerlin(
                s, seedIndex, terraincache::NOISE5_BASE + octave,
                terrainPermutationCache, terrainOffsetCache, loadTerrainCache,
                laneJumpMul, laneJumpAdd);
        const double sx = 200.0 * amplitude;
        const double sz = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
        if (octave == 0) s.noise5[lane] = 0.0 + value;
        else s.noise5[lane] += value;
        __syncthreads();
        amplitude /= 2.0;
    }

    const double d2 = s.temperature[lane];
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

    const int fullLane = axisValue(lane >> 3) * 16 + axisValue(lane & 7);
    const std::size_t fullColumnIndex = static_cast<std::size_t>(seedIndex) * stage0gpu::FULL_POINTS + fullLane;
    unsigned char upperMask = 0;
    for (int yi = 0; yi < YCOUNT; ++yi) {
        const int y = 11 + yi;
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
        if (densities != nullptr) {
            const std::size_t outIndex = p20UpperMasks != nullptr
                    ? fullColumnIndex * YCOUNT + yi
                    : static_cast<std::size_t>(seedIndex) * DENSITY_COUNT + idx;
            densities[outIndex] = d8;
        }
        if (d8 > 0.0) upperMask |= static_cast<unsigned char>(1u << yi);
    }

    if (p20UpperMasks != nullptr || columnShapeD5 != nullptr || columnShapeD7 != nullptr) {
        if (p20UpperMasks != nullptr) p20UpperMasks[fullColumnIndex] = upperMask;
        if (columnShapeD5 != nullptr) columnShapeD5[fullColumnIndex] = d5;
        if (columnShapeD7 != nullptr) columnShapeD7[fullColumnIndex] = d7;
    }

    if (upperMask != 0) atomicAdd(&s.positiveCount, 1);
    __syncthreads();
    if (lane == 0) positiveCounts[seedIndex] = s.positiveCount;
}

__global__ void generateDeterministicSeedsKernel(std::int64_t* seeds, int count, std::uint64_t sequenceSeed) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(sequenceSeed, static_cast<std::uint64_t>(i)));
}

} // namespace p20gpu

namespace {

#define HIP_CHECK(call) do { \
    hipError_t _err = (call); \
    if (_err != hipSuccess) throw std::runtime_error(std::string(#call) + ": " + hipGetErrorString(_err)); \
} while (0)

struct TerrainPerlinCacheBuffers {
    unsigned char* permutations = nullptr;
    double* offsets = nullptr;
    int capacity = 0;
};

void allocateTerrainPerlinCache(TerrainPerlinCacheBuffers& cache, int capacity) {
    if (capacity < 1) throw std::runtime_error("terrain Perlin cache capacity must be >= 1");
    cache.capacity = capacity;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&cache.permutations),
            terraincache::permutationBytesForCapacity(capacity)));
    try {
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&cache.offsets),
                terraincache::offsetBytesForCapacity(capacity)));
    } catch (...) {
        hipFree(cache.permutations);
        cache.permutations = nullptr;
        cache.capacity = 0;
        throw;
    }
}

void freeTerrainPerlinCache(TerrainPerlinCacheBuffers& cache) {
    if (cache.permutations != nullptr) hipFree(cache.permutations);
    if (cache.offsets != nullptr) hipFree(cache.offsets);
    cache.permutations = nullptr;
    cache.offsets = nullptr;
    cache.capacity = 0;
}


struct ClimatePerlinCacheBuffers {
    unsigned char* permutations = nullptr;
    double* offsets = nullptr;
    int capacity = 0;
};

void allocateClimatePerlinCache(ClimatePerlinCacheBuffers& cache, int capacity) {
    if (capacity < 1) throw std::runtime_error("climate Perlin cache capacity must be >= 1");
    cache.capacity = capacity;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&cache.permutations),
            climatecache::permutationBytesForCapacity(capacity)));
    try {
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&cache.offsets),
                climatecache::offsetBytesForCapacity(capacity)));
    } catch (...) {
        hipFree(cache.permutations);
        cache.permutations = nullptr;
        cache.capacity = 0;
        throw;
    }
}

void freeClimatePerlinCache(ClimatePerlinCacheBuffers& cache) {
    if (cache.permutations != nullptr) hipFree(cache.permutations);
    if (cache.offsets != nullptr) hipFree(cache.offsets);
    cache.permutations = nullptr;
    cache.offsets = nullptr;
    cache.capacity = 0;
}

struct ColumnShapeCacheBuffers {
    double* d5 = nullptr;
    double* d7 = nullptr;
    int capacity = 0;
};

std::size_t columnShapeCacheBytesForCapacity(int capacity) {
    return static_cast<std::size_t>(capacity) * stage0gpu::FULL_POINTS * 2u * sizeof(double);
}

void allocateColumnShapeCache(ColumnShapeCacheBuffers& cache, int capacity) {
    if (capacity < 1) throw std::runtime_error("column shape cache capacity must be >= 1");
    cache.capacity = capacity;
    const std::size_t oneArrayBytes = static_cast<std::size_t>(capacity) * stage0gpu::FULL_POINTS * sizeof(double);
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&cache.d5), oneArrayBytes));
    try {
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&cache.d7), oneArrayBytes));
    } catch (...) {
        hipFree(cache.d5);
        cache.d5 = nullptr;
        cache.capacity = 0;
        throw;
    }
}

void freeColumnShapeCache(ColumnShapeCacheBuffers& cache) {
    if (cache.d5 != nullptr) hipFree(cache.d5);
    if (cache.d7 != nullptr) hipFree(cache.d7);
    cache.d5 = nullptr;
    cache.d7 = nullptr;
    cache.capacity = 0;
}


std::uint32_t readLe32(std::istream& in) {
    unsigned char b[4]; in.read(reinterpret_cast<char*>(b), 4);
    return static_cast<std::uint32_t>(b[0])
         | (static_cast<std::uint32_t>(b[1]) << 8)
         | (static_cast<std::uint32_t>(b[2]) << 16)
         | (static_cast<std::uint32_t>(b[3]) << 24);
}

std::uint64_t readLe64(std::istream& in) {
    unsigned char b[8]; in.read(reinterpret_cast<char*>(b), 8);
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}

void printDevice(std::ostream& out = std::cout) {
    int device = 0;
    HIP_CHECK(hipGetDevice(&device));
    hipDeviceProp_t prop{};
    HIP_CHECK(hipGetDeviceProperties(&prop, device));
    out << "GPU: " << prop.name << "\n";
#if defined(BSF_NVIDIA_CUDA)
    out << "Architecture: sm_" << prop.major << prop.minor << "\n";
#else
    out << "Architecture: " << prop.gcnArchName << "\n";
#endif
    out << "Compute units: " << prop.multiProcessorCount << "\n"
        << "Global memory: " << (prop.totalGlobalMem / (1024.0 * 1024.0 * 1024.0)) << " GiB\n";
}

void writeLe32(std::ostream& out, std::uint32_t v) {
    unsigned char b[4] = {
        static_cast<unsigned char>(v),
        static_cast<unsigned char>(v >> 8),
        static_cast<unsigned char>(v >> 16),
        static_cast<unsigned char>(v >> 24)
    };
    out.write(reinterpret_cast<const char*>(b), 4);
}

void writeLe64(std::ostream& out, std::uint64_t v) {
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(v >> (i * 8));
    out.write(reinterpret_cast<const char*>(b), 8);
}

bool tryReadLe32(std::istream& in, std::uint32_t& v) {
    unsigned char b[4];
    in.read(reinterpret_cast<char*>(b), 4);
    if (in.gcount() == 0 && in.eof()) return false;
    if (in.gcount() != 4) throw std::runtime_error("truncated stream request header");
    v = static_cast<std::uint32_t>(b[0])
      | (static_cast<std::uint32_t>(b[1]) << 8)
      | (static_cast<std::uint32_t>(b[2]) << 16)
      | (static_cast<std::uint32_t>(b[3]) << 24);
    return true;
}

void fillMonsterShapeStats(
        const unsigned char* grid,
        int size,
        p19native::MonsterFeatures& out,
        bool y88
) {
    const int total = size * size;
    unsigned char visited[stage0gpu::FULL_POINTS]{};
    int queue[stage0gpu::FULL_POINTS];
    int largestCluster = 0;
    int minX = size;
    int maxX = -1;
    int minZ = size;
    int maxZ = -1;
    bool touchesBorder = false;

    for (int x = 0; x < size; ++x) {
        for (int z = 0; z < size; ++z) {
            const int index = x * size + z;
            if (index >= total || grid[index] == 0) continue;

            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minZ = std::min(minZ, z);
            maxZ = std::max(maxZ, z);
            if (x == 0 || z == 0 || x == size - 1 || z == size - 1) touchesBorder = true;

            if (visited[index] != 0) continue;
            int head = 0;
            int tail = 0;
            int cluster = 0;
            visited[index] = 1;
            queue[tail++] = index;

            while (head < tail) {
                const int current = queue[head++];
                ++cluster;
                const int cx = current / size;
                const int cz = current % size;
                const int nx[4] = {cx - 1, cx + 1, cx, cx};
                const int nz[4] = {cz, cz, cz - 1, cz + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || nz[k] < 0 || nx[k] >= size || nz[k] >= size) continue;
                    const int neighbor = nx[k] * size + nz[k];
                    if (grid[neighbor] == 0 || visited[neighbor] != 0) continue;
                    visited[neighbor] = 1;
                    queue[tail++] = neighbor;
                }
            }
            largestCluster = std::max(largestCluster, cluster);
        }
    }

    const int width = largestCluster == 0 ? 0 : maxX - minX + 1;
    const int depth = largestCluster == 0 ? 0 : maxZ - minZ + 1;
    if (y88) {
        out.stage0Y88LargestCluster = largestCluster;
        out.stage0Y88Width = width;
        out.stage0Y88Depth = depth;
        out.stage0Y88TouchesBorder = touchesBorder;
    } else {
        out.stage0Y96LargestCluster = largestCluster;
        out.stage0Y96Width = width;
        out.stage0Y96Depth = depth;
        out.stage0Y96TouchesBorder = touchesBorder;
    }
}

p19native::MonsterFeatures buildMonsterFeatures(const unsigned char* highestReentryY) {
    p19native::MonsterFeatures out{};
    unsigned char y88[stage0gpu::FULL_POINTS]{};
    unsigned char y96[stage0gpu::FULL_POINTS]{};

    for (int column = 0; column < stage0gpu::FULL_POINTS; ++column) {
        const unsigned char encoded = highestReentryY[column];
        if (encoded == 0xFFu) continue;
        const int highest = static_cast<int>(encoded);
        if (highest >= 11) {
            ++out.stage0FullY88;
            y88[column] = 1;
        }
        if (highest >= 12) {
            ++out.stage0FullY96;
            y96[column] = 1;
        }
        if (highest >= 13) ++out.stage0FullY104;
        if (highest >= 14) ++out.stage0FullY112;
    }

    fillMonsterShapeStats(y88, stage0gpu::FULL_SIZE, out, true);
    fillMonsterShapeStats(y96, stage0gpu::FULL_SIZE, out, false);
    return out;
}

static constexpr double MEGA_TOPOLOGY_SCORE_CEILING = 5.0;
static constexpr int MEGA_Y96_MIN_LARGEST_CLUSTER = 4;

bool megaTopologyReject(
        double p19Score,
        bool extremeBypass,
        const p19native::MonsterFeatures& features
) {
    if (extremeBypass) return false;
    if (!(p19Score < MEGA_TOPOLOGY_SCORE_CEILING)) return false;
    return features.stage0FullY112 == 0
            || features.stage0Y96LargestCluster < MEGA_Y96_MIN_LARGEST_CLUSTER;
}


void allocateCoarseBuffers(coarsegpu::Buffers& b, int capacity) {
    b.capacity = capacity;
    const std::size_t cols = static_cast<std::size_t>(capacity) * coarsecore::COLUMNS;
    const std::size_t cells = static_cast<std::size_t>(capacity) * coarsecore::CELLS;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.seeds), static_cast<std::size_t>(capacity) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.cacheSeedIndices), static_cast<std::size_t>(capacity) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.temp), cols * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.rain), cols * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.climateBlend), cols * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.noise1), cells * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.noise2), cells * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.noise3), cells * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.noise4), cols * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.noise5), cols * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.signs), cells * sizeof(unsigned char)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.labels), cells * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.queue), cells * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.columnSeen), cols * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.columnMinY), cols * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.componentColumns), cols * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&b.scores), static_cast<std::size_t>(capacity) * sizeof(int)));
}

void freeCoarseBuffers(coarsegpu::Buffers& b) {
    hipFree(b.seeds); hipFree(b.cacheSeedIndices); hipFree(b.temp); hipFree(b.rain); hipFree(b.climateBlend);
    hipFree(b.noise1); hipFree(b.noise2); hipFree(b.noise3); hipFree(b.noise4); hipFree(b.noise5);
    hipFree(b.signs); hipFree(b.labels); hipFree(b.queue); hipFree(b.columnSeen);
    hipFree(b.columnMinY); hipFree(b.componentColumns); hipFree(b.scores);
    b = {};
}

void runCoarseScoresAt(
        coarsegpu::Buffers& b,
        const std::int64_t* hostSeeds,
        int count,
        int* hostScores,
        int coarseOffsetX,
        int coarseOffsetZ
) {
    if (count < 1) return;
    if (count > b.capacity) throw std::runtime_error("coarse chunk exceeds GPU scratch capacity");
    HIP_CHECK(hipMemcpy(b.seeds, hostSeeds, static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
    hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel, dim3(count), dim3(coarsegpu::THREADS), 0, 0,
            b.seeds, count,
            b.temp, b.rain, b.climateBlend,
            b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
            b.signs, coarseOffsetX, coarseOffsetZ,
            nullptr, nullptr, nullptr, nullptr, nullptr);
    HIP_CHECK(hipGetLastError());
    hipLaunchKernelGGL(coarsegpu::scoreCoarseSignsKernel, dim3(count), dim3(1), 0, 0,
            b.signs, count, b.labels, b.queue, b.columnSeen, b.columnMinY,
            b.componentColumns, b.scores);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipMemcpy(hostScores, b.scores, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
}


void runCoarseScoresCachedAt(
        coarsegpu::Buffers& b,
        const std::int64_t* hostSeeds,
        const int* hostCacheSeedIndices,
        int count,
        int* hostScores,
        int coarseOffsetX,
        int coarseOffsetZ,
        const TerrainPerlinCacheBuffers& terrainCache,
        const struct ClimatePerlinCacheBuffers& climateCache
) {
    if (count < 1) return;
    if (count > b.capacity) throw std::runtime_error("coarse chunk exceeds GPU scratch capacity");
    HIP_CHECK(hipMemcpy(b.seeds, hostSeeds, static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(b.cacheSeedIndices, hostCacheSeedIndices,
            static_cast<std::size_t>(count) * sizeof(int), hipMemcpyHostToDevice));
    hipLaunchKernelGGL(p35coarse::generateCoarseSignsDirect23Kernel, dim3(count), dim3(coarsegpu::THREADS), 0, 0,
            b.seeds, count,
            b.temp, b.rain, b.climateBlend,
            b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
            b.signs, coarseOffsetX, coarseOffsetZ,
            b.cacheSeedIndices,
            terrainCache.permutations, terrainCache.offsets,
            climateCache.permutations, climateCache.offsets);
    HIP_CHECK(hipGetLastError());
    hipLaunchKernelGGL(p36score::scoreCoarseSignsParallelKernel<64>, dim3(count), dim3(64), 0, 0,
            b.signs, count, b.labels, b.queue, b.columnSeen, b.columnMinY,
            b.componentColumns, b.scores);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipMemcpy(hostScores, b.scores, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
}

void runCoarseScores(
        coarsegpu::Buffers& b,
        const std::int64_t* hostSeeds,
        int count,
        int* hostScores
) {
    runCoarseScoresAt(b, hostSeeds, count, hostScores, 0, 0);
}

enum class P35CoarseMode {
    BASELINE,
    DIRECT_NOISE23,
    DIRECT_ALL_3D
};

const char* p35CoarseModeName(P35CoarseMode mode) {
    switch (mode) {
        case P35CoarseMode::BASELINE: return "P33 cached coarse";
        case P35CoarseMode::DIRECT_NOISE23: return "direct-write noise2/3";
        case P35CoarseMode::DIRECT_ALL_3D: return "direct-write all 3D";
    }
    return "unknown";
}

void runCoarseScoresCachedP35At(
        coarsegpu::Buffers& b,
        const std::int64_t* hostSeeds,
        const int* hostCacheSeedIndices,
        int count,
        int* hostScores,
        int coarseOffsetX,
        int coarseOffsetZ,
        const TerrainPerlinCacheBuffers& terrainCache,
        const ClimatePerlinCacheBuffers& climateCache,
        P35CoarseMode mode,
        bool scoreAndCopy = true
) {
    if (count < 1) return;
    if (count > b.capacity) throw std::runtime_error("coarse chunk exceeds GPU scratch capacity");
    HIP_CHECK(hipMemcpy(b.seeds, hostSeeds, static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(b.cacheSeedIndices, hostCacheSeedIndices,
            static_cast<std::size_t>(count) * sizeof(int), hipMemcpyHostToDevice));

    if (mode == P35CoarseMode::BASELINE) {
        hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel,
                dim3(count), dim3(coarsegpu::THREADS), 0, 0,
                b.seeds, count,
                b.temp, b.rain, b.climateBlend,
                b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
                b.signs, coarseOffsetX, coarseOffsetZ,
                b.cacheSeedIndices,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets);
    } else if (mode == P35CoarseMode::DIRECT_NOISE23) {
        hipLaunchKernelGGL(p35coarse::generateCoarseSignsDirect23Kernel,
                dim3(count), dim3(coarsegpu::THREADS), 0, 0,
                b.seeds, count,
                b.temp, b.rain, b.climateBlend,
                b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
                b.signs, coarseOffsetX, coarseOffsetZ,
                b.cacheSeedIndices,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets);
    } else {
        hipLaunchKernelGGL(p35coarse::generateCoarseSignsDirectAllKernel,
                dim3(count), dim3(coarsegpu::THREADS), 0, 0,
                b.seeds, count,
                b.temp, b.rain, b.climateBlend,
                b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
                b.signs, coarseOffsetX, coarseOffsetZ,
                b.cacheSeedIndices,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets);
    }
    HIP_CHECK(hipGetLastError());
    if (!scoreAndCopy) return;

    hipLaunchKernelGGL(coarsegpu::scoreCoarseSignsKernel, dim3(count), dim3(1), 0, 0,
            b.signs, count, b.labels, b.queue, b.columnSeen, b.columnMinY,
            b.componentColumns, b.scores);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipMemcpy(hostScores, b.scores, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
}

void runCoarseScoresCachedSelectorAt(
        coarsegpu::Buffers& b,
        const std::int64_t* hostSeeds,
        const int* hostCacheSeedIndices,
        int count,
        int* hostScores,
        int coarseOffsetX,
        int coarseOffsetZ,
        const TerrainPerlinCacheBuffers& terrainCache,
        const ClimatePerlinCacheBuffers& climateCache,
        std::uint32_t* dNoise2Masks,
        std::uint32_t* dNoise3Masks,
        unsigned int* dSelectorStats = nullptr
) {
    if (count < 1) return;
    if (count > b.capacity) throw std::runtime_error("coarse chunk exceeds GPU scratch capacity");
    HIP_CHECK(hipMemcpy(b.seeds, hostSeeds, static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(b.cacheSeedIndices, hostCacheSeedIndices,
            static_cast<std::size_t>(count) * sizeof(int), hipMemcpyHostToDevice));
    hipLaunchKernelGGL(p29coarse::generateCoarseSignsSelectorFirstKernel,
            dim3(count), dim3(coarsegpu::THREADS), 0, 0,
            b.seeds, count,
            b.temp, b.rain, b.climateBlend,
            b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
            b.signs, coarseOffsetX, coarseOffsetZ,
            b.cacheSeedIndices,
            terrainCache.permutations, terrainCache.offsets,
            climateCache.permutations, climateCache.offsets,
            dNoise2Masks, dNoise3Masks, dSelectorStats);
    HIP_CHECK(hipGetLastError());
    hipLaunchKernelGGL(coarsegpu::scoreCoarseSignsKernel, dim3(count), dim3(1), 0, 0,
            b.signs, count, b.labels, b.queue, b.columnSeen, b.columnMinY,
            b.componentColumns, b.scores);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipMemcpy(hostScores, b.scores, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
}

int validateCoarse(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open coarse reference file: " + path);
    char magic[8]; in.read(magic, 8);
    if (std::string(magic, 8) != "CRSREF01") throw std::runtime_error("bad coarse reference magic");
    const auto version = readLe32(in);
    const auto records = readLe32(in);
    if (version != 1 || records < 1) throw std::runtime_error("unsupported coarse reference format");

    std::vector<std::int64_t> seeds(records);
    std::vector<int> expected(records);
    for (std::uint32_t i = 0; i < records; ++i) {
        seeds[i] = static_cast<std::int64_t>(readLe64(in));
        expected[i] = static_cast<int>(readLe32(in));
    }

    // Validate the actual production capacity, not just the tiny 64-record
    // reference file. Repeat the exact Java reference corpus until one full
    // 1024-seed production batch is filled, then launch it in a single call.
    const int chunkCapacity = 1024;
    const std::size_t validationRecords = static_cast<std::size_t>(chunkCapacity);
    std::vector<std::int64_t> validationSeeds(validationRecords);
    std::vector<int> validationExpected(validationRecords);
    for (std::size_t i = 0; i < validationRecords; ++i) {
        const std::size_t source = i % records;
        validationSeeds[i] = seeds[source];
        validationExpected[i] = expected[source];
    }

    coarsegpu::Buffers buffers;
    allocateCoarseBuffers(buffers, chunkCapacity);
    std::vector<int> actual(validationRecords, 0);

    hipEvent_t start{}, stop{};
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));
    HIP_CHECK(hipEventRecord(start));
    runCoarseScores(buffers, validationSeeds.data(), chunkCapacity, actual.data());
    HIP_CHECK(hipEventRecord(stop));
    HIP_CHECK(hipEventSynchronize(stop));
    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));

    std::uint64_t diffs = 0;
    std::uint64_t decisionDiffs = 0;
    for (std::size_t i = 0; i < validationRecords; ++i) {
        if (actual[i] != validationExpected[i]) {
            ++diffs;
            if ((actual[i] >= 85) != (validationExpected[i] >= 85)) ++decisionDiffs;
            if (diffs <= 5) {
                std::cout << "  mismatch seed=" << validationSeeds[i]
                          << " expected=" << validationExpected[i]
                          << " actual=" << actual[i] << "\n";
            }
        }
    }

    std::cout << "GPU exact coarse validation\n"
              << "  reference records:    " << records << "\n"
              << "  production batch:     " << validationRecords << "\n"
              << "  coarse score diffs:   " << diffs << "\n"
              << "  coarse decision diff: " << decisionDiffs << "\n"
              << "  total time:           " << std::fixed << std::setprecision(3) << ms << " ms\n"
              << "  survivor throughput:  " << std::setprecision(1) << (validationRecords * 1000.0 / ms) << " seeds/s\n";

    hipEventDestroy(start); hipEventDestroy(stop);
    freeCoarseBuffers(buffers);
    return (diffs == 0 && decisionDiffs == 0) ? 0 : 2;
}

int profileCoarse(const std::string& path, int iterations, bool noiseDetail = false) {
    if (iterations < 1) throw std::runtime_error("coarseprofile iterations must be >= 1");

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open coarse reference file: " + path);
    char magic[8]; in.read(magic, 8);
    if (std::string(magic, 8) != "CRSREF01") throw std::runtime_error("bad coarse reference magic");
    const auto version = readLe32(in);
    const auto records = readLe32(in);
    if (version != 1 || records < 1) throw std::runtime_error("unsupported coarse reference format");

    std::vector<std::int64_t> seeds(records);
    std::vector<int> expected(records);
    for (std::uint32_t i = 0; i < records; ++i) {
        seeds[i] = static_cast<std::int64_t>(readLe64(in));
        expected[i] = static_cast<int>(readLe32(in));
    }

    constexpr int chunkCapacity = 1024;
    const std::size_t batch = static_cast<std::size_t>(chunkCapacity);
    std::vector<std::int64_t> validationSeeds(batch);
    std::vector<int> validationExpected(batch);
    for (std::size_t i = 0; i < batch; ++i) {
        const std::size_t source = i % records;
        validationSeeds[i] = seeds[source];
        validationExpected[i] = expected[source];
    }

    coarsegpu::Buffers buffers;
    allocateCoarseBuffers(buffers, chunkCapacity);

    unsigned long long* dPhaseTicks = nullptr;
    const std::size_t phaseValues = batch * coarsegpu::COARSE_PROFILE_PHASES;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dPhaseTicks), phaseValues * sizeof(unsigned long long)));

    unsigned long long* dNoiseDetailTicks = nullptr;
    const std::size_t noiseDetailValues = batch * coarsegpu::COARSE_NOISE_DETAIL_VALUES;
    if (noiseDetail) {
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dNoiseDetailTicks),
                noiseDetailValues * sizeof(unsigned long long)));
    }

    std::vector<int> actual(batch, 0);
    std::vector<unsigned long long> phaseTicks(phaseValues, 0);
    std::vector<unsigned long long> noiseDetailTickValues(noiseDetail ? noiseDetailValues : 0, 0);
    long double phaseTickSums[coarsegpu::COARSE_PROFILE_PHASES] = {};
    long double noiseDetailTickSums[coarsegpu::COARSE_NOISE_DETAIL_SERIES][coarsegpu::COARSE_NOISE_DETAIL_OCTAVES] = {};

    hipEvent_t genStart{}, genStop{}, scoreStart{}, scoreStop{}, profileStart{}, profileStop{};
    HIP_CHECK(hipEventCreate(&genStart));
    HIP_CHECK(hipEventCreate(&genStop));
    HIP_CHECK(hipEventCreate(&scoreStart));
    HIP_CHECK(hipEventCreate(&scoreStop));
    HIP_CHECK(hipEventCreate(&profileStart));
    HIP_CHECK(hipEventCreate(&profileStop));

    try {
        HIP_CHECK(hipMemcpy(buffers.seeds, validationSeeds.data(), batch * sizeof(std::int64_t), hipMemcpyHostToDevice));

        // Warm up the exact production kernels so one-time launch/JIT effects do not
        // pollute the phase comparison.
        hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel, dim3(chunkCapacity), dim3(coarsegpu::THREADS), 0, 0,
                buffers.seeds, chunkCapacity,
                buffers.temp, buffers.rain, buffers.climateBlend,
                buffers.noise1, buffers.noise2, buffers.noise3, buffers.noise4, buffers.noise5,
                buffers.signs, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(coarsegpu::scoreCoarseSignsKernel, dim3(chunkCapacity), dim3(1), 0, 0,
                buffers.signs, chunkCapacity, buffers.labels, buffers.queue, buffers.columnSeen,
                buffers.columnMinY, buffers.componentColumns, buffers.scores);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipDeviceSynchronize());

        double productionGenerateMs = 0.0;
        double productionScoreMs = 0.0;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            HIP_CHECK(hipEventRecord(genStart));
            hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel, dim3(chunkCapacity), dim3(coarsegpu::THREADS), 0, 0,
                    buffers.seeds, chunkCapacity,
                    buffers.temp, buffers.rain, buffers.climateBlend,
                    buffers.noise1, buffers.noise2, buffers.noise3, buffers.noise4, buffers.noise5,
                    buffers.signs, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipEventRecord(genStop));
            HIP_CHECK(hipEventRecord(scoreStart));
            hipLaunchKernelGGL(coarsegpu::scoreCoarseSignsKernel, dim3(chunkCapacity), dim3(1), 0, 0,
                    buffers.signs, chunkCapacity, buffers.labels, buffers.queue, buffers.columnSeen,
                    buffers.columnMinY, buffers.componentColumns, buffers.scores);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipEventRecord(scoreStop));
            HIP_CHECK(hipEventSynchronize(scoreStop));

            float genMs = 0.0f;
            float scoreMs = 0.0f;
            HIP_CHECK(hipEventElapsedTime(&genMs, genStart, genStop));
            HIP_CHECK(hipEventElapsedTime(&scoreMs, scoreStart, scoreStop));
            productionGenerateMs += genMs;
            productionScoreMs += scoreMs;
        }
        productionGenerateMs /= static_cast<double>(iterations);
        productionScoreMs /= static_cast<double>(iterations);

        double profiledGenerateMs = 0.0;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            HIP_CHECK(hipMemset(dPhaseTicks, 0, phaseValues * sizeof(unsigned long long)));
            if (noiseDetail) {
                HIP_CHECK(hipMemset(dNoiseDetailTicks, 0,
                        noiseDetailValues * sizeof(unsigned long long)));
            }
            HIP_CHECK(hipEventRecord(profileStart));
            hipLaunchKernelGGL(coarsegpu::generateCoarseSignsProfileKernel, dim3(chunkCapacity), dim3(coarsegpu::THREADS), 0, 0,
                    buffers.seeds, chunkCapacity,
                    buffers.temp, buffers.rain, buffers.climateBlend,
                    buffers.noise1, buffers.noise2, buffers.noise3, buffers.noise4, buffers.noise5,
                    buffers.signs, dPhaseTicks, dNoiseDetailTicks);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipEventRecord(profileStop));
            HIP_CHECK(hipEventSynchronize(profileStop));

            float profileMs = 0.0f;
            HIP_CHECK(hipEventElapsedTime(&profileMs, profileStart, profileStop));
            profiledGenerateMs += profileMs;

            HIP_CHECK(hipMemcpy(phaseTicks.data(), dPhaseTicks,
                    phaseValues * sizeof(unsigned long long), hipMemcpyDeviceToHost));
            for (std::size_t seedIndex = 0; seedIndex < batch; ++seedIndex) {
                const std::size_t base = seedIndex * coarsegpu::COARSE_PROFILE_PHASES;
                for (int phase = 0; phase < coarsegpu::COARSE_PROFILE_PHASES; ++phase) {
                    phaseTickSums[phase] += static_cast<long double>(phaseTicks[base + phase]);
                }
            }

            if (noiseDetail) {
                HIP_CHECK(hipMemcpy(noiseDetailTickValues.data(), dNoiseDetailTicks,
                        noiseDetailValues * sizeof(unsigned long long), hipMemcpyDeviceToHost));
                for (std::size_t seedIndex = 0; seedIndex < batch; ++seedIndex) {
                    const std::size_t base = seedIndex * coarsegpu::COARSE_NOISE_DETAIL_VALUES;
                    for (int series = 0; series < coarsegpu::COARSE_NOISE_DETAIL_SERIES; ++series) {
                        for (int octave = 0; octave < coarsegpu::COARSE_NOISE_DETAIL_OCTAVES; ++octave) {
                            const std::size_t index = base
                                    + static_cast<std::size_t>(series) * coarsegpu::COARSE_NOISE_DETAIL_OCTAVES
                                    + static_cast<std::size_t>(octave);
                            noiseDetailTickSums[series][octave] +=
                                    static_cast<long double>(noiseDetailTickValues[index]);
                        }
                    }
                }
            }
        }
        profiledGenerateMs /= static_cast<double>(iterations);

        // Score the final profiled signs and verify that instrumentation did not
        // alter the exact coarse result.
        hipLaunchKernelGGL(coarsegpu::scoreCoarseSignsKernel, dim3(chunkCapacity), dim3(1), 0, 0,
                buffers.signs, chunkCapacity, buffers.labels, buffers.queue, buffers.columnSeen,
                buffers.columnMinY, buffers.componentColumns, buffers.scores);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipMemcpy(actual.data(), buffers.scores, batch * sizeof(int), hipMemcpyDeviceToHost));

        std::uint64_t diffs = 0;
        std::uint64_t decisionDiffs = 0;
        for (std::size_t i = 0; i < batch; ++i) {
            if (actual[i] != validationExpected[i]) {
                ++diffs;
                if ((actual[i] >= 85) != (validationExpected[i] >= 85)) ++decisionDiffs;
                if (diffs <= 5) {
                    std::cout << "  profile mismatch seed=" << validationSeeds[i]
                              << " expected=" << validationExpected[i]
                              << " actual=" << actual[i] << "\n";
                }
            }
        }

        const char* phaseNames[coarsegpu::COARSE_PROFILE_PHASES] = {
                "climate + normalization",
                "noise2 (16 octaves)",
                "noise3 (16 octaves)",
                "noise1 (8 octaves + RNG skip)",
                "noise4 + noise5 (26 octaves)",
                "terrain blend + sign write"
        };

        long double totalProfileTicks = 0.0L;
        for (int phase = 0; phase < coarsegpu::COARSE_PROFILE_PHASES; ++phase) {
            totalProfileTicks += phaseTickSums[phase];
        }
        const long double samples = static_cast<long double>(batch) * static_cast<long double>(iterations);
        const double productionKernelMs = productionGenerateMs + productionScoreMs;
        const double profilerOverheadPct = productionGenerateMs > 0.0
                ? (profiledGenerateMs / productionGenerateMs - 1.0) * 100.0
                : 0.0;

        std::cout << "GPU exact coarse phase profile\n"
                  << "  reference records:       " << records << "\n"
                  << "  production batch:        " << batch << "\n"
                  << "  iterations:              " << iterations << "\n"
                  << "  profile score diffs:     " << diffs << "\n"
                  << "  profile decision diff:   " << decisionDiffs << "\n"
                  << std::fixed << std::setprecision(3)
                  << "  production generate:     " << productionGenerateMs << " ms\n"
                  << "  production score:        " << productionScoreMs << " ms\n"
                  << "  production kernel total: " << productionKernelMs << " ms\n"
                  << "  profiled generate:       " << profiledGenerateMs << " ms\n"
                  << std::setprecision(2)
                  << "  profiler overhead:       " << profilerOverheadPct << "%\n"
                  << "\n"
                  << "Generate-kernel phase breakdown\n";

        int hottestPhase = 0;
        long double hottestTicks = phaseTickSums[0];
        for (int phase = 0; phase < coarsegpu::COARSE_PROFILE_PHASES; ++phase) {
            if (phaseTickSums[phase] > hottestTicks) {
                hottestTicks = phaseTickSums[phase];
                hottestPhase = phase;
            }
            const long double fraction = totalProfileTicks > 0.0L
                    ? phaseTickSums[phase] / totalProfileTicks
                    : 0.0L;
            const double generatePct = static_cast<double>(fraction * 100.0L);
            const double estimatedMs = productionGenerateMs * static_cast<double>(fraction);
            const double totalPct = productionKernelMs > 0.0
                    ? estimatedMs / productionKernelMs * 100.0
                    : 0.0;
            const long double avgTicks = samples > 0.0L ? phaseTickSums[phase] / samples : 0.0L;
            std::cout << "  " << std::left << std::setw(34) << phaseNames[phase]
                      << std::right << std::setw(8) << std::fixed << std::setprecision(2) << generatePct << "% gen"
                      << " | " << std::setw(8) << std::setprecision(3) << estimatedMs << " ms"
                      << " | " << std::setw(7) << std::setprecision(2) << totalPct << "% total"
                      << " | avg wall ticks/seed " << std::setprecision(0) << static_cast<double>(avgTicks)
                      << "\n";
        }

        const long double densityTicks = phaseTickSums[coarsegpu::COARSE_PROFILE_NOISE1]
                + phaseTickSums[coarsegpu::COARSE_PROFILE_NOISE2]
                + phaseTickSums[coarsegpu::COARSE_PROFILE_NOISE3];
        const long double densityFraction = totalProfileTicks > 0.0L ? densityTicks / totalProfileTicks : 0.0L;
        const double densityMs = productionGenerateMs * static_cast<double>(densityFraction);

        std::cout << "\n"
                  << "Grouped targets\n"
                  << "  noise1 + noise2 + noise3: " << std::fixed << std::setprecision(2)
                  << static_cast<double>(densityFraction * 100.0L) << "% gen | "
                  << std::setprecision(3) << densityMs << " ms/batch\n"
                  << "  score analysis:           " << std::setprecision(3) << productionScoreMs << " ms/batch | "
                  << std::setprecision(2)
                  << (productionKernelMs > 0.0 ? productionScoreMs / productionKernelMs * 100.0 : 0.0)
                  << "% total\n"
                  << "  hottest generate phase:   " << phaseNames[hottestPhase] << "\n";

        if (noiseDetail) {
            long double noise2SetupTicks = 0.0L;
            long double noise2EvalTicks = 0.0L;
            long double noise3SetupTicks = 0.0L;
            long double noise3EvalTicks = 0.0L;
            for (int octave = 0; octave < coarsegpu::COARSE_NOISE_DETAIL_OCTAVES; ++octave) {
                noise2SetupTicks += noiseDetailTickSums[coarsegpu::COARSE_DETAIL_NOISE2_SETUP][octave];
                noise2EvalTicks += noiseDetailTickSums[coarsegpu::COARSE_DETAIL_NOISE2_EVAL][octave];
                noise3SetupTicks += noiseDetailTickSums[coarsegpu::COARSE_DETAIL_NOISE3_SETUP][octave];
                noise3EvalTicks += noiseDetailTickSums[coarsegpu::COARSE_DETAIL_NOISE3_EVAL][octave];
            }
            const long double noise2Ticks = noise2SetupTicks + noise2EvalTicks;
            const long double noise3Ticks = noise3SetupTicks + noise3EvalTicks;
            const long double n2PhaseTicks = phaseTickSums[coarsegpu::COARSE_PROFILE_NOISE2];
            const long double n3PhaseTicks = phaseTickSums[coarsegpu::COARSE_PROFILE_NOISE3];
            const double n2PhaseMs = totalProfileTicks > 0.0L
                    ? productionGenerateMs * static_cast<double>(n2PhaseTicks / totalProfileTicks) : 0.0;
            const double n3PhaseMs = totalProfileTicks > 0.0L
                    ? productionGenerateMs * static_cast<double>(n3PhaseTicks / totalProfileTicks) : 0.0;

            std::cout << "\nNoise2 / noise3 octave detail\n"
                      << "  setup = initPerlin + sync + exact X/Y/Z axis preparation\n"
                      << "  eval  = 3D Perlin columns + exact global write/accumulate\n"
                      << "\n"
                      << "  oct | n2 setup | n2 eval | n2 total | n3 setup | n3 eval | n3 total | n2/n3\n";

            for (int octave = 0; octave < coarsegpu::COARSE_NOISE_DETAIL_OCTAVES; ++octave) {
                const long double n2s = noiseDetailTickSums[coarsegpu::COARSE_DETAIL_NOISE2_SETUP][octave];
                const long double n2e = noiseDetailTickSums[coarsegpu::COARSE_DETAIL_NOISE2_EVAL][octave];
                const long double n3s = noiseDetailTickSums[coarsegpu::COARSE_DETAIL_NOISE3_SETUP][octave];
                const long double n3e = noiseDetailTickSums[coarsegpu::COARSE_DETAIL_NOISE3_EVAL][octave];
                const long double n2 = n2s + n2e;
                const long double n3 = n3s + n3e;
                const double n2SetupMs = noise2Ticks > 0.0L ? n2PhaseMs * static_cast<double>(n2s / noise2Ticks) : 0.0;
                const double n2EvalMs = noise2Ticks > 0.0L ? n2PhaseMs * static_cast<double>(n2e / noise2Ticks) : 0.0;
                const double n3SetupMs = noise3Ticks > 0.0L ? n3PhaseMs * static_cast<double>(n3s / noise3Ticks) : 0.0;
                const double n3EvalMs = noise3Ticks > 0.0L ? n3PhaseMs * static_cast<double>(n3e / noise3Ticks) : 0.0;
                const double ratio = n3 > 0.0L ? static_cast<double>(n2 / n3) : 0.0;
                std::cout << "  " << std::setw(3) << octave
                          << " | " << std::setw(8) << std::fixed << std::setprecision(3) << n2SetupMs
                          << " | " << std::setw(7) << n2EvalMs
                          << " | " << std::setw(8) << (n2SetupMs + n2EvalMs)
                          << " | " << std::setw(8) << n3SetupMs
                          << " | " << std::setw(7) << n3EvalMs
                          << " | " << std::setw(8) << (n3SetupMs + n3EvalMs)
                          << " | " << std::setw(5) << std::setprecision(2) << ratio << "x\n";
            }

            auto sumRange = [&](int setupSeries, int evalSeries, int first, int lastExclusive) {
                long double sum = 0.0L;
                for (int octave = first; octave < lastExclusive; ++octave) {
                    sum += noiseDetailTickSums[setupSeries][octave] + noiseDetailTickSums[evalSeries][octave];
                }
                return sum;
            };
            const long double n2First = sumRange(coarsegpu::COARSE_DETAIL_NOISE2_SETUP, coarsegpu::COARSE_DETAIL_NOISE2_EVAL, 0, 1);
            const long double n2Early = sumRange(coarsegpu::COARSE_DETAIL_NOISE2_SETUP, coarsegpu::COARSE_DETAIL_NOISE2_EVAL, 0, 4);
            const long double n2Late = sumRange(coarsegpu::COARSE_DETAIL_NOISE2_SETUP, coarsegpu::COARSE_DETAIL_NOISE2_EVAL, 4, 16);
            const long double n3First = sumRange(coarsegpu::COARSE_DETAIL_NOISE3_SETUP, coarsegpu::COARSE_DETAIL_NOISE3_EVAL, 0, 1);
            const long double n3Early = sumRange(coarsegpu::COARSE_DETAIL_NOISE3_SETUP, coarsegpu::COARSE_DETAIL_NOISE3_EVAL, 0, 4);
            const long double n3Late = sumRange(coarsegpu::COARSE_DETAIL_NOISE3_SETUP, coarsegpu::COARSE_DETAIL_NOISE3_EVAL, 4, 16);

            std::cout << "\nDetail summary\n"
                      << "  measured noise2 phase:   " << std::fixed << std::setprecision(3) << n2PhaseMs << " ms\n"
                      << "  measured noise3 phase:   " << n3PhaseMs << " ms\n"
                      << "  noise2 setup share:      " << std::setprecision(2)
                      << (noise2Ticks > 0.0L ? static_cast<double>(noise2SetupTicks / noise2Ticks * 100.0L) : 0.0) << "%\n"
                      << "  noise2 eval share:       "
                      << (noise2Ticks > 0.0L ? static_cast<double>(noise2EvalTicks / noise2Ticks * 100.0L) : 0.0) << "%\n"
                      << "  noise3 setup share:      "
                      << (noise3Ticks > 0.0L ? static_cast<double>(noise3SetupTicks / noise3Ticks * 100.0L) : 0.0) << "%\n"
                      << "  noise3 eval share:       "
                      << (noise3Ticks > 0.0L ? static_cast<double>(noise3EvalTicks / noise3Ticks * 100.0L) : 0.0) << "%\n"
                      << "  overall raw n2/n3:       "
                      << (noise3Ticks > 0.0L ? static_cast<double>(noise2Ticks / noise3Ticks) : 0.0) << "x\n"
                      << "  octave 0 n2/n3:          "
                      << (n3First > 0.0L ? static_cast<double>(n2First / n3First) : 0.0) << "x\n"
                      << "  octaves 0-3 n2/n3:       "
                      << (n3Early > 0.0L ? static_cast<double>(n2Early / n3Early) : 0.0) << "x\n"
                      << "  octaves 4-15 n2/n3:      "
                      << (n3Late > 0.0L ? static_cast<double>(n2Late / n3Late) : 0.0) << "x\n";
        }

        hipEventDestroy(genStart); hipEventDestroy(genStop);
        hipEventDestroy(scoreStart); hipEventDestroy(scoreStop);
        hipEventDestroy(profileStart); hipEventDestroy(profileStop);
        hipFree(dPhaseTicks);
        if (dNoiseDetailTicks != nullptr) hipFree(dNoiseDetailTicks);
        freeCoarseBuffers(buffers);
        return (diffs == 0 && decisionDiffs == 0) ? 0 : 2;
    } catch (...) {
        hipEventDestroy(genStart); hipEventDestroy(genStop);
        hipEventDestroy(scoreStart); hipEventDestroy(scoreStop);
        hipEventDestroy(profileStart); hipEventDestroy(profileStop);
        hipFree(dPhaseTicks);
        if (dNoiseDetailTicks != nullptr) hipFree(dNoiseDetailTicks);
        freeCoarseBuffers(buffers);
        throw;
    }
}

int validate(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open reference file: " + path);
    char magic[8]; in.read(magic, 8);
    if (std::string(magic, 8) != "P20REF01") throw std::runtime_error("bad reference magic");
    const auto version = readLe32(in);
    const auto records = readLe32(in);
    const auto densityCount = readLe32(in);
    const auto axisSize = readLe32(in);
    if (version != 1 || densityCount != p20gpu::DENSITY_COUNT || axisSize != 8) {
        throw std::runtime_error("unsupported reference format");
    }

    std::vector<std::int64_t> seeds(records);
    std::vector<int> expectedCounts(records);
    std::vector<std::uint64_t> expectedBits(static_cast<std::size_t>(records) * densityCount);
    for (std::uint32_t r = 0; r < records; ++r) {
        seeds[r] = static_cast<std::int64_t>(readLe64(in));
        expectedCounts[r] = static_cast<int>(readLe32(in));
        for (std::uint32_t i = 0; i < densityCount; ++i) {
            expectedBits[static_cast<std::size_t>(r) * densityCount + i] = readLe64(in);
        }
    }

    std::int64_t* dSeeds = nullptr;
    int* dCounts = nullptr;
    double* dDensity = nullptr;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), seeds.size() * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dCounts), expectedCounts.size() * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dDensity), expectedBits.size() * sizeof(double)));
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), seeds.size() * sizeof(std::int64_t), hipMemcpyHostToDevice));

    hipEvent_t start{}, stop{};
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));
    HIP_CHECK(hipEventRecord(start));
    hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(records), dim3(64), 0, 0,
                       dSeeds, static_cast<int>(records), dCounts, dDensity, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipEventRecord(stop));
    HIP_CHECK(hipEventSynchronize(stop));
    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));

    std::vector<int> actualCounts(records);
    std::vector<double> actualDensity(expectedBits.size());
    HIP_CHECK(hipMemcpy(actualCounts.data(), dCounts, actualCounts.size() * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualDensity.data(), dDensity, actualDensity.size() * sizeof(double), hipMemcpyDeviceToHost));

    std::uint64_t rawMismatches = 0;
    std::uint64_t countMismatches = 0;
    std::uint64_t decisionMismatches = 0;
    std::size_t firstMismatch = static_cast<std::size_t>(-1);
    std::uint64_t firstExpected = 0, firstActual = 0;
    for (std::size_t r = 0; r < records; ++r) {
        if (actualCounts[r] != expectedCounts[r]) {
            ++countMismatches;
            if ((actualCounts[r] == 0) != (expectedCounts[r] == 0)) ++decisionMismatches;
        }
    }
    for (std::size_t i = 0; i < expectedBits.size(); ++i) {
        std::uint64_t bits;
        std::memcpy(&bits, &actualDensity[i], sizeof(bits));
        if (bits != expectedBits[i]) {
            ++rawMismatches;
            if (firstMismatch == static_cast<std::size_t>(-1)) {
                firstMismatch = i;
                firstExpected = expectedBits[i];
                firstActual = bits;
            }
        }
    }

    std::cout << "GPU exactness validation\n"
              << "  records:              " << records << "\n"
              << "  raw density mismatch: " << rawMismatches << "\n"
              << "  positive-count diff:  " << countMismatches << "\n"
              << "  P20 decision diff:    " << decisionMismatches << "\n"
              << "  kernel time:          " << std::fixed << std::setprecision(3) << ms << " ms\n"
              << "  kernel throughput:    " << std::setprecision(1) << (records * 1000.0 / ms) << " seeds/s\n";
    if (firstMismatch != static_cast<std::size_t>(-1)) {
        std::cout << "  first raw mismatch:   record=" << (firstMismatch / densityCount)
                  << " densityIndex=" << (firstMismatch % densityCount)
                  << " expectedBits=0x" << std::hex << firstExpected
                  << " actualBits=0x" << firstActual << std::dec << "\n";
    }

    hipEventDestroy(start); hipEventDestroy(stop);
    hipFree(dSeeds); hipFree(dCounts); hipFree(dDensity);
    return (rawMismatches == 0 && decisionMismatches == 0) ? 0 : 2;
}

int benchmark(int count, std::uint64_t sequenceSeed, int iterations) {
    if (count < 1 || iterations < 1) throw std::runtime_error("count and iterations must be >= 1");
    std::int64_t* dSeeds = nullptr;
    int* dCounts = nullptr;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dCounts), static_cast<std::size_t>(count) * sizeof(int)));

    const int seedThreads = 256;
    const int seedBlocks = (count + seedThreads - 1) / seedThreads;
    hipLaunchKernelGGL(p20gpu::generateDeterministicSeedsKernel, dim3(seedBlocks), dim3(seedThreads), 0, 0,
                       dSeeds, count, sequenceSeed);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    // Warm-up one full dispatch so shader compilation / clocks do not pollute timing.
    hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(64), 0, 0,
                       dSeeds, count, dCounts, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    hipEvent_t start{}, stop{};
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));
    HIP_CHECK(hipEventRecord(start));
    for (int i = 0; i < iterations; ++i) {
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(64), 0, 0,
                           dSeeds, count, dCounts, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0);
    }
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipEventRecord(stop));
    HIP_CHECK(hipEventSynchronize(stop));
    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));

    std::vector<int> counts(count);
    HIP_CHECK(hipMemcpy(counts.data(), dCounts, counts.size() * sizeof(int), hipMemcpyDeviceToHost));
    std::uint64_t checksum = 0;
    for (int v : counts) checksum += static_cast<std::uint64_t>(v);

    const double totalSeeds = static_cast<double>(count) * iterations;
    const double seconds = ms / 1000.0;
    std::cout << "GPU exact P20 scout benchmark\n"
              << "  seeds/iteration: " << count << "\n"
              << "  iterations:      " << iterations << "\n"
              << "  total time:      " << std::fixed << std::setprecision(3) << seconds << " s\n"
              << "  throughput:      " << std::setprecision(1) << (totalSeeds / seconds) << " seeds/s\n"
              << "  checksum:        " << checksum << "\n";

    hipEventDestroy(start); hipEventDestroy(stop);
    hipFree(dSeeds); hipFree(dCounts);
    return 0;
}



int validateStage0(const std::string& path, bool stagedP20) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open Stage0 reference file: " + path);
    char magic[8]; in.read(magic, 8);
    if (std::string(magic, 8) != "ST0REF01") throw std::runtime_error("bad Stage0 reference magic");
    const auto version = readLe32(in);
    const auto records = readLe32(in);
    const auto densityCount = readLe32(in);
    const auto columns = readLe32(in);
    if (version != 1 || densityCount != stage0gpu::FULL_POINTS * stage0gpu::FULL_YCOUNT
            || columns != stage0gpu::FULL_POINTS) {
        throw std::runtime_error("unsupported Stage0 reference format");
    }

    std::vector<std::int64_t> seeds(records);
    std::vector<int> expectedP20(records);
    std::vector<int> expectedUpper(records);
    std::vector<int> expectedHigh(records);
    std::vector<int> expectedCandidates(records);
    std::vector<std::uint32_t> expectedLowerMasks(static_cast<std::size_t>(records) * columns);
    std::vector<std::uint64_t> expectedBits(static_cast<std::size_t>(records) * densityCount);
    for (std::uint32_t r = 0; r < records; ++r) {
        seeds[r] = static_cast<std::int64_t>(readLe64(in));
        expectedP20[r] = static_cast<int>(readLe32(in));
        expectedUpper[r] = static_cast<int>(readLe32(in));
        expectedHigh[r] = static_cast<int>(readLe32(in));
        expectedCandidates[r] = static_cast<int>(readLe32(in));
        for (std::uint32_t c = 0; c < columns; ++c) {
            expectedLowerMasks[static_cast<std::size_t>(r) * columns + c] = readLe32(in);
        }
        for (std::uint32_t i = 0; i < densityCount; ++i) {
            expectedBits[static_cast<std::size_t>(r) * densityCount + i] = readLe64(in);
        }
    }

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    double* dUpperDensity = nullptr;
    double* dLowerDensity = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), seeds.size() * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), records * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), records * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), records * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(records) * columns));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperDensity),
            static_cast<std::size_t>(records) * columns * stage0gpu::UPPER_YCOUNT * sizeof(double)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dLowerDensity),
            static_cast<std::size_t>(records) * columns * stage0gpu::LOWER_YCOUNT * sizeof(double)));
    if (stagedP20) {
        allocateTerrainPerlinCache(terrainCache, static_cast<int>(records));
        allocateColumnShapeCache(shapeCache, static_cast<int>(records));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), seeds.size() * sizeof(std::int64_t), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemset(dHigh, 0, records * sizeof(int)));

    hipEvent_t start{}, stop{};
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));
    HIP_CHECK(hipEventRecord(start));
    if (stagedP20) {
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(records), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, static_cast<int>(records), dP20, dUpperDensity,
                terrainCache.permutations, terrainCache.offsets,
                nullptr, nullptr,
                dUpperMasks, shapeCache.d5, shapeCache.d7, 0, 0, 0, 0);
        HIP_CHECK(hipGetLastError());
    }
    hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(records), dim3(stage0gpu::FULL_POINTS), 0, 0,
            dSeeds, static_cast<int>(records), dP20, dUpper, dUpperMasks, dUpperDensity, stagedP20 ? 1 : 0,
            stagedP20 ? terrainCache.permutations : nullptr, stagedP20 ? terrainCache.offsets : nullptr,
            nullptr, nullptr,
            stagedP20 ? shapeCache.d5 : nullptr, stagedP20 ? shapeCache.d7 : nullptr, stagedP20 ? 1 : 0, 0, 0);
    HIP_CHECK(hipGetLastError());
    hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(records), dim3(stage0gpu::FULL_POINTS), 0, 0,
            dSeeds, static_cast<int>(records), dP20, dUpper, 5, dUpperMasks, dHigh, nullptr, dLowerDensity,
            stagedP20 ? terrainCache.permutations : nullptr, stagedP20 ? terrainCache.offsets : nullptr,
            stagedP20 ? shapeCache.d5 : nullptr, stagedP20 ? shapeCache.d7 : nullptr, 0, 0);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipEventRecord(stop));
    HIP_CHECK(hipEventSynchronize(stop));
    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));

    std::vector<int> actualP20(records), actualUpper(records), actualHigh(records);
    std::vector<unsigned char> actualUpperMasks(static_cast<std::size_t>(records) * columns);
    std::vector<double> actualUpperDensity(static_cast<std::size_t>(records) * columns * stage0gpu::UPPER_YCOUNT);
    std::vector<double> actualLowerDensity(static_cast<std::size_t>(records) * columns * stage0gpu::LOWER_YCOUNT);
    HIP_CHECK(hipMemcpy(actualP20.data(), dP20, records * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualUpper.data(), dUpper, records * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualHigh.data(), dHigh, records * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualUpperMasks.data(), dUpperMasks, actualUpperMasks.size(), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualUpperDensity.data(), dUpperDensity,
            actualUpperDensity.size() * sizeof(double), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualLowerDensity.data(), dLowerDensity,
            actualLowerDensity.size() * sizeof(double), hipMemcpyDeviceToHost));

    std::uint64_t p20CountDiff = 0;
    std::uint64_t p20DecisionDiff = 0;
    std::uint64_t upperCountDiff = 0;
    std::uint64_t stage1DecisionDiff = 0;
    std::uint64_t highCountDiff = 0;
    std::uint64_t stage0LowDecisionDiff = 0;
    std::uint64_t stage05DecisionDiff = 0;
    std::uint64_t upperRawMismatch = 0;
    std::uint64_t lowerRawMismatch = 0;
    std::size_t firstUpperMismatch = static_cast<std::size_t>(-1);
    std::size_t firstLowerMismatch = static_cast<std::size_t>(-1);

    for (std::size_t r = 0; r < records; ++r) {
        if (actualP20[r] != expectedP20[r]) {
            ++p20CountDiff;
            if ((actualP20[r] > 0) != (expectedP20[r] > 0)) ++p20DecisionDiff;
        }
        const bool upperEvaluated = !stagedP20 || expectedP20[r] > 0;
        const bool lowerEvaluated = !stagedP20 || (expectedP20[r] > 0 && expectedUpper[r] >= 5);
        if (upperEvaluated && actualUpper[r] != expectedUpper[r]) {
            ++upperCountDiff;
            if ((actualUpper[r] >= 5) != (expectedUpper[r] >= 5)) ++stage1DecisionDiff;
        }
        if (lowerEvaluated && actualHigh[r] != expectedHigh[r]) {
            ++highCountDiff;
            if ((actualHigh[r] >= 2) != (expectedHigh[r] >= 2)) ++stage0LowDecisionDiff;
            if ((actualHigh[r] >= 5) != (expectedHigh[r] >= 5)) ++stage05DecisionDiff;
        }

        for (std::size_t c = 0; c < columns; ++c) {
            if (upperEvaluated) for (int yi = 0; yi < stage0gpu::UPPER_YCOUNT; ++yi) {
                const std::size_t expectedIndex = r * densityCount + c * stage0gpu::FULL_YCOUNT
                        + stage0gpu::UPPER_Y_FROM + yi;
                const std::size_t actualIndex = (r * columns + c) * stage0gpu::UPPER_YCOUNT + yi;
                std::uint64_t bits;
                std::memcpy(&bits, &actualUpperDensity[actualIndex], sizeof(bits));
                if (bits != expectedBits[expectedIndex]) {
                    ++upperRawMismatch;
                    if (firstUpperMismatch == static_cast<std::size_t>(-1)) firstUpperMismatch = expectedIndex;
                }
            }

            const bool lowerExpected = lowerEvaluated && expectedLowerMasks[r * columns + c] != 0;
            if (!lowerExpected) continue;
            for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
                const std::size_t expectedIndex = r * densityCount + c * stage0gpu::FULL_YCOUNT + y;
                const std::size_t actualIndex = (r * columns + c) * stage0gpu::LOWER_YCOUNT + y;
                std::uint64_t bits;
                std::memcpy(&bits, &actualLowerDensity[actualIndex], sizeof(bits));
                if (bits != expectedBits[expectedIndex]) {
                    ++lowerRawMismatch;
                    if (firstLowerMismatch == static_cast<std::size_t>(-1)) firstLowerMismatch = expectedIndex;
                }
            }
        }
    }

    std::cout << (stagedP20 ? "GPU optimized staged-P20 Stage0-chain validation\n"
                               : "GPU exact full-raw Stage0-chain validation\n")
              << "  records:                 " << records << "\n"
              << "  upper raw mismatches:    " << upperRawMismatch << "\n"
              << "  lower raw mismatches:    " << lowerRawMismatch << "\n"
              << "  P20 count diff:          " << p20CountDiff << "\n"
              << "  P20 decision diff:       " << p20DecisionDiff << "\n"
              << "  Stage-1 count diff:      " << upperCountDiff << "\n"
              << "  Stage-1 decision diff:   " << stage1DecisionDiff << "\n"
              << "  Stage0 reentry diff:     " << highCountDiff << "\n"
              << "  Stage0 decision diff:    " << stage0LowDecisionDiff << "\n"
              << "  Stage0.5 decision diff:  " << stage05DecisionDiff << "\n"
              << (stagedP20 ? "  cached three-kernel time:" : "  full two-kernel time:   ")
              << std::fixed << std::setprecision(3) << ms << " ms\n"
              << "  chain throughput:        " << std::setprecision(1) << (records * 1000.0 / ms) << " seeds/s\n";
    if (firstUpperMismatch != static_cast<std::size_t>(-1)) {
        std::cout << "  first upper mismatch:    record=" << (firstUpperMismatch / densityCount)
                  << " densityIndex=" << (firstUpperMismatch % densityCount) << "\n";
    }
    if (firstLowerMismatch != static_cast<std::size_t>(-1)) {
        std::cout << "  first lower mismatch:    record=" << (firstLowerMismatch / densityCount)
                  << " densityIndex=" << (firstLowerMismatch % densityCount) << "\n";
    }

    hipEventDestroy(start); hipEventDestroy(stop);
    hipFree(dSeeds); hipFree(dP20); hipFree(dUpper); hipFree(dHigh);
    hipFree(dUpperMasks); hipFree(dUpperDensity); hipFree(dLowerDensity);
    freeTerrainPerlinCache(terrainCache);
    freeColumnShapeCache(shapeCache);

    return (upperRawMismatch == 0 && lowerRawMismatch == 0
            && p20DecisionDiff == 0 && stage1DecisionDiff == 0
            && stage0LowDecisionDiff == 0 && stage05DecisionDiff == 0) ? 0 : 2;
}

int benchmarkStage0(int count, std::uint64_t sequenceSeed, int iterations, bool stagedP20) {
    if (count < 1 || iterations < 1) throw std::runtime_error("count and iterations must be >= 1");

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    if (stagedP20) {
        allocateTerrainPerlinCache(terrainCache, count);
        allocateColumnShapeCache(shapeCache, count);
    }

    const int seedThreads = 256;
    const int seedBlocks = (count + seedThreads - 1) / seedThreads;
    hipLaunchKernelGGL(p20gpu::generateDeterministicSeedsKernel, dim3(seedBlocks), dim3(seedThreads), 0, 0,
            dSeeds, count, sequenceSeed);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    auto launchChain = [&]() {
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        if (stagedP20) {
            hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                    dSeeds, count, dP20, nullptr, terrainCache.permutations, terrainCache.offsets,
                    nullptr, nullptr,
                    dUpperMasks, shapeCache.d5, shapeCache.d7, 0, 0, 0, 0);
            HIP_CHECK(hipGetLastError());
        }
        hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, stagedP20 ? 1 : 0,
                stagedP20 ? terrainCache.permutations : nullptr, stagedP20 ? terrainCache.offsets : nullptr,
                nullptr, nullptr,
                stagedP20 ? shapeCache.d5 : nullptr, stagedP20 ? shapeCache.d7 : nullptr, stagedP20 ? 1 : 0, 0, 0);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, 5, dUpperMasks, dHigh, nullptr, nullptr,
                stagedP20 ? terrainCache.permutations : nullptr, stagedP20 ? terrainCache.offsets : nullptr,
                stagedP20 ? shapeCache.d5 : nullptr, stagedP20 ? shapeCache.d7 : nullptr, 0, 0);
        HIP_CHECK(hipGetLastError());
    };

    launchChain();
    HIP_CHECK(hipDeviceSynchronize());

    hipEvent_t start{}, stop{};
    HIP_CHECK(hipEventCreate(&start));
    HIP_CHECK(hipEventCreate(&stop));
    HIP_CHECK(hipEventRecord(start));
    for (int i = 0; i < iterations; ++i) launchChain();
    HIP_CHECK(hipEventRecord(stop));
    HIP_CHECK(hipEventSynchronize(stop));
    float ms = 0.0f;
    HIP_CHECK(hipEventElapsedTime(&ms, start, stop));

    std::vector<int> p20(count), upper(count), high(count);
    HIP_CHECK(hipMemcpy(p20.data(), dP20, p20.size() * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(upper.data(), dUpper, upper.size() * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(high.data(), dHigh, high.size() * sizeof(int), hipMemcpyDeviceToHost));
    std::uint64_t checksum = 0;
    std::uint64_t p20Passed = 0, stage1Passed = 0, stage0Passed = 0;
    for (int i = 0; i < count; ++i) {
        const int chainUpper = p20[i] > 0 ? upper[i] : 0;
        const int chainHigh = (p20[i] > 0 && upper[i] >= 5) ? high[i] : 0;
        checksum += static_cast<std::uint64_t>(p20[i])
                + 1000ULL * static_cast<std::uint64_t>(chainUpper)
                + 1000000ULL * static_cast<std::uint64_t>(chainHigh);
        if (p20[i] > 0) ++p20Passed;
        if (p20[i] > 0 && upper[i] >= 5) ++stage1Passed;
        if (p20[i] > 0 && upper[i] >= 5 && high[i] >= 5) ++stage0Passed;
    }

    const double totalSeeds = static_cast<double>(count) * iterations;
    const double seconds = ms / 1000.0;
    std::cout << (stagedP20 ? "GPU optimized staged-P20 + Stage1 + Stage0 chain benchmark\n"
                               : "GPU legacy full-upper Stage0 chain benchmark\n")
              << "  seeds/iteration: " << count << "\n"
              << "  iterations:      " << iterations << "\n"
              << "  total time:      " << std::fixed << std::setprecision(3) << seconds << " s\n"
              << "  throughput:      " << std::setprecision(1) << (totalSeeds / seconds) << " seeds/s\n"
              << "  P20 survivors:   " << p20Passed << "\n"
              << "  Stage1 survivors:" << stage1Passed << "\n"
              << "  Stage0 survivors:" << stage0Passed << "\n"
              << "  checksum:        " << checksum << "\n";

    hipEventDestroy(start); hipEventDestroy(stop);
    hipFree(dSeeds); hipFree(dP20); hipFree(dUpper); hipFree(dHigh); hipFree(dUpperMasks);
    freeTerrainPerlinCache(terrainCache);
    freeColumnShapeCache(shapeCache);
    return 0;
}

int validateP19(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open P19 reference file: " + path);
    char magic[8]; in.read(magic, 8);
    if (std::string(magic, 8) != "P19REF01") throw std::runtime_error("bad P19 reference magic");
    const auto version = readLe32(in);
    const auto records = readLe32(in);
    if (version != 1 || records < 1) throw std::runtime_error("unsupported P19 reference format");

    struct ExpectedRow {
        std::int64_t seed = 0;
        int p20 = 0;
        int upper = 0;
        int high = 0;
        p19native::MonsterFeatures features{};
        double score = 0.0;
        bool pass = false;
    };

    std::vector<ExpectedRow> expected(records);
    std::vector<std::int64_t> seeds(records);
    for (std::uint32_t i = 0; i < records; ++i) {
        ExpectedRow& row = expected[i];
        row.seed = static_cast<std::int64_t>(readLe64(in));
        seeds[i] = row.seed;
        row.p20 = static_cast<int>(readLe32(in));
        row.upper = static_cast<int>(readLe32(in));
        row.high = static_cast<int>(readLe32(in));
        auto& f = row.features;
        f.stage0FullY88 = static_cast<int>(readLe32(in));
        f.stage0FullY96 = static_cast<int>(readLe32(in));
        f.stage0FullY104 = static_cast<int>(readLe32(in));
        f.stage0FullY112 = static_cast<int>(readLe32(in));
        f.stage0Y88LargestCluster = static_cast<int>(readLe32(in));
        f.stage0Y88Width = static_cast<int>(readLe32(in));
        f.stage0Y88Depth = static_cast<int>(readLe32(in));
        f.stage0Y88TouchesBorder = readLe32(in) != 0;
        f.stage0Y96LargestCluster = static_cast<int>(readLe32(in));
        f.stage0Y96Width = static_cast<int>(readLe32(in));
        f.stage0Y96Depth = static_cast<int>(readLe32(in));
        f.stage0Y96TouchesBorder = readLe32(in) != 0;
        const std::uint64_t scoreBits = readLe64(in);
        std::memcpy(&row.score, &scoreBits, sizeof(row.score));
        row.pass = readLe32(in) != 0;
    }
    if (!in) throw std::runtime_error("truncated P19 reference file");

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(records) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(records) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(records) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(records) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(records) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(records) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(records) * sizeof(std::int64_t), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(records) * sizeof(int)));

    hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(records), dim3(p20gpu::POINTS), 0, 0,
            dSeeds, static_cast<int>(records), dP20, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0);
    HIP_CHECK(hipGetLastError());
    hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(records), dim3(stage0gpu::FULL_POINTS), 0, 0,
            dSeeds, static_cast<int>(records), dP20, dUpper, dUpperMasks, nullptr, 1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0);
    HIP_CHECK(hipGetLastError());
    hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(records), dim3(stage0gpu::FULL_POINTS), 0, 0,
            dSeeds, static_cast<int>(records), dP20, dUpper, 5, dUpperMasks, dHigh, dHighestReentryY, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0);
    HIP_CHECK(hipGetLastError());

    std::vector<int> actualP20(records), actualUpper(records), actualHigh(records);
    std::vector<unsigned char> actualTopology(static_cast<std::size_t>(records) * stage0gpu::FULL_POINTS);
    HIP_CHECK(hipMemcpy(actualP20.data(), dP20, static_cast<std::size_t>(records) * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualUpper.data(), dUpper, static_cast<std::size_t>(records) * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualHigh.data(), dHigh, static_cast<std::size_t>(records) * sizeof(int), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(actualTopology.data(), dHighestReentryY, actualTopology.size(), hipMemcpyDeviceToHost));

    std::uint64_t p20Diff = 0;
    std::uint64_t upperDiff = 0;
    std::uint64_t highDiff = 0;
    std::uint64_t highDecisionDiff = 0;
    std::uint64_t featureDiff = 0;
    std::uint64_t decisionDiff = 0;
    double maxScoreAbsDiff = 0.0;

    auto countFeatureDiffs = [](const p19native::MonsterFeatures& a, const p19native::MonsterFeatures& b) {
        int diff = 0;
        diff += a.stage0FullY88 != b.stage0FullY88;
        diff += a.stage0FullY96 != b.stage0FullY96;
        diff += a.stage0FullY104 != b.stage0FullY104;
        diff += a.stage0FullY112 != b.stage0FullY112;
        diff += a.stage0Y88LargestCluster != b.stage0Y88LargestCluster;
        diff += a.stage0Y88Width != b.stage0Y88Width;
        diff += a.stage0Y88Depth != b.stage0Y88Depth;
        diff += a.stage0Y88TouchesBorder != b.stage0Y88TouchesBorder;
        diff += a.stage0Y96LargestCluster != b.stage0Y96LargestCluster;
        diff += a.stage0Y96Width != b.stage0Y96Width;
        diff += a.stage0Y96Depth != b.stage0Y96Depth;
        diff += a.stage0Y96TouchesBorder != b.stage0Y96TouchesBorder;
        return diff;
    };

    for (std::uint32_t i = 0; i < records; ++i) {
        const ExpectedRow& row = expected[i];
        if (actualP20[i] != row.p20) ++p20Diff;
        if (actualUpper[i] != row.upper) ++upperDiff;
        if (actualHigh[i] != row.high) {
            ++highDiff;
            if ((actualHigh[i] >= 5) != (row.high >= 5)) ++highDecisionDiff;
        }
        const p19native::MonsterFeatures features = buildMonsterFeatures(
                actualTopology.data() + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS
        );
        featureDiff += static_cast<std::uint64_t>(countFeatureDiffs(features, row.features));
        const double actualScore = p19native::score(actualUpper[i], features);
        const double absDiff = actualScore >= row.score ? actualScore - row.score : row.score - actualScore;
        if (absDiff > maxScoreAbsDiff) maxScoreAbsDiff = absDiff;
        const bool actualPass = p19native::hasExtremeTopologySignal(features)
                || actualScore >= p19native::THRESHOLD;
        if (actualPass != row.pass) ++decisionDiff;
    }

    hipFree(dSeeds); hipFree(dP20); hipFree(dUpper); hipFree(dHigh);
    hipFree(dUpperMasks); hipFree(dHighestReentryY);

    std::cout << "GPU/native exact P19 Stage0.75 validation\n"
              << "  records:                 " << records << "\n"
              << "  P20 count diff:          " << p20Diff << "\n"
              << "  upper scout count diff:  " << upperDiff << "\n"
              << "  high reentry count diff: " << highDiff << "\n"
              << "  high reentry decision diff: " << highDecisionDiff << "\n"
              << "  P19 feature field diffs: " << featureDiff << "\n"
              << "  P19 decision diff:       " << decisionDiff << "\n"
              << "  max score abs diff:      " << std::setprecision(17) << maxScoreAbsDiff << "\n";

    return (p20Diff == 0 && upperDiff == 0 && highDecisionDiff == 0
            && featureDiff == 0 && decisionDiff == 0 && maxScoreAbsDiff <= 1.0E-12) ? 0 : 2;
}



struct PipelineProfileTimes {
    double seedH2D = 0.0;
    double highMemset = 0.0;
    double p20Gpu = 0.0;
    double upperGpu = 0.0;
    double lowerGpu = 0.0;
    double stageD2H = 0.0;
    double p19Cpu = 0.0;
    double coarseGatherScatter = 0.0;
    double coarseH2D = 0.0;
    double coarseGenerateGpu = 0.0;
    double coarseScoreGpu = 0.0;
    double coarseD2H = 0.0;
    double outputPackCpu = 0.0;
    double wallTotal = 0.0;
};

struct PipelineProfileCounts {
    std::uint64_t p20Passed = 0;
    std::uint64_t stage1Passed = 0;
    std::uint64_t stage05Passed = 0;
    std::uint64_t p19Passed = 0;
    std::uint64_t megaTopologyRejected = 0;
    std::uint64_t coarsePassed = 0;
    std::uint64_t coarseChunks = 0;
};

double hostElapsedMs(
        const std::chrono::steady_clock::time_point& begin,
        const std::chrono::steady_clock::time_point& end
) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void addPipelineTimes(PipelineProfileTimes& out, const PipelineProfileTimes& v) {
    out.seedH2D += v.seedH2D;
    out.highMemset += v.highMemset;
    out.p20Gpu += v.p20Gpu;
    out.upperGpu += v.upperGpu;
    out.lowerGpu += v.lowerGpu;
    out.stageD2H += v.stageD2H;
    out.p19Cpu += v.p19Cpu;
    out.coarseGatherScatter += v.coarseGatherScatter;
    out.coarseH2D += v.coarseH2D;
    out.coarseGenerateGpu += v.coarseGenerateGpu;
    out.coarseScoreGpu += v.coarseScoreGpu;
    out.coarseD2H += v.coarseD2H;
    out.outputPackCpu += v.outputPackCpu;
    out.wallTotal += v.wallTotal;
}

void dividePipelineTimes(PipelineProfileTimes& out, double divisor) {
    out.seedH2D /= divisor;
    out.highMemset /= divisor;
    out.p20Gpu /= divisor;
    out.upperGpu /= divisor;
    out.lowerGpu /= divisor;
    out.stageD2H /= divisor;
    out.p19Cpu /= divisor;
    out.coarseGatherScatter /= divisor;
    out.coarseH2D /= divisor;
    out.coarseGenerateGpu /= divisor;
    out.coarseScoreGpu /= divisor;
    out.coarseD2H /= divisor;
    out.outputPackCpu /= divisor;
    out.wallTotal /= divisor;
}

enum class P24ColumnCacheMode {
    BASELINE,
    SHAPE_ONLY,
    FULL_REUSE
};

const char* p24ColumnCacheModeName(P24ColumnCacheMode mode) {
    switch (mode) {
        case P24ColumnCacheMode::BASELINE: return "P23 baseline";
        case P24ColumnCacheMode::SHAPE_ONLY: return "P24 shape cache";
        default: return "P24 full reuse";
    }
}

int profileProductionPipeline(int count, std::uint64_t sequenceSeed, int iterations, bool megaMode,
                              P24ColumnCacheMode cacheMode) {
    if (count < 1) throw std::runtime_error("pipelineprofile count must be >= 1");
    if (iterations < 1) throw std::runtime_error("pipelineprofile iterations must be >= 1");

    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    if (cacheMode != P24ColumnCacheMode::BASELINE) allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }

    std::vector<int> p20(count), upper(count), high(count), coarse(count, 0);
    std::vector<unsigned char> highestReentryY(static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS);
    std::vector<unsigned char> p19Pass(count, 0);
    std::vector<unsigned char> megaRejected(count, 0);
    std::vector<unsigned char> output(static_cast<std::size_t>(count) * 6);
    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    hipEvent_t beforeHighMemset{}, stageStart{}, afterP20{}, afterUpper{}, afterLower{};
    hipEvent_t coarseStart{}, afterCoarseGenerate{}, afterCoarseScore{};
    HIP_CHECK(hipEventCreate(&beforeHighMemset));
    HIP_CHECK(hipEventCreate(&stageStart));
    HIP_CHECK(hipEventCreate(&afterP20));
    HIP_CHECK(hipEventCreate(&afterUpper));
    HIP_CHECK(hipEventCreate(&afterLower));
    HIP_CHECK(hipEventCreate(&coarseStart));
    HIP_CHECK(hipEventCreate(&afterCoarseGenerate));
    HIP_CHECK(hipEventCreate(&afterCoarseScore));

    auto runOne = [&](PipelineProfileTimes& times, PipelineProfileCounts& counts) {
        using Clock = std::chrono::steady_clock;
        const auto wallBegin = Clock::now();

        auto h0 = Clock::now();
        HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
        auto h1 = Clock::now();
        times.seedH2D += hostElapsedMs(h0, h1);

        HIP_CHECK(hipEventRecord(beforeHighMemset));
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        HIP_CHECK(hipEventRecord(stageStart));
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr, terrainCache.permutations, terrainCache.offsets,
                nullptr, nullptr,
                cacheMode == P24ColumnCacheMode::FULL_REUSE ? dUpperMasks : nullptr,
                cacheMode == P24ColumnCacheMode::FULL_REUSE ? shapeCache.d5 : nullptr,
                cacheMode == P24ColumnCacheMode::FULL_REUSE ? shapeCache.d7 : nullptr, 0, 0, 0, 0);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(afterP20));

        hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, 1,
                terrainCache.permutations, terrainCache.offsets,
                nullptr, nullptr,
                cacheMode == P24ColumnCacheMode::BASELINE ? nullptr : shapeCache.d5,
                cacheMode == P24ColumnCacheMode::BASELINE ? nullptr : shapeCache.d7,
                cacheMode == P24ColumnCacheMode::FULL_REUSE ? 1 : 0, 0, 0);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(afterUpper));

        hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks, dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                cacheMode == P24ColumnCacheMode::BASELINE ? nullptr : shapeCache.d5,
                cacheMode == P24ColumnCacheMode::BASELINE ? nullptr : shapeCache.d7, 0, 0);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(afterLower));
        HIP_CHECK(hipEventSynchronize(afterLower));

        float phaseMs = 0.0f;
        HIP_CHECK(hipEventElapsedTime(&phaseMs, beforeHighMemset, stageStart));
        times.highMemset += phaseMs;
        HIP_CHECK(hipEventElapsedTime(&phaseMs, stageStart, afterP20));
        times.p20Gpu += phaseMs;
        HIP_CHECK(hipEventElapsedTime(&phaseMs, afterP20, afterUpper));
        times.upperGpu += phaseMs;
        HIP_CHECK(hipEventElapsedTime(&phaseMs, afterUpper, afterLower));
        times.lowerGpu += phaseMs;

        h0 = Clock::now();
        HIP_CHECK(hipMemcpy(p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));
        h1 = Clock::now();
        times.stageD2H += hostElapsedMs(h0, h1);

        std::fill(coarse.begin(), coarse.end(), 0);
        std::fill(p19Pass.begin(), p19Pass.end(), 0);
        std::fill(megaRejected.begin(), megaRejected.end(), 0);
        coarseIndices.clear();

        h0 = Clock::now();
        for (int i = 0; i < count; ++i) {
            if (!(p20[i] > 0 && upper[i] >= upperMinCount && high[i] >= highMinCount)) continue;
            const unsigned char* topology = highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double score = p19native::score(upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || score >= p19native::THRESHOLD;
            const bool rejectMega = pass && megaMode && megaTopologyReject(score, extreme, features);
            p19Pass[i] = pass ? 1u : 0u;
            megaRejected[i] = rejectMega ? 1u : 0u;
            if (pass && !rejectMega) coarseIndices.push_back(i);
        }
        h1 = Clock::now();
        times.p19Cpu += hostElapsedMs(h0, h1);

        counts.coarseChunks = 0;
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            ++counts.coarseChunks;

            h0 = Clock::now();
            for (int j = 0; j < chunk; ++j) coarseSeeds[j] = seeds[coarseIndices[offset + j]];
            h1 = Clock::now();
            times.coarseGatherScatter += hostElapsedMs(h0, h1);

            h0 = Clock::now();
            HIP_CHECK(hipMemcpy(coarseBuffers.seeds, coarseSeeds.data(),
                    static_cast<std::size_t>(chunk) * sizeof(std::int64_t), hipMemcpyHostToDevice));
            h1 = Clock::now();
            times.coarseH2D += hostElapsedMs(h0, h1);

            HIP_CHECK(hipEventRecord(coarseStart));
            hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel, dim3(chunk), dim3(coarsegpu::THREADS), 0, 0,
                    coarseBuffers.seeds, chunk,
                    coarseBuffers.temp, coarseBuffers.rain, coarseBuffers.climateBlend,
                    coarseBuffers.noise1, coarseBuffers.noise2, coarseBuffers.noise3,
                    coarseBuffers.noise4, coarseBuffers.noise5,
                    coarseBuffers.signs, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipEventRecord(afterCoarseGenerate));

            hipLaunchKernelGGL(coarsegpu::scoreCoarseSignsKernel, dim3(chunk), dim3(1), 0, 0,
                    coarseBuffers.signs, chunk, coarseBuffers.labels, coarseBuffers.queue,
                    coarseBuffers.columnSeen, coarseBuffers.columnMinY,
                    coarseBuffers.componentColumns, coarseBuffers.scores);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipEventRecord(afterCoarseScore));
            HIP_CHECK(hipEventSynchronize(afterCoarseScore));

            HIP_CHECK(hipEventElapsedTime(&phaseMs, coarseStart, afterCoarseGenerate));
            times.coarseGenerateGpu += phaseMs;
            HIP_CHECK(hipEventElapsedTime(&phaseMs, afterCoarseGenerate, afterCoarseScore));
            times.coarseScoreGpu += phaseMs;

            h0 = Clock::now();
            HIP_CHECK(hipMemcpy(coarseScores.data(), coarseBuffers.scores,
                    static_cast<std::size_t>(chunk) * sizeof(int), hipMemcpyDeviceToHost));
            h1 = Clock::now();
            times.coarseD2H += hostElapsedMs(h0, h1);

            h0 = Clock::now();
            for (int j = 0; j < chunk; ++j) coarse[coarseIndices[offset + j]] = coarseScores[j];
            h1 = Clock::now();
            times.coarseGatherScatter += hostElapsedMs(h0, h1);
        }

        h0 = Clock::now();
        counts = {};
        counts.coarseChunks = static_cast<std::uint64_t>((coarseIndices.size() + coarseChunkCapacity - 1) / coarseChunkCapacity);
        for (int i = 0; i < count; ++i) {
            const int a = p20[i];
            const int b = upper[i];
            const int c = high[i];
            const int d = coarse[i];
            const unsigned char e = p19Pass[i];
            const std::size_t base = static_cast<std::size_t>(i) * 6;
            output[base] = static_cast<unsigned char>(a < 0 ? 0 : (a > 255 ? 255 : a));
            output[base + 1] = static_cast<unsigned char>(b < 0 ? 0 : (b > 255 ? 255 : b));
            output[base + 2] = static_cast<unsigned char>(c < 0 ? 0 : (c > 255 ? 255 : c));
            output[base + 3] = e;
            output[base + 4] = static_cast<unsigned char>(d & 0xFF);
            output[base + 5] = static_cast<unsigned char>((d >> 8) & 0xFF);
            if (a > 0) ++counts.p20Passed;
            if (a > 0 && b >= upperMinCount) ++counts.stage1Passed;
            if (a > 0 && b >= upperMinCount && c >= highMinCount) {
                ++counts.stage05Passed;
                if (e != 0) {
                    ++counts.p19Passed;
                    if (megaRejected[i] != 0) {
                        ++counts.megaTopologyRejected;
                    } else if (d >= 85) {
                        ++counts.coarsePassed;
                    }
                }
            }
        }
        h1 = Clock::now();
        times.outputPackCpu += hostElapsedMs(h0, h1);
        times.wallTotal += hostElapsedMs(wallBegin, Clock::now());
    };

    PipelineProfileTimes warmupTimes{};
    PipelineProfileCounts warmupCounts{};
    runOne(warmupTimes, warmupCounts);

    PipelineProfileTimes totalTimes{};
    PipelineProfileCounts counts{};
    for (int iteration = 0; iteration < iterations; ++iteration) {
        PipelineProfileTimes one{};
        PipelineProfileCounts oneCounts{};
        runOne(one, oneCounts);
        addPipelineTimes(totalTimes, one);
        counts = oneCounts;
    }
    dividePipelineTimes(totalTimes, static_cast<double>(iterations));

    const double accounted = totalTimes.seedH2D + totalTimes.highMemset
            + totalTimes.p20Gpu + totalTimes.upperGpu + totalTimes.lowerGpu
            + totalTimes.stageD2H + totalTimes.p19Cpu + totalTimes.coarseGatherScatter
            + totalTimes.coarseH2D + totalTimes.coarseGenerateGpu + totalTimes.coarseScoreGpu
            + totalTimes.coarseD2H + totalTimes.outputPackCpu;
    const double unaccounted = totalTimes.wallTotal - accounted;
    const double earlyGpu = totalTimes.p20Gpu + totalTimes.upperGpu + totalTimes.lowerGpu;
    const double coarseTotal = totalTimes.coarseGatherScatter + totalTimes.coarseH2D
            + totalTimes.coarseGenerateGpu + totalTimes.coarseScoreGpu + totalTimes.coarseD2H;
    const double transferPack = totalTimes.seedH2D + totalTimes.highMemset
            + totalTimes.stageD2H + totalTimes.outputPackCpu;

    struct NamedPhase { const char* name; double ms; };
    const NamedPhase phases[] = {
        {"seed H2D", totalTimes.seedH2D},
        {"dHigh memset", totalTimes.highMemset},
        {"P20 exact GPU", totalTimes.p20Gpu},
        {"upper GPU (Stage1/full upper)", totalTimes.upperGpu},
        {"lower GPU (Stage0/0.5 reentry)", totalTimes.lowerGpu},
        {"stage metadata D2H", totalTimes.stageD2H},
        {"P19 / Stage0.75 CPU gate", totalTimes.p19Cpu},
        {"coarse gather + scatter CPU", totalTimes.coarseGatherScatter},
        {"coarse seed H2D", totalTimes.coarseH2D},
        {"coarse generate GPU", totalTimes.coarseGenerateGpu},
        {"coarse score GPU", totalTimes.coarseScoreGpu},
        {"coarse score D2H", totalTimes.coarseD2H},
        {"response pack CPU", totalTimes.outputPackCpu}
    };
    const NamedPhase* hottest = &phases[0];
    for (const auto& phase : phases) if (phase.ms > hottest->ms) hottest = &phase;

    const auto pct = [&](double ms) {
        return totalTimes.wallTotal > 0.0 ? ms * 100.0 / totalTimes.wallTotal : 0.0;
    };
    const auto passPct = [&](std::uint64_t value) {
        return count > 0 ? static_cast<double>(value) * 100.0 / static_cast<double>(count) : 0.0;
    };

    std::cout << "GPU production pipeline profile\n"
              << "  profile:                " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  column cache mode:      " << p24ColumnCacheModeName(cacheMode) << "\n"
              << "  upper/high gates:       " << upperMinCount << "/" << highMinCount << "\n"
              << "  Mega topology reject:   " << (megaMode ? "ON" : "OFF") << "\n"
              << "  production batch:       " << count << " seeds\n"
              << "  deterministic seed:     " << sequenceSeed << "\n"
              << "  warmup batches:         1\n"
              << "  measured iterations:    " << iterations << "\n"
              << "  coarse chunk capacity:  " << coarseChunkCapacity << "\n"
              << "  terrain state cache:    " << std::fixed << std::setprecision(1)
              << (terraincache::totalBytesForCapacity(count) / (1024.0 * 1024.0)) << " MiB\n"
              << "  column shape cache:     " << (cacheMode == P24ColumnCacheMode::BASELINE ? 0.0 :
                     columnShapeCacheBytesForCapacity(count) / (1024.0 * 1024.0)) << " MiB\n"
              << "  coarse chunks/batch:    " << counts.coarseChunks << "\n"
              << "  average wall total:     " << std::fixed << std::setprecision(3) << totalTimes.wallTotal << " ms\n"
              << "  internal throughput:    " << std::setprecision(1)
              << (static_cast<double>(count) * 1000.0 / totalTimes.wallTotal) << " seeds/s\n"
              << "  note: excludes stdin/stdout pipe I/O; includes production compute, copies, P19 gate, and response packing\n\n";

    std::cout << "Production survivor counts\n"
              << std::fixed << std::setprecision(2)
              << "  P20 pass:                " << counts.p20Passed << " | " << passPct(counts.p20Passed) << "%\n"
              << "  Stage1 pass:             " << counts.stage1Passed << " | " << passPct(counts.stage1Passed) << "%\n"
              << "  Stage0.5 pass:           " << counts.stage05Passed << " | " << passPct(counts.stage05Passed) << "%\n"
              << "  P19 / Stage0.75 pass:    " << counts.p19Passed << " | " << passPct(counts.p19Passed) << "%\n"
              << "  Mega topology rejects:  " << counts.megaTopologyRejected << " | " << passPct(counts.megaTopologyRejected) << "%\n"
              << "  coarse pass >=85:        " << counts.coarsePassed << " | " << passPct(counts.coarsePassed) << "%\n\n";

    std::cout << "Per-batch phase breakdown\n";
    for (const auto& phase : phases) {
        std::cout << "  " << std::left << std::setw(35) << phase.name
                  << std::right << std::fixed << std::setprecision(3) << std::setw(10) << phase.ms << " ms | "
                  << std::setprecision(2) << std::setw(6) << pct(phase.ms) << "% total\n";
    }
    std::cout << "  " << std::left << std::setw(35) << "unaccounted / profiler overhead"
              << std::right << std::fixed << std::setprecision(3) << std::setw(10) << unaccounted << " ms | "
              << std::setprecision(2) << std::setw(6) << pct(unaccounted) << "% total\n\n";

    std::cout << "Grouped targets\n"
              << "  early GPU kernels:       " << std::fixed << std::setprecision(3) << earlyGpu << " ms | "
              << std::setprecision(2) << pct(earlyGpu) << "% total\n"
              << "  P19 / Stage0.75 CPU:     " << std::setprecision(3) << totalTimes.p19Cpu << " ms | "
              << std::setprecision(2) << pct(totalTimes.p19Cpu) << "% total\n"
              << "  exact coarse all-in:     " << std::setprecision(3) << coarseTotal << " ms | "
              << std::setprecision(2) << pct(coarseTotal) << "% total\n"
              << "  transfers + packing:     " << std::setprecision(3) << transferPack << " ms | "
              << std::setprecision(2) << pct(transferPack) << "% total\n"
              << "  hottest named phase:     " << hottest->name << "\n"
              << "  accounted phase sum:     " << std::setprecision(3) << accounted << " ms | "
              << std::setprecision(2) << pct(accounted) << "% total\n";

    hipEventDestroy(beforeHighMemset);
    hipEventDestroy(stageStart);
    hipEventDestroy(afterP20);
    hipEventDestroy(afterUpper);
    hipEventDestroy(afterLower);
    hipEventDestroy(coarseStart);
    hipEventDestroy(afterCoarseGenerate);
    hipEventDestroy(afterCoarseScore);
    freeCoarseBuffers(coarseBuffers);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    freeTerrainPerlinCache(terrainCache);
    freeColumnShapeCache(shapeCache);
    return 0;
}

struct MultiCenterCounts {
    std::uint64_t p20Passed = 0;
    std::uint64_t stage1Passed = 0;
    std::uint64_t stage05Passed = 0;
    std::uint64_t p19Passed = 0;
    std::uint64_t coarsePassed85 = 0;
    std::uint64_t coarseEvaluated = 0;
};

struct MultiCenterOutputs {
    std::vector<int> p20;
    std::vector<int> upper;
    std::vector<int> high;
    std::vector<int> coarse;
    std::vector<unsigned char> highestReentryY;
    std::vector<unsigned char> p19Pass;
    MultiCenterCounts counts;

    explicit MultiCenterOutputs(int count)
            : p20(count), upper(count), high(count), coarse(count, 0),
              highestReentryY(static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS),
              p19Pass(count, 0) {
    }
};

struct CenterChunk {
    int x;
    int z;
};

int benchmarkMultiCenterCoverage(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("multicenterbench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("multicenterbench iterations must be >= 1");
    if (centerSpacingChunks < 15) {
        throw std::runtime_error("center spacing must be >= 15 chunks to avoid overlapping radius-7 windows");
    }

    // Prefixes are intentionally useful layouts:
    // 1 = spawn only.
    // 2 = two adjacent windows.
    // 4 = compact 2x2 block.
    // 8 = the 2x2 block plus four windows around its west/south edges.
    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<int> centerCounts = {1, 2, 4, 8};

    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    auto uploadSeeds = [&]() {
        HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
    };

    auto evaluateCenter = [&](int coarseOffsetX, int coarseOffsetZ, bool loadTerrainCache,
                              MultiCenterOutputs& out) {
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                nullptr, nullptr,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ, loadTerrainCache ? 1 : 0, 0);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, 1,
                terrainCache.permutations, terrainCache.offsets,
                nullptr, nullptr,
                shapeCache.d5, shapeCache.d7, 1,
                coarseOffsetX, coarseOffsetZ);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ);
        HIP_CHECK(hipGetLastError());

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) coarseSeeds[j] = seeds[coarseIndices[offset + j]];
            runCoarseScoresAt(coarseBuffers, coarseSeeds.data(), chunk, coarseScores.data(),
                    coarseOffsetX, coarseOffsetZ);
            for (int j = 0; j < chunk; ++j) {
                const int seedIndex = coarseIndices[offset + j];
                out.coarse[seedIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    std::vector<MultiCenterOutputs> outputs;
    outputs.reserve(centers.size());
    for (std::size_t i = 0; i < centers.size(); ++i) outputs.emplace_back(count);
    MultiCenterOutputs standalone(count);

    auto runCenters = [&](int centerCount) {
        const auto begin = std::chrono::steady_clock::now();
        uploadSeeds();
        for (int i = 0; i < centerCount; ++i) {
            const int coarseOffsetX = centers[i].x * 4;
            const int coarseOffsetZ = centers[i].z * 4;
            evaluateCenter(coarseOffsetX, coarseOffsetZ, i != 0, outputs[i]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    // Warm each exact path once.
    for (int centerCount : centerCounts) (void) runCenters(centerCount);

    std::vector<double> totalMs(centerCounts.size(), 0.0);
    // Rotate benchmark order to reduce thermal/order bias.
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(centerCounts.size());
        for (int j = 0; j < static_cast<int>(centerCounts.size()); ++j) {
            const int index = (rotation + j) % static_cast<int>(centerCounts.size());
            totalMs[index] += runCenters(centerCounts[index]);
        }
    }
    for (double& ms : totalMs) ms /= static_cast<double>(iterations);

    // Refresh the complete eight-center output, then prove every reused center
    // against a standalone path that rebuilds all 66 terrain states.
    (void) runCenters(8);
    std::vector<std::uint64_t> exactnessDiffs(centers.size(), 0);
    for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
        uploadSeeds();
        evaluateCenter(centers[centerIndex].x * 4, centers[centerIndex].z * 4, false, standalone);
        const MultiCenterOutputs& reused = outputs[centerIndex];
        std::uint64_t diffs = 0;
        for (int i = 0; i < count; ++i) {
            diffs += standalone.p20[i] != reused.p20[i];
            diffs += standalone.upper[i] != reused.upper[i];
            diffs += standalone.high[i] != reused.high[i];
            diffs += standalone.p19Pass[i] != reused.p19Pass[i];
            diffs += standalone.coarse[i] != reused.coarse[i];
        }
        for (std::size_t i = 0; i < standalone.highestReentryY.size(); ++i) {
            diffs += standalone.highestReentryY[i] != reused.highestReentryY[i];
        }
        exactnessDiffs[centerIndex] = diffs;
    }

    struct CoverageSummary {
        std::uint64_t coarse85Occurrences = 0;
        std::uint64_t uniqueWorlds = 0;
        std::uint64_t multiHitWorlds = 0;
    };
    std::vector<CoverageSummary> coverage;
    coverage.reserve(centerCounts.size());
    for (int centerCount : centerCounts) {
        CoverageSummary summary;
        for (int centerIndex = 0; centerIndex < centerCount; ++centerIndex) {
            summary.coarse85Occurrences += outputs[centerIndex].counts.coarsePassed85;
        }
        for (int seedIndex = 0; seedIndex < count; ++seedIndex) {
            int hits = 0;
            for (int centerIndex = 0; centerIndex < centerCount; ++centerIndex) {
                if (outputs[centerIndex].coarse[seedIndex] >= 85) ++hits;
            }
            if (hits > 0) ++summary.uniqueWorlds;
            if (hits > 1) ++summary.multiHitWorlds;
        }
        coverage.push_back(summary);
    }

    const double baseWindowsPerSecond = static_cast<double>(count) * 1000.0 / totalMs[0];
    std::vector<double> windowsPerSecond(centerCounts.size());
    std::vector<double> worldsPerSecond(centerCounts.size());
    std::vector<double> gains(centerCounts.size());
    for (std::size_t i = 0; i < centerCounts.size(); ++i) {
        worldsPerSecond[i] = static_cast<double>(count) * 1000.0 / totalMs[i];
        windowsPerSecond[i] = worldsPerSecond[i] * static_cast<double>(centerCounts[i]);
        gains[i] = (windowsPerSecond[i] / baseWindowsPerSecond - 1.0) * 100.0;
    }

    auto printCounts = [&](int index, const MultiCenterCounts& c) {
        const CenterChunk center = centers[index];
        std::cout << "  center " << index << " chunk (" << std::setw(3) << center.x << ","
                  << std::setw(3) << center.z << ")"
                  << " P20=" << c.p20Passed
                  << " Stage1=" << c.stage1Passed
                  << " Stage0.5=" << c.stage05Passed
                  << " P19=" << c.p19Passed
                  << " CoarseEval=" << c.coarseEvaluated
                  << " Coarse85=" << c.coarsePassed85 << "\n";
    };

    std::cout << "P26 multi-center coverage benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic seeds:     " << count << "\n"
              << "  sequence seed:           " << sequenceSeed << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  tested center counts:    1, 2, 4, 8\n"
              << "  later terrain states:    reload P24 cache (no Fisher-Yates rebuild)\n"
              << "  exact coarse:            real offset density grids, no tiling approximation\n\n";

    std::cout << "Center layout and survivor counts\n";
    for (int i = 0; i < static_cast<int>(centers.size()); ++i) printCounts(i, outputs[i].counts);

    std::cout << "\nCoverage throughput\n"
              << "  centers   wall ms/batch   worlds/s   windows/s    gain vs 1    unique C85   C85 occurrences   multi-hit worlds\n";
    for (std::size_t i = 0; i < centerCounts.size(); ++i) {
        std::cout << std::fixed
                  << "  " << std::setw(7) << centerCounts[i]
                  << "   " << std::setw(13) << std::setprecision(3) << totalMs[i]
                  << "   " << std::setw(8) << std::setprecision(1) << worldsPerSecond[i]
                  << "   " << std::setw(9) << windowsPerSecond[i]
                  << "   " << std::showpos << std::setw(9) << gains[i] << std::noshowpos << "%"
                  << "   " << std::setw(10) << coverage[i].uniqueWorlds
                  << "   " << std::setw(15) << coverage[i].coarse85Occurrences
                  << "   " << coverage[i].multiHitWorlds << "\n";
    }

    std::cout << "\nMarginal added-center groups\n";
    for (std::size_t i = 1; i < centerCounts.size(); ++i) {
        const int addedCenters = centerCounts[i] - centerCounts[i - 1];
        const double addedMs = totalMs[i] - totalMs[i - 1];
        const double addedWindowsPerSecond = addedMs > 0.0
                ? static_cast<double>(count) * static_cast<double>(addedCenters) * 1000.0 / addedMs
                : std::numeric_limits<double>::infinity();
        std::cout << "  " << centerCounts[i - 1] << " -> " << centerCounts[i]
                  << ": added wall=" << std::fixed << std::setprecision(3) << addedMs
                  << " ms, marginal windows/s=" << std::setprecision(1) << addedWindowsPerSecond << "\n";
    }

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t i = 0; i < exactnessDiffs.size(); ++i) {
        totalDiffs += exactnessDiffs[i];
        std::cout << "  center " << i << " standalone/cache diffs: " << exactnessDiffs[i] << "\n";
    }
    std::cout << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT reuse path for all eight centers.\n"
                    : "  RESULT: MISMATCH - do not integrate.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}

enum class P28CacheMode {
    BASELINE,
    CLIMATE_ONLY,
    FULL
};

const char* p28ModeName(P28CacheMode mode) {
    switch (mode) {
        case P28CacheMode::BASELINE: return "P27 baseline";
        case P28CacheMode::CLIMATE_ONLY: return "climate cache";
        case P28CacheMode::FULL: return "climate + coarse cache";
    }
    return "unknown";
}

int benchmarkP28CacheReuse(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p28cachebench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p28cachebench iterations must be >= 1");
    if (centerSpacingChunks < 15) {
        throw std::runtime_error("center spacing must be >= 15 chunks");
    }

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<P28CacheMode> modes = {
        P28CacheMode::BASELINE,
        P28CacheMode::CLIMATE_ONLY,
        P28CacheMode::FULL
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    std::vector<std::vector<MultiCenterOutputs>> outputs;
    outputs.reserve(modes.size());
    for (std::size_t m = 0; m < modes.size(); ++m) {
        outputs.emplace_back();
        outputs.back().reserve(centers.size());
        for (std::size_t c = 0; c < centers.size(); ++c) outputs.back().emplace_back(count);
    }

    auto uploadSeeds = [&]() {
        HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
    };

    auto evaluateCenter = [&](P28CacheMode mode, int centerIndex, MultiCenterOutputs& out) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        const bool loadTerrainCache = centerIndex != 0;
        const bool useClimateCache = mode != P28CacheMode::BASELINE;
        const bool loadClimateCache = useClimateCache && centerIndex != 0;
        const bool useCachedCoarse = mode == P28CacheMode::FULL;

        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                useClimateCache ? climateCache.permutations : nullptr,
                useClimateCache ? climateCache.offsets : nullptr,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ,
                loadTerrainCache ? 1 : 0,
                loadClimateCache ? 1 : 0);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, 1,
                terrainCache.permutations, terrainCache.offsets,
                useClimateCache ? climateCache.permutations : nullptr,
                useClimateCache ? climateCache.offsets : nullptr,
                shapeCache.d5, shapeCache.d7, 1,
                coarseOffsetX, coarseOffsetZ);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ);
        HIP_CHECK(hipGetLastError());

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                coarseSeeds[j] = seeds[originalIndex];
                coarseCacheSeedIndices[j] = originalIndex;
            }
            if (useCachedCoarse) {
                runCoarseScoresCachedAt(coarseBuffers, coarseSeeds.data(), coarseCacheSeedIndices.data(),
                        chunk, coarseScores.data(), coarseOffsetX, coarseOffsetZ,
                        terrainCache, climateCache);
            } else {
                runCoarseScoresAt(coarseBuffers, coarseSeeds.data(), chunk, coarseScores.data(),
                        coarseOffsetX, coarseOffsetZ);
            }
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                out.coarse[originalIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    auto runMode = [&](int modeIndex) {
        const auto begin = std::chrono::steady_clock::now();
        uploadSeeds();
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            evaluateCenter(modes[modeIndex], centerIndex, outputs[modeIndex][centerIndex]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<double> totalMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            totalMs[modeIndex] += runMode(modeIndex);
        }
    }
    for (double& value : totalMs) value /= static_cast<double>(iterations);

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<std::uint64_t> exactnessDiffs(modes.size(), 0);
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::uint64_t diffs = 0;
        for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
            const MultiCenterOutputs& expected = outputs[0][centerIndex];
            const MultiCenterOutputs& actual = outputs[modeIndex][centerIndex];
            for (int i = 0; i < count; ++i) {
                diffs += expected.p20[i] != actual.p20[i];
                diffs += expected.upper[i] != actual.upper[i];
                diffs += expected.high[i] != actual.high[i];
                diffs += expected.p19Pass[i] != actual.p19Pass[i];
                diffs += expected.coarse[i] != actual.coarse[i];
            }
            for (std::size_t i = 0; i < expected.highestReentryY.size(); ++i) {
                diffs += expected.highestReentryY[i] != actual.highestReentryY[i];
            }
        }
        exactnessDiffs[modeIndex] = diffs;
    }

    const double regions = static_cast<double>(count) * static_cast<double>(centers.size());
    const double baselineRate = regions * 1000.0 / totalMs[0];
    std::cout << "P28 exact cache-reuse benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  total regions/batch:     " << static_cast<std::uint64_t>(regions) << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  climate cache size:      " << std::fixed << std::setprecision(1)
              << (climatecache::totalBytesForCapacity(count) / (1024.0 * 1024.0)) << " MiB\n"
              << "  cached coarse:           reloads original world-indexed 66 terrain + 10 climate states\n\n";

    std::cout << "Throughput\n"
              << "  mode                         wall ms/batch   worlds/s   regions/s   gain vs P27\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double worldsPerSecond = static_cast<double>(count) * 1000.0 / totalMs[modeIndex];
        const double regionsPerSecond = regions * 1000.0 / totalMs[modeIndex];
        const double gain = (regionsPerSecond / baselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p28ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << totalMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(8) << worldsPerSecond
                  << "   " << std::setw(9) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nSurvivor consistency (baseline center totals)\n";
    MultiCenterCounts totals{};
    for (const MultiCenterOutputs& center : outputs[0]) {
        totals.p20Passed += center.counts.p20Passed;
        totals.stage1Passed += center.counts.stage1Passed;
        totals.stage05Passed += center.counts.stage05Passed;
        totals.p19Passed += center.counts.p19Passed;
        totals.coarseEvaluated += center.counts.coarseEvaluated;
        totals.coarsePassed85 += center.counts.coarsePassed85;
    }
    std::cout << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19=" << totals.p19Passed
              << " CoarseEval=" << totals.coarseEvaluated
              << " Coarse85=" << totals.coarsePassed85 << "\n";

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        totalDiffs += exactnessDiffs[modeIndex];
        std::cout << "  " << p28ModeName(modes[modeIndex])
                  << " diffs vs P27: " << exactnessDiffs[modeIndex] << "\n";
    }
    std::cout << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT cache reuse path.\n"
                    : "  RESULT: MISMATCH - do not integrate P28.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}

struct P29CenterWork {
    int coarseOffsetX = 0;
    int coarseOffsetZ = 0;
    std::vector<int> coarseIndices;
};

int benchmarkP29SelectorCoarse(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p29coarsebench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p29coarsebench iterations must be >= 1");
    if (centerSpacingChunks < 15) throw std::runtime_error("center spacing must be >= 15 chunks");

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);
    std::uint32_t* dNoise2Masks = nullptr;
    std::uint32_t* dNoise3Masks = nullptr;
    unsigned int* dSelectorStats = nullptr;
    const std::size_t maskValues = static_cast<std::size_t>(coarseChunkCapacity) * coarsecore::COLUMNS;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dNoise2Masks), maskValues * sizeof(std::uint32_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dNoise3Masks), maskValues * sizeof(std::uint32_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSelectorStats),
            static_cast<std::size_t>(coarseChunkCapacity) * p29coarse::SELECTOR_STAT_COUNT * sizeof(unsigned int)));

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

    std::vector<MultiCenterOutputs> outputs;
    outputs.reserve(centers.size());
    for (std::size_t i = 0; i < centers.size(); ++i) outputs.emplace_back(count);
    std::vector<P29CenterWork> work(centers.size());
    MultiCenterCounts totals{};

    // Build the exact P28 caches and identify the real production coarse
    // survivors once. Both coarse modes below receive this identical workload.
    for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
        MultiCenterOutputs& out = outputs[centerIndex];
        P29CenterWork& centerWork = work[centerIndex];
        centerWork.coarseOffsetX = centers[centerIndex].x * 4;
        centerWork.coarseOffsetZ = centers[centerIndex].z * 4;
        centerWork.coarseIndices.reserve(static_cast<std::size_t>(count) / 64 + 64);
        const bool loadCache = centerIndex != 0;

        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                centerWork.coarseOffsetX, centerWork.coarseOffsetZ,
                loadCache ? 1 : 0, loadCache ? 1 : 0);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, 1,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                shapeCache.d5, shapeCache.d7, 1,
                centerWork.coarseOffsetX, centerWork.coarseOffsetZ);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                centerWork.coarseOffsetX, centerWork.coarseOffsetZ);
        HIP_CHECK(hipGetLastError());

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS, hipMemcpyDeviceToHost));

        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            if (pass && !megaReject) {
                out.p19Pass[i] = 1u;
                ++out.counts.p19Passed;
                centerWork.coarseIndices.push_back(i);
            }
        }
        out.counts.coarseEvaluated = centerWork.coarseIndices.size();
        totals.p20Passed += out.counts.p20Passed;
        totals.stage1Passed += out.counts.stage1Passed;
        totals.stage05Passed += out.counts.stage05Passed;
        totals.p19Passed += out.counts.p19Passed;
        totals.coarseEvaluated += out.counts.coarseEvaluated;
    }

    if (totals.coarseEvaluated == 0) throw std::runtime_error("deterministic batch produced no coarse survivors");

    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    auto gatherChunk = [&](const P29CenterWork& centerWork, std::size_t offset, int chunk) {
        for (int j = 0; j < chunk; ++j) {
            const int originalIndex = centerWork.coarseIndices[offset + j];
            coarseSeeds[j] = seeds[originalIndex];
            coarseCacheSeedIndices[j] = originalIndex;
        }
    };

    auto runCoarseSet = [&](bool selectorFirst) {
        const auto begin = std::chrono::steady_clock::now();
        for (const P29CenterWork& centerWork : work) {
            for (std::size_t offset = 0; offset < centerWork.coarseIndices.size(); offset += coarseChunkCapacity) {
                const int chunk = static_cast<int>(std::min<std::size_t>(
                        coarseChunkCapacity, centerWork.coarseIndices.size() - offset));
                gatherChunk(centerWork, offset, chunk);
                if (selectorFirst) {
                    runCoarseScoresCachedSelectorAt(coarseBuffers,
                            coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                            coarseScores.data(), centerWork.coarseOffsetX, centerWork.coarseOffsetZ,
                            terrainCache, climateCache, dNoise2Masks, dNoise3Masks, nullptr);
                } else {
                    runCoarseScoresCachedAt(coarseBuffers,
                            coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                            coarseScores.data(), centerWork.coarseOffsetX, centerWork.coarseOffsetZ,
                            terrainCache, climateCache);
                }
            }
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    (void) runCoarseSet(false);
    (void) runCoarseSet(true);

    double baselineMs = 0.0;
    double selectorMs = 0.0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        if ((iteration & 1) == 0) {
            baselineMs += runCoarseSet(false);
            selectorMs += runCoarseSet(true);
        } else {
            selectorMs += runCoarseSet(true);
            baselineMs += runCoarseSet(false);
        }
    }
    baselineMs /= static_cast<double>(iterations);
    selectorMs /= static_cast<double>(iterations);

    // Strong exactness test: compare every sign cell as well as the final
    // connected-component score for every actual coarse survivor.
    std::uint64_t scoreDiffs = 0;
    std::uint64_t signDiffs = 0;
    std::uint64_t baselineCoarse85 = 0;
    std::uint64_t selectorCoarse85 = 0;
    std::vector<int> expectedScores(coarseChunkCapacity);
    std::vector<int> actualScores(coarseChunkCapacity);
    std::vector<unsigned char> expectedSigns(
            static_cast<std::size_t>(coarseChunkCapacity) * coarsecore::CELLS);

    for (const P29CenterWork& centerWork : work) {
        for (std::size_t offset = 0; offset < centerWork.coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, centerWork.coarseIndices.size() - offset));
            gatherChunk(centerWork, offset, chunk);
            runCoarseScoresCachedAt(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    expectedScores.data(), centerWork.coarseOffsetX, centerWork.coarseOffsetZ,
                    terrainCache, climateCache);
            const std::size_t signBytes = static_cast<std::size_t>(chunk) * coarsecore::CELLS;
            HIP_CHECK(hipMemcpy(expectedSigns.data(), coarseBuffers.signs, signBytes, hipMemcpyDeviceToHost));

            runCoarseScoresCachedSelectorAt(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    actualScores.data(), centerWork.coarseOffsetX, centerWork.coarseOffsetZ,
                    terrainCache, climateCache, dNoise2Masks, dNoise3Masks, nullptr);
            for (int j = 0; j < chunk; ++j) {
                scoreDiffs += expectedScores[j] != actualScores[j];
                baselineCoarse85 += expectedScores[j] >= 85;
                selectorCoarse85 += actualScores[j] >= 85;
            }
            std::vector<unsigned char> actualSigns(signBytes);
            HIP_CHECK(hipMemcpy(actualSigns.data(), coarseBuffers.signs, signBytes, hipMemcpyDeviceToHost));
            for (std::size_t i = 0; i < signBytes; ++i) signDiffs += expectedSigns[i] != actualSigns[i];
        }
    }

    // Run once with counters enabled. Timings above do not include this
    // instrumentation overhead.
    std::uint64_t selectorTotals[p29coarse::SELECTOR_STAT_COUNT] = {};
    std::vector<unsigned int> hostStats(
            static_cast<std::size_t>(coarseChunkCapacity) * p29coarse::SELECTOR_STAT_COUNT);
    for (const P29CenterWork& centerWork : work) {
        for (std::size_t offset = 0; offset < centerWork.coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, centerWork.coarseIndices.size() - offset));
            gatherChunk(centerWork, offset, chunk);
            runCoarseScoresCachedSelectorAt(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    coarseScores.data(), centerWork.coarseOffsetX, centerWork.coarseOffsetZ,
                    terrainCache, climateCache, dNoise2Masks, dNoise3Masks, dSelectorStats);
            HIP_CHECK(hipMemcpy(hostStats.data(), dSelectorStats,
                    static_cast<std::size_t>(chunk) * p29coarse::SELECTOR_STAT_COUNT * sizeof(unsigned int),
                    hipMemcpyDeviceToHost));
            for (int j = 0; j < chunk; ++j) {
                for (int stat = 0; stat < p29coarse::SELECTOR_STAT_COUNT; ++stat) {
                    selectorTotals[stat] += hostStats[static_cast<std::size_t>(j) * p29coarse::SELECTOR_STAT_COUNT + stat];
                }
            }
        }
    }

    const double evaluations = static_cast<double>(totals.coarseEvaluated);
    const double baselineRate = evaluations * 1000.0 / baselineMs;
    const double selectorRate = evaluations * 1000.0 / selectorMs;
    const double coarseGain = (selectorRate / baselineRate - 1.0) * 100.0;
    const double speedFactor = baselineMs / selectorMs;
    const std::uint64_t totalCells = selectorTotals[p29coarse::CELL_NOISE2_ONLY]
            + selectorTotals[p29coarse::CELL_NOISE3_ONLY]
            + selectorTotals[p29coarse::CELL_BOTH];
    const std::uint64_t totalColumns = selectorTotals[p29coarse::COLUMN_NOISE2_ONLY]
            + selectorTotals[p29coarse::COLUMN_NOISE3_ONLY]
            + selectorTotals[p29coarse::COLUMN_BOTH];
    auto percent = [](std::uint64_t value, std::uint64_t total) {
        return total == 0 ? 0.0 : static_cast<double>(value) * 100.0 / static_cast<double>(total);
    };
    auto projectedGain = [&](double coarseShare) {
        const double newTime = (1.0 - coarseShare) + coarseShare / speedFactor;
        return (1.0 / newTime - 1.0) * 100.0;
    };

    std::cout << "P29 selector-first exact coarse benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  actual coarse survivors: " << totals.coarseEvaluated << "\n"
              << "  selector timing counters:OFF during benchmark\n\n";

    std::cout << "Upstream survivor totals\n"
              << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19/CoarseEval=" << totals.p19Passed << "\n\n";

    std::cout << "Exact coarse throughput (same survivors, includes transfers + scoring)\n"
              << "  mode                         wall ms/set   coarse eval/s   gain\n"
              << "  P28 cached coarse          " << std::fixed << std::setprecision(3) << std::setw(13) << baselineMs
              << "   " << std::setprecision(1) << std::setw(13) << baselineRate << "   +0.0%\n"
              << "  selector-first masked      " << std::setprecision(3) << std::setw(13) << selectorMs
              << "   " << std::setprecision(1) << std::setw(13) << selectorRate
              << "   " << std::showpos << std::setprecision(1) << coarseGain << std::noshowpos << "%\n";

    std::cout << "\nnoise1 selector distribution\n"
              << "  cells noise2-only:         " << selectorTotals[p29coarse::CELL_NOISE2_ONLY]
              << " (" << std::fixed << std::setprecision(2)
              << percent(selectorTotals[p29coarse::CELL_NOISE2_ONLY], totalCells) << "%)\n"
              << "  cells noise3-only:         " << selectorTotals[p29coarse::CELL_NOISE3_ONLY]
              << " (" << percent(selectorTotals[p29coarse::CELL_NOISE3_ONLY], totalCells) << "%)\n"
              << "  cells requiring both:      " << selectorTotals[p29coarse::CELL_BOTH]
              << " (" << percent(selectorTotals[p29coarse::CELL_BOTH], totalCells) << "%)\n"
              << "  columns noise2-only:       " << selectorTotals[p29coarse::COLUMN_NOISE2_ONLY]
              << " (" << percent(selectorTotals[p29coarse::COLUMN_NOISE2_ONLY], totalColumns) << "%)\n"
              << "  columns noise3-only:       " << selectorTotals[p29coarse::COLUMN_NOISE3_ONLY]
              << " (" << percent(selectorTotals[p29coarse::COLUMN_NOISE3_ONLY], totalColumns) << "%)\n"
              << "  columns needing both maps: " << selectorTotals[p29coarse::COLUMN_BOTH]
              << " (" << percent(selectorTotals[p29coarse::COLUMN_BOTH], totalColumns) << "%)\n"
              << "  avoidable map-cell values: "
              << (selectorTotals[p29coarse::CELL_NOISE2_ONLY] + selectorTotals[p29coarse::CELL_NOISE3_ONLY])
              << " / " << (totalCells * 2ULL) << " ("
              << percent(selectorTotals[p29coarse::CELL_NOISE2_ONLY]
                         + selectorTotals[p29coarse::CELL_NOISE3_ONLY], totalCells * 2ULL) << "%)\n";

    std::cout << "\nProjected whole-pipeline gain if exact coarse is the stated share\n"
              << "  at 20% coarse share:       " << std::showpos << std::setprecision(2) << projectedGain(0.20) << std::noshowpos << "%\n"
              << "  at 22% coarse share:       " << std::showpos << projectedGain(0.22) << std::noshowpos << "%\n"
              << "  at 25% coarse share:       " << std::showpos << projectedGain(0.25) << std::noshowpos << "%\n";

    const std::uint64_t totalDiffs = scoreDiffs + signDiffs;
    std::cout << "\nExactness checks\n"
              << "  sign-cell diffs:           " << signDiffs << "\n"
              << "  coarse-score diffs:        " << scoreDiffs << "\n"
              << "  P28 Coarse85:              " << baselineCoarse85 << "\n"
              << "  P29 Coarse85:              " << selectorCoarse85 << "\n"
              << "  total diffs:               " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT selector-first path.\n"
                    : "  RESULT: MISMATCH - do not integrate P29.\n");

    hipFree(dNoise2Masks);
    hipFree(dNoise3Masks);
    hipFree(dSelectorStats);
    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}


enum class P30LowerMode {
    BASELINE_256,
    COMPACT_32,
    COMPACT_64
};

const char* p30ModeName(P30LowerMode mode) {
    switch (mode) {
        case P30LowerMode::BASELINE_256: return "P28 baseline 256-thread";
        case P30LowerMode::COMPACT_32: return "compact 32-thread";
        case P30LowerMode::COMPACT_64: return "compact 64-thread";
    }
    return "unknown";
}

int benchmarkP30CompactLower(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p30lowerbench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p30lowerbench iterations must be >= 1");
    if (centerSpacingChunks < 15) throw std::runtime_error("center spacing must be >= 15 chunks");

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<P30LowerMode> modes = {
        P30LowerMode::BASELINE_256,
        P30LowerMode::COMPACT_32,
        P30LowerMode::COMPACT_64
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    int* dActiveCounts = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dActiveCounts), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    std::vector<std::vector<MultiCenterOutputs>> outputs;
    outputs.reserve(modes.size());
    for (std::size_t m = 0; m < modes.size(); ++m) {
        outputs.emplace_back();
        outputs.back().reserve(centers.size());
        for (std::size_t c = 0; c < centers.size(); ++c) outputs.back().emplace_back(count);
    }

    auto launchLower = [&](P30LowerMode mode, int coarseOffsetX, int coarseOffsetZ, bool collectActiveCounts) {
        if (mode == P30LowerMode::BASELINE_256) {
            hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel,
                    dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                    dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                    dHigh, dHighestReentryY, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        } else if (mode == P30LowerMode::COMPACT_32) {
            hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<32>,
                    dim3(count), dim3(32), 0, 0,
                    dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                    dHigh, dHighestReentryY, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ,
                    collectActiveCounts ? dActiveCounts : nullptr);
        } else {
            hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<64>,
                    dim3(count), dim3(64), 0, 0,
                    dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                    dHigh, dHighestReentryY, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ,
                    collectActiveCounts ? dActiveCounts : nullptr);
        }
        HIP_CHECK(hipGetLastError());
    };

    auto prepareUpper = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        const bool loadCache = centerIndex != 0;
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ,
                loadCache ? 1 : 0, loadCache ? 1 : 0);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds,
                dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, 1,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                shapeCache.d5, shapeCache.d7, 1,
                coarseOffsetX, coarseOffsetZ);
        HIP_CHECK(hipGetLastError());
    };

    auto evaluateCenter = [&](P30LowerMode mode, int centerIndex, MultiCenterOutputs& out) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        prepareUpper(centerIndex);
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        launchLower(mode, coarseOffsetX, coarseOffsetZ, false);

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                coarseSeeds[j] = seeds[originalIndex];
                coarseCacheSeedIndices[j] = originalIndex;
            }
            runCoarseScoresCachedAt(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    coarseScores.data(), coarseOffsetX, coarseOffsetZ,
                    terrainCache, climateCache);
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                out.coarse[originalIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    auto runMode = [&](int modeIndex) {
        const auto begin = std::chrono::steady_clock::now();
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            evaluateCenter(modes[modeIndex], centerIndex, outputs[modeIndex][centerIndex]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<double> fullMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            fullMs[modeIndex] += runMode(modeIndex);
        }
    }
    for (double& value : fullMs) value /= static_cast<double>(iterations);

    // Isolate the lower kernel on the same exact upper masks and shape caches.
    std::vector<double> lowerOnlyMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            prepareUpper(centerIndex);
            HIP_CHECK(hipDeviceSynchronize());
            const int coarseOffsetX = centers[centerIndex].x * 4;
            const int coarseOffsetZ = centers[centerIndex].z * 4;
            const int rotation = (iteration + centerIndex) % static_cast<int>(modes.size());
            for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
                const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
                HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
                HIP_CHECK(hipDeviceSynchronize());
                const auto begin = std::chrono::steady_clock::now();
                launchLower(modes[modeIndex], coarseOffsetX, coarseOffsetZ, false);
                HIP_CHECK(hipDeviceSynchronize());
                lowerOnlyMs[modeIndex] += hostElapsedMs(begin, std::chrono::steady_clock::now());
            }
        }
    }
    for (double& value : lowerOnlyMs) value /= static_cast<double>(iterations);

    // Refresh outputs after timing for a clean exactness comparison.
    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<std::uint64_t> exactnessDiffs(modes.size(), 0);
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::uint64_t diffs = 0;
        for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
            const MultiCenterOutputs& expected = outputs[0][centerIndex];
            const MultiCenterOutputs& actual = outputs[modeIndex][centerIndex];
            for (int i = 0; i < count; ++i) {
                diffs += expected.p20[i] != actual.p20[i];
                diffs += expected.upper[i] != actual.upper[i];
                diffs += expected.high[i] != actual.high[i];
                diffs += expected.p19Pass[i] != actual.p19Pass[i];
                diffs += expected.coarse[i] != actual.coarse[i];
                if (expected.p20[i] > 0 && expected.upper[i] >= upperMinCount) {
                    const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
                    for (int point = 0; point < stage0gpu::FULL_POINTS; ++point) {
                        diffs += expected.highestReentryY[base + point]
                                != actual.highestReentryY[base + point];
                    }
                }
            }
        }
        exactnessDiffs[modeIndex] = diffs;
    }

    MultiCenterCounts totals{};
    std::vector<int> activeColumns;
    for (const MultiCenterOutputs& center : outputs[0]) {
        totals.p20Passed += center.counts.p20Passed;
        totals.stage1Passed += center.counts.stage1Passed;
        totals.stage05Passed += center.counts.stage05Passed;
        totals.p19Passed += center.counts.p19Passed;
        totals.coarseEvaluated += center.counts.coarseEvaluated;
        totals.coarsePassed85 += center.counts.coarsePassed85;
        for (int i = 0; i < count; ++i) {
            if (center.p20[i] > 0 && center.upper[i] >= upperMinCount) {
                activeColumns.push_back(center.upper[i]);
            }
        }
    }
    std::sort(activeColumns.begin(), activeColumns.end());
    auto percentile = [&](double p) {
        if (activeColumns.empty()) return 0;
        const std::size_t index = static_cast<std::size_t>(p * static_cast<double>(activeColumns.size() - 1));
        return activeColumns[index];
    };
    std::uint64_t activeTotal = 0;
    std::uint64_t atMost32 = 0;
    std::uint64_t atMost64 = 0;
    std::uint64_t atMost128 = 0;
    for (int value : activeColumns) {
        activeTotal += static_cast<std::uint64_t>(value);
        atMost32 += value <= 32;
        atMost64 += value <= 64;
        atMost128 += value <= 128;
    }
    const double activeMean = activeColumns.empty() ? 0.0
            : static_cast<double>(activeTotal) / static_cast<double>(activeColumns.size());
    auto pctCount = [&](std::uint64_t value) {
        return activeColumns.empty() ? 0.0
                : static_cast<double>(value) * 100.0 / static_cast<double>(activeColumns.size());
    };

    const double regions = static_cast<double>(count) * static_cast<double>(centers.size());
    const double baselineRate = regions * 1000.0 / fullMs[0];
    const double lowerBaselineRate = regions * 1000.0 / lowerOnlyMs[0];

    std::cout << "P30 compact lower-stage benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  total regions/batch:     " << static_cast<std::uint64_t>(regions) << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  production baseline:     P28 ST0R2808 exact caches\n"
              << "  compact work:            dense active-column batches, no filter changes\n\n";

    std::cout << "Whole P28 pipeline throughput\n"
              << "  mode                         wall ms/batch   worlds/s   regions/s   gain vs P28\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double worldsPerSecond = static_cast<double>(count) * 1000.0 / fullMs[modeIndex];
        const double regionsPerSecond = regions * 1000.0 / fullMs[modeIndex];
        const double gain = (regionsPerSecond / baselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p30ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << fullMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(8) << worldsPerSecond
                  << "   " << std::setw(9) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nLower kernel only (all eight centers; upstream excluded)\n"
              << "  mode                         wall ms/set     regions/s   gain vs 256\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double regionsPerSecond = regions * 1000.0 / lowerOnlyMs[modeIndex];
        const double gain = (regionsPerSecond / lowerBaselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p30ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << lowerOnlyMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(11) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nActive lower-column distribution (launched regions only)\n"
              << "  launched lower regions:  " << activeColumns.size() << "\n"
              << "  mean active columns:      " << std::fixed << std::setprecision(2) << activeMean << " / 256\n"
              << "  p50 / p90 / p99 / max:   " << percentile(0.50) << " / " << percentile(0.90)
              << " / " << percentile(0.99) << " / " << (activeColumns.empty() ? 0 : activeColumns.back()) << "\n"
              << "  fits one 32-lane batch:   " << atMost32 << " (" << std::setprecision(2) << pctCount(atMost32) << "%)\n"
              << "  fits one 64-lane batch:   " << atMost64 << " (" << pctCount(atMost64) << "%)\n"
              << "  fits <=128 lanes:         " << atMost128 << " (" << pctCount(atMost128) << "%)\n";

    std::cout << "\nSurvivor totals\n"
              << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19=" << totals.p19Passed
              << " CoarseEval=" << totals.coarseEvaluated
              << " Coarse85=" << totals.coarsePassed85 << "\n";

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        totalDiffs += exactnessDiffs[modeIndex];
        std::cout << "  " << p30ModeName(modes[modeIndex])
                  << " diffs vs P28: " << exactnessDiffs[modeIndex] << "\n";
    }
    std::cout << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT compact lower paths.\n"
                    : "  RESULT: MISMATCH - do not integrate P30.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dActiveCounts);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}

enum class P31UpperMode {
    BASELINE_256,
    COMPACT_192
};

const char* p31ModeName(P31UpperMode mode) {
    switch (mode) {
        case P31UpperMode::BASELINE_256: return "P30 baseline upper 256";
        case P31UpperMode::COMPACT_192: return "compact upper 192";
    }
    return "unknown";
}

int benchmarkP31CompactUpper(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p31upperbench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p31upperbench iterations must be >= 1");
    if (centerSpacingChunks < 15) throw std::runtime_error("center spacing must be >= 15 chunks");

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<P31UpperMode> modes = {
        P31UpperMode::BASELINE_256,
        P31UpperMode::COMPACT_192
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    std::vector<std::vector<MultiCenterOutputs>> outputs;
    outputs.reserve(modes.size());
    for (std::size_t m = 0; m < modes.size(); ++m) {
        outputs.emplace_back();
        outputs.back().reserve(centers.size());
        for (std::size_t c = 0; c < centers.size(); ++c) outputs.back().emplace_back(count);
    }

    auto launchP20 = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        const bool loadCache = centerIndex != 0;
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ,
                loadCache ? 1 : 0, loadCache ? 1 : 0);
        HIP_CHECK(hipGetLastError());
    };

    auto launchUpper = [&](P31UpperMode mode, int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        if (mode == P31UpperMode::BASELINE_256) {
            hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds,
                    dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, 1,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7, 1,
                    coarseOffsetX, coarseOffsetZ);
        } else {
            hipLaunchKernelGGL(p31upper::stage0UpperCompact192Kernel,
                    dim3(count), dim3(p31upper::THREADS), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        }
        HIP_CHECK(hipGetLastError());
    };

    auto launchLower = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<32>,
                dim3(count), dim3(32), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ, nullptr);
        HIP_CHECK(hipGetLastError());
    };

    auto evaluateCenter = [&](P31UpperMode mode, int centerIndex, MultiCenterOutputs& out) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        launchP20(centerIndex);
        launchUpper(mode, centerIndex);
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        launchLower(centerIndex);

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                coarseSeeds[j] = seeds[originalIndex];
                coarseCacheSeedIndices[j] = originalIndex;
            }
            runCoarseScoresCachedAt(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    coarseScores.data(), coarseOffsetX, coarseOffsetZ,
                    terrainCache, climateCache);
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                out.coarse[originalIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    auto runMode = [&](int modeIndex) {
        const auto begin = std::chrono::steady_clock::now();
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            evaluateCenter(modes[modeIndex], centerIndex, outputs[modeIndex][centerIndex]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<double> fullMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            fullMs[modeIndex] += runMode(modeIndex);
        }
    }
    for (double& value : fullMs) value /= static_cast<double>(iterations);

    std::vector<double> upperOnlyMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            launchP20(centerIndex);
            HIP_CHECK(hipDeviceSynchronize());
            const int rotation = (iteration + centerIndex) % static_cast<int>(modes.size());
            for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
                const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
                HIP_CHECK(hipDeviceSynchronize());
                const auto begin = std::chrono::steady_clock::now();
                launchUpper(modes[modeIndex], centerIndex);
                HIP_CHECK(hipDeviceSynchronize());
                upperOnlyMs[modeIndex] += hostElapsedMs(begin, std::chrono::steady_clock::now());
            }
        }
    }
    for (double& value : upperOnlyMs) value /= static_cast<double>(iterations);

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<std::uint64_t> exactnessDiffs(modes.size(), 0);
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::uint64_t diffs = 0;
        for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
            const MultiCenterOutputs& expected = outputs[0][centerIndex];
            const MultiCenterOutputs& actual = outputs[modeIndex][centerIndex];
            for (int i = 0; i < count; ++i) {
                diffs += expected.p20[i] != actual.p20[i];
                diffs += expected.upper[i] != actual.upper[i];
                diffs += expected.high[i] != actual.high[i];
                diffs += expected.p19Pass[i] != actual.p19Pass[i];
                diffs += expected.coarse[i] != actual.coarse[i];
                if (expected.p20[i] > 0 && expected.upper[i] >= upperMinCount) {
                    const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
                    for (int point = 0; point < stage0gpu::FULL_POINTS; ++point) {
                        diffs += expected.highestReentryY[base + point]
                                != actual.highestReentryY[base + point];
                    }
                }
            }
        }
        exactnessDiffs[modeIndex] = diffs;
    }

    MultiCenterCounts totals{};
    for (const MultiCenterOutputs& center : outputs[0]) {
        totals.p20Passed += center.counts.p20Passed;
        totals.stage1Passed += center.counts.stage1Passed;
        totals.stage05Passed += center.counts.stage05Passed;
        totals.p19Passed += center.counts.p19Passed;
        totals.coarseEvaluated += center.counts.coarseEvaluated;
        totals.coarsePassed85 += center.counts.coarsePassed85;
    }

    const double regions = static_cast<double>(count) * static_cast<double>(centers.size());
    const double baselineRate = regions * 1000.0 / fullMs[0];
    const double upperBaselineRate = regions * 1000.0 / upperOnlyMs[0];

    std::cout << "P31 compact upper-stage benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  total regions/batch:     " << static_cast<std::uint64_t>(regions) << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  production baseline:     P30 compact lower 32 + P28 exact caches\n"
              << "  compact upper work:      192 non-P20 columns; cached states; exact math\n\n";

    std::cout << "Whole P30 pipeline throughput\n"
              << "  mode                         wall ms/batch   worlds/s   regions/s   gain vs P30\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double worldsPerSecond = static_cast<double>(count) * 1000.0 / fullMs[modeIndex];
        const double regionsPerSecond = regions * 1000.0 / fullMs[modeIndex];
        const double gain = (regionsPerSecond / baselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p31ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << fullMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(8) << worldsPerSecond
                  << "   " << std::setw(9) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nUpper kernel only (all eight centers; P20/lower/coarse excluded)\n"
              << "  mode                         wall ms/set     regions/s   gain vs 256\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double regionsPerSecond = regions * 1000.0 / upperOnlyMs[modeIndex];
        const double gain = (regionsPerSecond / upperBaselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p31ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << upperOnlyMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(11) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nWork removed\n"
              << "  P20 columns reused:       64 / 256\n"
              << "  upper columns evaluated:  192 / 256\n"
              << "  idle upper lanes removed: 25.0%\n";

    std::cout << "\nSurvivor totals\n"
              << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19=" << totals.p19Passed
              << " CoarseEval=" << totals.coarseEvaluated
              << " Coarse85=" << totals.coarsePassed85 << "\n";

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        totalDiffs += exactnessDiffs[modeIndex];
        std::cout << "  " << p31ModeName(modes[modeIndex])
                  << " diffs vs P30: " << exactnessDiffs[modeIndex] << "\n";
    }
    std::cout << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT compact upper path.\n"
                    : "  RESULT: MISMATCH - do not integrate P31.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}

enum class P33YMode {
    P31_BASELINE,
    UPPER_SHARED_Y,
    P20_AND_UPPER_SHARED_Y
};

const char* p33ModeName(P33YMode mode) {
    switch (mode) {
        case P33YMode::P31_BASELINE: return "P31 baseline";
        case P33YMode::UPPER_SHARED_Y: return "shared Y upper only";
        case P33YMode::P20_AND_UPPER_SHARED_Y: return "shared Y P20 + upper";
    }
    return "unknown";
}

int benchmarkP33SharedYAxis(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p34permbench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p34permbench iterations must be >= 1");
    if (centerSpacingChunks < 15) throw std::runtime_error("center spacing must be >= 15 chunks");

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<P33YMode> modes = {
        P33YMode::P31_BASELINE,
        P33YMode::UPPER_SHARED_Y,
        P33YMode::P20_AND_UPPER_SHARED_Y
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    std::vector<std::vector<MultiCenterOutputs>> outputs;
    outputs.reserve(modes.size());
    for (std::size_t m = 0; m < modes.size(); ++m) {
        outputs.emplace_back();
        outputs.back().reserve(centers.size());
        for (std::size_t c = 0; c < centers.size(); ++c) outputs.back().emplace_back(count);
    }

    auto launchP20 = [&](P33YMode mode, int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        const bool loadCache = centerIndex != 0;
        if (mode == P33YMode::P20_AND_UPPER_SHARED_Y) {
            hipLaunchKernelGGL(p20gpu::p20KernelFromSeedsSharedY,
                    dim3(count), dim3(p20gpu::POINTS), 0, 0,
                    dSeeds, count, dP20, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    dUpperMasks, shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ,
                    loadCache ? 1 : 0, loadCache ? 1 : 0);
        } else {
            hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds,
                    dim3(count), dim3(p20gpu::POINTS), 0, 0,
                    dSeeds, count, dP20, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    dUpperMasks, shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ,
                    loadCache ? 1 : 0, loadCache ? 1 : 0);
        }
        HIP_CHECK(hipGetLastError());
    };

    auto launchUpper = [&](P33YMode mode, int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        if (mode == P33YMode::P31_BASELINE) {
            hipLaunchKernelGGL(p31upper::stage0UpperCompact192Kernel,
                    dim3(count), dim3(p31upper::THREADS), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        } else {
            hipLaunchKernelGGL(p33y::stage0UpperCompact192SharedYKernel,
                    dim3(count), dim3(p33y::THREADS), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        }
        HIP_CHECK(hipGetLastError());
    };

    auto launchLower = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<32>,
                dim3(count), dim3(32), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ, nullptr);
        HIP_CHECK(hipGetLastError());
    };

    auto evaluateCenter = [&](P33YMode mode, int centerIndex, MultiCenterOutputs& out) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        launchP20(mode, centerIndex);
        launchUpper(mode, centerIndex);
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        launchLower(centerIndex);

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                coarseSeeds[j] = seeds[originalIndex];
                coarseCacheSeedIndices[j] = originalIndex;
            }
            runCoarseScoresCachedAt(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    coarseScores.data(), coarseOffsetX, coarseOffsetZ,
                    terrainCache, climateCache);
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                out.coarse[originalIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    auto runMode = [&](int modeIndex) {
        const auto begin = std::chrono::steady_clock::now();
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            evaluateCenter(modes[modeIndex], centerIndex, outputs[modeIndex][centerIndex]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<double> fullMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            fullMs[modeIndex] += runMode(modeIndex);
        }
    }
    for (double& value : fullMs) value /= static_cast<double>(iterations);

    std::vector<double> p20OnlyMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            HIP_CHECK(hipDeviceSynchronize());
            const auto begin = std::chrono::steady_clock::now();
            for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
                launchP20(modes[modeIndex], centerIndex);
            }
            HIP_CHECK(hipDeviceSynchronize());
            p20OnlyMs[modeIndex] += hostElapsedMs(begin, std::chrono::steady_clock::now());
        }
    }
    for (double& value : p20OnlyMs) value /= static_cast<double>(iterations);

    std::vector<double> upperOnlyMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            const int rotation = (iteration + centerIndex) % static_cast<int>(modes.size());
            for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
                const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
                launchP20(modes[modeIndex], centerIndex);
                HIP_CHECK(hipDeviceSynchronize());
                const auto begin = std::chrono::steady_clock::now();
                launchUpper(modes[modeIndex], centerIndex);
                HIP_CHECK(hipDeviceSynchronize());
                upperOnlyMs[modeIndex] += hostElapsedMs(begin, std::chrono::steady_clock::now());
            }
        }
    }
    for (double& value : upperOnlyMs) value /= static_cast<double>(iterations);

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<std::uint64_t> exactnessDiffs(modes.size(), 0);
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::uint64_t diffs = 0;
        for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
            const MultiCenterOutputs& expected = outputs[0][centerIndex];
            const MultiCenterOutputs& actual = outputs[modeIndex][centerIndex];
            for (int i = 0; i < count; ++i) {
                diffs += expected.p20[i] != actual.p20[i];
                diffs += expected.upper[i] != actual.upper[i];
                diffs += expected.high[i] != actual.high[i];
                diffs += expected.p19Pass[i] != actual.p19Pass[i];
                diffs += expected.coarse[i] != actual.coarse[i];
                if (expected.p20[i] > 0 && expected.upper[i] >= upperMinCount) {
                    const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
                    for (int point = 0; point < stage0gpu::FULL_POINTS; ++point) {
                        diffs += expected.highestReentryY[base + point]
                                != actual.highestReentryY[base + point];
                    }
                }
            }
        }
        exactnessDiffs[modeIndex] = diffs;
    }

    MultiCenterCounts totals{};
    for (const MultiCenterOutputs& center : outputs[0]) {
        totals.p20Passed += center.counts.p20Passed;
        totals.stage1Passed += center.counts.stage1Passed;
        totals.stage05Passed += center.counts.stage05Passed;
        totals.p19Passed += center.counts.p19Passed;
        totals.coarseEvaluated += center.counts.coarseEvaluated;
        totals.coarsePassed85 += center.counts.coarsePassed85;
    }

    const double regions = static_cast<double>(count) * static_cast<double>(centers.size());
    const double baselineRate = regions * 1000.0 / fullMs[0];
    const double p20BaselineRate = regions * 1000.0 / p20OnlyMs[0];
    const double upperBaselineRate = regions * 1000.0 / upperOnlyMs[0];

    std::cout << "P33 shared Y-axis benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  total regions/batch:     " << static_cast<std::uint64_t>(regions) << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  production baseline:     P31 upper192 + lower32 + exact caches\n"
              << "  shared Y work:           one exact 17-value Y setup per 3D octave/block\n\n";

    std::cout << "Whole P31 pipeline throughput\n"
              << "  mode                         wall ms/batch   worlds/s   regions/s   gain vs P31\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double worldsPerSecond = static_cast<double>(count) * 1000.0 / fullMs[modeIndex];
        const double regionsPerSecond = regions * 1000.0 / fullMs[modeIndex];
        const double gain = (regionsPerSecond / baselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p33ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << fullMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(8) << worldsPerSecond
                  << "   " << std::setw(9) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nP20 kernel only (all eight centers; later stages excluded)\n"
              << "  mode                         wall ms/set     regions/s   gain vs P31\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double regionsPerSecond = regions * 1000.0 / p20OnlyMs[modeIndex];
        const double gain = (regionsPerSecond / p20BaselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p33ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << p20OnlyMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(11) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nUpper kernel only (all eight centers; P20/lower/coarse excluded)\n"
              << "  mode                         wall ms/set     regions/s   gain vs P31\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double regionsPerSecond = regions * 1000.0 / upperOnlyMs[modeIndex];
        const double gain = (regionsPerSecond / upperBaselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p33ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << upperOnlyMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(11) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nExact repeated setup removed\n"
              << "  P20 3D octaves:          40 per region, Y setup shared across 64 lanes\n"
              << "  Upper 3D octaves:        40 per launched region, Y setup shared across 192 lanes\n"
              << "  Y cache per octave:      17 indices + 17 cell starts + 51 doubles\n";

    std::cout << "\nSurvivor totals\n"
              << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19=" << totals.p19Passed
              << " CoarseEval=" << totals.coarseEvaluated
              << " Coarse85=" << totals.coarsePassed85 << "\n";

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        totalDiffs += exactnessDiffs[modeIndex];
        std::cout << "  " << p33ModeName(modes[modeIndex])
                  << " diffs vs P31: " << exactnessDiffs[modeIndex] << "\n";
    }
    std::cout << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT shared Y-axis paths.\n"
                    : "  RESULT: MISMATCH - do not integrate P33.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}

enum class P34PermMode {
    P33_BASELINE,
    COMPACT_PERM_UPPER
};

const char* p34ModeName(P34PermMode mode) {
    switch (mode) {
        case P34PermMode::P33_BASELINE: return "P33 shared-Y upper";
        case P34PermMode::COMPACT_PERM_UPPER: return "compact 256-byte permutation";
    }
    return "unknown";
}

int benchmarkP34CompactPermutation(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p33ybench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p33ybench iterations must be >= 1");
    if (centerSpacingChunks < 15) throw std::runtime_error("center spacing must be >= 15 chunks");

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<P34PermMode> modes = {
        P34PermMode::P33_BASELINE,
        P34PermMode::COMPACT_PERM_UPPER
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    std::vector<std::vector<MultiCenterOutputs>> outputs;
    outputs.reserve(modes.size());
    for (std::size_t m = 0; m < modes.size(); ++m) {
        outputs.emplace_back();
        outputs.back().reserve(centers.size());
        for (std::size_t c = 0; c < centers.size(); ++c) outputs.back().emplace_back(count);
    }

    auto launchP20 = [&](P34PermMode mode, int centerIndex) {
        (void) mode;
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        const bool loadCache = centerIndex != 0;
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds,
                dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ,
                loadCache ? 1 : 0, loadCache ? 1 : 0);
        HIP_CHECK(hipGetLastError());
    };

    auto launchUpper = [&](P34PermMode mode, int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        if (mode == P34PermMode::P33_BASELINE) {
            hipLaunchKernelGGL(p33y::stage0UpperCompact192SharedYKernel,
                    dim3(count), dim3(p33y::THREADS), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        } else {
            hipLaunchKernelGGL(p34perm::stage0UpperCompact192SharedYCompactPermKernel,
                    dim3(count), dim3(p34perm::THREADS), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        }
        HIP_CHECK(hipGetLastError());
    };

    auto launchLower = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<32>,
                dim3(count), dim3(32), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ, nullptr);
        HIP_CHECK(hipGetLastError());
    };

    auto evaluateCenter = [&](P34PermMode mode, int centerIndex, MultiCenterOutputs& out) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        launchP20(mode, centerIndex);
        launchUpper(mode, centerIndex);
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        launchLower(centerIndex);

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                coarseSeeds[j] = seeds[originalIndex];
                coarseCacheSeedIndices[j] = originalIndex;
            }
            runCoarseScoresCachedAt(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    coarseScores.data(), coarseOffsetX, coarseOffsetZ,
                    terrainCache, climateCache);
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                out.coarse[originalIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    auto runMode = [&](int modeIndex) {
        const auto begin = std::chrono::steady_clock::now();
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            evaluateCenter(modes[modeIndex], centerIndex, outputs[modeIndex][centerIndex]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<double> fullMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            fullMs[modeIndex] += runMode(modeIndex);
        }
    }
    for (double& value : fullMs) value /= static_cast<double>(iterations);

    std::vector<double> p20OnlyMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            HIP_CHECK(hipDeviceSynchronize());
            const auto begin = std::chrono::steady_clock::now();
            for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
                launchP20(modes[modeIndex], centerIndex);
            }
            HIP_CHECK(hipDeviceSynchronize());
            p20OnlyMs[modeIndex] += hostElapsedMs(begin, std::chrono::steady_clock::now());
        }
    }
    for (double& value : p20OnlyMs) value /= static_cast<double>(iterations);

    std::vector<double> upperOnlyMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            const int rotation = (iteration + centerIndex) % static_cast<int>(modes.size());
            for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
                const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
                launchP20(modes[modeIndex], centerIndex);
                HIP_CHECK(hipDeviceSynchronize());
                const auto begin = std::chrono::steady_clock::now();
                launchUpper(modes[modeIndex], centerIndex);
                HIP_CHECK(hipDeviceSynchronize());
                upperOnlyMs[modeIndex] += hostElapsedMs(begin, std::chrono::steady_clock::now());
            }
        }
    }
    for (double& value : upperOnlyMs) value /= static_cast<double>(iterations);

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<std::uint64_t> exactnessDiffs(modes.size(), 0);
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::uint64_t diffs = 0;
        for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
            const MultiCenterOutputs& expected = outputs[0][centerIndex];
            const MultiCenterOutputs& actual = outputs[modeIndex][centerIndex];
            for (int i = 0; i < count; ++i) {
                diffs += expected.p20[i] != actual.p20[i];
                diffs += expected.upper[i] != actual.upper[i];
                diffs += expected.high[i] != actual.high[i];
                diffs += expected.p19Pass[i] != actual.p19Pass[i];
                diffs += expected.coarse[i] != actual.coarse[i];
                if (expected.p20[i] > 0 && expected.upper[i] >= upperMinCount) {
                    const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
                    for (int point = 0; point < stage0gpu::FULL_POINTS; ++point) {
                        diffs += expected.highestReentryY[base + point]
                                != actual.highestReentryY[base + point];
                    }
                }
            }
        }
        exactnessDiffs[modeIndex] = diffs;
    }

    MultiCenterCounts totals{};
    for (const MultiCenterOutputs& center : outputs[0]) {
        totals.p20Passed += center.counts.p20Passed;
        totals.stage1Passed += center.counts.stage1Passed;
        totals.stage05Passed += center.counts.stage05Passed;
        totals.p19Passed += center.counts.p19Passed;
        totals.coarseEvaluated += center.counts.coarseEvaluated;
        totals.coarsePassed85 += center.counts.coarsePassed85;
    }

    const double regions = static_cast<double>(count) * static_cast<double>(centers.size());
    const double baselineRate = regions * 1000.0 / fullMs[0];
    const double p20BaselineRate = regions * 1000.0 / p20OnlyMs[0];
    const double upperBaselineRate = regions * 1000.0 / upperOnlyMs[0];

    std::cout << "P34 compact permutation benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  total regions/batch:     " << static_cast<std::uint64_t>(regions) << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  production baseline:     P33 shared-Y upper192 + compact lower32 + exact caches\n"
              << "  compact permutation:     256 shared bytes + masked indexing; no 512-int expansion\n\n";

    std::cout << "Whole P33 pipeline throughput\n"
              << "  mode                         wall ms/batch   worlds/s   regions/s   gain vs P33\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double worldsPerSecond = static_cast<double>(count) * 1000.0 / fullMs[modeIndex];
        const double regionsPerSecond = regions * 1000.0 / fullMs[modeIndex];
        const double gain = (regionsPerSecond / baselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p34ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << fullMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(8) << worldsPerSecond
                  << "   " << std::setw(9) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nP20 kernel only (all eight centers; later stages excluded)\n"
              << "  mode                         wall ms/set     regions/s   gain vs P33\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double regionsPerSecond = regions * 1000.0 / p20OnlyMs[modeIndex];
        const double gain = (regionsPerSecond / p20BaselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p34ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << p20OnlyMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(11) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nUpper kernel only (all eight centers; P20/lower/coarse excluded)\n"
              << "  mode                         wall ms/set     regions/s   gain vs P33\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double regionsPerSecond = regions * 1000.0 / upperOnlyMs[modeIndex];
        const double gain = (regionsPerSecond / upperBaselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p34ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << upperOnlyMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(11) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nShared permutation representation\n"
              << "  baseline PerlinState:    " << sizeof(p20::PerlinState) << " bytes (512 ints + 3 doubles)\n"
              << "  compact PerlinState:     " << sizeof(p34perm::CompactPerlinState) << " bytes (256 bytes + 3 doubles)\n"
              << "  indexing:                exact perm[index & 255]\n"
              << "  tested scope:            climate + noise1/2/3/4/5 inside Upper-192\n";

    std::cout << "\nSurvivor totals\n"
              << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19=" << totals.p19Passed
              << " CoarseEval=" << totals.coarseEvaluated
              << " Coarse85=" << totals.coarsePassed85 << "\n";

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        totalDiffs += exactnessDiffs[modeIndex];
        std::cout << "  " << p34ModeName(modes[modeIndex])
                  << " diffs vs P33: " << exactnessDiffs[modeIndex] << "\n";
    }
    std::cout << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT compact permutation path.\n"
                    : "  RESULT: MISMATCH - do not integrate P34.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}

struct P35CenterWork {
    int coarseOffsetX = 0;
    int coarseOffsetZ = 0;
    std::vector<int> coarseIndices;
};

int benchmarkP35DirectCoarse(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p35directcoarsebench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p35directcoarsebench iterations must be >= 1");
    if (centerSpacingChunks < 15) throw std::runtime_error("center spacing must be >= 15 chunks");

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<P35CoarseMode> modes = {
        P35CoarseMode::BASELINE,
        P35CoarseMode::DIRECT_NOISE23,
        P35CoarseMode::DIRECT_ALL_3D
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    std::vector<std::vector<MultiCenterOutputs>> outputs;
    outputs.reserve(modes.size());
    for (std::size_t m = 0; m < modes.size(); ++m) {
        outputs.emplace_back();
        outputs.back().reserve(centers.size());
        for (std::size_t c = 0; c < centers.size(); ++c) outputs.back().emplace_back(count);
    }

    auto launchP20 = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        const bool loadCache = centerIndex != 0;
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds,
                dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ,
                loadCache ? 1 : 0, loadCache ? 1 : 0);
        HIP_CHECK(hipGetLastError());
    };

    auto launchUpper = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        hipLaunchKernelGGL(p33y::stage0UpperCompact192SharedYKernel,
                dim3(count), dim3(p33y::THREADS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ);
        HIP_CHECK(hipGetLastError());
    };

    auto launchLower = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<32>,
                dim3(count), dim3(32), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ, nullptr);
        HIP_CHECK(hipGetLastError());
    };

    auto evaluateCenter = [&](P35CoarseMode mode, int centerIndex, MultiCenterOutputs& out) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        launchP20(centerIndex);
        launchUpper(centerIndex);
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        launchLower(centerIndex);

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                coarseSeeds[j] = seeds[originalIndex];
                coarseCacheSeedIndices[j] = originalIndex;
            }
            runCoarseScoresCachedP35At(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    coarseScores.data(), coarseOffsetX, coarseOffsetZ,
                    terrainCache, climateCache, mode);
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                out.coarse[originalIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    auto runMode = [&](int modeIndex) {
        const auto begin = std::chrono::steady_clock::now();
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            evaluateCenter(modes[modeIndex], centerIndex, outputs[modeIndex][centerIndex]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<double> fullMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            fullMs[modeIndex] += runMode(modeIndex);
        }
    }
    for (double& value : fullMs) value /= static_cast<double>(iterations);

    // Refresh baseline outputs and preserve the exact real P33 coarse workload.
    (void) runMode(0);
    std::vector<P35CenterWork> work(centers.size());
    MultiCenterCounts totals{};
    for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
        P35CenterWork& w = work[centerIndex];
        w.coarseOffsetX = centers[centerIndex].x * 4;
        w.coarseOffsetZ = centers[centerIndex].z * 4;
        const MultiCenterOutputs& out = outputs[0][centerIndex];
        w.coarseIndices.reserve(out.counts.coarseEvaluated);
        for (int i = 0; i < count; ++i) {
            if (out.p19Pass[i] != 0) w.coarseIndices.push_back(i);
        }
        totals.p20Passed += out.counts.p20Passed;
        totals.stage1Passed += out.counts.stage1Passed;
        totals.stage05Passed += out.counts.stage05Passed;
        totals.p19Passed += out.counts.p19Passed;
        totals.coarseEvaluated += out.counts.coarseEvaluated;
        totals.coarsePassed85 += out.counts.coarsePassed85;
    }
    if (totals.coarseEvaluated == 0) throw std::runtime_error("deterministic batch produced no coarse survivors");

    auto gatherChunk = [&](const P35CenterWork& w, std::size_t offset, int chunk) {
        for (int j = 0; j < chunk; ++j) {
            const int originalIndex = w.coarseIndices[offset + j];
            coarseSeeds[j] = seeds[originalIndex];
            coarseCacheSeedIndices[j] = originalIndex;
        }
    };

    auto runCoarseSet = [&](P35CoarseMode mode, bool scoreAndCopy) {
        HIP_CHECK(hipDeviceSynchronize());
        const auto begin = std::chrono::steady_clock::now();
        for (const P35CenterWork& w : work) {
            for (std::size_t offset = 0; offset < w.coarseIndices.size(); offset += coarseChunkCapacity) {
                const int chunk = static_cast<int>(std::min<std::size_t>(
                        coarseChunkCapacity, w.coarseIndices.size() - offset));
                gatherChunk(w, offset, chunk);
                runCoarseScoresCachedP35At(coarseBuffers,
                        coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                        coarseScores.data(), w.coarseOffsetX, w.coarseOffsetZ,
                        terrainCache, climateCache, mode, scoreAndCopy);
            }
        }
        HIP_CHECK(hipDeviceSynchronize());
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (P35CoarseMode mode : modes) {
        (void) runCoarseSet(mode, true);
        (void) runCoarseSet(mode, false);
    }

    std::vector<double> coarseMs(modes.size(), 0.0);
    std::vector<double> generationMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            coarseMs[modeIndex] += runCoarseSet(modes[modeIndex], true);
        }
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            generationMs[modeIndex] += runCoarseSet(modes[modeIndex], false);
        }
    }
    for (double& value : coarseMs) value /= static_cast<double>(iterations);
    for (double& value : generationMs) value /= static_cast<double>(iterations);

    std::vector<std::uint64_t> fullDiffs(modes.size(), 0);
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::uint64_t diffs = 0;
        for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
            const MultiCenterOutputs& expected = outputs[0][centerIndex];
            const MultiCenterOutputs& actual = outputs[modeIndex][centerIndex];
            for (int i = 0; i < count; ++i) {
                diffs += expected.p20[i] != actual.p20[i];
                diffs += expected.upper[i] != actual.upper[i];
                diffs += expected.high[i] != actual.high[i];
                diffs += expected.p19Pass[i] != actual.p19Pass[i];
                diffs += expected.coarse[i] != actual.coarse[i];
                if (expected.p20[i] > 0 && expected.upper[i] >= upperMinCount) {
                    const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
                    for (int point = 0; point < stage0gpu::FULL_POINTS; ++point) {
                        diffs += expected.highestReentryY[base + point]
                                != actual.highestReentryY[base + point];
                    }
                }
            }
        }
        fullDiffs[modeIndex] = diffs;
    }

    std::vector<std::uint64_t> signDiffs(modes.size(), 0);
    std::vector<std::uint64_t> scoreDiffs(modes.size(), 0);
    std::vector<std::uint64_t> coarse85(modes.size(), 0);
    std::vector<int> expectedScores(coarseChunkCapacity);
    std::vector<int> actualScores(coarseChunkCapacity);
    std::vector<unsigned char> expectedSigns(
            static_cast<std::size_t>(coarseChunkCapacity) * coarsecore::CELLS);
    std::vector<unsigned char> actualSigns(
            static_cast<std::size_t>(coarseChunkCapacity) * coarsecore::CELLS);

    for (const P35CenterWork& w : work) {
        for (std::size_t offset = 0; offset < w.coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, w.coarseIndices.size() - offset));
            gatherChunk(w, offset, chunk);
            runCoarseScoresCachedP35At(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    expectedScores.data(), w.coarseOffsetX, w.coarseOffsetZ,
                    terrainCache, climateCache, P35CoarseMode::BASELINE);
            const std::size_t signBytes = static_cast<std::size_t>(chunk) * coarsecore::CELLS;
            HIP_CHECK(hipMemcpy(expectedSigns.data(), coarseBuffers.signs, signBytes, hipMemcpyDeviceToHost));
            for (int j = 0; j < chunk; ++j) coarse85[0] += expectedScores[j] >= 85;

            for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
                runCoarseScoresCachedP35At(coarseBuffers,
                        coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                        actualScores.data(), w.coarseOffsetX, w.coarseOffsetZ,
                        terrainCache, climateCache, modes[modeIndex]);
                HIP_CHECK(hipMemcpy(actualSigns.data(), coarseBuffers.signs, signBytes, hipMemcpyDeviceToHost));
                for (int j = 0; j < chunk; ++j) {
                    scoreDiffs[modeIndex] += expectedScores[j] != actualScores[j];
                    coarse85[modeIndex] += actualScores[j] >= 85;
                }
                for (std::size_t i = 0; i < signBytes; ++i) {
                    signDiffs[modeIndex] += expectedSigns[i] != actualSigns[i];
                }
            }
        }
    }

    const double regions = static_cast<double>(count) * static_cast<double>(centers.size());
    const double evaluations = static_cast<double>(totals.coarseEvaluated);
    const double baselineFullRate = regions * 1000.0 / fullMs[0];
    const double baselineCoarseRate = evaluations * 1000.0 / coarseMs[0];
    const double baselineGenerationRate = evaluations * 1000.0 / generationMs[0];

    std::cout << "P35 direct-write exact coarse benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  total regions/batch:     " << static_cast<std::uint64_t>(regions) << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  actual coarse survivors: " << totals.coarseEvaluated << "\n"
              << "  production baseline:     P33 shared-Y upper192 + compact lower32 + exact caches\n"
              << "  existing coarse cache:   shared 61 X + 61 Z + 17 Y axis setup already active\n"
              << "  candidate:               remove lane-local 17-double 3D result arrays\n\n";

    std::cout << "Whole P33 pipeline throughput\n"
              << "  mode                         wall ms/batch   worlds/s   regions/s   gain vs P33\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double worldsPerSecond = static_cast<double>(count) * 1000.0 / fullMs[modeIndex];
        const double regionsPerSecond = regions * 1000.0 / fullMs[modeIndex];
        const double gain = (regionsPerSecond / baselineFullRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p35CoarseModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << fullMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(8) << worldsPerSecond
                  << "   " << std::setw(9) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nExact coarse throughput (same survivors; transfers + scoring included)\n"
              << "  mode                         wall ms/set   coarse eval/s   gain vs baseline\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double rate = evaluations * 1000.0 / coarseMs[modeIndex];
        const double gain = (rate / baselineCoarseRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p35CoarseModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << coarseMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(13) << rate
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nCoarse generation only (same survivors; no component scoring)\n"
              << "  mode                         wall ms/set   coarse eval/s   gain vs baseline\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double rate = evaluations * 1000.0 / generationMs[modeIndex];
        const double gain = (rate / baselineGenerationRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p35CoarseModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << generationMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(13) << rate
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nTemporary result arrays removed\n"
              << "  baseline:                 17 doubles per lane for each 3D octave call\n"
              << "  direct noise2/3:          removed across 32 expensive 3D octaves\n"
              << "  direct all 3D:            removed across all 40 3D octaves\n"
              << "  axis setup:               unchanged; already shared exactly in production\n";

    std::cout << "\nSurvivor totals\n"
              << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19=" << totals.p19Passed
              << " CoarseEval=" << totals.coarseEvaluated
              << " Coarse85=" << totals.coarsePassed85 << "\n";

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        const std::uint64_t diffs = fullDiffs[modeIndex] + signDiffs[modeIndex] + scoreDiffs[modeIndex];
        totalDiffs += diffs;
        std::cout << "  " << p35CoarseModeName(modes[modeIndex]) << "\n"
                  << "    full-pipeline diffs:    " << fullDiffs[modeIndex] << "\n"
                  << "    sign-cell diffs:        " << signDiffs[modeIndex] << "\n"
                  << "    coarse-score diffs:     " << scoreDiffs[modeIndex] << "\n"
                  << "    Coarse85:               " << coarse85[modeIndex] << "\n";
    }
    std::cout << "  baseline Coarse85:        " << coarse85[0] << "\n"
              << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT direct-write coarse paths.\n"
                    : "  RESULT: MISMATCH - do not integrate P35.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}



enum class P36ScoreMode {
    SERIAL_P35,
    PARALLEL_32,
    PARALLEL_64,
    PARALLEL_128
};

const char* p36ScoreModeName(P36ScoreMode mode) {
    switch (mode) {
        case P36ScoreMode::SERIAL_P35: return "P35 serial scorer";
        case P36ScoreMode::PARALLEL_32: return "parallel BFS block 32";
        case P36ScoreMode::PARALLEL_64: return "parallel BFS block 64";
        case P36ScoreMode::PARALLEL_128: return "parallel BFS block 128";
    }
    return "unknown";
}

void launchP36ScoreKernel(
        coarsegpu::Buffers& b,
        int count,
        P36ScoreMode mode
) {
    if (mode == P36ScoreMode::SERIAL_P35) {
        hipLaunchKernelGGL(coarsegpu::scoreCoarseSignsKernel,
                dim3(count), dim3(1), 0, 0,
                b.signs, count, b.labels, b.queue, b.columnSeen, b.columnMinY,
                b.componentColumns, b.scores);
    } else if (mode == P36ScoreMode::PARALLEL_32) {
        hipLaunchKernelGGL(p36score::scoreCoarseSignsParallelKernel<32>,
                dim3(count), dim3(32), 0, 0,
                b.signs, count, b.labels, b.queue, b.columnSeen, b.columnMinY,
                b.componentColumns, b.scores);
    } else if (mode == P36ScoreMode::PARALLEL_64) {
        hipLaunchKernelGGL(p36score::scoreCoarseSignsParallelKernel<64>,
                dim3(count), dim3(64), 0, 0,
                b.signs, count, b.labels, b.queue, b.columnSeen, b.columnMinY,
                b.componentColumns, b.scores);
    } else {
        hipLaunchKernelGGL(p36score::scoreCoarseSignsParallelKernel<128>,
                dim3(count), dim3(128), 0, 0,
                b.signs, count, b.labels, b.queue, b.columnSeen, b.columnMinY,
                b.componentColumns, b.scores);
    }
    HIP_CHECK(hipGetLastError());
}

void launchP35ProductionCoarseGeneration(
        coarsegpu::Buffers& b,
        int count,
        int coarseOffsetX,
        int coarseOffsetZ,
        const TerrainPerlinCacheBuffers& terrainCache,
        const ClimatePerlinCacheBuffers& climateCache
) {
    hipLaunchKernelGGL(p35coarse::generateCoarseSignsDirect23Kernel,
            dim3(count), dim3(coarsegpu::THREADS), 0, 0,
            b.seeds, count,
            b.temp, b.rain, b.climateBlend,
            b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
            b.signs, coarseOffsetX, coarseOffsetZ,
            b.cacheSeedIndices,
            terrainCache.permutations, terrainCache.offsets,
            climateCache.permutations, climateCache.offsets);
    HIP_CHECK(hipGetLastError());
}

void runCoarseScoresCachedP36At(
        coarsegpu::Buffers& b,
        const std::int64_t* hostSeeds,
        const int* hostCacheSeedIndices,
        int count,
        int* hostScores,
        int coarseOffsetX,
        int coarseOffsetZ,
        const TerrainPerlinCacheBuffers& terrainCache,
        const ClimatePerlinCacheBuffers& climateCache,
        P36ScoreMode mode,
        bool copyScores = true
) {
    if (count < 1) return;
    if (count > b.capacity) throw std::runtime_error("coarse chunk exceeds GPU scratch capacity");
    HIP_CHECK(hipMemcpy(b.seeds, hostSeeds,
            static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(b.cacheSeedIndices, hostCacheSeedIndices,
            static_cast<std::size_t>(count) * sizeof(int), hipMemcpyHostToDevice));
    launchP35ProductionCoarseGeneration(b, count, coarseOffsetX, coarseOffsetZ,
            terrainCache, climateCache);
    launchP36ScoreKernel(b, count, mode);
    if (copyScores) {
        HIP_CHECK(hipMemcpy(hostScores, b.scores,
                static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
    }
}

int benchmarkP36ParallelCoarseScore(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p36scorebench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p36scorebench iterations must be >= 1");
    if (centerSpacingChunks < 15) throw std::runtime_error("center spacing must be >= 15 chunks");

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<P36ScoreMode> modes = {
        P36ScoreMode::SERIAL_P35,
        P36ScoreMode::PARALLEL_32,
        P36ScoreMode::PARALLEL_64,
        P36ScoreMode::PARALLEL_128
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds),
            static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20),
            static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper),
            static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh),
            static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks),
            static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY),
            static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(),
            static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    std::vector<std::vector<MultiCenterOutputs>> outputs;
    outputs.reserve(modes.size());
    for (std::size_t m = 0; m < modes.size(); ++m) {
        outputs.emplace_back();
        outputs.back().reserve(centers.size());
        for (std::size_t c = 0; c < centers.size(); ++c) outputs.back().emplace_back(count);
    }

    auto launchP20 = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        const bool loadCache = centerIndex != 0;
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds,
                dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ,
                loadCache ? 1 : 0, loadCache ? 1 : 0);
        HIP_CHECK(hipGetLastError());
    };

    auto launchUpper = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        hipLaunchKernelGGL(p33y::stage0UpperCompact192SharedYKernel,
                dim3(count), dim3(p33y::THREADS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ);
        HIP_CHECK(hipGetLastError());
    };

    auto launchLower = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<32>,
                dim3(count), dim3(32), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ, nullptr);
        HIP_CHECK(hipGetLastError());
    };

    auto evaluateCenter = [&](P36ScoreMode mode, int centerIndex, MultiCenterOutputs& out) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        launchP20(centerIndex);
        launchUpper(centerIndex);
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        launchLower(centerIndex);

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20,
                static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper,
                static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh,
                static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode
                    && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                coarseSeeds[j] = seeds[originalIndex];
                coarseCacheSeedIndices[j] = originalIndex;
            }
            runCoarseScoresCachedP36At(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    coarseScores.data(), coarseOffsetX, coarseOffsetZ,
                    terrainCache, climateCache, mode);
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                out.coarse[originalIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    auto runMode = [&](int modeIndex) {
        const auto begin = std::chrono::steady_clock::now();
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            evaluateCenter(modes[modeIndex], centerIndex, outputs[modeIndex][centerIndex]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void) runMode(modeIndex);
    }

    std::vector<double> fullMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            fullMs[modeIndex] += runMode(modeIndex);
        }
    }
    for (double& value : fullMs) value /= static_cast<double>(iterations);

    // Preserve the exact production workload from the refreshed serial baseline.
    (void) runMode(0);
    std::vector<P35CenterWork> work;
    work.reserve(centers.size());
    MultiCenterCounts totals{};
    for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
        P35CenterWork item;
        item.coarseOffsetX = centers[centerIndex].x * 4;
        item.coarseOffsetZ = centers[centerIndex].z * 4;
        const MultiCenterOutputs& out = outputs[0][centerIndex];
        item.coarseIndices.reserve(out.counts.coarseEvaluated);
        for (int i = 0; i < count; ++i) {
            if (out.p19Pass[i] != 0) item.coarseIndices.push_back(i);
        }
        work.push_back(std::move(item));
        totals.p20Passed += out.counts.p20Passed;
        totals.stage1Passed += out.counts.stage1Passed;
        totals.stage05Passed += out.counts.stage05Passed;
        totals.p19Passed += out.counts.p19Passed;
        totals.coarseEvaluated += out.counts.coarseEvaluated;
        totals.coarsePassed85 += out.counts.coarsePassed85;
    }

    auto gatherChunk = [&](const P35CenterWork& w, std::size_t offset, int chunk) {
        for (int j = 0; j < chunk; ++j) {
            const int originalIndex = w.coarseIndices[offset + j];
            coarseSeeds[j] = seeds[originalIndex];
            coarseCacheSeedIndices[j] = originalIndex;
        }
    };

    auto runCoarseSet = [&](P36ScoreMode mode) {
        const auto begin = std::chrono::steady_clock::now();
        for (const P35CenterWork& w : work) {
            for (std::size_t offset = 0; offset < w.coarseIndices.size(); offset += coarseChunkCapacity) {
                const int chunk = static_cast<int>(std::min<std::size_t>(
                        coarseChunkCapacity, w.coarseIndices.size() - offset));
                gatherChunk(w, offset, chunk);
                runCoarseScoresCachedP36At(coarseBuffers,
                        coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                        coarseScores.data(), w.coarseOffsetX, w.coarseOffsetZ,
                        terrainCache, climateCache, mode);
            }
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (P36ScoreMode mode : modes) (void) runCoarseSet(mode);
    std::vector<double> coarseMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            coarseMs[modeIndex] += runCoarseSet(modes[modeIndex]);
        }
    }
    for (double& value : coarseMs) value /= static_cast<double>(iterations);

    // Score the exact same generated sign grids with each scorer. Generation is
    // performed once per chunk and excluded from these scorer-only timings.
    std::vector<double> scorerMs(modes.size(), 0.0);
    std::vector<std::uint64_t> scoreDiffs(modes.size(), 0);
    std::vector<std::uint64_t> coarse85(modes.size(), 0);
    std::vector<int> expectedScores(coarseChunkCapacity);
    std::vector<int> actualScores(coarseChunkCapacity);

    for (const P35CenterWork& w : work) {
        for (std::size_t offset = 0; offset < w.coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, w.coarseIndices.size() - offset));
            gatherChunk(w, offset, chunk);
            HIP_CHECK(hipMemcpy(coarseBuffers.seeds, coarseSeeds.data(),
                    static_cast<std::size_t>(chunk) * sizeof(std::int64_t), hipMemcpyHostToDevice));
            HIP_CHECK(hipMemcpy(coarseBuffers.cacheSeedIndices, coarseCacheSeedIndices.data(),
                    static_cast<std::size_t>(chunk) * sizeof(int), hipMemcpyHostToDevice));
            launchP35ProductionCoarseGeneration(coarseBuffers, chunk,
                    w.coarseOffsetX, w.coarseOffsetZ, terrainCache, climateCache);
            HIP_CHECK(hipDeviceSynchronize());

            for (P36ScoreMode mode : modes) {
                launchP36ScoreKernel(coarseBuffers, chunk, mode);
                HIP_CHECK(hipDeviceSynchronize());
            }
            for (int iteration = 0; iteration < iterations; ++iteration) {
                const int rotation = iteration % static_cast<int>(modes.size());
                for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
                    const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
                    const auto begin = std::chrono::steady_clock::now();
                    launchP36ScoreKernel(coarseBuffers, chunk, modes[modeIndex]);
                    HIP_CHECK(hipDeviceSynchronize());
                    scorerMs[modeIndex] += hostElapsedMs(begin, std::chrono::steady_clock::now());
                }
            }

            launchP36ScoreKernel(coarseBuffers, chunk, modes[0]);
            HIP_CHECK(hipMemcpy(expectedScores.data(), coarseBuffers.scores,
                    static_cast<std::size_t>(chunk) * sizeof(int), hipMemcpyDeviceToHost));
            for (int j = 0; j < chunk; ++j) coarse85[0] += expectedScores[j] >= 85;
            for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
                launchP36ScoreKernel(coarseBuffers, chunk, modes[modeIndex]);
                HIP_CHECK(hipMemcpy(actualScores.data(), coarseBuffers.scores,
                        static_cast<std::size_t>(chunk) * sizeof(int), hipMemcpyDeviceToHost));
                for (int j = 0; j < chunk; ++j) {
                    scoreDiffs[modeIndex] += expectedScores[j] != actualScores[j];
                    coarse85[modeIndex] += actualScores[j] >= 85;
                }
            }
        }
    }
    for (double& value : scorerMs) value /= static_cast<double>(iterations);

    std::vector<std::uint64_t> fullDiffs(modes.size(), 0);
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::uint64_t diffs = 0;
        for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
            const MultiCenterOutputs& expected = outputs[0][centerIndex];
            const MultiCenterOutputs& actual = outputs[modeIndex][centerIndex];
            for (int i = 0; i < count; ++i) {
                diffs += expected.p20[i] != actual.p20[i];
                diffs += expected.upper[i] != actual.upper[i];
                diffs += expected.high[i] != actual.high[i];
                diffs += expected.p19Pass[i] != actual.p19Pass[i];
                diffs += expected.coarse[i] != actual.coarse[i];
                if (expected.p20[i] > 0 && expected.upper[i] >= upperMinCount) {
                    const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
                    for (int point = 0; point < stage0gpu::FULL_POINTS; ++point) {
                        diffs += expected.highestReentryY[base + point]
                                != actual.highestReentryY[base + point];
                    }
                }
            }
        }
        fullDiffs[modeIndex] = diffs;
    }

    const double regions = static_cast<double>(count) * static_cast<double>(centers.size());
    const double evaluations = static_cast<double>(totals.coarseEvaluated);
    const double baselineFullRate = regions * 1000.0 / fullMs[0];
    const double baselineCoarseRate = evaluations * 1000.0 / coarseMs[0];
    const double baselineScorerRate = evaluations * 1000.0 / scorerMs[0];

    std::cout << "P36 parallel exact coarse-scoring benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  total regions/batch:     " << static_cast<std::uint64_t>(regions) << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  actual coarse survivors: " << totals.coarseEvaluated << "\n"
              << "  production baseline:     P35 direct noise2/3 + P33 upper/lower/cache path\n"
              << "  scorer baseline:         one serial GPU thread per 61x61x17 sign grid\n"
              << "  candidates:              exact block-parallel BFS frontiers (32/64/128 threads)\n\n";

    std::cout << "Whole P35 pipeline throughput\n"
              << "  mode                         wall ms/batch   worlds/s   regions/s   gain vs P35\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double worldsPerSecond = static_cast<double>(count) * 1000.0 / fullMs[modeIndex];
        const double regionsPerSecond = regions * 1000.0 / fullMs[modeIndex];
        const double gain = (regionsPerSecond / baselineFullRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p36ScoreModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << fullMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(8) << worldsPerSecond
                  << "   " << std::setw(9) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nExact coarse throughput (direct generation + scoring + transfers)\n"
              << "  mode                         wall ms/set   coarse eval/s   gain vs serial\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double rate = evaluations * 1000.0 / coarseMs[modeIndex];
        const double gain = (rate / baselineCoarseRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p36ScoreModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << coarseMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(13) << rate
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nComponent scorer only (same generated sign grids)\n"
              << "  mode                         wall ms/set   coarse eval/s   gain vs serial\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double rate = evaluations * 1000.0 / scorerMs[modeIndex];
        const double gain = (rate / baselineScorerRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(28) << p36ScoreModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << scorerMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(13) << rate
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nParallel scorer design\n"
              << "  connectivity:             exact six-neighbour BFS\n"
              << "  component start order:    same linear X/Z/Y order as production\n"
              << "  boundary and re-entry:    unchanged exact rules\n"
              << "  queue and labels:         existing per-survivor global scratch reused\n";

    std::cout << "\nSurvivor totals\n"
              << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19=" << totals.p19Passed
              << " CoarseEval=" << totals.coarseEvaluated
              << " Coarse85=" << totals.coarsePassed85 << "\n";

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        const std::uint64_t diffs = fullDiffs[modeIndex] + scoreDiffs[modeIndex];
        totalDiffs += diffs;
        std::cout << "  " << p36ScoreModeName(modes[modeIndex]) << "\n"
                  << "    full-pipeline diffs:    " << fullDiffs[modeIndex] << "\n"
                  << "    coarse-score diffs:     " << scoreDiffs[modeIndex] << "\n"
                  << "    Coarse85:               " << coarse85[modeIndex] << "\n";
    }
    std::cout << "  baseline Coarse85:        " << coarse85[0] << "\n"
              << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT parallel coarse-scoring paths.\n"
                    : "  RESULT: MISMATCH - do not integrate P36.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}


enum class P32FusionMode {
    SEPARATE_P31,
    FUSED_64,
    FUSED_128,
    FUSED_192
};

const char* p32ModeName(P32FusionMode mode) {
    switch (mode) {
        case P32FusionMode::SEPARATE_P31: return "P31 separate P20 + Upper192";
        case P32FusionMode::FUSED_64: return "fused cached block 64";
        case P32FusionMode::FUSED_128: return "fused cached block 128";
        case P32FusionMode::FUSED_192: return "fused cached block 192";
    }
    return "unknown";
}

int benchmarkP32FusedP20Upper(
        int count,
        std::uint64_t sequenceSeed,
        int iterations,
        bool megaMode,
        int centerSpacingChunks
) {
    if (count < 1) throw std::runtime_error("p32fusionbench count must be >= 1");
    if (iterations < 1) throw std::runtime_error("p32fusionbench iterations must be >= 1");
    if (centerSpacingChunks < 15) throw std::runtime_error("center spacing must be >= 15 chunks");

    const std::vector<CenterChunk> centers = {
        {0, 0},
        {centerSpacingChunks, 0},
        {0, centerSpacingChunks},
        {centerSpacingChunks, centerSpacingChunks},
        {-centerSpacingChunks, 0},
        {-centerSpacingChunks, centerSpacingChunks},
        {0, -centerSpacingChunks},
        {centerSpacingChunks, -centerSpacingChunks}
    };
    const std::vector<P32FusionMode> modes = {
        P32FusionMode::SEPARATE_P31,
        P32FusionMode::FUSED_64,
        P32FusionMode::FUSED_128,
        P32FusionMode::FUSED_192
    };
    const int upperMinCount = megaMode ? 8 : 5;
    const int highMinCount = megaMode ? 6 : 5;

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(count) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(count) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, count);
    allocateClimatePerlinCache(climateCache, count);
    allocateColumnShapeCache(shapeCache, count);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

    std::vector<int> coarseIndices;
    coarseIndices.reserve(static_cast<std::size_t>(count) / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    std::vector<std::vector<MultiCenterOutputs>> outputs;
    outputs.reserve(modes.size());
    for (std::size_t m = 0; m < modes.size(); ++m) {
        outputs.emplace_back();
        outputs.back().reserve(centers.size());
        for (std::size_t c = 0; c < centers.size(); ++c) outputs.back().emplace_back(count);
    }

    auto launchSeparate = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        const bool loadCache = centerIndex != 0;
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                dUpperMasks, shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ,
                loadCache ? 1 : 0, loadCache ? 1 : 0);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(p31upper::stage0UpperCompact192Kernel,
                dim3(count), dim3(p31upper::THREADS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                climateCache.permutations, climateCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ);
        HIP_CHECK(hipGetLastError());
    };

    auto launchFused = [&](P32FusionMode mode, int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        if (centerIndex == 0 || mode == P32FusionMode::SEPARATE_P31) {
            launchSeparate(centerIndex);
            return;
        }
        if (mode == P32FusionMode::FUSED_64) {
            hipLaunchKernelGGL(p32fused::fusedCachedP20UpperKernel<64>,
                    dim3(count), dim3(64), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        } else if (mode == P32FusionMode::FUSED_128) {
            hipLaunchKernelGGL(p32fused::fusedCachedP20UpperKernel<128>,
                    dim3(count), dim3(128), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        } else {
            hipLaunchKernelGGL(p32fused::fusedCachedP20UpperKernel<192>,
                    dim3(count), dim3(192), 0, 0,
                    dSeeds, count, dP20, dUpper, dUpperMasks, nullptr,
                    terrainCache.permutations, terrainCache.offsets,
                    climateCache.permutations, climateCache.offsets,
                    shapeCache.d5, shapeCache.d7,
                    coarseOffsetX, coarseOffsetZ);
        }
        HIP_CHECK(hipGetLastError());
    };

    auto launchLower = [&](int centerIndex) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<32>,
                dim3(count), dim3(32), 0, 0,
                dSeeds, count, dP20, dUpper, upperMinCount, dUpperMasks,
                dHigh, dHighestReentryY, nullptr,
                terrainCache.permutations, terrainCache.offsets,
                shapeCache.d5, shapeCache.d7,
                coarseOffsetX, coarseOffsetZ, nullptr);
        HIP_CHECK(hipGetLastError());
    };

    auto evaluateCenter = [&](P32FusionMode mode, int centerIndex, MultiCenterOutputs& out) {
        const int coarseOffsetX = centers[centerIndex].x * 4;
        const int coarseOffsetZ = centers[centerIndex].z * 4;
        launchFused(mode, centerIndex);
        HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
        launchLower(centerIndex);

        HIP_CHECK(hipMemcpy(out.p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(out.highestReentryY.data(), dHighestReentryY,
                static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS,
                hipMemcpyDeviceToHost));

        std::fill(out.coarse.begin(), out.coarse.end(), 0);
        std::fill(out.p19Pass.begin(), out.p19Pass.end(), 0);
        out.counts = {};
        coarseIndices.clear();
        for (int i = 0; i < count; ++i) {
            if (out.p20[i] > 0) ++out.counts.p20Passed;
            if (out.p20[i] > 0 && out.upper[i] >= upperMinCount) ++out.counts.stage1Passed;
            if (!(out.p20[i] > 0 && out.upper[i] >= upperMinCount && out.high[i] >= highMinCount)) continue;
            ++out.counts.stage05Passed;
            const unsigned char* topology = out.highestReentryY.data()
                    + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
            const double p19Score = p19native::score(out.upper[i], features);
            const bool extreme = p19native::hasExtremeTopologySignal(features);
            const bool pass = extreme || p19Score >= p19native::THRESHOLD;
            const bool megaReject = pass && megaMode && megaTopologyReject(p19Score, extreme, features);
            out.p19Pass[i] = (pass && !megaReject) ? 1u : 0u;
            if (out.p19Pass[i] != 0) {
                ++out.counts.p19Passed;
                coarseIndices.push_back(i);
            }
        }

        out.counts.coarseEvaluated = coarseIndices.size();
        for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                    coarseChunkCapacity, coarseIndices.size() - offset));
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                coarseSeeds[j] = seeds[originalIndex];
                coarseCacheSeedIndices[j] = originalIndex;
            }
            runCoarseScoresCachedAt(coarseBuffers,
                    coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                    coarseScores.data(), coarseOffsetX, coarseOffsetZ,
                    terrainCache, climateCache);
            for (int j = 0; j < chunk; ++j) {
                const int originalIndex = coarseIndices[offset + j];
                out.coarse[originalIndex] = coarseScores[j];
                if (coarseScores[j] >= 85) ++out.counts.coarsePassed85;
            }
        }
    };

    auto runMode = [&](int modeIndex) {
        const auto begin = std::chrono::steady_clock::now();
        for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
            evaluateCenter(modes[modeIndex], centerIndex, outputs[modeIndex][centerIndex]);
        }
        return hostElapsedMs(begin, std::chrono::steady_clock::now());
    };

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void)runMode(modeIndex);
    }

    std::vector<double> fullMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            fullMs[modeIndex] += runMode(modeIndex);
        }
    }
    for (double& value : fullMs) value /= static_cast<double>(iterations);

    std::vector<double> earlyMs(modes.size(), 0.0);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int rotation = iteration % static_cast<int>(modes.size());
        for (int step = 0; step < static_cast<int>(modes.size()); ++step) {
            const int modeIndex = (rotation + step) % static_cast<int>(modes.size());
            HIP_CHECK(hipDeviceSynchronize());
            const auto begin = std::chrono::steady_clock::now();
            for (int centerIndex = 0; centerIndex < static_cast<int>(centers.size()); ++centerIndex) {
                launchFused(modes[modeIndex], centerIndex);
            }
            HIP_CHECK(hipDeviceSynchronize());
            earlyMs[modeIndex] += hostElapsedMs(begin, std::chrono::steady_clock::now());
        }
    }
    for (double& value : earlyMs) value /= static_cast<double>(iterations);

    for (int modeIndex = 0; modeIndex < static_cast<int>(modes.size()); ++modeIndex) {
        (void)runMode(modeIndex);
    }

    std::vector<std::uint64_t> exactnessDiffs(modes.size(), 0);
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::uint64_t diffs = 0;
        for (std::size_t centerIndex = 0; centerIndex < centers.size(); ++centerIndex) {
            const MultiCenterOutputs& expected = outputs[0][centerIndex];
            const MultiCenterOutputs& actual = outputs[modeIndex][centerIndex];
            for (int i = 0; i < count; ++i) {
                diffs += expected.p20[i] != actual.p20[i];
                diffs += expected.upper[i] != actual.upper[i];
                diffs += expected.high[i] != actual.high[i];
                diffs += expected.p19Pass[i] != actual.p19Pass[i];
                diffs += expected.coarse[i] != actual.coarse[i];
                if (expected.p20[i] > 0 && expected.upper[i] >= upperMinCount) {
                    const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
                    for (int point = 0; point < stage0gpu::FULL_POINTS; ++point) {
                        diffs += expected.highestReentryY[base + point]
                                != actual.highestReentryY[base + point];
                    }
                }
            }
        }
        exactnessDiffs[modeIndex] = diffs;
    }

    MultiCenterCounts totals{};
    for (const MultiCenterOutputs& center : outputs[0]) {
        totals.p20Passed += center.counts.p20Passed;
        totals.stage1Passed += center.counts.stage1Passed;
        totals.stage05Passed += center.counts.stage05Passed;
        totals.p19Passed += center.counts.p19Passed;
        totals.coarseEvaluated += center.counts.coarseEvaluated;
        totals.coarsePassed85 += center.counts.coarsePassed85;
    }

    const double regions = static_cast<double>(count) * static_cast<double>(centers.size());
    const double baselineRate = regions * 1000.0 / fullMs[0];
    const double earlyBaselineRate = regions * 1000.0 / earlyMs[0];

    std::cout << "P32 fused cached P20+Upper benchmark\n"
              << "  profile:                 " << (megaMode ? "MEGA 30k+" : "GENERAL") << "\n"
              << "  deterministic worlds:    " << count << "\n"
              << "  regions/world:           " << centers.size() << "\n"
              << "  total regions/batch:     " << static_cast<std::uint64_t>(regions) << "\n"
              << "  measured iterations:     " << iterations << "\n"
              << "  center spacing:          " << centerSpacingChunks << " chunks / "
              << (centerSpacingChunks * 16) << " blocks\n"
              << "  production baseline:     P31 compact upper 192 + compact lower 32 + exact caches\n"
              << "  fused scope:             cached centers 1-7; center 0 keeps cache-building P31 path\n"
              << "  early rejection:         exact P20 decision before remaining 192 upper columns\n\n";

    std::cout << "Whole P31 pipeline throughput\n"
              << "  mode                              wall ms/batch   worlds/s   regions/s   gain vs P31\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double worldsPerSecond = static_cast<double>(count) * 1000.0 / fullMs[modeIndex];
        const double regionsPerSecond = regions * 1000.0 / fullMs[modeIndex];
        const double gain = (regionsPerSecond / baselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(33) << p32ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << fullMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(8) << worldsPerSecond
                  << "   " << std::setw(9) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nP20+Upper pair only (all eight centers)\n"
              << "  mode                              wall ms/set     regions/s   gain vs separate\n";
    for (std::size_t modeIndex = 0; modeIndex < modes.size(); ++modeIndex) {
        const double regionsPerSecond = regions * 1000.0 / earlyMs[modeIndex];
        const double gain = (regionsPerSecond / earlyBaselineRate - 1.0) * 100.0;
        std::cout << "  " << std::left << std::setw(33) << p32ModeName(modes[modeIndex])
                  << std::right << std::fixed << std::setprecision(3) << std::setw(13) << earlyMs[modeIndex]
                  << "   " << std::setprecision(1) << std::setw(11) << regionsPerSecond
                  << "   " << std::showpos << std::setw(9) << gain << std::noshowpos << "%\n";
    }

    std::cout << "\nSurvivor totals\n"
              << "  P20=" << totals.p20Passed
              << " Stage1=" << totals.stage1Passed
              << " Stage0.5=" << totals.stage05Passed
              << " P19=" << totals.p19Passed
              << " CoarseEval=" << totals.coarseEvaluated
              << " Coarse85=" << totals.coarsePassed85 << "\n";

    std::uint64_t totalDiffs = 0;
    std::cout << "\nExactness checks\n";
    for (std::size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        totalDiffs += exactnessDiffs[modeIndex];
        std::cout << "  " << p32ModeName(modes[modeIndex])
                  << " diffs vs P31: " << exactnessDiffs[modeIndex] << "\n";
    }
    std::cout << "  total diffs:              " << totalDiffs << "\n"
              << (totalDiffs == 0
                    ? "  RESULT: EXACT fused cached paths.\n"
                    : "  RESULT: MISMATCH - do not integrate P32.\n");

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    return totalDiffs == 0 ? 0 : 2;
}

int profileEarlyKernels(int count, std::uint64_t sequenceSeed, int iterations, bool includeInitDetail = false) {
    if (count < 1) throw std::runtime_error("earlyprofile count must be >= 1");
    if (iterations < 1) throw std::runtime_error("earlyprofile iterations must be >= 1");

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    unsigned long long* dP20Ticks = nullptr;
    unsigned long long* dUpperTicks = nullptr;
    unsigned long long* dLowerTicks = nullptr;

    const std::size_t seedBytes = static_cast<std::size_t>(count) * sizeof(std::int64_t);
    const std::size_t intBytes = static_cast<std::size_t>(count) * sizeof(int);
    const std::size_t topologyBytes = static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS;
    const std::size_t tickValues = static_cast<std::size_t>(count) * earlyprofile::PHASES;
    const std::size_t tickBytes = tickValues * sizeof(unsigned long long);

    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), seedBytes));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), intBytes));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), intBytes));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), intBytes));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), topologyBytes));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), topologyBytes));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20Ticks), tickBytes));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperTicks), tickBytes));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dLowerTicks), tickBytes));

    std::vector<std::int64_t> seeds(count);
    for (int i = 0; i < count; ++i) {
        seeds[i] = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(
                sequenceSeed, static_cast<std::uint64_t>(i)));
    }
    HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), seedBytes, hipMemcpyHostToDevice));

    std::vector<int> referenceP20(count), referenceUpper(count), referenceHigh(count);
    std::vector<unsigned char> referenceUpperMasks(topologyBytes);
    std::vector<unsigned char> referenceHighest(topologyBytes);
    std::vector<int> profiledP20(count), profiledUpper(count), profiledHigh(count);
    std::vector<unsigned char> profiledUpperMasks(topologyBytes);
    std::vector<unsigned char> profiledHighest(topologyBytes);

    auto launchProduction = [&]() {
        HIP_CHECK(hipMemset(dHigh, 0, intBytes));
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, 1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0);
        HIP_CHECK(hipGetLastError());
        hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, 5, dUpperMasks, dHigh, dHighestReentryY, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0);
        HIP_CHECK(hipGetLastError());
    };

    launchProduction();
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(referenceP20.data(), dP20, intBytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(referenceUpper.data(), dUpper, intBytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(referenceHigh.data(), dHigh, intBytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(referenceUpperMasks.data(), dUpperMasks, topologyBytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(referenceHighest.data(), dHighestReentryY, topologyBytes, hipMemcpyDeviceToHost));

    std::uint64_t p20Passed = 0;
    std::uint64_t stage1Passed = 0;
    std::uint64_t stage05Passed = 0;
    std::uint64_t lowerCandidateColumns = 0;
    for (int i = 0; i < count; ++i) {
        if (referenceP20[i] > 0) ++p20Passed;
        if (referenceP20[i] > 0 && referenceUpper[i] >= 5) {
            ++stage1Passed;
            const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
            for (int lane = 0; lane < stage0gpu::FULL_POINTS; ++lane) {
                if (referenceUpperMasks[base + lane] != 0) ++lowerCandidateColumns;
            }
        }
        if (referenceP20[i] > 0 && referenceUpper[i] >= 5 && referenceHigh[i] >= 5) ++stage05Passed;
    }

    hipEvent_t prodStart{}, prodAfterP20{}, prodAfterUpper{}, prodAfterLower{};
    hipEvent_t profStart{}, profAfterP20{}, profAfterUpper{}, profAfterLower{};
    HIP_CHECK(hipEventCreate(&prodStart));
    HIP_CHECK(hipEventCreate(&prodAfterP20));
    HIP_CHECK(hipEventCreate(&prodAfterUpper));
    HIP_CHECK(hipEventCreate(&prodAfterLower));
    HIP_CHECK(hipEventCreate(&profStart));
    HIP_CHECK(hipEventCreate(&profAfterP20));
    HIP_CHECK(hipEventCreate(&profAfterUpper));
    HIP_CHECK(hipEventCreate(&profAfterLower));

    double productionMs[3] = {};
    double profiledMs[3] = {};
    long double tickSums[3][earlyprofile::PHASES] = {};
    std::vector<unsigned long long> hostTicks(tickValues);

    auto measureProduction = [&]() {
        HIP_CHECK(hipMemset(dHigh, 0, intBytes));
        HIP_CHECK(hipEventRecord(prodStart));
        hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                dSeeds, count, dP20, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(prodAfterP20));
        hipLaunchKernelGGL(stage0gpu::stage0UpperKernelFromSeeds, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, nullptr, 1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(prodAfterUpper));
        hipLaunchKernelGGL(stage0gpu::stage0LowerReentryKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, 5, dUpperMasks, dHigh, dHighestReentryY, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(prodAfterLower));
        HIP_CHECK(hipEventSynchronize(prodAfterLower));

        float ms = 0.0f;
        HIP_CHECK(hipEventElapsedTime(&ms, prodStart, prodAfterP20));
        productionMs[0] += ms;
        HIP_CHECK(hipEventElapsedTime(&ms, prodAfterP20, prodAfterUpper));
        productionMs[1] += ms;
        HIP_CHECK(hipEventElapsedTime(&ms, prodAfterUpper, prodAfterLower));
        productionMs[2] += ms;
    };

    auto launchProfile = [&]() {
        HIP_CHECK(hipMemset(dHigh, 0, intBytes));
        HIP_CHECK(hipMemset(dP20Ticks, 0, tickBytes));
        HIP_CHECK(hipMemset(dUpperTicks, 0, tickBytes));
        HIP_CHECK(hipMemset(dLowerTicks, 0, tickBytes));

        HIP_CHECK(hipEventRecord(profStart));
        hipLaunchKernelGGL(earlyprofile::p20ProfileKernel, dim3(count), dim3(earlyprofile::P20_POINTS), 0, 0,
                dSeeds, count, dP20, dP20Ticks);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(profAfterP20));
        hipLaunchKernelGGL(earlyprofile::upperProfileKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, dUpperTicks);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(profAfterUpper));
        hipLaunchKernelGGL(earlyprofile::lowerProfileKernel, dim3(count), dim3(stage0gpu::FULL_POINTS), 0, 0,
                dSeeds, count, dP20, dUpper, dUpperMasks, dHigh, dHighestReentryY, dLowerTicks);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipEventRecord(profAfterLower));
        HIP_CHECK(hipEventSynchronize(profAfterLower));
    };

    // Warm up both paths. Production timing below remains the uninstrumented source of truth.
    measureProduction();
    productionMs[0] = productionMs[1] = productionMs[2] = 0.0;
    launchProfile();

    for (int iteration = 0; iteration < iterations; ++iteration) measureProduction();
    for (int stage = 0; stage < 3; ++stage) productionMs[stage] /= static_cast<double>(iterations);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        launchProfile();

        float ms = 0.0f;
        HIP_CHECK(hipEventElapsedTime(&ms, profStart, profAfterP20));
        profiledMs[0] += ms;
        HIP_CHECK(hipEventElapsedTime(&ms, profAfterP20, profAfterUpper));
        profiledMs[1] += ms;
        HIP_CHECK(hipEventElapsedTime(&ms, profAfterUpper, profAfterLower));
        profiledMs[2] += ms;

        unsigned long long* deviceTickBuffers[3] = {dP20Ticks, dUpperTicks, dLowerTicks};
        for (int stage = 0; stage < 3; ++stage) {
            HIP_CHECK(hipMemcpy(hostTicks.data(), deviceTickBuffers[stage], tickBytes, hipMemcpyDeviceToHost));
            for (int seedIndex = 0; seedIndex < count; ++seedIndex) {
                const std::size_t base = static_cast<std::size_t>(seedIndex) * earlyprofile::PHASES;
                for (int phase = 0; phase < earlyprofile::PHASES; ++phase) {
                    tickSums[stage][phase] += static_cast<long double>(hostTicks[base + phase]);
                }
            }
        }
    }
    for (int stage = 0; stage < 3; ++stage) profiledMs[stage] /= static_cast<double>(iterations);

    HIP_CHECK(hipMemcpy(profiledP20.data(), dP20, intBytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(profiledUpper.data(), dUpper, intBytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(profiledHigh.data(), dHigh, intBytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(profiledUpperMasks.data(), dUpperMasks, topologyBytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(profiledHighest.data(), dHighestReentryY, topologyBytes, hipMemcpyDeviceToHost));

    std::uint64_t p20Diff = 0;
    std::uint64_t upperDiff = 0;
    std::uint64_t highDiff = 0;
    std::uint64_t highDecisionDiff = 0;
    std::uint64_t upperMaskDiff = 0;
    std::uint64_t topologyDiff = 0;
    for (int i = 0; i < count; ++i) {
        if (profiledP20[i] != referenceP20[i]) ++p20Diff;
        if (profiledUpper[i] != referenceUpper[i]) ++upperDiff;
        if (profiledHigh[i] != referenceHigh[i]) {
            ++highDiff;
            if ((profiledHigh[i] >= 5) != (referenceHigh[i] >= 5)) ++highDecisionDiff;
        }
        const std::size_t base = static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
        if (referenceP20[i] > 0) {
            for (int lane = 0; lane < stage0gpu::FULL_POINTS; ++lane) {
                if (profiledUpperMasks[base + lane] != referenceUpperMasks[base + lane]) ++upperMaskDiff;
            }
        }
        if (referenceP20[i] > 0 && referenceUpper[i] >= 5) {
            for (int lane = 0; lane < stage0gpu::FULL_POINTS; ++lane) {
                if (profiledHighest[base + lane] != referenceHighest[base + lane]) ++topologyDiff;
            }
        }
    }

    const char* stageNames[3] = {
        "P20 exact GPU",
        "upper GPU (Stage1/full upper)",
        "lower GPU (Stage0/0.5 reentry)"
    };
    const char* phaseNames[earlyprofile::PHASES] = {
        "climate + normalization",
        "noise2 setup",
        "noise2 eval",
        "noise3 setup",
        "noise3 eval",
        "noise1/blend setup",
        "noise1/blend eval",
        "noise4+5 setup",
        "noise4+5 eval",
        "terrain + reduction"
    };
    const std::uint64_t activeSeeds[3] = {
        static_cast<std::uint64_t>(count),
        p20Passed,
        stage1Passed
    };

    const double productionEarlyTotal = productionMs[0] + productionMs[1] + productionMs[2];
    long double totalTicks[3] = {};
    double estimatedMs[3][earlyprofile::PHASES] = {};
    for (int stage = 0; stage < 3; ++stage) {
        for (int phase = 0; phase < earlyprofile::PHASES; ++phase) totalTicks[stage] += tickSums[stage][phase];
        if (totalTicks[stage] > 0.0L) {
            for (int phase = 0; phase < earlyprofile::PHASES; ++phase) {
                estimatedMs[stage][phase] = productionMs[stage]
                        * static_cast<double>(tickSums[stage][phase] / totalTicks[stage]);
            }
        }
    }

    std::cout << "GPU early-kernel internal profile\n"
              << "  production batch:       " << count << " seeds\n"
              << "  deterministic seed:     " << sequenceSeed << "\n"
              << "  warmup batches:         1 production + 1 profiled\n"
              << "  measured iterations:    " << iterations << "\n"
              << "  production early total: " << std::fixed << std::setprecision(3) << productionEarlyTotal << " ms\n"
              << "  production throughput:  " << std::setprecision(1)
              << (static_cast<double>(count) * 1000.0 / productionEarlyTotal) << " seeds/s (early kernels only)\n"
              << "  note: internal phase ms are scaled to the uninstrumented production stage time\n\n";

    std::cout << "Production survivor counts\n"
              << std::fixed << std::setprecision(2)
              << "  P20 pass:                " << p20Passed << " | "
              << (static_cast<double>(p20Passed) * 100.0 / static_cast<double>(count)) << "%\n"
              << "  Stage1 pass:             " << stage1Passed << " | "
              << (static_cast<double>(stage1Passed) * 100.0 / static_cast<double>(count)) << "%\n"
              << "  Stage0.5 pass:           " << stage05Passed << " | "
              << (static_cast<double>(stage05Passed) * 100.0 / static_cast<double>(count)) << "%\n";
    const double candidatePct = stage1Passed > 0
            ? static_cast<double>(lowerCandidateColumns) * 100.0
                / (static_cast<double>(stage1Passed) * stage0gpu::FULL_POINTS)
            : 0.0;
    std::cout << "  lower candidate columns: " << lowerCandidateColumns << " | "
              << candidatePct << "% of columns in active lower blocks\n\n";

    std::cout << "Profile-kernel exactness cross-check\n"
              << "  P20 count diffs:         " << p20Diff << "\n"
              << "  upper count diffs:       " << upperDiff << "\n"
              << "  upper mask byte diffs:   " << upperMaskDiff << "\n"
              << "  high reentry count diffs:" << highDiff << "\n"
              << "  high decision diffs:     " << highDecisionDiff << "\n"
              << "  topology byte diffs:     " << topologyDiff << "\n\n";

    std::cout << "Stage timing and profiler overhead\n";
    for (int stage = 0; stage < 3; ++stage) {
        const double overhead = productionMs[stage] > 0.0
                ? (profiledMs[stage] / productionMs[stage] - 1.0) * 100.0
                : 0.0;
        std::cout << "  " << std::left << std::setw(35) << stageNames[stage]
                  << std::right << std::fixed << std::setprecision(3)
                  << std::setw(10) << productionMs[stage] << " ms prod | "
                  << std::setw(10) << profiledMs[stage] << " ms prof | "
                  << std::setprecision(2) << std::setw(7) << overhead << "% overhead\n";
    }
    std::cout << "\n";

    double groupedClimate = 0.0;
    double grouped3DSetup = 0.0;
    double grouped3DEval = 0.0;
    double grouped45 = 0.0;
    double groupedTerrain = 0.0;
    const char* hottestStage = stageNames[0];
    const char* hottestPhase = phaseNames[0];
    double hottestMs = -1.0;

    for (int stage = 0; stage < 3; ++stage) {
        std::cout << stageNames[stage] << " detail\n";
        std::cout << "  active seed blocks: " << activeSeeds[stage] << "\n";
        for (int phase = 0; phase < earlyprofile::PHASES; ++phase) {
            const double stagePct = productionMs[stage] > 0.0
                    ? estimatedMs[stage][phase] * 100.0 / productionMs[stage]
                    : 0.0;
            const double totalPct = productionEarlyTotal > 0.0
                    ? estimatedMs[stage][phase] * 100.0 / productionEarlyTotal
                    : 0.0;
            const long double denom = static_cast<long double>(iterations)
                    * static_cast<long double>(activeSeeds[stage] == 0 ? 1 : activeSeeds[stage]);
            const long double avgTicks = tickSums[stage][phase] / denom;
            std::cout << "  " << std::left << std::setw(27) << phaseNames[phase]
                      << std::right << std::fixed << std::setprecision(2)
                      << std::setw(7) << stagePct << "% stage | "
                      << std::setprecision(3) << std::setw(9) << estimatedMs[stage][phase] << " ms | "
                      << std::setprecision(2) << std::setw(6) << totalPct << "% early | avg ticks/active seed "
                      << std::fixed << std::setprecision(0) << static_cast<double>(avgTicks) << "\n";
            if (estimatedMs[stage][phase] > hottestMs) {
                hottestMs = estimatedMs[stage][phase];
                hottestStage = stageNames[stage];
                hottestPhase = phaseNames[phase];
            }
        }
        std::cout << "\n";

        groupedClimate += estimatedMs[stage][earlyprofile::PHASE_CLIMATE];
        grouped3DSetup += estimatedMs[stage][earlyprofile::PHASE_NOISE2_SETUP]
                + estimatedMs[stage][earlyprofile::PHASE_NOISE3_SETUP]
                + estimatedMs[stage][earlyprofile::PHASE_NOISE1_SETUP];
        grouped3DEval += estimatedMs[stage][earlyprofile::PHASE_NOISE2_EVAL]
                + estimatedMs[stage][earlyprofile::PHASE_NOISE3_EVAL]
                + estimatedMs[stage][earlyprofile::PHASE_NOISE1_EVAL];
        grouped45 += estimatedMs[stage][earlyprofile::PHASE_NOISE45_SETUP]
                + estimatedMs[stage][earlyprofile::PHASE_NOISE45_EVAL];
        groupedTerrain += estimatedMs[stage][earlyprofile::PHASE_TERRAIN_REDUCE];
    }

    const auto earlyPct = [&](double ms) {
        return productionEarlyTotal > 0.0 ? ms * 100.0 / productionEarlyTotal : 0.0;
    };
    std::cout << "Cross-stage grouped targets\n"
              << "  climate + normalization: " << std::fixed << std::setprecision(3) << groupedClimate << " ms | "
              << std::setprecision(2) << earlyPct(groupedClimate) << "% early\n"
              << "  3D Perlin setup:         " << std::setprecision(3) << grouped3DSetup << " ms | "
              << std::setprecision(2) << earlyPct(grouped3DSetup) << "% early\n"
              << "  3D Perlin eval:          " << std::setprecision(3) << grouped3DEval << " ms | "
              << std::setprecision(2) << earlyPct(grouped3DEval) << "% early\n"
              << "  noise4 + noise5 all:     " << std::setprecision(3) << grouped45 << " ms | "
              << std::setprecision(2) << earlyPct(grouped45) << "% early\n"
              << "  terrain + reduction:     " << std::setprecision(3) << groupedTerrain << " ms | "
              << std::setprecision(2) << earlyPct(groupedTerrain) << "% early\n"
              << "  hottest individual phase:" << hottestStage << " -> " << hottestPhase
              << " | " << std::setprecision(3) << hottestMs << " ms\n";


    std::uint64_t initHashDiffTotal = 0;
    if (includeInitDetail) {
        const std::size_t initTickValues = static_cast<std::size_t>(count)
                * initprofile::GROUPS * initprofile::COMPONENTS;
        const std::size_t initTickBytes = initTickValues * sizeof(unsigned long long);
        const std::size_t hashBytes = static_cast<std::size_t>(count) * sizeof(unsigned long long);

        unsigned long long* dInitTicks = nullptr;
        unsigned long long* dReferenceHashes = nullptr;
        unsigned long long* dProfileHashes = nullptr;
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dInitTicks), initTickBytes));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dReferenceHashes), hashBytes));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dProfileHashes), hashBytes));

        std::vector<unsigned long long> hostInitTicks(initTickValues);
        std::vector<unsigned long long> referenceHashes(count);
        std::vector<unsigned long long> profileHashes(count);

        long double initTickSums[3][initprofile::GROUPS][initprofile::COMPONENTS] = {};
        double initReferenceMs[3] = {};
        double initProfiledMs[3] = {};
        std::uint64_t initHashDiffs[3] = {};

        const int stageThreads[3] = {p20gpu::POINTS, stage0gpu::FULL_POINTS, stage0gpu::FULL_POINTS};
        const int groupCalls[initprofile::GROUPS] = {16, 16, 8, 8, 10, 16};
        const char* groupNames[initprofile::GROUPS] = {
            "noise2",
            "noise3",
            "noise1 active",
            "noise1 skipped",
            "noise4",
            "noise5"
        };
        const char* componentNames[initprofile::COMPONENTS] = {
            "entry synchronization",
            "offset RNG (3 doubles)",
            "identity permutation fill",
            "Fisher-Yates shuffle",
            "exit synchronization"
        };

        hipEvent_t initStart{}, initStop{};
        HIP_CHECK(hipEventCreate(&initStart));
        HIP_CHECK(hipEventCreate(&initStop));

        for (int stage = 0; stage < 3; ++stage) {
            const dim3 grid(count);
            const dim3 block(stageThreads[stage]);

            // Reference hash is generated by the exact production initPerlin helper.
            hipLaunchKernelGGL(initprofile::referenceSequenceKernel, grid, block, 0, 0,
                    dSeeds, count, stage, dP20, dUpper, dReferenceHashes);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipDeviceSynchronize());

            // Warm up the detailed path once before timing it.
            HIP_CHECK(hipMemset(dInitTicks, 0, initTickBytes));
            hipLaunchKernelGGL(initprofile::profileSequenceKernel, grid, block, 0, 0,
                    dSeeds, count, stage, dP20, dUpper, dInitTicks, dProfileHashes);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipDeviceSynchronize());

            for (int iteration = 0; iteration < iterations; ++iteration) {
                HIP_CHECK(hipEventRecord(initStart));
                hipLaunchKernelGGL(initprofile::referenceSequenceKernel, grid, block, 0, 0,
                        dSeeds, count, stage, dP20, dUpper, dReferenceHashes);
                HIP_CHECK(hipGetLastError());
                HIP_CHECK(hipEventRecord(initStop));
                HIP_CHECK(hipEventSynchronize(initStop));
                float ms = 0.0f;
                HIP_CHECK(hipEventElapsedTime(&ms, initStart, initStop));
                initReferenceMs[stage] += ms;
            }
            initReferenceMs[stage] /= static_cast<double>(iterations);

            for (int iteration = 0; iteration < iterations; ++iteration) {
                HIP_CHECK(hipMemset(dInitTicks, 0, initTickBytes));
                HIP_CHECK(hipEventRecord(initStart));
                hipLaunchKernelGGL(initprofile::profileSequenceKernel, grid, block, 0, 0,
                        dSeeds, count, stage, dP20, dUpper, dInitTicks, dProfileHashes);
                HIP_CHECK(hipGetLastError());
                HIP_CHECK(hipEventRecord(initStop));
                HIP_CHECK(hipEventSynchronize(initStop));
                float ms = 0.0f;
                HIP_CHECK(hipEventElapsedTime(&ms, initStart, initStop));
                initProfiledMs[stage] += ms;

                HIP_CHECK(hipMemcpy(hostInitTicks.data(), dInitTicks, initTickBytes, hipMemcpyDeviceToHost));
                for (int seedIndex = 0; seedIndex < count; ++seedIndex) {
                    const std::size_t seedBase = static_cast<std::size_t>(seedIndex)
                            * initprofile::GROUPS * initprofile::COMPONENTS;
                    for (int group = 0; group < initprofile::GROUPS; ++group) {
                        const std::size_t groupBase = seedBase
                                + static_cast<std::size_t>(group) * initprofile::COMPONENTS;
                        for (int component = 0; component < initprofile::COMPONENTS; ++component) {
                            initTickSums[stage][group][component] += static_cast<long double>(
                                    hostInitTicks[groupBase + component]);
                        }
                    }
                }
            }
            initProfiledMs[stage] /= static_cast<double>(iterations);

            HIP_CHECK(hipMemcpy(referenceHashes.data(), dReferenceHashes, hashBytes, hipMemcpyDeviceToHost));
            HIP_CHECK(hipMemcpy(profileHashes.data(), dProfileHashes, hashBytes, hipMemcpyDeviceToHost));
            for (int seedIndex = 0; seedIndex < count; ++seedIndex) {
                if (referenceHashes[seedIndex] != profileHashes[seedIndex]) ++initHashDiffs[stage];
            }
            initHashDiffTotal += initHashDiffs[stage];
        }

        HIP_CHECK(hipEventDestroy(initStart));
        HIP_CHECK(hipEventDestroy(initStop));
        HIP_CHECK(hipFree(dInitTicks));
        HIP_CHECK(hipFree(dReferenceHashes));
        HIP_CHECK(hipFree(dProfileHashes));

        // Scale detailed component shares back to the uninstrumented production setup
        // milliseconds already measured above. This covers every terrain-generator
        // initPerlin call: noise2, noise3, noise1 active+skip, noise4, and noise5.
        double groupEstimatedMs[3][initprofile::GROUPS] = {};
        double componentEstimatedMs[3][initprofile::COMPONENTS] = {};
        double crossStageComponentMs[initprofile::COMPONENTS] = {};
        double setupBudgetByStage[3] = {};

        for (int stage = 0; stage < 3; ++stage) {
            long double groupTotals[initprofile::GROUPS] = {};
            for (int group = 0; group < initprofile::GROUPS; ++group) {
                for (int component = 0; component < initprofile::COMPONENTS; ++component) {
                    groupTotals[group] += initTickSums[stage][group][component];
                }
            }

            const double noise2Budget = estimatedMs[stage][earlyprofile::PHASE_NOISE2_SETUP];
            const double noise3Budget = estimatedMs[stage][earlyprofile::PHASE_NOISE3_SETUP];
            const double noise1Budget = estimatedMs[stage][earlyprofile::PHASE_NOISE1_SETUP];
            const double noise45Budget = estimatedMs[stage][earlyprofile::PHASE_NOISE45_SETUP];
            setupBudgetByStage[stage] = noise2Budget + noise3Budget + noise1Budget + noise45Budget;

            groupEstimatedMs[stage][initprofile::GROUP_NOISE2] = noise2Budget;
            groupEstimatedMs[stage][initprofile::GROUP_NOISE3] = noise3Budget;

            const long double noise1Ticks = groupTotals[initprofile::GROUP_NOISE1_ACTIVE]
                    + groupTotals[initprofile::GROUP_NOISE1_SKIP];
            if (noise1Ticks > 0.0L) {
                groupEstimatedMs[stage][initprofile::GROUP_NOISE1_ACTIVE] = noise1Budget
                        * static_cast<double>(groupTotals[initprofile::GROUP_NOISE1_ACTIVE] / noise1Ticks);
                groupEstimatedMs[stage][initprofile::GROUP_NOISE1_SKIP] = noise1Budget
                        * static_cast<double>(groupTotals[initprofile::GROUP_NOISE1_SKIP] / noise1Ticks);
            }

            const long double noise45Ticks = groupTotals[initprofile::GROUP_NOISE4]
                    + groupTotals[initprofile::GROUP_NOISE5];
            if (noise45Ticks > 0.0L) {
                groupEstimatedMs[stage][initprofile::GROUP_NOISE4] = noise45Budget
                        * static_cast<double>(groupTotals[initprofile::GROUP_NOISE4] / noise45Ticks);
                groupEstimatedMs[stage][initprofile::GROUP_NOISE5] = noise45Budget
                        * static_cast<double>(groupTotals[initprofile::GROUP_NOISE5] / noise45Ticks);
            }

            for (int group = 0; group < initprofile::GROUPS; ++group) {
                if (groupTotals[group] <= 0.0L) continue;
                for (int component = 0; component < initprofile::COMPONENTS; ++component) {
                    const double ms = groupEstimatedMs[stage][group]
                            * static_cast<double>(initTickSums[stage][group][component] / groupTotals[group]);
                    componentEstimatedMs[stage][component] += ms;
                    crossStageComponentMs[component] += ms;
                }
            }
        }

        double totalSetupBudget = 0.0;
        for (double stageBudget : setupBudgetByStage) totalSetupBudget += stageBudget;
        std::uint64_t totalInitCalls = 0;
        for (int stage = 0; stage < 3; ++stage) {
            totalInitCalls += activeSeeds[stage] * static_cast<std::uint64_t>(initprofile::TERRAIN_INIT_CALLS);
        }

        std::cout << "\nInitPerlin internal detail\n"
                  << "  scope: terrain noise setup only (noise2/3/1/4/5)\n"
                  << "  exact init calls/block: " << initprofile::TERRAIN_INIT_CALLS << "\n"
                  << "  exact init calls/32K production batch: " << totalInitCalls << "\n"
                  << "  production setup budget represented: " << std::fixed << std::setprecision(3)
                  << totalSetupBudget << " ms | " << std::setprecision(2)
                  << (productionEarlyTotal > 0.0 ? totalSetupBudget * 100.0 / productionEarlyTotal : 0.0)
                  << "% of early GPU time\n"
                  << "  note: component shares come from exact profiled init sequences and are scaled to the\n"
                  << "        uninstrumented setup milliseconds measured by the early-kernel profiler\n\n";

        std::cout << "Init sequence exactness and instrumentation overhead\n";
        for (int stage = 0; stage < 3; ++stage) {
            const double overhead = initReferenceMs[stage] > 0.0
                    ? (initProfiledMs[stage] / initReferenceMs[stage] - 1.0) * 100.0
                    : 0.0;
            std::cout << "  " << std::left << std::setw(35) << stageNames[stage]
                      << std::right << "hash diffs=" << std::setw(3) << initHashDiffs[stage]
                      << " | exact sequence " << std::fixed << std::setprecision(3)
                      << std::setw(9) << initReferenceMs[stage] << " ms"
                      << " | profiled " << std::setw(9) << initProfiledMs[stage] << " ms"
                      << " | " << std::setprecision(2) << std::setw(7) << overhead << "% overhead\n";
        }
        std::cout << "\n";

        for (int stage = 0; stage < 3; ++stage) {
            std::cout << stageNames[stage] << " init groups\n"
                      << "  active seed blocks: " << activeSeeds[stage] << "\n";
            for (int group = 0; group < initprofile::GROUPS; ++group) {
                long double groupTicks = 0.0L;
                for (int component = 0; component < initprofile::COMPONENTS; ++component) {
                    groupTicks += initTickSums[stage][group][component];
                }
                const long double denom = static_cast<long double>(iterations)
                        * static_cast<long double>(activeSeeds[stage] == 0 ? 1 : activeSeeds[stage])
                        * static_cast<long double>(groupCalls[group]);
                const long double avgTicksPerInit = denom > 0.0L ? groupTicks / denom : 0.0L;
                std::cout << "  " << std::left << std::setw(18) << groupNames[group]
                          << std::right << "calls/block=" << std::setw(2) << groupCalls[group]
                          << " | estimated " << std::fixed << std::setprecision(3)
                          << std::setw(9) << groupEstimatedMs[stage][group] << " ms"
                          << " | avg ticks/init " << std::setprecision(0)
                          << std::setw(8) << static_cast<double>(avgTicksPerInit) << "\n";
            }
            std::cout << "  component estimate within this stage's " << std::fixed << std::setprecision(3)
                      << setupBudgetByStage[stage] << " ms setup budget\n";
            for (int component = 0; component < initprofile::COMPONENTS; ++component) {
                const double pct = setupBudgetByStage[stage] > 0.0
                        ? componentEstimatedMs[stage][component] * 100.0 / setupBudgetByStage[stage]
                        : 0.0;
                std::cout << "    " << std::left << std::setw(29) << componentNames[component]
                          << std::right << std::fixed << std::setprecision(3)
                          << std::setw(9) << componentEstimatedMs[stage][component] << " ms | "
                          << std::setprecision(2) << std::setw(6) << pct << "%\n";
            }
            std::cout << "\n";
        }

        const char* hottestComponent = componentNames[0];
        double hottestComponentMs = crossStageComponentMs[0];
        std::cout << "Cross-stage InitPerlin component estimate\n";
        for (int component = 0; component < initprofile::COMPONENTS; ++component) {
            const double setupPct = totalSetupBudget > 0.0
                    ? crossStageComponentMs[component] * 100.0 / totalSetupBudget
                    : 0.0;
            const double earlyPctValue = productionEarlyTotal > 0.0
                    ? crossStageComponentMs[component] * 100.0 / productionEarlyTotal
                    : 0.0;
            std::cout << "  " << std::left << std::setw(29) << componentNames[component]
                      << std::right << std::fixed << std::setprecision(3)
                      << std::setw(10) << crossStageComponentMs[component] << " ms | "
                      << std::setprecision(2) << std::setw(6) << setupPct << "% setup | "
                      << std::setw(6) << earlyPctValue << "% early\n";
            if (crossStageComponentMs[component] > hottestComponentMs) {
                hottestComponentMs = crossStageComponentMs[component];
                hottestComponent = componentNames[component];
            }
        }
        std::cout << "  hottest init component: " << hottestComponent << " | "
                  << std::fixed << std::setprecision(3) << hottestComponentMs << " ms estimated\n"
                  << "\n"
                  << (initHashDiffTotal == 0
                        ? "ALL INITPERLIN DETAIL PROFILE SEQUENCES MATCH EXACT initPerlin.\n"
                        : "INITPERLIN DETAIL PROFILE HASH MISMATCHES DETECTED.\n");
    }

    const bool exact = p20Diff == 0 && upperDiff == 0 && upperMaskDiff == 0
            && highDiff == 0 && highDecisionDiff == 0 && topologyDiff == 0 && initHashDiffTotal == 0;
    std::cout << "\n" << (exact
            ? "ALL EARLY DETAIL PROFILE OUTPUTS MATCH PRODUCTION.\n"
            : "EARLY DETAIL PROFILE OUTPUT MISMATCHES DETECTED.\n");

    hipEventDestroy(prodStart);
    hipEventDestroy(prodAfterP20);
    hipEventDestroy(prodAfterUpper);
    hipEventDestroy(prodAfterLower);
    hipEventDestroy(profStart);
    hipEventDestroy(profAfterP20);
    hipEventDestroy(profAfterUpper);
    hipEventDestroy(profAfterLower);
    hipFree(dSeeds);
    hipFree(dP20);
    hipFree(dUpper);
    hipFree(dHigh);
    hipFree(dUpperMasks);
    hipFree(dHighestReentryY);
    hipFree(dP20Ticks);
    hipFree(dUpperTicks);
    hipFree(dLowerTicks);
    return exact ? 0 : 2;
}


enum class HuntProfile {
    GENERAL,
    MEGA,
    RECORD60,
    RECORD80
};

struct HuntProfileConfig {
    HuntProfile profile;
    const char* argument;
    const char* displayName;
    int p20MinCount;
    int upperMinCount;
    int highMinCount;
    double p19MinScore;
    int coarseMinCells;
    bool megaTopologyFilter;
};

const HuntProfileConfig& huntProfileConfig(HuntProfile profile) {
    static const HuntProfileConfig GENERAL_CONFIG{
            HuntProfile::GENERAL, "general", "GENERAL", 1, 5, 5,
            p19native::THRESHOLD, 85, false};
    static const HuntProfileConfig MEGA_CONFIG{
            HuntProfile::MEGA, "mega", "MEGA 30k+", 1, 8, 6,
            p19native::THRESHOLD, 85, true};
    static const HuntProfileConfig RECORD60_CONFIG{
            HuntProfile::RECORD60, "record60", "RECORD 60k+ (empirical)", 3, 19, 12,
            6.70, 95, false};
    static const HuntProfileConfig RECORD80_CONFIG{
            HuntProfile::RECORD80, "record80", "WORLD RECORD COARSE 700 (compact-thick safe)", 3, 19, 20,
            6.70, 700, false};
    switch (profile) {
        case HuntProfile::GENERAL: return GENERAL_CONFIG;
        case HuntProfile::MEGA: return MEGA_CONFIG;
        case HuntProfile::RECORD60: return RECORD60_CONFIG;
        case HuntProfile::RECORD80: return RECORD80_CONFIG;
    }
    return GENERAL_CONFIG;
}

HuntProfile parseHuntProfile(const std::string& value) {
    if (value == "general") return HuntProfile::GENERAL;
    if (value == "mega") return HuntProfile::MEGA;
    if (value == "record60") return HuntProfile::RECORD60;
    if (value == "record80") return HuntProfile::RECORD80;
    throw std::runtime_error("hunt profile must be general, mega, record60, or record80");
}

__global__ void applyP20MinimumGateKernel(
        const int* actualCounts,
        int* gatedCounts,
        int count,
        int minimum
) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < count) gatedCounts[i] = actualCounts[i] >= minimum ? actualCounts[i] : 0;
}

int streamStage0Filter(int capacity, HuntProfile profile, bool researchMode, int centerCount) {
    if (capacity < 1) throw std::runtime_error("Stage0 stream capacity must be >= 1");
    if (centerCount != 1 && centerCount != 8) {
        throw std::runtime_error("Stage0 stream center count must be 1 or 8");
    }

    static constexpr int CENTER_CHUNK_X[8] = {0, 15, 0, 15, -15, -15, 0, 15};
    static constexpr int CENTER_CHUNK_Z[8] = {0, 0, 15, 15, 0, 15, -15, -15};
    const HuntProfileConfig& profileConfig = huntProfileConfig(profile);
    const int p20MinCount = profileConfig.p20MinCount;
    const int upperMinCount = profileConfig.upperMinCount;
    const int highMinCount = profileConfig.highMinCount;

#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) throw std::runtime_error("failed to set stdin binary mode");
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) throw std::runtime_error("failed to set stdout binary mode");
#endif

    printDevice(std::cerr);
    std::cerr << "Stage0+P19+coarse stream capacity: " << capacity << " worlds / "
              << (static_cast<long long>(capacity) * centerCount) << " regions\n";
    std::cerr << "P38 production multi-center: " << centerCount
              << " exact radius-7 regions/world | 15-chunk spacing | region budget semantics\n";
    std::cerr << "P38 profile: " << profileConfig.displayName
              << " | P20>=" << p20MinCount
              << " | Upper>=" << upperMinCount
              << " | High>=" << highMinCount
              << " | P19>=" << profileConfig.p19MinScore << "/extreme"
              << " | Coarse>=" << profileConfig.coarseMinCells
              << (profileConfig.megaTopologyFilter ? " | Mega topology reject ON" : " | topology reject OFF") << "\n";
    if (profile == HuntProfile::RECORD60 || profile == HuntProfile::RECORD80) {
        std::cerr << "WARNING: record profile is empirically validated, not mathematically lossless.\n";
    }
    std::cerr << "Stage0+P19+coarse pipeline: exact P20 -> full upper survivors -> lower reentry -> exact cached Stage0.75 gate -> optional Mega topology reject -> exact radius-7 coarse score\n";

    std::int64_t* dSeeds = nullptr;
    int* dP20 = nullptr;
    int* dP20Gate = nullptr;
    int* dUpper = nullptr;
    int* dHigh = nullptr;
    unsigned char* dUpperMasks = nullptr;
    unsigned char* dHighestReentryY = nullptr;
    TerrainPerlinCacheBuffers terrainCache;
    ClimatePerlinCacheBuffers climateCache;
    ColumnShapeCacheBuffers shapeCache;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(capacity) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20), static_cast<std::size_t>(capacity) * sizeof(int)));
    if (p20MinCount > 1) {
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dP20Gate), static_cast<std::size_t>(capacity) * sizeof(int)));
    }
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpper), static_cast<std::size_t>(capacity) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHigh), static_cast<std::size_t>(capacity) * sizeof(int)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dUpperMasks), static_cast<std::size_t>(capacity) * stage0gpu::FULL_POINTS));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dHighestReentryY), static_cast<std::size_t>(capacity) * stage0gpu::FULL_POINTS));
    allocateTerrainPerlinCache(terrainCache, capacity);
    allocateClimatePerlinCache(climateCache, capacity);
    allocateColumnShapeCache(shapeCache, capacity);

    constexpr int coarseChunkCapacity = 1024;
    coarsegpu::Buffers coarseBuffers;
    allocateCoarseBuffers(coarseBuffers, coarseChunkCapacity);
    std::cerr << "Exact coarse micro-batch capacity: " << coarseChunkCapacity << " regions\n";
    std::cerr << "P38 exact cache reuse ON | first center stores 66 terrain + 10 climate states; later centers reload both\n";
    std::cerr << "P38 cached coarse ON | coarse survivors reload original world-indexed terrain + climate states\n";
    std::cerr << "P38 compact lower ON | dense 32-thread active-column batches\n";
    std::cerr << "P38 compact upper ON | 192 non-P20 columns, no idle P20 lanes\n";
    std::cerr << "P38 shared upper Y-axis ON | one exact 17-value setup per 3D octave/block\n";
    std::cerr << "P38 direct-write coarse ON | noise2/noise3 write exact Y values without lane-local 17-double arrays\n";
    std::cerr << "P38 parallel coarse scoring ON | exact 64-thread six-neighbour BFS\n";
    std::cerr << "Response telemetry: " << (researchMode ? "RESEARCH 34 bytes/region" : "LEAN 16 bytes/region + exact P19 double") << "\n";

    static constexpr std::size_t RESEARCH_RESPONSE_BYTES = 34;
    static constexpr std::size_t LEAN_RESPONSE_BYTES = 16;
    const std::size_t responseBytesPerRegion = researchMode ? RESEARCH_RESPONSE_BYTES : LEAN_RESPONSE_BYTES;
    std::vector<std::int64_t> seeds(capacity);
    std::vector<int> p20(capacity), upper(capacity), high(capacity), coarse(capacity, 0);
    std::vector<unsigned char> highestReentryY(static_cast<std::size_t>(capacity) * stage0gpu::FULL_POINTS);
    std::vector<unsigned char> p19Pass(capacity, 0);
    std::vector<unsigned char> megaTopologyRejected(capacity, 0);
    std::vector<unsigned char> p19Extreme(capacity, 0);
    std::vector<double> p19Scores(capacity, std::numeric_limits<double>::quiet_NaN());
    std::vector<p19native::MonsterFeatures> p19Features(capacity);
    std::vector<unsigned char> output(
            static_cast<std::size_t>(capacity) * centerCount * responseBytesPerRegion);
    std::vector<int> coarseIndices;
    coarseIndices.reserve(capacity / 8 + 64);
    std::vector<std::int64_t> coarseSeeds(coarseChunkCapacity);
    std::vector<int> coarseCacheSeedIndices(coarseChunkCapacity);
    std::vector<int> coarseScores(coarseChunkCapacity);

    const char magic8[8] = {'S','T','0','R','3','8','1','6'};
    std::cout.write(magic8, 8);
    std::cout.flush();

    std::uint64_t totalWorlds = 0, totalRegions = 0;
    std::uint64_t p20Passed = 0, stage1Passed = 0, stage05Passed = 0;
    std::uint64_t stage075Evaluated = 0, stage075Passed = 0, stage075Rejected = 0;
    std::uint64_t megaTopologyRejectedCount = 0;
    std::uint64_t coarseEvaluated = 0, coarsePassed = 0;

    try {
        while (true) {
            std::uint32_t count = 0;
            if (!tryReadLe32(std::cin, count)) break;
            if (count == 0) break;
            if (count > static_cast<std::uint32_t>(capacity)) {
                throw std::runtime_error("Stage0 stream batch exceeds configured world capacity");
            }
            for (std::uint32_t i = 0; i < count; ++i) seeds[i] = static_cast<std::int64_t>(readLe64(std::cin));
            if (!std::cin) throw std::runtime_error("truncated Stage0 stream seed batch");

            HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));

            for (int centerIndex = 0; centerIndex < centerCount; ++centerIndex) {
                const int coarseOffsetX = CENTER_CHUNK_X[centerIndex] * 4;
                const int coarseOffsetZ = CENTER_CHUNK_Z[centerIndex] * 4;
                const bool reloadTerrainCache = centerIndex != 0;
                const bool reloadClimateCache = centerIndex != 0;

                HIP_CHECK(hipMemset(dHigh, 0, static_cast<std::size_t>(count) * sizeof(int)));
                hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(p20gpu::POINTS), 0, 0,
                        dSeeds, static_cast<int>(count), dP20, nullptr,
                        terrainCache.permutations, terrainCache.offsets,
                        climateCache.permutations, climateCache.offsets,
                        dUpperMasks, shapeCache.d5, shapeCache.d7,
                        coarseOffsetX, coarseOffsetZ, reloadTerrainCache ? 1 : 0, reloadClimateCache ? 1 : 0);
                HIP_CHECK(hipGetLastError());
                int* dP20ForLater = dP20;
                if (p20MinCount > 1) {
                    const int gateThreads = 256;
                    const int gateBlocks = (static_cast<int>(count) + gateThreads - 1) / gateThreads;
                    hipLaunchKernelGGL(applyP20MinimumGateKernel, dim3(gateBlocks), dim3(gateThreads), 0, 0,
                            dP20, dP20Gate, static_cast<int>(count), p20MinCount);
                    HIP_CHECK(hipGetLastError());
                    dP20ForLater = dP20Gate;
                }
                hipLaunchKernelGGL(p33y::stage0UpperCompact192SharedYKernel, dim3(count), dim3(p33y::THREADS), 0, 0,
                        dSeeds, static_cast<int>(count), dP20ForLater, dUpper, dUpperMasks, nullptr,
                        terrainCache.permutations, terrainCache.offsets,
                        climateCache.permutations, climateCache.offsets,
                        shapeCache.d5, shapeCache.d7,
                        coarseOffsetX, coarseOffsetZ);
                HIP_CHECK(hipGetLastError());
                hipLaunchKernelGGL(p30lower::stage0LowerCompactKernel<32>, dim3(count), dim3(32), 0, 0,
                        dSeeds, static_cast<int>(count), dP20ForLater, dUpper, upperMinCount, dUpperMasks, dHigh, dHighestReentryY, nullptr,
                        terrainCache.permutations, terrainCache.offsets, shapeCache.d5, shapeCache.d7,
                        coarseOffsetX, coarseOffsetZ, nullptr);
                HIP_CHECK(hipGetLastError());

                HIP_CHECK(hipMemcpy(p20.data(), dP20, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
                HIP_CHECK(hipMemcpy(upper.data(), dUpper, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
                HIP_CHECK(hipMemcpy(high.data(), dHigh, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));
                HIP_CHECK(hipMemcpy(highestReentryY.data(), dHighestReentryY,
                        static_cast<std::size_t>(count) * stage0gpu::FULL_POINTS, hipMemcpyDeviceToHost));

                std::fill(coarse.begin(), coarse.begin() + count, 0);
                std::fill(p19Pass.begin(), p19Pass.begin() + count, 0);
                std::fill(megaTopologyRejected.begin(), megaTopologyRejected.begin() + count, 0);
                std::fill(p19Extreme.begin(), p19Extreme.begin() + count, 0);
                std::fill(p19Scores.begin(), p19Scores.begin() + count, std::numeric_limits<double>::quiet_NaN());
                std::fill(p19Features.begin(), p19Features.begin() + count, p19native::MonsterFeatures{});
                coarseIndices.clear();

                for (std::uint32_t i = 0; i < count; ++i) {
                    if (!(p20[i] >= p20MinCount && upper[i] >= upperMinCount && high[i] >= highMinCount)) continue;
                    const unsigned char* topology = highestReentryY.data()
                            + static_cast<std::size_t>(i) * stage0gpu::FULL_POINTS;
                    const p19native::MonsterFeatures features = buildMonsterFeatures(topology);
                    const double score = p19native::score(upper[i], features);
                    const bool extreme = p19native::hasExtremeTopologySignal(features);
                    const bool pass = extreme || score >= profileConfig.p19MinScore;
                    const bool megaReject = pass && profileConfig.megaTopologyFilter
                            && megaTopologyReject(score, extreme, features);
                    p19Features[i] = features;
                    p19Scores[i] = score;
                    p19Extreme[i] = extreme ? 1u : 0u;
                    p19Pass[i] = pass ? 1u : 0u;
                    megaTopologyRejected[i] = megaReject ? 1u : 0u;
                    if (pass && !megaReject) coarseIndices.push_back(static_cast<int>(i));
                }

                for (std::size_t offset = 0; offset < coarseIndices.size(); offset += coarseChunkCapacity) {
                    const int chunk = static_cast<int>(std::min<std::size_t>(coarseChunkCapacity, coarseIndices.size() - offset));
                    for (int j = 0; j < chunk; ++j) {
                        const int originalIndex = coarseIndices[offset + j];
                        coarseSeeds[j] = seeds[originalIndex];
                        coarseCacheSeedIndices[j] = originalIndex;
                    }
                    runCoarseScoresCachedAt(
                            coarseBuffers, coarseSeeds.data(), coarseCacheSeedIndices.data(), chunk,
                            coarseScores.data(), coarseOffsetX, coarseOffsetZ, terrainCache, climateCache);
                    for (int j = 0; j < chunk; ++j) coarse[coarseIndices[offset + j]] = coarseScores[j];
                }

                for (std::uint32_t i = 0; i < count; ++i) {
                    const int a = p20[i];
                    const int b = upper[i];
                    const int c = high[i];
                    const int d = coarse[i];
                    const unsigned char e = p19Pass[i];
                    const p19native::MonsterFeatures& f = p19Features[i];
                    const std::size_t regionIndex = static_cast<std::size_t>(i) * centerCount + centerIndex;
                    const std::size_t base = regionIndex * responseBytesPerRegion;
                    auto putU16 = [&](std::size_t offset, int value) {
                        const unsigned int v = static_cast<unsigned int>(value < 0 ? 0 : (value > 65535 ? 65535 : value));
                        output[base + offset] = static_cast<unsigned char>(v & 0xFFu);
                        output[base + offset + 1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
                    };
                    auto putU64 = [&](std::size_t offset, std::uint64_t value) {
                        for (int byte = 0; byte < 8; ++byte) {
                            output[base + offset + static_cast<std::size_t>(byte)] = static_cast<unsigned char>(value >> (byte * 8));
                        }
                    };
                    output[base] = static_cast<unsigned char>(a < 0 ? 0 : (a > 255 ? 255 : a));
                    output[base + 1] = static_cast<unsigned char>(b < 0 ? 0 : (b > 255 ? 255 : b));
                    output[base + 2] = static_cast<unsigned char>(c < 0 ? 0 : (c > 255 ? 255 : c));
                    output[base + 3] = e;
                    output[base + 4] = static_cast<unsigned char>(d & 0xFF);
                    output[base + 5] = static_cast<unsigned char>((d >> 8) & 0xFF);
                    output[base + 6] = p19Extreme[i];
                    output[base + 7] = megaTopologyRejected[i];
                    if (researchMode) {
                        std::uint64_t scoreBits = 0;
                        static_assert(sizeof(scoreBits) == sizeof(double), "double must be 64-bit");
                        std::memcpy(&scoreBits, &p19Scores[i], sizeof(scoreBits));
                        putU64(8, scoreBits);
                        putU16(16, f.stage0FullY88);
                        putU16(18, f.stage0FullY96);
                        putU16(20, f.stage0FullY104);
                        putU16(22, f.stage0FullY112);
                        putU16(24, f.stage0Y88LargestCluster);
                        output[base + 26] = static_cast<unsigned char>(f.stage0Y88Width);
                        output[base + 27] = static_cast<unsigned char>(f.stage0Y88Depth);
                        output[base + 28] = f.stage0Y88TouchesBorder ? 1u : 0u;
                        putU16(29, f.stage0Y96LargestCluster);
                        output[base + 31] = static_cast<unsigned char>(f.stage0Y96Width);
                        output[base + 32] = static_cast<unsigned char>(f.stage0Y96Depth);
                        output[base + 33] = f.stage0Y96TouchesBorder ? 1u : 0u;
                    } else {
                        std::uint64_t scoreBits = 0;
                        static_assert(sizeof(scoreBits) == sizeof(double), "double must be 64-bit");
                        std::memcpy(&scoreBits, &p19Scores[i], sizeof(scoreBits));
                        putU64(8, scoreBits);
                    }

                    if (a >= p20MinCount) ++p20Passed;
                    if (a >= p20MinCount && b >= upperMinCount) ++stage1Passed;
                    if (a >= p20MinCount && b >= upperMinCount && c >= highMinCount) {
                        ++stage05Passed;
                        ++stage075Evaluated;
                        if (e != 0) {
                            ++stage075Passed;
                            if (megaTopologyRejected[i] != 0) {
                                ++megaTopologyRejectedCount;
                            } else {
                                ++coarseEvaluated;
                                if (d >= profileConfig.coarseMinCells) ++coarsePassed;
                            }
                        } else {
                            ++stage075Rejected;
                        }
                    }
                }
            }

            writeLe32(std::cout, count);
            std::cout.write(reinterpret_cast<const char*>(output.data()),
                    static_cast<std::streamsize>(count) * centerCount
                            * static_cast<std::streamsize>(responseBytesPerRegion));
            std::cout.flush();
            if (!std::cout) {
                std::cerr << "Stage0+P19+coarse stream parent closed response pipe; exiting cleanly\n";
                break;
            }
            totalWorlds += count;
            totalRegions += static_cast<std::uint64_t>(count) * centerCount;
        }
    } catch (...) {
        freeCoarseBuffers(coarseBuffers);
        freeTerrainPerlinCache(terrainCache);
        freeClimatePerlinCache(climateCache);
        freeColumnShapeCache(shapeCache);
        hipFree(dSeeds); hipFree(dP20); if (dP20Gate) hipFree(dP20Gate); hipFree(dUpper); hipFree(dHigh); hipFree(dUpperMasks); hipFree(dHighestReentryY);
        throw;
    }

    freeCoarseBuffers(coarseBuffers);
    freeTerrainPerlinCache(terrainCache);
    freeClimatePerlinCache(climateCache);
    freeColumnShapeCache(shapeCache);
    hipFree(dSeeds); hipFree(dP20); if (dP20Gate) hipFree(dP20Gate); hipFree(dUpper); hipFree(dHigh); hipFree(dUpperMasks); hipFree(dHighestReentryY);
    std::cerr << "Stage0+P19+coarse stream closed | worlds=" << totalWorlds
              << " | regions=" << totalRegions
              << " | P20Passed=" << p20Passed
              << " | Stage1Passed=" << stage1Passed
              << " | Stage0.5Passed=" << stage05Passed
              << " | Stage0.75Evaluated=" << stage075Evaluated
              << " | Stage0.75Passed=" << stage075Passed
              << " | Stage0.75Rejected=" << stage075Rejected
              << " | MegaTopologyRejected=" << megaTopologyRejectedCount
              << " | CoarseEvaluated=" << coarseEvaluated
              << " | CoarsePassedTarget=" << coarsePassed << "\n";
    return 0;
}
int streamFilter(int capacity) {
    if (capacity < 1) throw std::runtime_error("stream capacity must be >= 1");

#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) throw std::runtime_error("failed to set stdin binary mode");
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) throw std::runtime_error("failed to set stdout binary mode");
#endif

    printDevice(std::cerr);
    std::cerr << "P20 stream capacity: " << capacity << " seeds\n";

    std::int64_t* dSeeds = nullptr;
    int* dCounts = nullptr;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dSeeds), static_cast<std::size_t>(capacity) * sizeof(std::int64_t)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dCounts), static_cast<std::size_t>(capacity) * sizeof(int)));

    std::vector<std::int64_t> seeds(capacity);
    std::vector<int> counts(capacity);
    std::vector<unsigned char> output(capacity);

    const char magic[8] = {'P','2','0','S','T','R','0','1'};
    std::cout.write(magic, 8);
    std::cout.flush();

    std::uint64_t totalSeeds = 0;
    std::uint64_t totalPassed = 0;
    try {
        while (true) {
            std::uint32_t count = 0;
            if (!tryReadLe32(std::cin, count)) break;
            if (count == 0) break;
            if (count > static_cast<std::uint32_t>(capacity)) {
                throw std::runtime_error("stream batch exceeds configured capacity");
            }

            for (std::uint32_t i = 0; i < count; ++i) {
                seeds[i] = static_cast<std::int64_t>(readLe64(std::cin));
            }
            if (!std::cin) throw std::runtime_error("truncated stream seed batch");

            HIP_CHECK(hipMemcpy(dSeeds, seeds.data(), static_cast<std::size_t>(count) * sizeof(std::int64_t), hipMemcpyHostToDevice));
            hipLaunchKernelGGL(p20gpu::p20KernelFromSeeds, dim3(count), dim3(64), 0, 0,
                               dSeeds, static_cast<int>(count), dCounts, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0);
            HIP_CHECK(hipGetLastError());
            HIP_CHECK(hipMemcpy(counts.data(), dCounts, static_cast<std::size_t>(count) * sizeof(int), hipMemcpyDeviceToHost));

            std::uint64_t passed = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                const int value = counts[i];
                output[i] = static_cast<unsigned char>(value < 0 ? 0 : (value > 255 ? 255 : value));
                if (value > 0) ++passed;
            }

            writeLe32(std::cout, count);
            std::cout.write(reinterpret_cast<const char*>(output.data()), count);
            std::cout.flush();
            if (!std::cout) throw std::runtime_error("failed writing stream response");

            totalSeeds += count;
            totalPassed += passed;
        }
    } catch (...) {
        hipFree(dSeeds);
        hipFree(dCounts);
        throw;
    }

    hipFree(dSeeds);
    hipFree(dCounts);
    std::cerr << "P20 stream closed | seeds=" << totalSeeds << " | passed=" << totalPassed << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "Usage:\n"
                      << "  gpu_p20_benchmark validate <reference.bin>\n"
                      << "  gpu_p20_benchmark bench [count=100000] [sequenceSeed=123456789] [iterations=3]\n"
                      << "  gpu_p20_benchmark stream [capacity=32768]\n"
                      << "  gpu_p20_benchmark stage0validate <reference.bin>\n"
                      << "  gpu_p20_benchmark stage0optvalidate <reference.bin>\n"
                      << "  gpu_p20_benchmark stage0bench [count=100000] [sequenceSeed=123456789] [iterations=3]\n"
                      << "  gpu_p20_benchmark stage0benchlegacy [count=100000] [sequenceSeed=123456789] [iterations=3]\n"
                      << "  gpu_p20_benchmark coarsevalidate <reference.bin>\n"
                      << "  gpu_p20_benchmark coarseprofile <reference.bin> [iterations=3]\n"
                      << "  gpu_p20_benchmark noisedetail <reference.bin> [iterations=3]\n"
                      << "  gpu_p20_benchmark pipelineprofile [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [baseline|shape|full]\n"
                      << "  gpu_p20_benchmark multicenterbench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p28cachebench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p29coarsebench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p30lowerbench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p31upperbench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p32fusionbench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p33ybench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p34permbench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p35directcoarsebench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark p36scorebench [count=32768] [sequenceSeed=123456789] [iterations=5] [general|mega] [spacingChunks=15]\n"
                      << "  gpu_p20_benchmark earlyprofile [count=32768] [sequenceSeed=123456789] [iterations=5]\n"
                      << "  gpu_p20_benchmark initprofile [count=32768] [sequenceSeed=123456789] [iterations=5]\n"
                      << "  gpu_p20_benchmark stage0stream [capacity=32768] [general|mega|record60|record80] [research|lean12] [centers=1|8]\n";
            return 1;
        }
        const std::string mode = argv[1];
        if (mode == "stream") {
            const int capacity = argc > 2 ? std::stoi(argv[2]) : 32768;
            return streamFilter(capacity);
        }
        if (mode == "stage0stream") {
            const int capacity = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::string profile = argc > 3 ? argv[3] : "general";
            const std::string telemetry = argc > 4 ? argv[4] : "research";
            const int centers = argc > 5 ? std::stoi(argv[5]) : 1;
            const HuntProfile huntProfile = parseHuntProfile(profile);
            if (telemetry != "research" && telemetry != "lean") {
                throw std::runtime_error("stage0stream telemetry must be research or lean");
            }
            return streamStage0Filter(capacity, huntProfile, telemetry == "research", centers);
        }

        printDevice();
        if (mode == "validate") {
            if (argc < 3) throw std::runtime_error("validate requires reference file path");
            return validate(argv[2]);
        }
        if (mode == "bench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 100000;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 3;
            return benchmark(count, sequenceSeed, iterations);
        }
        if (mode == "stage0validate") {
            if (argc < 3) throw std::runtime_error("stage0validate requires reference file path");
            return validateStage0(argv[2], false);
        }
        if (mode == "stage0optvalidate") {
            if (argc < 3) throw std::runtime_error("stage0optvalidate requires reference file path");
            return validateStage0(argv[2], true);
        }
        if (mode == "coarsevalidate") {
            if (argc < 3) throw std::runtime_error("coarsevalidate requires reference file path");
            return validateCoarse(argv[2]);
        }
        if (mode == "coarseprofile") {
            if (argc < 3) throw std::runtime_error("coarseprofile requires reference file path");
            const int iterations = argc > 3 ? std::stoi(argv[3]) : 3;
            return profileCoarse(argv[2], iterations);
        }
        if (mode == "noisedetail") {
            if (argc < 3) throw std::runtime_error("noisedetail requires reference file path");
            const int iterations = argc > 3 ? std::stoi(argv[3]) : 3;
            return profileCoarse(argv[2], iterations, true);
        }
        if (mode == "pipelineprofile") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "general";
            const std::string cache = argc > 6 ? argv[6] : "full";
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("pipelineprofile profile must be general or mega");
            }
            P24ColumnCacheMode cacheMode;
            if (cache == "baseline") cacheMode = P24ColumnCacheMode::BASELINE;
            else if (cache == "shape") cacheMode = P24ColumnCacheMode::SHAPE_ONLY;
            else if (cache == "full") cacheMode = P24ColumnCacheMode::FULL_REUSE;
            else throw std::runtime_error("pipelineprofile cache mode must be baseline, shape, or full");
            return profileProductionPipeline(count, sequenceSeed, iterations, profile == "mega", cacheMode);
        }
        if (mode == "multicenterbench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "general";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("multicenterbench profile must be general or mega");
            }
            return benchmarkMultiCenterCoverage(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p28cachebench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p28cachebench profile must be general or mega");
            }
            return benchmarkP28CacheReuse(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p29coarsebench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p29coarsebench profile must be general or mega");
            }
            return benchmarkP29SelectorCoarse(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p30lowerbench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p30lowerbench profile must be general or mega");
            }
            return benchmarkP30CompactLower(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p31upperbench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p31upperbench profile must be general or mega");
            }
            return benchmarkP31CompactUpper(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p32fusionbench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p32fusionbench profile must be general or mega");
            }
            return benchmarkP32FusedP20Upper(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p33ybench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p33ybench profile must be general or mega");
            }
            return benchmarkP33SharedYAxis(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p34permbench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p34permbench profile must be general or mega");
            }
            return benchmarkP34CompactPermutation(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p35directcoarsebench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p35directcoarsebench profile must be general or mega");
            }
            return benchmarkP35DirectCoarse(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "p36scorebench") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            const std::string profile = argc > 5 ? argv[5] : "mega";
            const int spacingChunks = argc > 6 ? std::stoi(argv[6]) : 15;
            if (profile != "general" && profile != "mega") {
                throw std::runtime_error("p36scorebench profile must be general or mega");
            }
            return benchmarkP36ParallelCoarseScore(
                    count, sequenceSeed, iterations, profile == "mega", spacingChunks);
        }
        if (mode == "earlyprofile" || mode == "initprofile") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 32768;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 5;
            return profileEarlyKernels(count, sequenceSeed, iterations, mode == "initprofile");
        }
        if (mode == "p19validate") {
            if (argc < 3) throw std::runtime_error("p19validate requires reference file path");
            return validateP19(argv[2]);
        }
        if (mode == "stage0bench" || mode == "stage0benchlegacy") {
            const int count = argc > 2 ? std::stoi(argv[2]) : 100000;
            const std::uint64_t sequenceSeed = argc > 3 ? std::stoull(argv[3]) : 123456789ULL;
            const int iterations = argc > 4 ? std::stoi(argv[4]) : 3;
            return benchmarkStage0(count, sequenceSeed, iterations, mode == "stage0bench");
        }
        throw std::runtime_error("unknown mode: " + mode);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
