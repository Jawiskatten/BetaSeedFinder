#pragma once

#include "gpu_runtime_compat.hpp"

#include "p20_exact_math.hpp"
#include "stage0_exact_gpu.hpp"

#include <cstddef>
#include <cstdint>

namespace earlyprofile {

static constexpr int PHASE_CLIMATE = 0;
static constexpr int PHASE_NOISE2_SETUP = 1;
static constexpr int PHASE_NOISE2_EVAL = 2;
static constexpr int PHASE_NOISE3_SETUP = 3;
static constexpr int PHASE_NOISE3_EVAL = 4;
static constexpr int PHASE_NOISE1_SETUP = 5;
static constexpr int PHASE_NOISE1_EVAL = 6;
static constexpr int PHASE_NOISE45_SETUP = 7;
static constexpr int PHASE_NOISE45_EVAL = 8;
static constexpr int PHASE_TERRAIN_REDUCE = 9;
static constexpr int PHASES = 10;

__device__ __forceinline__ std::size_t phaseIndex(int seedIndex, int phase) {
    return static_cast<std::size_t>(seedIndex) * PHASES + static_cast<std::size_t>(phase);
}

__device__ __forceinline__ void zeroProfileRow(unsigned long long* phaseTicks, int seedIndex, int lane) {
    if (lane == 0) {
        const std::size_t base = static_cast<std::size_t>(seedIndex) * PHASES;
        for (int phase = 0; phase < PHASES; ++phase) phaseTicks[base + phase] = 0ULL;
    }
}

__device__ __forceinline__ void writePhase(
        unsigned long long* phaseTicks,
        int seedIndex,
        int phase,
        unsigned long long ticks,
        int lane
) {
    if (lane == 0) phaseTicks[phaseIndex(seedIndex, phase)] = ticks;
}

// -----------------------------------------------------------------------------
// P20 detail profiler
// -----------------------------------------------------------------------------

static constexpr int P20_POINTS = 64;
static constexpr int P20_YCOUNT = 6;
static constexpr int P20_DENSITY_COUNT = P20_POINTS * P20_YCOUNT;

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

__device__ __forceinline__ void p20PointCoordinates(
        int lane,
        double& coarseX,
        double& coarseZ,
        double& climateX,
        double& climateZ
) {
    const int ix = lane >> 3;
    const int iz = lane & 7;
    const int gx = p20AxisValue(ix);
    const int gz = p20AxisValue(iz);
    coarseX = -28.0 + static_cast<double>(gx * 4);
    coarseZ = -28.0 + static_cast<double>(gz * 4);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
}

struct P20Scratch {
    p20::JavaRandom rng;
    p20::PerlinState perlin;
    double tempRaw[P20_POINTS];
    double rainRaw[P20_POINTS];
    double climateBlend[P20_POINTS];
    double temperature[P20_POINTS];
    double rain[P20_POINTS];
    double noise1[P20_DENSITY_COUNT];
    double noise2[P20_DENSITY_COUNT];
    double noise3[P20_DENSITY_COUNT];
    double noise4[P20_POINTS];
    double noise5[P20_POINTS];
    int positiveCount;
};

__device__ __forceinline__ void p20AccumulateSimplexGroup(
        P20Scratch& s,
        std::int64_t seed,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double octavePersistence,
        double climateX,
        double climateZ,
        double* out
) {
    const int lane = threadIdx.x;
    if (lane == 0) s.rng.setSeed(seed);
    out[lane] = 0.0;
    __syncthreads();

    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < octaves; ++octave) {
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        const double scaleX = (startScaleX / 1.5) * d7;
        const double scaleZ = (startScaleZ / 1.5) * d7;
        const double weight = 0.55 / d6;
        out[lane] += p20::simplex2(s.perlin, climateX * scaleX, climateZ * scaleZ) * weight;
        __syncthreads();
        d7 *= octaveScale;
        d6 *= octavePersistence;
    }
}

__global__ void p20ProfileKernel(
        const std::int64_t* seeds,
        int count,
        int* positiveCounts,
        unsigned long long* phaseTicks
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= P20_POINTS) return;

    zeroProfileRow(phaseTicks, seedIndex, lane);
    __syncthreads();

    __shared__ P20Scratch s;
    const std::int64_t seed = seeds[seedIndex];

    double coarseX, coarseZ, climateX, climateZ;
    p20PointCoordinates(lane, coarseX, coarseZ, climateX, climateZ);

    unsigned long long phaseStart = 0ULL;
    if (lane == 0) phaseStart = wall_clock64();
    __syncthreads();

    const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
    const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
    const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);

    p20AccumulateSimplexGroup(s, tempSeed, 4,
            0.02500000037252903, 0.02500000037252903, 0.25, 0.5,
            climateX, climateZ, s.tempRaw);
    p20AccumulateSimplexGroup(s, rainSeed, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333, 0.5,
            climateX, climateZ, s.rainRaw);
    p20AccumulateSimplexGroup(s, blendSeed, 2,
            0.25, 0.25, 0.5882352941176471, 0.5,
            climateX, climateZ, s.climateBlend);

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

    unsigned long long climateTicks = 0ULL;
    if (lane == 0) climateTicks = wall_clock64() - phaseStart;
    writePhase(phaseTicks, seedIndex, PHASE_CLIMATE, climateTicks, lane);
    __syncthreads();

    if (lane == 0) s.rng.setSeed(seed);
    for (int yi = 0; yi < P20_YCOUNT; ++yi) {
        const int idx = lane * P20_YCOUNT + yi;
        s.noise1[idx] = 0.0;
        s.noise2[idx] = 0.0;
        s.noise3[idx] = 0.0;
    }
    s.noise4[lane] = 0.0;
    s.noise5[lane] = 0.0;
    if (lane == 0) s.positiveCount = 0;
    __syncthreads();

    unsigned long long setupTicks = 0ULL;
    unsigned long long evalTicks = 0ULL;

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        double values[P20_YCOUNT];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < P20_YCOUNT; ++yi) {
            const int idx = lane * P20_YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise2[idx] = value;
            else s.noise2[idx] += value;
        }
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE2_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE2_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        double values[P20_YCOUNT];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < P20_YCOUNT; ++yi) {
            const int idx = lane * P20_YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise3[idx] = value;
            else s.noise3[idx] += value;
        }
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE3_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE3_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        double values[P20_YCOUNT];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < P20_YCOUNT; ++yi) {
            const int idx = lane * P20_YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise1[idx] = value;
            else s.noise1[idx] += value;
        }
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    if (lane == 0) {
        const unsigned long long skipStart = wall_clock64();
        for (int i = 0; i < 8; ++i) p20::consumePerlin(s.rng, s.perlin);
        setupTicks += wall_clock64() - skipStart;
    }
    __syncthreads();
    writePhase(phaseTicks, seedIndex, PHASE_NOISE1_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE1_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = 1.121 * amplitude;
        const double sz = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
        if (octave == 0) s.noise4[lane] = value;
        else s.noise4[lane] += value;
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = 200.0 * amplitude;
        const double sz = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
        if (octave == 0) s.noise5[lane] = value;
        else s.noise5[lane] += value;
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE45_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE45_EVAL, evalTicks, lane);
    __syncthreads();

    phaseStart = 0ULL;
    if (lane == 0) phaseStart = wall_clock64();
    __syncthreads();

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

    bool positive = false;
    for (int yi = 0; yi < P20_YCOUNT; ++yi) {
        const int y = 11 + yi;
        const int idx = lane * P20_YCOUNT + yi;
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
        if (d8 > 0.0) positive = true;
    }

    if (positive) atomicAdd(&s.positiveCount, 1);
    __syncthreads();
    if (lane == 0) {
        positiveCounts[seedIndex] = s.positiveCount;
        writePhase(phaseTicks, seedIndex, PHASE_TERRAIN_REDUCE,
                wall_clock64() - phaseStart, lane);
    }
}

// -----------------------------------------------------------------------------
// Upper 256-column detail profiler
// -----------------------------------------------------------------------------

__global__ void upperProfileKernel(
        const std::int64_t* seeds,
        int count,
        const int* p20Counts,
        int* fullUpperCounts,
        unsigned char* upperMasks,
        unsigned long long* phaseTicks
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= stage0gpu::FULL_POINTS) return;

    zeroProfileRow(phaseTicks, seedIndex, lane);
    __syncthreads();

    if (p20Counts[seedIndex] <= 0) {
        if (lane == 0) fullUpperCounts[seedIndex] = 0;
        return;
    }

    __shared__ stage0gpu::UpperScratch s;
#if defined(BSF_NVIDIA_CUDA)
    double noise5Local = 0.0;
#endif
    const std::int64_t seed = seeds[seedIndex];

    double coarseX, coarseZ, climateX, climateZ;
    stage0gpu::fullPointCoordinates(lane, coarseX, coarseZ, climateX, climateZ);

    unsigned long long phaseStart = 0ULL;
    if (lane == 0) phaseStart = wall_clock64();
    __syncthreads();

    const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
    const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
    const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);

    stage0gpu::accumulateSimplexShared(s, tempSeed, 4,
            0.02500000037252903, 0.02500000037252903, 0.25, 0.5,
            climateX, climateZ, s.temp);
    stage0gpu::accumulateSimplexShared(s, rainSeed, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333, 0.5,
            climateX, climateZ, s.rain);
    stage0gpu::accumulateSimplexShared(s, blendSeed, 2,
            0.25, 0.25, 0.5882352941176471, 0.5,
            climateX, climateZ, s.climateBlend);

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
    __syncthreads();

    unsigned long long climateTicks = 0ULL;
    if (lane == 0) climateTicks = wall_clock64() - phaseStart;
    writePhase(phaseTicks, seedIndex, PHASE_CLIMATE, climateTicks, lane);
    __syncthreads();

    if (lane == 0) s.rng.setSeed(seed);
    for (int yi = 0; yi < stage0gpu::UPPER_YCOUNT; ++yi) {
        const int idx = lane * stage0gpu::UPPER_YCOUNT + yi;
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
    if (lane == 0) s.fullUpperCount = 0;
    __syncthreads();

    unsigned long long setupTicks = 0ULL;
    unsigned long long evalTicks = 0ULL;

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        double values[stage0gpu::UPPER_YCOUNT];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < stage0gpu::UPPER_YCOUNT; ++yi) {
            const int idx = lane * stage0gpu::UPPER_YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise2[idx] = value;
            else s.noise2[idx] += value;
        }
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE2_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE2_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        double values[stage0gpu::UPPER_YCOUNT];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < stage0gpu::UPPER_YCOUNT; ++yi) {
            const int idx = lane * stage0gpu::UPPER_YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise3[idx] = value;
            else s.noise3[idx] += value;
        }
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE3_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE3_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        double values[stage0gpu::UPPER_YCOUNT];
        p20::perlin3Upper6(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
        for (int yi = 0; yi < stage0gpu::UPPER_YCOUNT; ++yi) {
            const int idx = lane * stage0gpu::UPPER_YCOUNT + yi;
            const double value = values[yi] * weight;
            if (octave == 0) s.noise1[idx] = value;
            else s.noise1[idx] += value;
        }
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    if (lane == 0) {
        const unsigned long long skipStart = wall_clock64();
        for (int i = 0; i < 8; ++i) p20::consumePerlin(s.rng, s.perlin);
        setupTicks += wall_clock64() - skipStart;
    }
    __syncthreads();
    writePhase(phaseTicks, seedIndex, PHASE_NOISE1_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE1_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = 1.121 * amplitude;
        const double sz = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
        if (octave == 0) s.noise4[lane] = value;
        else s.noise4[lane] += value;
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        const double sx = 200.0 * amplitude;
        const double sz = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        const double value = p20::perlin2(s.perlin, coarseX * sx, coarseZ * sz) * weight;
#if defined(BSF_NVIDIA_CUDA)
        if (octave == 0) noise5Local = value;
        else noise5Local += value;
#else
        if (octave == 0) s.noise5[lane] = value;
        else s.noise5[lane] += value;
#endif
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE45_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE45_EVAL, evalTicks, lane);
    __syncthreads();

    phaseStart = 0ULL;
    if (lane == 0) phaseStart = wall_clock64();
    __syncthreads();

    const double d2 = s.temp[lane];
    const double d3 = s.rain[lane] * d2;
    double d4 = 1.0 - d3;
    d4 *= d4;
    d4 *= d4;
    d4 = 1.0 - d4;

    double d5 = (s.noise4[lane] + 256.0) / 512.0;
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
    const double d7 = 17.0 / 2.0 + d6 * 4.0;

    unsigned char mask = 0;
    for (int yi = 0; yi < stage0gpu::UPPER_YCOUNT; ++yi) {
        const int y = stage0gpu::UPPER_Y_FROM + yi;
        const int idx = lane * stage0gpu::UPPER_YCOUNT + yi;
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
        if (d8 > 0.0) mask |= static_cast<unsigned char>(1u << yi);
    }

    upperMasks[static_cast<std::size_t>(seedIndex) * stage0gpu::FULL_POINTS + lane] = mask;
    if (mask != 0) atomicAdd(&s.fullUpperCount, 1);
    __syncthreads();

    if (lane == 0) {
        fullUpperCounts[seedIndex] = s.fullUpperCount;
        writePhase(phaseTicks, seedIndex, PHASE_TERRAIN_REDUCE,
                wall_clock64() - phaseStart, lane);
    }
}

// -----------------------------------------------------------------------------
// Lower reentry detail profiler
// -----------------------------------------------------------------------------

__global__ void lowerProfileKernel(
        const std::int64_t* seeds,
        int count,
        const int* p20Counts,
        const int* fullUpperCounts,
        const unsigned char* upperMasks,
        int* highReentryCounts,
        unsigned char* highestReentryY,
        unsigned long long* phaseTicks
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= stage0gpu::FULL_POINTS) return;

    zeroProfileRow(phaseTicks, seedIndex, lane);
    __syncthreads();

    if (p20Counts[seedIndex] <= 0 || fullUpperCounts[seedIndex] < 5) return;

    __shared__ stage0gpu::LowerScratch s;
    if (lane == 0) s.highReentryCount = 0;
    __syncthreads();

    unsigned long long phaseStart = 0ULL;
    if (lane == 0) phaseStart = wall_clock64();
    __syncthreads();

    const std::size_t topologyIndex = static_cast<std::size_t>(seedIndex) * stage0gpu::FULL_POINTS + lane;
    if (highestReentryY != nullptr) highestReentryY[topologyIndex] = 0xFFu;

    const unsigned char upperMask = upperMasks[topologyIndex];
    const bool candidate = upperMask != 0;
    const std::int64_t seed = seeds[seedIndex];

    double coarseX, coarseZ, climateX, climateZ;
    stage0gpu::fullPointCoordinates(lane, coarseX, coarseZ, climateX, climateZ);

    const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
    const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
    const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);

    double temp = stage0gpu::accumulateSimplexLocal(s, tempSeed, 4,
            0.02500000037252903, 0.02500000037252903, 0.25, 0.5,
            climateX, climateZ);
    double rain = stage0gpu::accumulateSimplexLocal(s, rainSeed, 4,
            0.05000000074505806, 0.05000000074505806, 0.3333333333333333, 0.5,
            climateX, climateZ);
    const double climateBlend = stage0gpu::accumulateSimplexLocal(s, blendSeed, 2,
            0.25, 0.25, 0.5882352941176471, 0.5,
            climateX, climateZ);

    {
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

    unsigned long long climateTicks = 0ULL;
    __syncthreads();
    if (lane == 0) climateTicks = wall_clock64() - phaseStart;
    writePhase(phaseTicks, seedIndex, PHASE_CLIMATE, climateTicks, lane);
    __syncthreads();

    if (lane == 0) s.rng.setSeed(seed);
    double noise3[stage0gpu::LOWER_YCOUNT];
    for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
        const int idx = lane * stage0gpu::LOWER_YCOUNT + y;
        if (candidate) {
            s.noise2[idx] = 0.0;
            s.blend[idx] = 0.0;
        }
        noise3[y] = 0.0;
    }
    __syncthreads();

    unsigned long long setupTicks = 0ULL;
    unsigned long long evalTicks = 0ULL;

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        if (candidate) {
            const double sx = 684.412 * amplitude;
            const double sy = 684.412 * amplitude;
            const double sz = 684.412 * amplitude;
            const double weight = 1.0 / amplitude;
            double values[stage0gpu::LOWER_YCOUNT];
            stage0gpu::perlin3Lower11(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
                const int idx = lane * stage0gpu::LOWER_YCOUNT + y;
                const double value = values[y] * weight;
                if (octave == 0) s.noise2[idx] = value;
                else s.noise2[idx] += value;
            }
        }
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE2_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE2_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        if (candidate) {
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
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE3_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE3_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
        if (candidate) {
            const double sx = (684.412 / 80.0) * amplitude;
            const double sy = (684.412 / 160.0) * amplitude;
            const double sz = (684.412 / 80.0) * amplitude;
            const double weight = 1.0 / amplitude;
            double values[stage0gpu::LOWER_YCOUNT];
            stage0gpu::perlin3Lower11(s.perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
                const int idx = lane * stage0gpu::LOWER_YCOUNT + y;
                const double value = values[y] * weight;
                if (octave == 0) s.blend[idx] = value;
                else s.blend[idx] += value;
            }
        }
        __syncthreads();
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    if (lane == 0) {
        const unsigned long long skipStart = wall_clock64();
        for (int i = 0; i < 8; ++i) p20::consumePerlin(s.rng, s.perlin);
        setupTicks += wall_clock64() - skipStart;
    }
    __syncthreads();
    writePhase(phaseTicks, seedIndex, PHASE_NOISE1_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE1_EVAL, evalTicks, lane);
    __syncthreads();

    setupTicks = 0ULL;
    evalTicks = 0ULL;
    double noise4 = 0.0;
    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
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
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }

    double noise5 = 0.0;
    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        unsigned long long detailStart = 0ULL;
        unsigned long long detailSetupEnd = 0ULL;
        if (lane == 0) detailStart = wall_clock64();
        __syncthreads();
        if (lane == 0) p20::initPerlin(s.rng, s.perlin);
        __syncthreads();
        if (lane == 0) {
            detailSetupEnd = wall_clock64();
            setupTicks += detailSetupEnd - detailStart;
        }
        __syncthreads();
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
        if (lane == 0) evalTicks += wall_clock64() - detailSetupEnd;
        __syncthreads();
        amplitude /= 2.0;
    }
    writePhase(phaseTicks, seedIndex, PHASE_NOISE45_SETUP, setupTicks, lane);
    writePhase(phaseTicks, seedIndex, PHASE_NOISE45_EVAL, evalTicks, lane);
    __syncthreads();

    phaseStart = 0ULL;
    if (lane == 0) phaseStart = wall_clock64();
    __syncthreads();

    if (candidate) {
        const double d2 = temp;
        const double d3 = rain * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;

        double d5 = (noise4 + 256.0) / 512.0;
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
        const double d7 = 17.0 / 2.0 + d6 * 4.0;

        std::uint32_t positiveMask = static_cast<std::uint32_t>(upperMask) << stage0gpu::UPPER_Y_FROM;
        for (int y = 0; y < stage0gpu::LOWER_YCOUNT; ++y) {
            const int idx = lane * stage0gpu::LOWER_YCOUNT + y;
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
            if (d8 > 0.0) positiveMask |= (1u << y);
        }

        const int highestReentryYIndexValue = stage0gpu::highestReentryYIndex(positiveMask);
        if (highestReentryY != nullptr) {
            highestReentryY[topologyIndex] = highestReentryYIndexValue < 0
                    ? 0xFFu
                    : static_cast<unsigned char>(highestReentryYIndexValue);
        }
        if (highestReentryYIndexValue >= stage0gpu::UPPER_Y_FROM) atomicAdd(&s.highReentryCount, 1);
    }
    __syncthreads();

    if (lane == 0) {
        highReentryCounts[seedIndex] = s.highReentryCount;
        writePhase(phaseTicks, seedIndex, PHASE_TERRAIN_REDUCE,
                wall_clock64() - phaseStart, lane);
    }
}

} // namespace earlyprofile
