#pragma once

#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"

#include <cstddef>

namespace climatecache {

static constexpr int TEMP_BASE = 0;
static constexpr int RAIN_BASE = 4;
static constexpr int BLEND_BASE = 8;
static constexpr int STATE_COUNT = 10;
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

} // namespace climatecache
