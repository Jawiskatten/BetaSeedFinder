#pragma once

#include "p20_exact_math.hpp"
#include <cstdint>

namespace p20 {

static constexpr int AXIS64[8] = {0, 2, 5, 7, 8, 10, 13, 15};
static constexpr int POINTS = 64;
static constexpr int YCOUNT = 6;
static constexpr int DENSITY_COUNT = POINTS * YCOUNT;

struct P20Output {
    int upperPositiveColumns = 0;
    double density[DENSITY_COUNT]{};
};

struct P20Debug {
    double temperature0 = 0.0;
    double rain0 = 0.0;
    double noise1_0 = 0.0;
    double noise2_0 = 0.0;
    double noise3_0 = 0.0;
    double noise4_0 = 0.0;
    double noise5_0 = 0.0;
};

inline void accumulateSimplexOctaves(
        std::int64_t seed,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double octavePersistence,
        const double* worldX,
        const double* worldZ,
        double* out
) {
    JavaRandom rng{};
    rng.setSeed(seed);
    double d6 = 1.0;
    double d7 = 1.0;
    PerlinState state{};
    for (int octave = 0; octave < octaves; ++octave) {
        initPerlin(rng, state);
        const double scaleX = (startScaleX / 1.5) * d7;
        const double scaleZ = (startScaleZ / 1.5) * d7;
        const double weight = 0.55 / d6;
        for (int p = 0; p < POINTS; ++p) {
            out[p] += simplex2(state, worldX[p] * scaleX, worldZ[p] * scaleZ) * weight;
        }
        d7 *= octaveScale;
        d6 *= octavePersistence;
    }
}

inline P20Output exactP20(std::int64_t seed, P20Debug* debug = nullptr) {
    P20Output result{};

    double coarseX[POINTS];
    double coarseZ[POINTS];
    double climateX[POINTS];
    double climateZ[POINTS];
    int pIndex = 0;
    for (int ix = 0; ix < 8; ++ix) {
        const int gx = AXIS64[ix];
        for (int iz = 0; iz < 8; ++iz) {
            const int gz = AXIS64[iz];
            const double cx = -28.0 + static_cast<double>(gx * 4);
            const double cz = -28.0 + static_cast<double>(gz * 4);
            coarseX[pIndex] = cx;
            coarseZ[pIndex] = cz;
            climateX[pIndex] = cx * 4.0 + 2.0;
            climateZ[pIndex] = cz * 4.0 + 2.0;
            ++pIndex;
        }
    }

    double tempRaw[POINTS]{};
    double rainRaw[POINTS]{};
    double climateBlend[POINTS]{};
    const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
    const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
    const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);
    accumulateSimplexOctaves(tempSeed, 4, 0.02500000037252903, 0.02500000037252903,
                             0.25, 0.5, climateX, climateZ, tempRaw);
    accumulateSimplexOctaves(rainSeed, 4, 0.05000000074505806, 0.05000000074505806,
                             0.3333333333333333, 0.5, climateX, climateZ, rainRaw);
    accumulateSimplexOctaves(blendSeed, 2, 0.25, 0.25,
                             0.5882352941176471, 0.5, climateX, climateZ, climateBlend);

    double temperature[POINTS];
    double rain[POINTS];
    for (int p = 0; p < POINTS; ++p) {
        const double d0 = climateBlend[p] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (tempRaw[p] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (rainRaw[p] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        temperature[p] = d3;
        rain[p] = d4;
    }

    double noise1[DENSITY_COUNT]{};
    double noise2[DENSITY_COUNT]{};
    double noise3[DENSITY_COUNT]{};
    double noise4[POINTS]{};
    double noise5[POINTS]{};

    JavaRandom terrainRng{};
    terrainRng.setSeed(seed);
    PerlinState state{};
    double amplitude = 1.0;

    // terrainNoise2Generator: 16 octaves
    for (int octave = 0; octave < 16; ++octave) {
        initPerlin(terrainRng, state);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int p = 0; p < POINTS; ++p) {
            double values[6];
            perlin3Upper6(state, coarseX[p] * sx, sy, coarseZ[p] * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = p * YCOUNT + yi;
                const double value = values[yi] * weight;
                if (octave == 0) noise2[idx] = 0.0 + value;
                else noise2[idx] += value;
            }
        }
        amplitude /= 2.0;
    }

    // terrainNoise3Generator: 16 octaves
    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        initPerlin(terrainRng, state);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int p = 0; p < POINTS; ++p) {
            double values[6];
            perlin3Upper6(state, coarseX[p] * sx, sy, coarseZ[p] * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = p * YCOUNT + yi;
                const double value = values[yi] * weight;
                if (octave == 0) noise3[idx] = 0.0 + value;
                else noise3[idx] += value;
            }
        }
        amplitude /= 2.0;
    }

    // terrainNoise1Generator: 8 octaves
    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        initPerlin(terrainRng, state);
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        for (int p = 0; p < POINTS; ++p) {
            double values[6];
            perlin3Upper6(state, coarseX[p] * sx, sy, coarseZ[p] * sz, values);
            for (int yi = 0; yi < YCOUNT; ++yi) {
                const int idx = p * YCOUNT + yi;
                const double value = values[yi] * weight;
                if (octave == 0) noise1[idx] = 0.0 + value;
                else noise1[idx] += value;
            }
        }
        amplitude /= 2.0;
    }

    // Consume two unused 4-octave generators exactly.
    for (int i = 0; i < 8; ++i) consumePerlin(terrainRng, state);

    // terrainNoise4Generator: 10 octaves, exact 2D path
    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        initPerlin(terrainRng, state);
        const double sx = 1.121 * amplitude;
        const double sz = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int p = 0; p < POINTS; ++p) {
            const double value = perlin2(state, coarseX[p] * sx, coarseZ[p] * sz) * weight;
            if (octave == 0) noise4[p] = 0.0 + value;
            else noise4[p] += value;
        }
        amplitude /= 2.0;
    }

    // terrainNoise5Generator: 16 octaves, exact 2D path
    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        initPerlin(terrainRng, state);
        const double sx = 200.0 * amplitude;
        const double sz = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int p = 0; p < POINTS; ++p) {
            const double value = perlin2(state, coarseX[p] * sx, coarseZ[p] * sz) * weight;
            if (octave == 0) noise5[p] = 0.0 + value;
            else noise5[p] += value;
        }
        amplitude /= 2.0;
    }

    if (debug) {
        debug->temperature0 = temperature[0];
        debug->rain0 = rain[0];
        debug->noise1_0 = noise1[0];
        debug->noise2_0 = noise2[0];
        debug->noise3_0 = noise3[0];
        debug->noise4_0 = noise4[0];
        debug->noise5_0 = noise5[0];
    }

    for (int p = 0; p < POINTS; ++p) {
        const double d2 = temperature[p];
        const double d3 = rain[p] * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;

        double d5 = (noise4[p] + 256.0) / 512.0;
        d5 *= d4;
        if (d5 > 1.0) d5 = 1.0;

        double d6 = noise5[p] / 8000.0;
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
        for (int yi = 0; yi < YCOUNT; ++yi) {
            const int y = 11 + yi;
            const int idx = p * YCOUNT + yi;
            double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
            if (d9 < 0.0) d9 *= 4.0;

            const double blend = (noise1[idx] / 10.0 + 1.0) / 2.0;
            double d8;
            if (blend < 0.0) {
                d8 = noise2[idx] / 512.0;
            } else if (blend > 1.0) {
                d8 = noise3[idx] / 512.0;
            } else {
                const double d10 = noise2[idx] / 512.0;
                const double d11 = noise3[idx] / 512.0;
                d8 = d10 + (d11 - d10) * blend;
            }
            d8 -= d9;
            if (y > 13) {
                const double d13 = static_cast<double>(static_cast<float>(y - 13) / 3.0F);
                d8 = d8 * (1.0 - d13) + -10.0 * d13;
            }
            result.density[idx] = d8;
            if (d8 > 0.0) positive = true;
        }
        if (positive) ++result.upperPositiveColumns;
    }

    return result;
}

} // namespace p20
