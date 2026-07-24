#pragma once

#include "gpu_runtime_compat.hpp"

#include "p20_exact_math.hpp"

#include <cstddef>
#include <cstdint>

namespace p20shuffleprofile {

static constexpr int POINTS = 64;
static constexpr int SHUFFLE_STEPS = 256;
static constexpr int ACTIVE_INIT_CALLS = 66;
static constexpr int SKIPPED_INIT_CALLS = 8;
static constexpr int TOTAL_INIT_CALLS = ACTIVE_INIT_CALLS + SKIPPED_INIT_CALLS;

static constexpr int COMP_OFFSET_RNG = 0;
static constexpr int COMP_IDENTITY_FILL = 1;
static constexpr int COMP_NEXTINT_GENERATION = 2;
static constexpr int COMP_SWAP_PAIR = 3;
static constexpr int COMP_DUPLICATE_WRITE = 4;
static constexpr int COMPONENTS = 5;

static constexpr int COUNT_NEXTINT_CALLS = 0;
static constexpr int COUNT_RNG_DRAWS = 1;
static constexpr int COUNT_REJECTED_DRAWS = 2;
static constexpr int COUNT_RETRYING_CALLS = 3;
static constexpr int COUNT_POWER_OF_TWO_CALLS = 4;
static constexpr int COUNT_ACTIVE_STATES = 5;
static constexpr int COUNTERS = 6;

struct SharedScratch {
    p20::JavaRandom rng;
    p20::PerlinState perlin;
    int jValues[SHUFFLE_STEPS];
    int duplicateValues[SHUFFLE_STEPS];
    unsigned long long localTicks[COMPONENTS];
    unsigned long long localCounters[COUNTERS];
};

__device__ __forceinline__ std::size_t tickIndex(int seedIndex, int component) {
    return static_cast<std::size_t>(seedIndex) * COMPONENTS + static_cast<std::size_t>(component);
}

__device__ __forceinline__ std::size_t counterIndex(int seedIndex, int counter) {
    return static_cast<std::size_t>(seedIndex) * COUNTERS + static_cast<std::size_t>(counter);
}

__device__ __forceinline__ unsigned long long doubleBits(double value) {
    union Bits {
        double d;
        unsigned long long u;
    } bits;
    bits.d = value;
    return bits.u;
}

__device__ __forceinline__ unsigned long long finalStateHash(
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

__device__ __forceinline__ int nextIntExactCounted(
        p20::JavaRandom& random,
        int bound,
        unsigned long long& rngDraws,
        unsigned long long& rejectedDraws,
        unsigned long long& retryingCalls,
        unsigned long long& powerOfTwoCalls
) {
    if (bound <= 0) return 0;
    if ((bound & -bound) == bound) {
        ++powerOfTwoCalls;
        ++rngDraws;
        return static_cast<int>((static_cast<std::int64_t>(bound) * random.nextBits(31)) >> 31);
    }

    unsigned long long drawsThisCall = 0ULL;
    for (;;) {
        ++rngDraws;
        ++drawsThisCall;
        const std::int32_t bits = static_cast<std::int32_t>(random.nextBits(31));
        const std::int32_t val = bits % bound;
        const std::uint32_t wrapped = static_cast<std::uint32_t>(bits)
                - static_cast<std::uint32_t>(val)
                + static_cast<std::uint32_t>(bound - 1);
        if (static_cast<std::int32_t>(wrapped) >= 0) {
            if (drawsThisCall > 1ULL) ++retryingCalls;
            return val;
        }
        ++rejectedDraws;
    }
}

__device__ __forceinline__ void referenceActiveInit(
        p20::JavaRandom& random,
        p20::PerlinState& perlin
) {
    const int lane = threadIdx.x;
    if (lane == 0) p20::initPerlin(random, perlin);
    __syncthreads();
}

__device__ __forceinline__ void referenceSkippedGroup(
        p20::JavaRandom& random,
        p20::PerlinState& perlin
) {
    const int lane = threadIdx.x;
    if (lane == 0) {
        for (int i = 0; i < SKIPPED_INIT_CALLS; ++i) p20::initPerlin(random, perlin);
    }
    __syncthreads();
}

__device__ __forceinline__ void profiledInitBody(
        SharedScratch& s,
        unsigned long long* localTicks,
        unsigned long long* localCounters
) {
    unsigned long long t0 = wall_clock64();
    s.perlin.a = s.rng.nextDouble() * 256.0;
    s.perlin.b = s.rng.nextDouble() * 256.0;
    s.perlin.c = s.rng.nextDouble() * 256.0;
    unsigned long long t1 = wall_clock64();

    for (int i = 0; i < SHUFFLE_STEPS; ++i) s.perlin.perm[i] = i;
    unsigned long long t2 = wall_clock64();

    for (int i = 0; i < SHUFFLE_STEPS; ++i) {
        ++localCounters[COUNT_NEXTINT_CALLS];
        const int bound = SHUFFLE_STEPS - i;
        s.jValues[i] = nextIntExactCounted(
                s.rng,
                bound,
                localCounters[COUNT_RNG_DRAWS],
                localCounters[COUNT_REJECTED_DRAWS],
                localCounters[COUNT_RETRYING_CALLS],
                localCounters[COUNT_POWER_OF_TWO_CALLS]) + i;
    }
    unsigned long long t3 = wall_clock64();

    for (int i = 0; i < SHUFFLE_STEPS; ++i) {
        const int j = s.jValues[i];
        const int tmp = s.perlin.perm[i];
        s.perlin.perm[i] = s.perlin.perm[j];
        s.perlin.perm[j] = tmp;
        s.duplicateValues[i] = s.perlin.perm[i];
    }
    unsigned long long t4 = wall_clock64();

    for (int i = 0; i < SHUFFLE_STEPS; ++i) {
        s.perlin.perm[i + SHUFFLE_STEPS] = s.duplicateValues[i];
    }
    unsigned long long t5 = wall_clock64();

    localTicks[COMP_OFFSET_RNG] += t1 - t0;
    localTicks[COMP_IDENTITY_FILL] += t2 - t1;
    localTicks[COMP_NEXTINT_GENERATION] += t3 - t2;
    localTicks[COMP_SWAP_PAIR] += t4 - t3;
    localTicks[COMP_DUPLICATE_WRITE] += t5 - t4;
}

__device__ __forceinline__ void profiledActiveInit(
        SharedScratch& s,
        unsigned long long* localTicks,
        unsigned long long* localCounters
) {
    const int lane = threadIdx.x;
    if (lane == 0) {
        profiledInitBody(s, localTicks, localCounters);
        ++localCounters[COUNT_ACTIVE_STATES];
    }
    __syncthreads();
}

__device__ __forceinline__ void profiledSkippedGroup(
        SharedScratch& s,
        unsigned long long* localTicks,
        unsigned long long* localCounters
) {
    const int lane = threadIdx.x;
    if (lane == 0) {
        for (int i = 0; i < SKIPPED_INIT_CALLS; ++i) {
            profiledInitBody(s, localTicks, localCounters);
        }
    }
    __syncthreads();
}

__global__ void referenceSequenceKernel(
        const std::int64_t* seeds,
        int count,
        unsigned long long* hashes
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= POINTS) return;

    __shared__ SharedScratch s;
    if (lane == 0) s.rng.setSeed(seeds[seedIndex]);
    __syncthreads();

    for (int i = 0; i < 16; ++i) referenceActiveInit(s.rng, s.perlin);
    for (int i = 0; i < 16; ++i) referenceActiveInit(s.rng, s.perlin);
    for (int i = 0; i < 8; ++i) referenceActiveInit(s.rng, s.perlin);
    referenceSkippedGroup(s.rng, s.perlin);
    for (int i = 0; i < 10; ++i) referenceActiveInit(s.rng, s.perlin);
    for (int i = 0; i < 16; ++i) referenceActiveInit(s.rng, s.perlin);

    if (lane == 0) hashes[seedIndex] = finalStateHash(s.rng, s.perlin);
}

__global__ void profileSequenceKernel(
        const std::int64_t* seeds,
        int count,
        unsigned long long* ticks,
        unsigned long long* counters,
        unsigned long long* hashes
) {
    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count || lane >= POINTS) return;

    __shared__ SharedScratch s;
    if (lane == 0) {
        s.rng.setSeed(seeds[seedIndex]);
        for (int i = 0; i < COMPONENTS; ++i) s.localTicks[i] = 0ULL;
        for (int i = 0; i < COUNTERS; ++i) s.localCounters[i] = 0ULL;
    }
    __syncthreads();

    for (int i = 0; i < 16; ++i) profiledActiveInit(s, s.localTicks, s.localCounters);
    for (int i = 0; i < 16; ++i) profiledActiveInit(s, s.localTicks, s.localCounters);
    for (int i = 0; i < 8; ++i) profiledActiveInit(s, s.localTicks, s.localCounters);
    profiledSkippedGroup(s, s.localTicks, s.localCounters);
    for (int i = 0; i < 10; ++i) profiledActiveInit(s, s.localTicks, s.localCounters);
    for (int i = 0; i < 16; ++i) profiledActiveInit(s, s.localTicks, s.localCounters);

    if (lane == 0) {
        for (int component = 0; component < COMPONENTS; ++component) {
            ticks[tickIndex(seedIndex, component)] = s.localTicks[component];
        }
        for (int counter = 0; counter < COUNTERS; ++counter) {
            counters[counterIndex(seedIndex, counter)] = s.localCounters[counter];
        }
        hashes[seedIndex] = finalStateHash(s.rng, s.perlin);
    }
}

} // namespace p20shuffleprofile
