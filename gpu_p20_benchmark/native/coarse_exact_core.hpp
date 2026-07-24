#pragma once

#include "p20_exact_math.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coarsecore {

static constexpr int SIZE = 61;
static constexpr int Y_LEVELS = 17;
static constexpr int COLUMNS = SIZE * SIZE;
static constexpr int CELLS = COLUMNS * Y_LEVELS;
static constexpr int FROM_COARSE = -28;
static constexpr int MIN_INTERESTING_Y = 8;


P20_HD void perlin3Full17(
        const p20::PerlinState& p,
        double xCoord,
        double yScale,
        double zCoord,
        double out17[Y_LEVELS]
) {
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
    const int xp0 = p.perm[xi];
    const int xp1 = p.perm[xi + 1];

    int previousYi = -2147483647;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int y = 0; y < Y_LEVELS; ++y) {
        double v = static_cast<double>(y) * yScale + p.b;
        const int floor = p20::javaFloor(v);
        const int yi = floor & 255;
        v -= static_cast<double>(floor);
        const double yf = v;
        const double yfm1 = v - 1.0;
        const double yfade = p20::fade(v);
        if (y == 0 || yi != previousYi) {
            previousYi = yi;
            const int p0 = xp0 + yi;
            const int p00 = p.perm[p0] + zi;
            const int p01 = p.perm[p0 + 1] + zi;
            const int p1 = xp1 + yi;
            const int p10 = p.perm[p1] + zi;
            const int p11 = p.perm[p1 + 1] + zi;
            q00 = p20::lerp(xf, p20::grad3(p.perm[p00], x, yf, z), p20::grad3(p.perm[p10], x1, yf, z));
            q01 = p20::lerp(xf, p20::grad3(p.perm[p01], x, yfm1, z), p20::grad3(p.perm[p11], x1, yfm1, z));
            q10 = p20::lerp(xf, p20::grad3(p.perm[p00 + 1], x, yf, z1), p20::grad3(p.perm[p10 + 1], x1, yf, z1));
            q11 = p20::lerp(xf, p20::grad3(p.perm[p01 + 1], x, yfm1, z1), p20::grad3(p.perm[p11 + 1], x1, yfm1, z1));
        }
        const double r0 = p20::lerp(yfade, q00, q01);
        const double r1 = p20::lerp(yfade, q10, q11);
        out17[y] = p20::lerp(zf, r0, r1);
    }
}

P20_HD int index3(int x, int y, int z) {
    return (x * SIZE + z) * Y_LEVELS + y;
}

inline void columnCoordinates(int column, double& coarseX, double& coarseZ, double& climateX, double& climateZ) {
    const int x = column / SIZE;
    const int z = column - x * SIZE;
    coarseX = static_cast<double>(FROM_COARSE + x);
    coarseZ = static_cast<double>(FROM_COARSE + z);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
}

inline int scoreSigns(
        const unsigned char* signs,
        int* labels,
        int* queue,
        int* columnSeen,
        int* columnMinY,
        int* componentColumns
) {
    std::fill(labels, labels + CELLS, 0);
    std::fill(columnSeen, columnSeen + COLUMNS, 0);
    int best = 0;
    int nextId = 1;

    for (int x = 0; x < SIZE; ++x) {
        for (int z = 0; z < SIZE; ++z) {
            for (int y = 0; y < Y_LEVELS; ++y) {
                const int start = index3(x, y, z);
                if (labels[start] != 0 || signs[start] == 0) continue;

                const int id = nextId++;
                int head = 0;
                int tail = 0;
                queue[tail++] = start;
                labels[start] = id;
                int cells = 0;
                int maxY = 0;
                int columnCount = 0;
                bool touchesBottom = false;
                bool touchesSide = false;

                while (head < tail) {
                    const int idx = queue[head++];
                    const int cy = idx % Y_LEVELS;
                    const int tmp = idx / Y_LEVELS;
                    const int cz = tmp % SIZE;
                    const int cx = tmp / SIZE;
                    ++cells;
                    if (cy > maxY) maxY = cy;
                    if (cy == 0) touchesBottom = true;
                    if (cx == 0 || cz == 0 || cx == SIZE - 1 || cz == SIZE - 1) touchesSide = true;

                    const int col = cx * SIZE + cz;
                    if (columnSeen[col] != id) {
                        columnSeen[col] = id;
                        componentColumns[columnCount++] = col;
                        columnMinY[col] = cy;
                    } else if (cy < columnMinY[col]) {
                        columnMinY[col] = cy;
                    }

                    auto enqueue = [&](int ni) {
                        if (labels[ni] == 0 && signs[ni] != 0) {
                            labels[ni] = id;
                            queue[tail++] = ni;
                        }
                    };
                    if (cx + 1 < SIZE) enqueue(index3(cx + 1, cy, cz));
                    if (cx > 0) enqueue(index3(cx - 1, cy, cz));
                    if (cy + 1 < Y_LEVELS) enqueue(index3(cx, cy + 1, cz));
                    if (cy > 0) enqueue(index3(cx, cy - 1, cz));
                    if (cz + 1 < SIZE) enqueue(index3(cx, cy, cz + 1));
                    if (cz > 0) enqueue(index3(cx, cy, cz - 1));
                }

                if (cells <= best || maxY < MIN_INTERESTING_Y || touchesBottom || touchesSide) continue;

                bool reentry = false;
                for (int i = 0; i < columnCount && !reentry; ++i) {
                    const int col = componentColumns[i];
                    const int cx = col / SIZE;
                    const int cz = col - cx * SIZE;
                    const int minY = columnMinY[col];
                    if (minY <= 0 || minY >= Y_LEVELS) continue;
                    for (int lowerY = minY - 1; lowerY >= 0; --lowerY) {
                        const int idx = index3(cx, lowerY, cz);
                        if (signs[idx] != 0 && labels[idx] != id) {
                            if (minY - lowerY - 1 >= 1) reentry = true;
                            break;
                        }
                    }
                }
                if (reentry) best = cells;
            }
        }
    }
    return best;
}

struct HostScratch {
    std::vector<double> temp = std::vector<double>(COLUMNS);
    std::vector<double> rain = std::vector<double>(COLUMNS);
    std::vector<double> blendClimate = std::vector<double>(COLUMNS);
    std::vector<double> noise1 = std::vector<double>(CELLS);
    std::vector<double> noise2 = std::vector<double>(CELLS);
    std::vector<double> noise3 = std::vector<double>(CELLS);
    std::vector<double> noise4 = std::vector<double>(COLUMNS);
    std::vector<double> noise5 = std::vector<double>(COLUMNS);
    std::vector<unsigned char> signs = std::vector<unsigned char>(CELLS);
    std::vector<int> labels = std::vector<int>(CELLS);
    std::vector<int> queue = std::vector<int>(CELLS);
    std::vector<int> columnSeen = std::vector<int>(COLUMNS);
    std::vector<int> columnMinY = std::vector<int>(COLUMNS);
    std::vector<int> componentColumns = std::vector<int>(COLUMNS);
};

inline void accumulateClimate(
        std::int64_t seed,
        int octaves,
        double startScaleX,
        double startScaleZ,
        double octaveScale,
        double octavePersistence,
        std::vector<double>& out
) {
    p20::JavaRandom rng;
    p20::PerlinState perlin;
    rng.setSeed(seed);
    std::fill(out.begin(), out.end(), 0.0);
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < octaves; ++octave) {
        p20::initPerlin(rng, perlin);
        const double scaleX = (startScaleX / 1.5) * d7;
        const double scaleZ = (startScaleZ / 1.5) * d7;
        const double weight = 0.55 / d6;
        for (int column = 0; column < COLUMNS; ++column) {
            double coarseX, coarseZ, climateX, climateZ;
            columnCoordinates(column, coarseX, coarseZ, climateX, climateZ);
            out[column] += p20::simplex2(perlin, climateX * scaleX, climateZ * scaleZ) * weight;
        }
        d7 *= octaveScale;
        d6 *= octavePersistence;
    }
}

inline int generateAndScoreHost(std::int64_t seed, HostScratch& s) {
    const std::int64_t tempSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 9871ULL);
    const std::int64_t rainSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 39811ULL);
    const std::int64_t blendSeed = static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * 543321ULL);
    accumulateClimate(tempSeed, 4, 0.02500000037252903, 0.02500000037252903, 0.25, 0.5, s.temp);
    accumulateClimate(rainSeed, 4, 0.05000000074505806, 0.05000000074505806, 0.3333333333333333, 0.5, s.rain);
    accumulateClimate(blendSeed, 2, 0.25, 0.25, 0.5882352941176471, 0.5, s.blendClimate);

    for (int column = 0; column < COLUMNS; ++column) {
        const double d0 = s.blendClimate[column] * 1.1 + 0.5;
        double d1 = 0.01;
        double d2 = 1.0 - d1;
        double d3 = (s.temp[column] * 0.15 + 0.7) * d2 + d0 * d1;
        d1 = 0.0020;
        d2 = 1.0 - d1;
        double d4 = (s.rain[column] * 0.15 + 0.5) * d2 + d0 * d1;
        d3 = 1.0 - (1.0 - d3) * (1.0 - d3);
        if (d3 < 0.0) d3 = 0.0;
        if (d4 < 0.0) d4 = 0.0;
        if (d3 > 1.0) d3 = 1.0;
        if (d4 > 1.0) d4 = 1.0;
        s.temp[column] = d3;
        s.rain[column] = d4;
    }

    std::fill(s.noise1.begin(), s.noise1.end(), 0.0);
    std::fill(s.noise2.begin(), s.noise2.end(), 0.0);
    std::fill(s.noise3.begin(), s.noise3.end(), 0.0);
    std::fill(s.noise4.begin(), s.noise4.end(), 0.0);
    std::fill(s.noise5.begin(), s.noise5.end(), 0.0);

    p20::JavaRandom rng;
    p20::PerlinState perlin;
    rng.setSeed(seed);

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        p20::initPerlin(rng, perlin);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int column = 0; column < COLUMNS; ++column) {
            double coarseX, coarseZ, climateX, climateZ;
            columnCoordinates(column, coarseX, coarseZ, climateX, climateZ);
            const int base = column * Y_LEVELS;
            double values[Y_LEVELS];
            perlin3Full17(perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int y = 0; y < Y_LEVELS; ++y) s.noise2[base + y] += values[y] * weight;
        }
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        p20::initPerlin(rng, perlin);
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int column = 0; column < COLUMNS; ++column) {
            double coarseX, coarseZ, climateX, climateZ;
            columnCoordinates(column, coarseX, coarseZ, climateX, climateZ);
            const int base = column * Y_LEVELS;
            double values[Y_LEVELS];
            perlin3Full17(perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int y = 0; y < Y_LEVELS; ++y) s.noise3[base + y] += values[y] * weight;
        }
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        p20::initPerlin(rng, perlin);
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        for (int column = 0; column < COLUMNS; ++column) {
            double coarseX, coarseZ, climateX, climateZ;
            columnCoordinates(column, coarseX, coarseZ, climateX, climateZ);
            const int base = column * Y_LEVELS;
            double values[Y_LEVELS];
            perlin3Full17(perlin, coarseX * sx, sy, coarseZ * sz, values);
            for (int y = 0; y < Y_LEVELS; ++y) s.noise1[base + y] += values[y] * weight;
        }
        amplitude /= 2.0;
    }

    for (int i = 0; i < 8; ++i) p20::consumePerlin(rng, perlin);

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        p20::initPerlin(rng, perlin);
        const double scale = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int column = 0; column < COLUMNS; ++column) {
            double coarseX, coarseZ, climateX, climateZ;
            columnCoordinates(column, coarseX, coarseZ, climateX, climateZ);
            s.noise4[column] += p20::perlin2(perlin, coarseX * scale, coarseZ * scale) * weight;
        }
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        p20::initPerlin(rng, perlin);
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        for (int column = 0; column < COLUMNS; ++column) {
            double coarseX, coarseZ, climateX, climateZ;
            columnCoordinates(column, coarseX, coarseZ, climateX, climateZ);
            s.noise5[column] += p20::perlin2(perlin, coarseX * scale, coarseZ * scale) * weight;
        }
        amplitude /= 2.0;
    }

    for (int column = 0; column < COLUMNS; ++column) {
        const double d2 = s.temp[column];
        const double d3 = s.rain[column] * d2;
        double d4 = 1.0 - d3;
        d4 *= d4;
        d4 *= d4;
        d4 = 1.0 - d4;
        double d5 = (s.noise4[column] + 256.0) / 512.0;
        d5 *= d4;
        if (d5 > 1.0) d5 = 1.0;
        double d6 = s.noise5[column] / 8000.0;
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
        d6 = d6 * static_cast<double>(Y_LEVELS) / 16.0;
        const double d7 = static_cast<double>(Y_LEVELS) / 2.0 + d6 * 4.0;
        const int base = column * Y_LEVELS;
        for (int y = 0; y < Y_LEVELS; ++y) {
            double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
            if (d9 < 0.0) d9 *= 4.0;
            const double blend = (s.noise1[base + y] / 10.0 + 1.0) / 2.0;
            double d8;
            if (blend < 0.0) d8 = s.noise2[base + y] / 512.0;
            else if (blend > 1.0) d8 = s.noise3[base + y] / 512.0;
            else {
                const double d10 = s.noise2[base + y] / 512.0;
                const double d11 = s.noise3[base + y] / 512.0;
                d8 = d10 + (d11 - d10) * blend;
            }
            d8 -= d9;
            if (y > Y_LEVELS - 4) {
                const double d13 = static_cast<double>(static_cast<float>(y - (Y_LEVELS - 4)) / 3.0F);
                d8 = d8 * (1.0 - d13) + -10.0 * d13;
            }
            s.signs[base + y] = d8 > 0.0 ? 1 : 0;
        }
    }

    return scoreSigns(s.signs.data(), s.labels.data(), s.queue.data(), s.columnSeen.data(),
            s.columnMinY.data(), s.componentColumns.data());
}

} // namespace coarsecore
