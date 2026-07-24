#pragma once

#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"

#include <cstddef>
#include <cstdint>

namespace terraincache {

// Exact terrain-noise Perlin states that are actually evaluated by the early chain.
// The eight skipped noise1 octaves are still consumed by P20 to preserve Java RNG
// order, but they do not need to be cached because Upper/Lower no longer replay RNG.
static constexpr int NOISE2_BASE = 0;
static constexpr int NOISE3_BASE = 16;
static constexpr int NOISE1_BASE = 32;
static constexpr int NOISE4_BASE = 40;
static constexpr int NOISE5_BASE = 50;
static constexpr int STATE_COUNT = 66;
static constexpr int PERM_SIZE = 256;
static constexpr int OFFSET_COUNT = 3;

inline std::size_t permutationBytesForCapacity(int capacity) {
    return static_cast<std::size_t>(capacity) * STATE_COUNT * PERM_SIZE * sizeof(unsigned char);
}

inline std::size_t offsetBytesForCapacity(int capacity) {
    return static_cast<std::size_t>(capacity) * STATE_COUNT * OFFSET_COUNT * sizeof(double);
}

inline std::size_t totalBytesForCapacity(int capacity) {
    return permutationBytesForCapacity(capacity) + offsetBytesForCapacity(capacity);
}

__device__ __forceinline__ std::size_t stateIndex(int seedIndex, int state) {
    return static_cast<std::size_t>(seedIndex) * STATE_COUNT + static_cast<std::size_t>(state);
}

// P20 has already built the exact shared Perlin state. Persist only the canonical
// 256-byte permutation plus the three offsets. The duplicated 256..511 half is
// reconstructed on load. No barrier is required after the stores: later kernels
// cannot begin until this kernel completes, while this kernel may evaluate the same
// shared state concurrently with its own cache writes.
__device__ __forceinline__ void storeStateCooperative(
        const p20::PerlinState& state,
        int seedIndex,
        int stateNumber,
        unsigned char* permutationCache,
        double* offsetCache,
        int lane,
        int laneCount
) {
    const std::size_t stateLinear = stateIndex(seedIndex, stateNumber);
    if (lane == 0) {
        const std::size_t offsetBase = stateLinear * OFFSET_COUNT;
        offsetCache[offsetBase] = state.a;
        offsetCache[offsetBase + 1] = state.b;
        offsetCache[offsetBase + 2] = state.c;
    }
    const std::size_t permBase = stateLinear * PERM_SIZE;
    for (int i = lane; i < PERM_SIZE; i += laneCount) {
        permutationCache[permBase + static_cast<std::size_t>(i)] =
                static_cast<unsigned char>(state.perm[i]);
    }
}

// Upper/Lower cooperatively rebuild the exact shared PerlinState. Each block has
// 256 threads, so the common case is one coalesced byte load per lane plus two
// shared-memory integer writes. The barrier makes the reconstructed state visible
// before any lane enters Perlin evaluation.
__device__ __forceinline__ void loadStateCooperative(
        p20::PerlinState& state,
        int seedIndex,
        int stateNumber,
        const unsigned char* permutationCache,
        const double* offsetCache,
        int lane,
        int laneCount
) {
    const std::size_t stateLinear = stateIndex(seedIndex, stateNumber);
    if (lane == 0) {
        const std::size_t offsetBase = stateLinear * OFFSET_COUNT;
        state.a = offsetCache[offsetBase];
        state.b = offsetCache[offsetBase + 1];
        state.c = offsetCache[offsetBase + 2];
    }
    const std::size_t permBase = stateLinear * PERM_SIZE;
    for (int i = lane; i < PERM_SIZE; i += laneCount) {
        const int value = static_cast<int>(permutationCache[permBase + static_cast<std::size_t>(i)]);
        state.perm[i] = value;
        state.perm[i + PERM_SIZE] = value;
    }
    __syncthreads();
}

} // namespace terraincache
