#pragma once

#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace singlebiome {
namespace dryplains {

static constexpr int TERRAIN_STATE_COUNT = 66;
static constexpr int THREADS = 256;

inline void check(hipError_t err, const char* what) {
    if (err != hipSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + hipGetErrorString(err));
    }
}

inline std::int64_t multipliedSeed(std::int64_t seed, std::uint64_t multiplier) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * multiplier);
}

__device__ __forceinline__ double climateNoise4(
        const p20::PerlinState* states,
        double x,
        double z,
        double baseScale,
        double octaveScale
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 4; ++octave) {
        const double scale = (baseScale / 1.5) * d7;
        total += p20::simplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= octaveScale;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double climateNoise2(
        const p20::PerlinState* states,
        double x,
        double z
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 2; ++octave) {
        const double scale = (0.25 / 1.5) * d7;
        total += p20::simplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= 0.5882352941176471;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

__device__ __forceinline__ double densityAtY(
        double noise1,
        double noise2,
        double noise3,
        double d5,
        double d7,
        int y
) {
    double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
    if (d9 < 0.0) d9 *= 4.0;

    const double blend = (noise1 / 10.0 + 1.0) / 2.0;
    double d8;
    if (blend < 0.0) {
        d8 = noise2 / 512.0;
    } else if (blend > 1.0) {
        d8 = noise3 / 512.0;
    } else {
        const double d10 = noise2 / 512.0;
        const double d11 = noise3 / 512.0;
        d8 = d10 + (d11 - d10) * blend;
    }
    d8 -= d9;
    return d8;
}

__global__ void seaDensityKernel(
        const p20::PerlinState* terrain,
        const p20::PerlinState* tempStates,
        const p20::PerlinState* rainStates,
        const p20::PerlinState* blendStates,
        int coarseMinX,
        int coarseMinZ,
        int coarsePoints,
        double* seaDensity
) {
    const int idx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int pointCount = coarsePoints * coarsePoints;
    if (idx >= pointCount) return;

    const int ix = idx / coarsePoints;
    const int iz = idx - ix * coarsePoints;
    const double coarseX = static_cast<double>(coarseMinX + ix);
    const double coarseZ = static_cast<double>(coarseMinZ + iz);
    const double climateX = coarseX * 4.0 + 2.0;
    const double climateZ = coarseZ * 4.0 + 2.0;

    const double tempRaw = climateNoise4(
            tempStates, climateX, climateZ,
            0.02500000037252903, 0.25);
    const double rainRaw = climateNoise4(
            rainStates, climateX, climateZ,
            0.05000000074505806, 0.3333333333333333);
    const double blendRaw = climateNoise2(blendStates, climateX, climateZ);

    const double climateBlend = blendRaw * 1.1 + 0.5;
    double temperature = (tempRaw * 0.15 + 0.7) * 0.99 + climateBlend * 0.01;
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + climateBlend * 0.002;
    temperature = 1.0 - (1.0 - temperature) * (1.0 - temperature);
    temperature = clamp01(temperature);
    rain = clamp01(rain);

    double n2y7 = 0.0, n2y8 = 0.0;
    double n3y7 = 0.0, n3y8 = 0.0;
    double n1y7 = 0.0, n1y8 = 0.0;
    double n4 = 0.0;
    double n5 = 0.0;

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[octave];
        n2y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n2y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[16 + octave];
        n3y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n3y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[32 + octave];
        n1y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n1y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        const double scale = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        n4 += p20::perlin2(
                terrain[40 + octave], coarseX * scale, coarseZ * scale) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        n5 += p20::perlin2(
                terrain[50 + octave], coarseX * scale, coarseZ * scale) * weight;
        amplitude /= 2.0;
    }

    const double d2 = temperature;
    const double d3 = rain * d2;
    double d4 = 1.0 - d3;
    d4 *= d4;
    d4 *= d4;
    d4 = 1.0 - d4;

    double d5 = (n4 + 256.0) / 512.0;
    d5 *= d4;
    if (d5 > 1.0) d5 = 1.0;

    double d6 = n5 / 8000.0;
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
    d6 *= 17.0 / 16.0;
    const double d7 = 17.0 / 2.0 + d6 * 4.0;

    const double density7 = densityAtY(n1y7, n2y7, n3y7, d5, d7, 7);
    const double density8 = densityAtY(n1y8, n2y8, n3y8, d5, d7, 8);

    const double step = (density8 - density7) * 0.125;
    double v = density7;
    for (int subY = 0; subY < 7; ++subY) v += step;
    seaDensity[idx] = v;
}

__global__ void applyDryMaskKernel(
        const double* seaDensity,
        int coarseCells,
        int coarsePoints,
        int side,
        unsigned char* plainsMap
) {
    const int cell = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (cell >= coarseCells * coarseCells) return;

    const int cx = cell / coarseCells;
    const int cz = cell - cx * coarseCells;
    const int row0 = cx * coarsePoints;
    const int row1 = (cx + 1) * coarsePoints;

    const double q00 = seaDensity[row0 + cz];
    const double q01 = seaDensity[row0 + cz + 1];
    const double q10 = seaDensity[row1 + cz];
    const double q11 = seaDensity[row1 + cz + 1];

    double x0 = q00;
    double x1 = q01;
    const double dx0 = (q10 - q00) * 0.25;
    const double dx1 = (q11 - q01) * 0.25;

    for (int subX = 0; subX < 4; ++subX) {
        double v = x0;
        const double dz = (x1 - x0) * 0.25;
        for (int subZ = 0; subZ < 4; ++subZ) {
            if (v <= 0.0) {
                const int localX = cx * 4 + subX;
                const int localZ = cz * 4 + subZ;
                plainsMap[localZ * side + localX] = 0;
            }
            v += dz;
        }
        x0 += dx0;
        x1 += dx1;
    }
}

inline void buildStates(
        std::int64_t seed,
        std::vector<p20::PerlinState>& terrain,
        std::vector<p20::PerlinState>& temp,
        std::vector<p20::PerlinState>& rain,
        std::vector<p20::PerlinState>& blend
) {
    terrain.resize(TERRAIN_STATE_COUNT);
    temp.resize(4);
    rain.resize(4);
    blend.resize(2);

    p20::JavaRandom rng;
    p20::PerlinState scratch;

    rng.setSeed(multipliedSeed(seed, 9871ULL));
    for (int i = 0; i < 4; ++i) p20::initPerlin(rng, temp[i]);

    rng.setSeed(multipliedSeed(seed, 39811ULL));
    for (int i = 0; i < 4; ++i) p20::initPerlin(rng, rain[i]);

    rng.setSeed(multipliedSeed(seed, 543321ULL));
    for (int i = 0; i < 2; ++i) p20::initPerlin(rng, blend[i]);

    rng.setSeed(seed);
    for (int i = 0; i < 40; ++i) p20::initPerlin(rng, terrain[i]);
    for (int i = 0; i < 8; ++i) p20::initPerlin(rng, scratch);
    for (int i = 40; i < TERRAIN_STATE_COUNT; ++i) p20::initPerlin(rng, terrain[i]);
}

inline void applyExactDryMask(
        std::int64_t seed,
        int centerX,
        int centerZ,
        int target,
        unsigned char* dMap
) {
    const int side = 2 * target;
    if ((side % 4) != 0 || ((centerX - target) % 4) != 0 || ((centerZ - target) % 4) != 0) {
        throw std::runtime_error("Dry Plains terrain mask requires square origin aligned to 4 blocks");
    }

    const int coarseCells = side / 4;
    const int coarsePoints = coarseCells + 1;
    const int coarsePointCount = coarsePoints * coarsePoints;
    const int coarseMinX = (centerX - target) / 4;
    const int coarseMinZ = (centerZ - target) / 4;

    std::vector<p20::PerlinState> terrain;
    std::vector<p20::PerlinState> temp;
    std::vector<p20::PerlinState> rain;
    std::vector<p20::PerlinState> blend;
    buildStates(seed, terrain, temp, rain, blend);

    p20::PerlinState* dTerrain = nullptr;
    p20::PerlinState* dTemp = nullptr;
    p20::PerlinState* dRain = nullptr;
    p20::PerlinState* dBlend = nullptr;
    double* dDensity = nullptr;

    check(hipMalloc(reinterpret_cast<void**>(&dTerrain),
                    terrain.size() * sizeof(p20::PerlinState)), "hipMalloc(dTerrain)");
    check(hipMalloc(reinterpret_cast<void**>(&dTemp),
                    temp.size() * sizeof(p20::PerlinState)), "hipMalloc(dTemp)");
    check(hipMalloc(reinterpret_cast<void**>(&dRain),
                    rain.size() * sizeof(p20::PerlinState)), "hipMalloc(dRain)");
    check(hipMalloc(reinterpret_cast<void**>(&dBlend),
                    blend.size() * sizeof(p20::PerlinState)), "hipMalloc(dBlend)");
    check(hipMalloc(reinterpret_cast<void**>(&dDensity),
                    static_cast<std::size_t>(coarsePointCount) * sizeof(double)), "hipMalloc(dDensity)");

    try {
        check(hipMemcpy(dTerrain, terrain.data(),
                        terrain.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice), "hipMemcpy(dTerrain)");
        check(hipMemcpy(dTemp, temp.data(),
                        temp.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice), "hipMemcpy(dTemp)");
        check(hipMemcpy(dRain, rain.data(),
                        rain.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice), "hipMemcpy(dRain)");
        check(hipMemcpy(dBlend, blend.data(),
                        blend.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice), "hipMemcpy(dBlend)");

        const int pointBlocks = (coarsePointCount + THREADS - 1) / THREADS;
        hipLaunchKernelGGL(
                seaDensityKernel,
                dim3(pointBlocks), dim3(THREADS), 0, 0,
                dTerrain, dTemp, dRain, dBlend,
                coarseMinX, coarseMinZ, coarsePoints, dDensity);
        check(hipGetLastError(), "seaDensityKernel launch");

        const int cellCount = coarseCells * coarseCells;
        const int cellBlocks = (cellCount + THREADS - 1) / THREADS;
        hipLaunchKernelGGL(
                applyDryMaskKernel,
                dim3(cellBlocks), dim3(THREADS), 0, 0,
                dDensity, coarseCells, coarsePoints, side, dMap);
        check(hipGetLastError(), "applyDryMaskKernel launch");
        check(hipDeviceSynchronize(), "dry Plains terrain synchronize");
    } catch (...) {
        if (dDensity) (void)hipFree(dDensity);
        if (dBlend) (void)hipFree(dBlend);
        if (dRain) (void)hipFree(dRain);
        if (dTemp) (void)hipFree(dTemp);
        if (dTerrain) (void)hipFree(dTerrain);
        throw;
    }

    (void)hipFree(dDensity);
    (void)hipFree(dBlend);
    (void)hipFree(dRain);
    (void)hipFree(dTemp);
    (void)hipFree(dTerrain);
}

} // namespace dryplains
} // namespace singlebiome
