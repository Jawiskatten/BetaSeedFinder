#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef P5_LANES
#define P5_LANES 32
#endif

#if defined(P5_PERM_U8)
using P5Perm = unsigned char;
#elif defined(P5_PERM_U16)
using P5Perm = unsigned short;
#else
using P5Perm = unsigned int;
#endif

static_assert(P5_LANES == 32 || P5_LANES == 64, "P5_LANES must be 32 or 64");

namespace floating_island_spawn_p5_wave {

static constexpr std::uint64_t JAVA_SEED_PERIOD = 1ULL << 48;
static constexpr std::uint64_t JAVA_SEED_MASK = JAVA_SEED_PERIOD - 1ULL;
static constexpr int Y_BASE = 7;
static constexpr int STORED_Y = 9; // exact Beta nodes 7..15; node 16 is exactly -10
static constexpr int FULL_Y = 17;

struct ScoutHit {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    int spawnSurfaceY = -1;
    int firstUpperY = -1;
    int airGap = 0;
    int playerFeetY = -1;
    int supportY = -1;
    int sandReason = 0;
};

struct Config {
    std::filesystem::path candidateOut;
    std::uint64_t count = 1000000;
    std::uint64_t startIndex = 0;
    std::uint64_t randomKey = 0;
    int seedMode = 1; // 0=splitmix64, 1=unique48
    int batch = 262144;
    int progressMs = 1000;
    int yieldMs = 0;
    bool selfTest = false;
};

[[noreturn]] static void failHip(const char* operation, hipError_t error) {
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(error));
}
static void checkHip(hipError_t error, const char* operation) {
    if (error != hipSuccess) failHip(operation, error);
}
template <typename T>
static void allocateArray(T*& pointer, std::size_t count, const char* label) {
    checkHip(hipMalloc(reinterpret_cast<void**>(&pointer), count * sizeof(T)), label);
}

__device__ __forceinline__ std::uint64_t splitMix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

__device__ __forceinline__ std::uint64_t permute48(std::uint64_t x) {
    x &= JAVA_SEED_MASK;
    x ^= x >> 24;
    x = (x * 0xD6E8FEB86659ULL) & JAVA_SEED_MASK;
    x ^= x >> 23;
    x = (x * 0xA5A3564E27F5ULL) & JAVA_SEED_MASK;
    x ^= x >> 24;
    return x & JAVA_SEED_MASK;
}

__device__ __forceinline__ int permGet(const P5Perm* perm, int lane, int index) {
    return static_cast<int>(perm[(index & 255) * P5_LANES + lane]);
}
__device__ __forceinline__ void permSet(P5Perm* perm, int lane, int index, int value) {
    perm[(index & 255) * P5_LANES + lane] = static_cast<P5Perm>(value);
}

struct SlimPerlin {
    double a;
    double b;
    double c;
};

// One independent seed per lane. The permutation is transposed as [permIndex][lane].
// With the U32 variant, a wave's accesses to a common permutation index land in
// distinct LDS banks. Unlike P4, no lane waits at a block-wide barrier for another
// seed's serial Java RNG/permutation construction.
__device__ __forceinline__ SlimPerlin initSlimPerlin(
        p20::JavaRandom& rng, P5Perm* perm, int lane) {
    SlimPerlin p;
    p.a = rng.nextDouble() * 256.0;
    p.b = rng.nextDouble() * 256.0;
    p.c = rng.nextDouble() * 256.0;
    for (int i = 0; i < 256; ++i) permSet(perm, lane, i, i);
    for (int i = 0; i < 256; ++i) {
        const int j = rng.nextInt(256 - i) + i;
        const int vi = permGet(perm, lane, i);
        const int vj = permGet(perm, lane, j);
        permSet(perm, lane, i, vj);
        permSet(perm, lane, j, vi);
    }
    return p;
}

__device__ __forceinline__ double slimSimplex2(
        const SlimPerlin& s, const P5Perm* perm, int lane,
        double xCoord, double zCoord) {
    constexpr double F = 0.3660254037844386;
    constexpr double G = 0.21132486540518713;
    const double x = xCoord + s.a;
    const double z = zCoord + s.b;
    const double skew = (x + z) * F;
    const int i = p20::simplexFastFloor(x + skew);
    const int j = p20::simplexFastFloor(z + skew);
    const double unskew = static_cast<double>(i + j) * G;
    const double x0 = x - (static_cast<double>(i) - unskew);
    const double z0 = z - (static_cast<double>(j) - unskew);
    const int i1 = x0 > z0 ? 1 : 0;
    const int j1 = x0 > z0 ? 0 : 1;
    const double x1 = x0 - static_cast<double>(i1) + G;
    const double z1 = z0 - static_cast<double>(j1) + G;
    const double x2 = x0 - 1.0 + 2.0 * G;
    const double z2 = z0 - 1.0 + 2.0 * G;
    const int ii = i & 255;
    const int jj = j & 255;
    const int g0 = permGet(perm, lane, ii + permGet(perm, lane, jj)) % 12;
    const int g1 = permGet(perm, lane, ii + i1 + permGet(perm, lane, jj + j1)) % 12;
    const int g2 = permGet(perm, lane, ii + 1 + permGet(perm, lane, jj + 1)) % 12;

    double t0 = 0.5 - x0 * x0 - z0 * z0;
    double n0;
    if (t0 < 0.0) n0 = 0.0;
    else { t0 *= t0; n0 = t0 * t0 * p20::simplexGrad2(g0, x0, z0); }
    double t1 = 0.5 - x1 * x1 - z1 * z1;
    double n1;
    if (t1 < 0.0) n1 = 0.0;
    else { t1 *= t1; n1 = t1 * t1 * p20::simplexGrad2(g1, x1, z1); }
    double t2 = 0.5 - x2 * x2 - z2 * z2;
    double n2;
    if (t2 < 0.0) n2 = 0.0;
    else { t2 *= t2; n2 = t2 * t2 * p20::simplexGrad2(g2, x2, z2); }
    return 70.0 * (n0 + n1 + n2);
}

__device__ __forceinline__ double slimPerlin2(
        const SlimPerlin& p, const P5Perm* perm, int lane,
        double xCoord, double zCoord) {
    double x = xCoord + p.a;
    double z = zCoord + p.c;
    const int fx = p20::javaFloor(x);
    const int fz = p20::javaFloor(z);
    const int xi = fx & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double z1 = z - 1.0;
    const double xf = p20::fade(x);
    const double zf = p20::fade(z);
    const int xp0 = permGet(perm, lane, xi);
    const int xp1 = permGet(perm, lane, xi + 1);
    const int p00 = permGet(perm, lane, xp0) + zi;
    const int p10 = permGet(perm, lane, xp1) + zi;
    const double q0 = p20::lerp(xf,
        p20::grad2Legacy(permGet(perm, lane, p00), x, z),
        p20::grad3(permGet(perm, lane, p10), x1, 0.0, z));
    const double q1 = p20::lerp(xf,
        p20::grad3(permGet(perm, lane, p00 + 1), x, 0.0, z1),
        p20::grad3(permGet(perm, lane, p10 + 1), x1, 0.0, z1));
    return p20::lerp(zf, q0, q1);
}

__device__ __forceinline__ double slimPerlin3Point(
        const SlimPerlin& p, const P5Perm* perm, int lane,
        double xCoord, double yCoord, double zCoord) {
    double x = xCoord + p.a;
    double y = yCoord + p.b;
    double z = zCoord + p.c;
    const int fx = p20::javaFloor(x);
    const int fy = p20::javaFloor(y);
    const int fz = p20::javaFloor(z);
    const int xi = fx & 255;
    const int yi = fy & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    y -= static_cast<double>(fy);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double y1 = y - 1.0;
    const double z1 = z - 1.0;
    const double xf = p20::fade(x);
    const double yf = p20::fade(y);
    const double zf = p20::fade(z);
    const int xp0 = permGet(perm, lane, xi);
    const int xp1 = permGet(perm, lane, xi + 1);
    const int p0 = xp0 + yi;
    const int p00 = permGet(perm, lane, p0) + zi;
    const int p01 = permGet(perm, lane, p0 + 1) + zi;
    const int p1 = xp1 + yi;
    const int p10 = permGet(perm, lane, p1) + zi;
    const int p11 = permGet(perm, lane, p1 + 1) + zi;
    const double q00 = p20::lerp(xf,
        p20::grad3(permGet(perm, lane, p00), x, y, z),
        p20::grad3(permGet(perm, lane, p10), x1, y, z));
    const double q01 = p20::lerp(xf,
        p20::grad3(permGet(perm, lane, p01), x, y1, z),
        p20::grad3(permGet(perm, lane, p11), x1, y1, z));
    const double q10 = p20::lerp(xf,
        p20::grad3(permGet(perm, lane, p00 + 1), x, y, z1),
        p20::grad3(permGet(perm, lane, p10 + 1), x1, y, z1));
    const double q11 = p20::lerp(xf,
        p20::grad3(permGet(perm, lane, p01 + 1), x, y1, z1),
        p20::grad3(permGet(perm, lane, p11 + 1), x1, y1, z1));
    const double r0 = p20::lerp(yf, q00, q01);
    const double r1 = p20::lerp(yf, q10, q11);
    return p20::lerp(zf, r0, r1);
}

// Evaluate the original Beta 17-node vertical series so the legacy gradient-carry
// behavior is bit-for-bit the same, then retain only nodes 7..15 needed by spawn.
__device__ __forceinline__ void slimPerlin3Stored9(
        const SlimPerlin& p, const P5Perm* perm, int lane,
        double yScale, double out9[STORED_Y]) {
    double x = p.a;
    double z = p.c;
    const int fx = p20::javaFloor(x);
    const int fz = p20::javaFloor(z);
    const int xi = fx & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double z1 = z - 1.0;
    const double xf = p20::fade(x);
    const double zf = p20::fade(z);
    const int xp0 = permGet(perm, lane, xi);
    const int xp1 = permGet(perm, lane, xi + 1);

    int previousYi = -2147483647;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int worldNodeY = 0; worldNodeY < FULL_Y; ++worldNodeY) {
        double v = static_cast<double>(worldNodeY) * yScale + p.b;
        const int floor = p20::javaFloor(v);
        const int yi = floor & 255;
        v -= static_cast<double>(floor);
        const double yf = v;
        const double yfm1 = v - 1.0;
        const double yfade = p20::fade(v);
        if (worldNodeY == 0 || yi != previousYi) {
            previousYi = yi;
            const int p0 = xp0 + yi;
            const int p00 = permGet(perm, lane, p0) + zi;
            const int p01 = permGet(perm, lane, p0 + 1) + zi;
            const int p1 = xp1 + yi;
            const int p10 = permGet(perm, lane, p1) + zi;
            const int p11 = permGet(perm, lane, p1 + 1) + zi;
            q00 = p20::lerp(xf,
                p20::grad3(permGet(perm, lane, p00), x, yf, z),
                p20::grad3(permGet(perm, lane, p10), x1, yf, z));
            q01 = p20::lerp(xf,
                p20::grad3(permGet(perm, lane, p01), x, yfm1, z),
                p20::grad3(permGet(perm, lane, p11), x1, yfm1, z));
            q10 = p20::lerp(xf,
                p20::grad3(permGet(perm, lane, p00 + 1), x, yf, z1),
                p20::grad3(permGet(perm, lane, p10 + 1), x1, yf, z1));
            q11 = p20::lerp(xf,
                p20::grad3(permGet(perm, lane, p01 + 1), x, yfm1, z1),
                p20::grad3(permGet(perm, lane, p11 + 1), x1, yfm1, z1));
        }
        if (worldNodeY >= Y_BASE && worldNodeY < Y_BASE + STORED_Y) {
            const double r0 = p20::lerp(yfade, q00, q01);
            const double r1 = p20::lerp(yfade, q10, q11);
            out9[worldNodeY - Y_BASE] = p20::lerp(zf, r0, r1);
        }
    }
}

__device__ __forceinline__ bool betaBiomeIsDesert(double temperature, double rainfall) {
    int ti = static_cast<int>(temperature * 63.0);
    int ri = static_cast<int>(rainfall * 63.0);
    if (ti < 0) ti = 0; else if (ti > 63) ti = 63;
    if (ri < 0) ri = 0; else if (ri > 63) ri = 63;
    const float f = static_cast<float>(ti) / 63.0f;
    float wet = static_cast<float>(ri) / 63.0f;
    wet *= f;
    return wet < 0.2f && f >= 0.95f;
}

__device__ __forceinline__ double nodeDensity9(const double density[STORED_Y], int localY) {
    if (localY < 0) return 10.0;
    if (localY >= STORED_Y) return -10.0;
    return density[localY];
}
__device__ __forceinline__ bool solidAtWorldY9(const double density[STORED_Y], int worldY) {
    if (worldY < Y_BASE * 8) return true;
    if (worldY >= 128) return false;
    const int localY = (worldY >> 3) - Y_BASE;
    const int inCell = worldY & 7;
    const double d0 = nodeDensity9(density, localY);
    const double d1 = nodeDensity9(density, localY + 1);
    const double d = d0 + (d1 - d0) * (static_cast<double>(inCell) * 0.125);
    return d > 0.0;
}
__device__ __forceinline__ int spawnCheckY9(const double density[STORED_Y]) {
    int y = 63;
    while (y + 1 < 128 && solidAtWorldY9(density, y + 1)) ++y;
    return solidAtWorldY9(density, y) ? y : -1;
}
__device__ __forceinline__ int playerFeetY9(const double density[STORED_Y]) {
    int feetY = 65;
    while (feetY < 128 && (solidAtWorldY9(density, feetY) || solidAtWorldY9(density, feetY + 1))) ++feetY;
    return feetY;
}
__device__ __forceinline__ int highestSolidAtOrBelow9(const double density[STORED_Y], int startY) {
    int y = startY > 127 ? 127 : startY;
    for (; y >= 0; --y) if (solidAtWorldY9(density, y)) return y;
    return -1;
}

__device__ __forceinline__ void transformClimate(double rawTemp, double rawRain, double rawBlend,
                                                  double& outTemp, double& outRain) {
    const double d0 = rawBlend * 1.1 + 0.5;
    double d1 = 0.01;
    double d2 = 1.0 - d1;
    double d3 = (rawTemp * 0.15 + 0.7) * d2 + d0 * d1;
    d1 = 0.0020;
    d2 = 1.0 - d1;
    double d4 = (rawRain * 0.15 + 0.5) * d2 + d0 * d1;
    d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
    if (d3 < 0.0) d3 = 0.0;
    if (d4 < 0.0) d4 = 0.0;
    if (d3 > 1.0) d3 = 1.0;
    if (d4 > 1.0) d4 = 1.0;
    outTemp = d3;
    outRain = d4;
}

__device__ __forceinline__ bool evaluateSeed(
        std::int64_t seed, P5Perm* perm, int lane, ScoutHit& hit) {
    // Terrain climate is sampled at the coarse-cell center (2,2), while biome
    // classification for block (0,0) needs the exact climate sample at (0,0).
    double terrainRaw[3] = {0.0, 0.0, 0.0};
    double originRaw[3] = {0.0, 0.0, 0.0};
    const std::int64_t climateSeeds[3] = {
        static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL),
        static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL),
        static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL)
    };
    const int climateOctaves[3] = {4, 4, 2};
    const double startScale[3] = {0.02500000037252903, 0.05000000074505806, 0.25};
    const double octaveScale[3] = {0.25, 0.3333333333333333, 0.5882352941176471};
    for (int kind = 0; kind < 3; ++kind) {
        p20::JavaRandom crng;
        crng.setSeed(climateSeeds[kind]);
        double d6 = 1.0;
        double d7 = 1.0;
        for (int octave = 0; octave < climateOctaves[kind]; ++octave) {
            const SlimPerlin p = initSlimPerlin(crng, perm, lane);
            const double scale = (startScale[kind] / 1.5) * d7;
            const double weight = 0.55 / d6;
            const double terrainValue = slimSimplex2(p, perm, lane, 2.0 * scale, 2.0 * scale) * weight;
            const double originValue = slimSimplex2(p, perm, lane, 0.0, 0.0) * weight;
            if (octave == 0) {
                terrainRaw[kind] = terrainValue;
                originRaw[kind] = originValue;
            } else {
                terrainRaw[kind] += terrainValue;
                originRaw[kind] += originValue;
            }
            d7 *= octaveScale[kind];
            d6 *= 0.5;
        }
    }

    double terrainTemp, terrainRain;
    transformClimate(terrainRaw[0], terrainRaw[1], terrainRaw[2], terrainTemp, terrainRain);
    double originTemp, originRain;
    transformClimate(originRaw[0], originRaw[1], originRaw[2], originTemp, originRain);

    double noise2[STORED_Y] = {};
    double noise3[STORED_Y] = {};
    double noise1[STORED_Y] = {};
    double values[STORED_Y];
    p20::JavaRandom rng;
    rng.setSeed(seed);

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const SlimPerlin p = initSlimPerlin(rng, perm, lane);
        slimPerlin3Stored9(p, perm, lane, 684.412 * amplitude, values);
        const double weight = 1.0 / amplitude;
        for (int y = 0; y < STORED_Y; ++y) {
            const double v = values[y] * weight;
            if (octave == 0) noise2[y] = v; else noise2[y] += v;
        }
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const SlimPerlin p = initSlimPerlin(rng, perm, lane);
        slimPerlin3Stored9(p, perm, lane, 684.412 * amplitude, values);
        const double weight = 1.0 / amplitude;
        for (int y = 0; y < STORED_Y; ++y) {
            const double v = values[y] * weight;
            if (octave == 0) noise3[y] = v; else noise3[y] += v;
        }
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        const SlimPerlin p = initSlimPerlin(rng, perm, lane);
        slimPerlin3Stored9(p, perm, lane, (684.412 / 160.0) * amplitude, values);
        const double weight = 1.0 / amplitude;
        for (int y = 0; y < STORED_Y; ++y) {
            const double v = values[y] * weight;
            if (octave == 0) noise1[y] = v; else noise1[y] += v;
        }
        amplitude /= 2.0;
    }

    // Vanilla's two four-octave surface generators occur here in the terrain RNG
    // stream. P4 previously generated them inside its specialized generic kernel.
    double sandNoise = 0.0;
    double stoneNoise = 0.0;
    double surfaceAmplitude = 1.0;
    for (int i = 0; i < 8; ++i) {
        if (i == 4) surfaceAmplitude = 1.0;
        const SlimPerlin p = initSlimPerlin(rng, perm, lane);
        const double contribution = slimPerlin3Point(p, perm, lane, 0.0, 0.0, 0.0) / surfaceAmplitude;
        if (i < 4) sandNoise += contribution;
        else stoneNoise += contribution;
        surfaceAmplitude *= 0.5;
    }

    double noise4 = 0.0;
    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        const SlimPerlin p = initSlimPerlin(rng, perm, lane);
        const double v = slimPerlin2(p, perm, lane, 0.0, 0.0) / amplitude;
        if (octave == 0) noise4 = v; else noise4 += v;
        amplitude /= 2.0;
    }

    double noise5 = 0.0;
    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const SlimPerlin p = initSlimPerlin(rng, perm, lane);
        const double v = slimPerlin2(p, perm, lane, 0.0, 0.0) / amplitude;
        if (octave == 0) noise5 = v; else noise5 += v;
        amplitude /= 2.0;
    }

    const double climateWet = terrainRain * terrainTemp;
    double d4 = 1.0 - climateWet;
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
    d6 = d6 * static_cast<double>(FULL_Y) / 16.0;
    const double d7 = static_cast<double>(FULL_Y) / 2.0 + d6 * 4.0;

    double density[STORED_Y];
    for (int y = 0; y < STORED_Y; ++y) {
        const int worldNodeY = y + Y_BASE;
        double d9 = (static_cast<double>(worldNodeY) - d7) * 12.0 / d5;
        if (d9 < 0.0) d9 *= 4.0;
        const double blend = (noise1[y] / 10.0 + 1.0) / 2.0;
        double d8;
        if (blend < 0.0) d8 = noise2[y] / 512.0;
        else if (blend > 1.0) d8 = noise3[y] / 512.0;
        else {
            const double d10 = noise2[y] / 512.0;
            const double d11 = noise3[y] / 512.0;
            d8 = d10 + (d11 - d10) * blend;
        }
        d8 -= d9;
        if (worldNodeY > FULL_Y - 4) {
            const double d13 = static_cast<double>(static_cast<float>(worldNodeY - (FULL_Y - 4)) / 3.0F);
            d8 = d8 * (1.0 - d13) + -10.0 * d13;
        }
        density[y] = d8;
    }

    const int spawnSurfaceY = spawnCheckY9(density);
    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool desert = betaBiomeIsDesert(originTemp, originRain);
    const bool beachSand = sandNoise + sandJitter * 0.2 > 0.0;
    const int surfaceDepth = static_cast<int>(stoneNoise / 3.0 + 3.0 + depthJitter * 0.25);
    const bool beachBand = spawnSurfaceY >= 60 && spawnSurfaceY <= 65;
    const int sandReason = desert ? 2 : ((beachBand && beachSand) ? 1 : 0);
    if (spawnSurfaceY < 63 || surfaceDepth <= 0 || sandReason == 0) return false;

    const int feetY = playerFeetY9(density);
    if (feetY <= 65) return false;
    const int supportY = highestSolidAtOrBelow9(density, feetY - 1);
    if (supportY < 0 || supportY != feetY - 1) return false;
    int firstUpperY = -1;
    for (int y = spawnSurfaceY + 1; y <= supportY; ++y) {
        if (solidAtWorldY9(density, y)) { firstUpperY = y; break; }
    }
    if (firstUpperY < 0) return false;
    const int airGap = firstUpperY - spawnSurfaceY - 1;
    if (airGap < 1 || airGap > 2) return false;

    hit.seed = seed;
    hit.spawnSurfaceY = spawnSurfaceY;
    hit.firstUpperY = firstUpperY;
    hit.airGap = airGap;
    hit.playerFeetY = feetY;
    hit.supportY = supportY;
    hit.sandReason = sandReason;
    return true;
}

__global__ void p5WaveScoutKernel(
        std::uint64_t baseIndex,
        int count,
        std::uint64_t randomKey,
        int seedMode,
        int explicitSeedMode,
        std::int64_t explicitSeed,
        unsigned int* hitCount,
        ScoutHit* hits,
        unsigned int hitCapacity) {
    __shared__ P5Perm permutations[256 * P5_LANES];
    const int lane = static_cast<int>(threadIdx.x);
    const int localIndex = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (localIndex >= count) return;
    const std::uint64_t sequence = baseIndex + static_cast<std::uint64_t>(localIndex);
    std::int64_t seed;
    if (explicitSeedMode != 0) {
        seed = explicitSeed;
    } else if (seedMode == 1) {
        const std::uint64_t low = permute48(sequence + (randomKey & JAVA_SEED_MASK));
        const std::uint64_t high = splitMix64(randomKey ^ sequence) & ~JAVA_SEED_MASK;
        seed = static_cast<std::int64_t>(high | low);
    } else {
        seed = static_cast<std::int64_t>(splitMix64(randomKey + sequence));
    }

    ScoutHit h;
    if (!evaluateSeed(seed, permutations, lane, h)) return;
    h.sequenceIndex = explicitSeedMode != 0 ? 0 : sequence;
    const unsigned int slot = atomicAdd(hitCount, 1u);
    if (slot < hitCapacity) hits[slot] = h;
}

static std::uint64_t parseU64(const std::string& value, const char* name) {
    std::size_t used = 0;
    const auto out = std::stoull(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}
static int parseInt(const std::string& value, const char* name) {
    std::size_t used = 0;
    const int out = std::stoi(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}

static Config parseArgs(int argc, char** argv) {
    Config c;
    bool keySet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (++i >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[i];
        };
        if (arg == "--candidate-out") c.candidateOut = value("--candidate-out");
        else if (arg == "--count") c.count = parseU64(value("--count"), "count");
        else if (arg == "--start-index") c.startIndex = parseU64(value("--start-index"), "start-index");
        else if (arg == "--random-key") { c.randomKey = parseU64(value("--random-key"), "random-key"); keySet = true; }
        else if (arg == "--seed-mode") {
            const auto mode = value("--seed-mode");
            if (mode == "unique48") c.seedMode = 1;
            else if (mode == "splitmix64") c.seedMode = 0;
            else throw std::invalid_argument("--seed-mode must be unique48 or splitmix64");
        }
        else if (arg == "--batch") c.batch = parseInt(value("--batch"), "batch");
        else if (arg == "--progress-ms") c.progressMs = parseInt(value("--progress-ms"), "progress-ms");
        else if (arg == "--yield-ms") c.yieldMs = parseInt(value("--yield-ms"), "yield-ms");
        else if (arg == "--self-test") c.selfTest = true;
        else throw std::invalid_argument("unknown argument: " + arg);
    }
    if (!c.selfTest && c.candidateOut.empty()) throw std::invalid_argument("--candidate-out is required");
    if (!c.selfTest && !keySet) throw std::invalid_argument("--random-key is required");
    if (c.batch < 1 || c.batch > 4194304) throw std::invalid_argument("--batch must be 1..4194304");
    if (c.progressMs < 100 || c.progressMs > 60000) throw std::invalid_argument("--progress-ms must be 100..60000");
    if (c.yieldMs < 0 || c.yieldMs > 50) throw std::invalid_argument("--yield-ms must be 0..50");
    if (c.seedMode == 1 && c.startIndex + c.count > JAVA_SEED_PERIOD) throw std::invalid_argument("unique48 range exceeds 2^48");
    return c;
}

static void launchBatch(const Config& c, std::uint64_t baseIndex, int count,
                        unsigned int* dCount, ScoutHit* dHits, std::vector<ScoutHit>& host,
                        bool explicitMode = false, std::int64_t explicitSeed = 0) {
    checkHip(hipMemset(dCount, 0, sizeof(unsigned int)), "reset P5 hit count");
    const int blocks = (count + P5_LANES - 1) / P5_LANES;
    hipLaunchKernelGGL(p5WaveScoutKernel, dim3(blocks), dim3(P5_LANES), 0, 0,
        baseIndex, count, c.randomKey, c.seedMode, explicitMode ? 1 : 0, explicitSeed,
        dCount, dHits, static_cast<unsigned int>(c.batch));
    checkHip(hipGetLastError(), "launch P5 wave scout");
    checkHip(hipDeviceSynchronize(), "finish P5 wave scout");
    unsigned int n = 0;
    checkHip(hipMemcpy(&n, dCount, sizeof(n), hipMemcpyDeviceToHost), "copy P5 hit count");
    if (n > static_cast<unsigned int>(c.batch)) throw std::runtime_error("P5 hit buffer overflow");
    host.resize(n);
    if (n) {
        checkHip(hipMemcpy(host.data(), dHits, static_cast<std::size_t>(n) * sizeof(ScoutHit), hipMemcpyDeviceToHost), "copy P5 hits");
        std::sort(host.begin(), host.end(), [](const ScoutHit& a, const ScoutHit& b) { return a.sequenceIndex < b.sequenceIndex; });
    }
}

static int selfTest() {
    Config c;
    c.batch = 1;
    unsigned int* dCount = nullptr;
    ScoutHit* dHits = nullptr;
    allocateArray(dCount, 1, "allocate P5 self-test counter");
    allocateArray(dHits, 1, "allocate P5 self-test hit");
    std::vector<ScoutHit> host;
    try {
        launchBatch(c, 0, 1, dCount, dHits, host, true, 6430576860599818994LL);
        if (host.size() != 1) throw std::runtime_error("known P3/P4 pillar seed failed P5 wave scout");
        const auto& h = host.front();
        if (h.spawnSurfaceY != 63 || h.firstUpperY != 65 || h.airGap != 1 || h.playerFeetY != 74 || h.supportY != 73) {
            throw std::runtime_error("P5 wave metadata disagrees with known exact seed");
        }
        std::cout << "P5 WAVE SELFTEST OK seed=" << h.seed
                  << " spawnSurfaceY=" << h.spawnSurfaceY
                  << " firstUpperY=" << h.firstUpperY
                  << " airGap=" << h.airGap
                  << " playerFeetY=" << h.playerFeetY
                  << " supportY=" << h.supportY
                  << " lanes=" << P5_LANES
                  << " permBytes=" << sizeof(P5Perm) << '\n';
    } catch (...) {
        if (dHits) (void)hipFree(dHits);
        if (dCount) (void)hipFree(dCount);
        throw;
    }
    checkHip(hipFree(dHits), "free P5 self-test hits");
    checkHip(hipFree(dCount), "free P5 self-test counter");
    return 0;
}

static void writeHeader(std::ofstream& f) {
    f << "seed,sequence_index,spawn_surface_y,first_upper_y,air_gap,player_feet_y,support_y,sand_reason\n";
}
static void writeHit(std::ofstream& f, const ScoutHit& h) {
    f << h.seed << ',' << h.sequenceIndex << ',' << h.spawnSurfaceY << ',' << h.firstUpperY << ','
      << h.airGap << ',' << h.playerFeetY << ',' << h.supportY << ',' << h.sandReason << '\n';
}

static int run(const Config& c) {
    if (c.selfTest) return selfTest();
    std::filesystem::create_directories(c.candidateOut.parent_path());
    std::ofstream out(c.candidateOut, std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write candidate file: " + c.candidateOut.string());
    writeHeader(out);

    unsigned int* dCount = nullptr;
    ScoutHit* dHits = nullptr;
    allocateArray(dCount, 1, "allocate P5 hit counter");
    allocateArray(dHits, static_cast<std::size_t>(c.batch), "allocate P5 hit buffer");
    std::vector<ScoutHit> hits;
    std::uint64_t processed = 0;
    std::uint64_t totalHits = 0;
    const auto start = std::chrono::steady_clock::now();
    auto lastProgress = start;
    try {
        while (processed < c.count) {
            const int n = static_cast<int>(std::min<std::uint64_t>(c.count - processed, static_cast<std::uint64_t>(c.batch)));
            launchBatch(c, c.startIndex + processed, n, dCount, dHits, hits);
            for (const auto& h : hits) writeHit(out, h);
            totalHits += hits.size();
            processed += static_cast<std::uint64_t>(n);
            if (c.yieldMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(c.yieldMs));
            const auto now = std::chrono::steady_clock::now();
            const auto elapsedProgress = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgress).count();
            if (elapsedProgress >= c.progressMs || processed == c.count) {
                const double seconds = std::chrono::duration<double>(now - start).count();
                const double rate = seconds > 0.0 ? static_cast<double>(processed) / seconds : 0.0;
                std::cout << "P5 wave progress checked=" << processed << '/' << c.count
                          << " rate=" << std::fixed << std::setprecision(1) << rate << " seeds/s"
                          << " candidates=" << totalHits << '\n';
                lastProgress = now;
            }
        }
    } catch (...) {
        if (dHits) (void)hipFree(dHits);
        if (dCount) (void)hipFree(dCount);
        throw;
    }
    checkHip(hipFree(dHits), "free P5 hits");
    checkHip(hipFree(dCount), "free P5 counter");
    out.flush();
    std::cout << "P5 wave candidates=" << totalHits << " file=" << c.candidateOut.string()
              << " lanes=" << P5_LANES << " permBytes=" << sizeof(P5Perm) << '\n';
    return 0;
}

} // namespace floating_island_spawn_p5_wave

int main(int argc, char** argv) {
    try {
        const auto c = floating_island_spawn_p5_wave::parseArgs(argc, argv);
        return floating_island_spawn_p5_wave::run(c);
    } catch (const std::exception& e) {
        std::cerr << "FloatingIslandSpawn P5 WAVE ERROR: " << e.what() << '\n';
        return 1;
    }
}
