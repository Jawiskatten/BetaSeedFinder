#pragma once

#include "gpu_runtime_compat.hpp"

#include "p20_exact_math.hpp"

#include <cstddef>
#include <cstdint>

namespace initprofile {

static constexpr int GROUP_NOISE2 = 0;
static constexpr int GROUP_NOISE3 = 1;
static constexpr int GROUP_NOISE1_ACTIVE = 2;
static constexpr int GROUP_NOISE1_SKIP = 3;
static constexpr int GROUP_NOISE4 = 4;
static constexpr int GROUP_NOISE5 = 5;
static constexpr int GROUPS = 6;

static constexpr int COMP_ENTRY_SYNC = 0;
static constexpr int COMP_OFFSET_RNG = 1;
static constexpr int COMP_IDENTITY_FILL = 2;
static constexpr int COMP_FISHER_YATES = 3;
static constexpr int COMP_EXIT_SYNC = 4;
static constexpr int COMPONENTS = 5;

static constexpr int TERRAIN_INIT_CALLS = 74;

__device__ __forceinline__ std::size_t tickIndex(int seedIndex, int group, int component) {
    return (static_cast<std::size_t>(seedIndex) * GROUPS + static_cast<std::size_t>(group))
            * COMPONENTS + static_cast<std::size_t>(component);
}

__device__ __forceinline__ bool stageActive(
        int stageId,
        int seedIndex,
        const int* p20Counts,
        const int* upperCounts
) {
    if (stageId == 0) return true;
    if (stageId == 1) return p20Counts[seedIndex] > 0;
    return p20Counts[seedIndex] > 0 && upperCounts[seedIndex] >= 5;
}

__device__ __forceinline__ unsigned long long doubleBits(double value) {
    union Bits {
        double d;
        unsigned long long u;
    } bits;
    bits.d = value;
    return bits.u;
}

__device__ __forceinline__ unsigned long long stateHash(
        const p20::JavaRandom& rng,
        const p20::PerlinState& perlin
) {
    unsigned long long h = 1469598103934665603ULL;
    h = (h ^ rng.state) * 1099511628211ULL;
    h = (h ^ doubleBits(perlin.a)) * 1099511628211ULL;
    h = (h ^ doubleBits(perlin.b)) * 1099511628211ULL;
    h = (h ^ doubleBits(perlin.c)) * 1099511628211ULL;
    for (int i = 0; i < 512; ++i) {
        h = (h ^ static_cast<unsigned long long>(static_cast<unsigned int>(perlin.perm[i])))
                * 1099511628211ULL;
    }
    return h;
}

__device__ __forceinline__ void addTick(
        unsigned long long* ticks,
        int seedIndex,
        int group,
        int component,
        unsigned long long value,
        int lane
) {
    if (lane == 0) ticks[tickIndex(seedIndex, group, component)] += value;
}

// Exact initPerlin body with coarse-grained timers around the four pieces that can
// plausibly dominate setup. The two barriers match the setup window used by the
// early-kernel detail profiler, so these component shares can be scaled directly
// to its measured production setup milliseconds.
__device__ __forceinline__ void profiledInitCall(
        p20::JavaRandom& random,
        p20::PerlinState& out,
        unsigned long long* ticks,
        int seedIndex,
        int group
) {
    const int lane = threadIdx.x;
    unsigned long long t0 = 0ULL;
    unsigned long long t1 = 0ULL;
    unsigned long long t2 = 0ULL;
    unsigned long long t3 = 0ULL;
    unsigned long long t4 = 0ULL;

    if (lane == 0) t0 = wall_clock64();
    __syncthreads();

    if (lane == 0) {
        t1 = wall_clock64();

        out.a = random.nextDouble() * 256.0;
        out.b = random.nextDouble() * 256.0;
        out.c = random.nextDouble() * 256.0;
        t2 = wall_clock64();

        for (int i = 0; i < 256; ++i) out.perm[i] = i;
        t3 = wall_clock64();

        for (int i = 0; i < 256; ++i) {
            const int j = random.nextInt(256 - i) + i;
            const int tmp = out.perm[i];
            out.perm[i] = out.perm[j];
            out.perm[j] = tmp;
            out.perm[i + 256] = out.perm[i];
        }
        t4 = wall_clock64();
    }

    __syncthreads();

    if (lane == 0) {
        const unsigned long long t5 = wall_clock64();
        addTick(ticks, seedIndex, group, COMP_ENTRY_SYNC, t1 - t0, lane);
        addTick(ticks, seedIndex, group, COMP_OFFSET_RNG, t2 - t1, lane);
        addTick(ticks, seedIndex, group, COMP_IDENTITY_FILL, t3 - t2, lane);
        addTick(ticks, seedIndex, group, COMP_FISHER_YATES, t4 - t3, lane);
        addTick(ticks, seedIndex, group, COMP_EXIT_SYNC, t5 - t4, lane);
    }
}

// The eight skipped noise1 octaves are consumed back-to-back by lane 0 in the
// production profiler. There is no per-call barrier in that window, so profile
// the exact same shape and intentionally charge zero sync time to this subgroup.
__device__ __forceinline__ void profiledSkipGroup(
        p20::JavaRandom& random,
        p20::PerlinState& out,
        unsigned long long* ticks,
        int seedIndex
) {
    const int lane = threadIdx.x;
    if (lane == 0) {
        unsigned long long offsetTicks = 0ULL;
        unsigned long long identityTicks = 0ULL;
        unsigned long long shuffleTicks = 0ULL;
        for (int call = 0; call < 8; ++call) {
            const unsigned long long t0 = wall_clock64();
            out.a = random.nextDouble() * 256.0;
            out.b = random.nextDouble() * 256.0;
            out.c = random.nextDouble() * 256.0;
            const unsigned long long t1 = wall_clock64();

            for (int i = 0; i < 256; ++i) out.perm[i] = i;
            const unsigned long long t2 = wall_clock64();

            for (int i = 0; i < 256; ++i) {
                const int j = random.nextInt(256 - i) + i;
                const int tmp = out.perm[i];
                out.perm[i] = out.perm[j];
                out.perm[j] = tmp;
                out.perm[i + 256] = out.perm[i];
            }
            const unsigned long long t3 = wall_clock64();
            offsetTicks += t1 - t0;
            identityTicks += t2 - t1;
            shuffleTicks += t3 - t2;
        }
        addTick(ticks, seedIndex, GROUP_NOISE1_SKIP, COMP_OFFSET_RNG, offsetTicks, lane);
        addTick(ticks, seedIndex, GROUP_NOISE1_SKIP, COMP_IDENTITY_FILL, identityTicks, lane);
        addTick(ticks, seedIndex, GROUP_NOISE1_SKIP, COMP_FISHER_YATES, shuffleTicks, lane);
    }
    __syncthreads();
}

__device__ __forceinline__ void referenceInitCall(
        p20::JavaRandom& random,
        p20::PerlinState& out
) {
    const int lane = threadIdx.x;
    __syncthreads();
    if (lane == 0) p20::initPerlin(random, out);
    __syncthreads();
}

__global__ void referenceSequenceKernel(
        const std::int64_t* seeds,
        int count,
        int stageId,
        const int* p20Counts,
        const int* upperCounts,
        unsigned long long* hashes
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count) return;

    if (!stageActive(stageId, seedIndex, p20Counts, upperCounts)) {
        if (lane == 0) hashes[seedIndex] = 0ULL;
        return;
    }

    __shared__ p20::JavaRandom rng;
    __shared__ p20::PerlinState perlin;
    if (lane == 0) rng.setSeed(seeds[seedIndex]);
    __syncthreads();

    for (int i = 0; i < 16; ++i) referenceInitCall(rng, perlin);
    for (int i = 0; i < 16; ++i) referenceInitCall(rng, perlin);
    for (int i = 0; i < 8; ++i) referenceInitCall(rng, perlin);

    if (lane == 0) {
        for (int i = 0; i < 8; ++i) p20::initPerlin(rng, perlin);
    }
    __syncthreads();

    for (int i = 0; i < 10; ++i) referenceInitCall(rng, perlin);
    for (int i = 0; i < 16; ++i) referenceInitCall(rng, perlin);

    if (lane == 0) hashes[seedIndex] = stateHash(rng, perlin);
}

__global__ void profileSequenceKernel(
        const std::int64_t* seeds,
        int count,
        int stageId,
        const int* p20Counts,
        const int* upperCounts,
        unsigned long long* ticks,
        unsigned long long* hashes
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count) return;

    if (!stageActive(stageId, seedIndex, p20Counts, upperCounts)) {
        if (lane == 0) hashes[seedIndex] = 0ULL;
        return;
    }

    if (lane == 0) {
        const std::size_t base = static_cast<std::size_t>(seedIndex) * GROUPS * COMPONENTS;
        for (int i = 0; i < GROUPS * COMPONENTS; ++i) ticks[base + i] = 0ULL;
    }
    __syncthreads();

    __shared__ p20::JavaRandom rng;
    __shared__ p20::PerlinState perlin;
    if (lane == 0) rng.setSeed(seeds[seedIndex]);
    __syncthreads();

    for (int i = 0; i < 16; ++i) profiledInitCall(rng, perlin, ticks, seedIndex, GROUP_NOISE2);
    for (int i = 0; i < 16; ++i) profiledInitCall(rng, perlin, ticks, seedIndex, GROUP_NOISE3);
    for (int i = 0; i < 8; ++i) profiledInitCall(rng, perlin, ticks, seedIndex, GROUP_NOISE1_ACTIVE);
    profiledSkipGroup(rng, perlin, ticks, seedIndex);
    for (int i = 0; i < 10; ++i) profiledInitCall(rng, perlin, ticks, seedIndex, GROUP_NOISE4);
    for (int i = 0; i < 16; ++i) profiledInitCall(rng, perlin, ticks, seedIndex, GROUP_NOISE5);

    if (lane == 0) hashes[seedIndex] = stateHash(rng, perlin);
}

} // namespace initprofile
