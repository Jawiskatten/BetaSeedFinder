#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>

#if defined(__HIPCC__) || defined(__CUDACC__)
#define P20_HD __host__ __device__ __forceinline__
#else
#define P20_HD inline
#endif

namespace p20 {

static constexpr std::uint64_t JAVA_MULT = 0x5DEECE66DULL;
static constexpr std::uint64_t JAVA_ADD = 0xBULL;
static constexpr std::uint64_t JAVA_MASK = (1ULL << 48) - 1ULL;

struct JavaRandom {
    std::uint64_t state;

    P20_HD void setSeed(std::int64_t seed) {
        state = (static_cast<std::uint64_t>(seed) ^ JAVA_MULT) & JAVA_MASK;
    }

    P20_HD std::uint32_t nextBits(int bits) {
        state = (state * JAVA_MULT + JAVA_ADD) & JAVA_MASK;
        return static_cast<std::uint32_t>(state >> (48 - bits));
    }

    P20_HD double nextDouble() {
        const std::uint64_t hi = static_cast<std::uint64_t>(nextBits(26));
        const std::uint64_t lo = static_cast<std::uint64_t>(nextBits(27));
        const std::uint64_t combined = (hi << 27) + lo;
        return static_cast<double>(combined) / 9007199254740992.0;
    }

    P20_HD int nextInt(int bound) {
        if (bound <= 0) return 0;
        if ((bound & -bound) == bound) {
            return static_cast<int>((static_cast<std::int64_t>(bound) * nextBits(31)) >> 31);
        }
        for (;;) {
            const std::int32_t bits = static_cast<std::int32_t>(nextBits(31));
            const std::int32_t val = bits % bound;
            const std::uint32_t wrapped = static_cast<std::uint32_t>(bits)
                    - static_cast<std::uint32_t>(val)
                    + static_cast<std::uint32_t>(bound - 1);
            if (static_cast<std::int32_t>(wrapped) >= 0) return val;
        }
    }
};

struct PerlinState {
    int perm[512];
    double a;
    double b;
    double c;
};

P20_HD void initPerlin(JavaRandom& random, PerlinState& out) {
    out.a = random.nextDouble() * 256.0;
    out.b = random.nextDouble() * 256.0;
    out.c = random.nextDouble() * 256.0;
    for (int i = 0; i < 256; ++i) out.perm[i] = i;
    for (int i = 0; i < 256; ++i) {
        const int j = random.nextInt(256 - i) + i;
        const int tmp = out.perm[i];
        out.perm[i] = out.perm[j];
        out.perm[j] = tmp;
        out.perm[i + 256] = out.perm[i];
    }
}

P20_HD void consumePerlin(JavaRandom& random, PerlinState& scratch) {
    initPerlin(random, scratch);
}

P20_HD int javaFloor(double v) {
    int i = static_cast<int>(v);
    if (v < static_cast<double>(i)) --i;
    return i;
}

P20_HD double fade(double v) {
    return v * v * v * (v * (v * 6.0 - 15.0) + 10.0);
}

P20_HD double lerp(double t, double a, double b) {
    return a + t * (b - a);
}

// Exact branchless form of the legacy 16-case improved-Perlin gradient dispatch.
// The hash is effectively random across GPU lanes, so the old switch can make a
// wavefront execute many divergent case paths. These selects preserve the exact
// operand order and signs for every hash value while allowing predicated lowering.
P20_HD double grad3(int hash, double x, double y, double z) {
    const int h = hash & 15;
    const double u = (h < 8) ? x : y;
    const double v = (h < 4) ? y : ((h == 12 || h == 14) ? x : z);
    const double su = ((h & 1) == 0) ? u : -u;
    const double sv = ((h & 2) == 0) ? v : -v;
    return su + sv;
}

P20_HD double grad2Legacy(int hash, double x, double z) {
    const double zeroX = 0.0 * x;
    switch (hash & 15) {
        case 0: return x + 0.0;
        case 1: return -x + 0.0;
        case 2: return x + -0.0;
        case 3: return -x + -0.0;
        case 4: return x + z;
        case 5: return -x + z;
        case 6: return x + -z;
        case 7: return -x + -z;
        case 8: return zeroX + z;
        case 9: return -zeroX + z;
        case 10: return zeroX + -z;
        case 11: return -zeroX + -z;
        case 12: return zeroX + x;
        case 13: return -zeroX + z;
        case 14: return zeroX + -x;
        default: return -zeroX + -z;
    }
}

/** Matches addNoiseCachedAxesStrided for one exact point. */
P20_HD double perlin3(const PerlinState& p, double xCoord, double yCoord, double zCoord) {
    double x = xCoord + p.a;
    double y = yCoord + p.b;
    double z = zCoord + p.c;
    const int fx = javaFloor(x);
    const int fy = javaFloor(y);
    const int fz = javaFloor(z);
    const int xi = fx & 255;
    const int yi = fy & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    y -= static_cast<double>(fy);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double y1 = y - 1.0;
    const double z1 = z - 1.0;
    const double xf = fade(x);
    const double yf = fade(y);
    const double zf = fade(z);

    const int xp0 = p.perm[xi];
    const int xp1 = p.perm[xi + 1];
    const int p0 = xp0 + yi;
    const int p00 = p.perm[p0] + zi;
    const int p01 = p.perm[p0 + 1] + zi;
    const int p1 = xp1 + yi;
    const int p10 = p.perm[p1] + zi;
    const int p11 = p.perm[p1 + 1] + zi;

    const double q00 = lerp(xf, grad3(p.perm[p00], x, y, z), grad3(p.perm[p10], x1, y, z));
    const double q01 = lerp(xf, grad3(p.perm[p01], x, y1, z), grad3(p.perm[p11], x1, y1, z));
    const double q10 = lerp(xf, grad3(p.perm[p00 + 1], x, y, z1), grad3(p.perm[p10 + 1], x1, y, z1));
    const double q11 = lerp(xf, grad3(p.perm[p01 + 1], x, y1, z1), grad3(p.perm[p11 + 1], x1, y1, z1));
    const double r0 = lerp(yf, q00, q01);
    const double r1 = lerp(yf, q10, q11);
    return lerp(zf, r0, r1);
}



/** Matches addNoiseCachedAxesStridedActiveY with yLen=17 and active y=11..16. */
P20_HD void perlin3Upper6(
        const PerlinState& p,
        double xCoord,
        double yScale,
        double zCoord,
        double out6[6]
) {
    double x = xCoord + p.a;
    double z = zCoord + p.c;
    const int fx = javaFloor(x);
    const int fz = javaFloor(z);
    const int xi = fx & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double z1 = z - 1.0;
    const double xf = fade(x);
    const double zf = fade(z);
    const int xp0 = p.perm[xi];
    const int xp1 = p.perm[xi + 1];

    int yIndex[17];
    int yCellStart[17];
    double yFrac[17];
    double yFracMinus1[17];
    double yFade[17];
    int previousYi = -2147483647;
    int cellStart = 0;
    for (int y = 0; y < 17; ++y) {
        double v = static_cast<double>(y) * yScale + p.b;
        const int floor = javaFloor(v);
        const int yi = floor & 255;
        if (y == 0 || yi != previousYi) {
            cellStart = y;
            previousYi = yi;
        }
        yIndex[y] = yi;
        yCellStart[y] = cellStart;
        v -= static_cast<double>(floor);
        yFrac[y] = v;
        yFracMinus1[y] = v - 1.0;
        yFade[y] = fade(v);
    }

    int activeCellStart = -1;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int k = 0; k < 6; ++k) {
        const int y = 11 + k;
        const int yi = yIndex[y];
        const int firstY = yCellStart[y];
        if (firstY != activeCellStart) {
            activeCellStart = firstY;
            const double yf = yFrac[firstY];
            const double yfm1 = yFracMinus1[firstY];
            const int p0 = xp0 + yi;
            const int p00 = p.perm[p0] + zi;
            const int p01 = p.perm[p0 + 1] + zi;
            const int p1 = xp1 + yi;
            const int p10 = p.perm[p1] + zi;
            const int p11 = p.perm[p1 + 1] + zi;
            q00 = lerp(xf, grad3(p.perm[p00], x, yf, z), grad3(p.perm[p10], x1, yf, z));
            q01 = lerp(xf, grad3(p.perm[p01], x, yfm1, z), grad3(p.perm[p11], x1, yfm1, z));
            q10 = lerp(xf, grad3(p.perm[p00 + 1], x, yf, z1), grad3(p.perm[p10 + 1], x1, yf, z1));
            q11 = lerp(xf, grad3(p.perm[p01 + 1], x, yfm1, z1), grad3(p.perm[p11 + 1], x1, yfm1, z1));
        }
        const double r0 = lerp(yFade[y], q00, q01);
        const double r1 = lerp(yFade[y], q10, q11);
        out6[k] = lerp(zf, r0, r1);
    }
}


// Shared exact Y-axis setup for the six upper terrain lattice samples (y=11..16).
// The values depend only on the octave Y scale and Perlin Y offset, so one lane
// can compute them once per block and all X/Z lanes can reuse them.
struct Upper6YAxisCache {
    int yIndex[17];
    int yCellStart[17];
    double yFrac[17];
    double yFracMinus1[17];
    double yFade[17];
};

P20_HD void buildUpper6YAxisCache(
        const PerlinState& p,
        double yScale,
        Upper6YAxisCache& cache
) {
    int previousYi = -2147483647;
    int cellStart = 0;
    for (int y = 0; y < 17; ++y) {
        double v = static_cast<double>(y) * yScale + p.b;
        const int floor = javaFloor(v);
        const int yi = floor & 255;
        if (y == 0 || yi != previousYi) {
            cellStart = y;
            previousYi = yi;
        }
        cache.yIndex[y] = yi;
        cache.yCellStart[y] = cellStart;
        v -= static_cast<double>(floor);
        cache.yFrac[y] = v;
        cache.yFracMinus1[y] = v - 1.0;
        cache.yFade[y] = fade(v);
    }
}

P20_HD void perlin3Upper6SharedY(
        const PerlinState& p,
        double xCoord,
        double zCoord,
        const Upper6YAxisCache& cache,
        double out6[6]
) {
    double x = xCoord + p.a;
    double z = zCoord + p.c;
    const int fx = javaFloor(x);
    const int fz = javaFloor(z);
    const int xi = fx & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double z1 = z - 1.0;
    const double xf = fade(x);
    const double zf = fade(z);
    const int xp0 = p.perm[xi];
    const int xp1 = p.perm[xi + 1];

    int activeCellStart = -1;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int k = 0; k < 6; ++k) {
        const int y = 11 + k;
        const int yi = cache.yIndex[y];
        const int firstY = cache.yCellStart[y];
        if (firstY != activeCellStart) {
            activeCellStart = firstY;
            const double yf = cache.yFrac[firstY];
            const double yfm1 = cache.yFracMinus1[firstY];
            const int p0 = xp0 + yi;
            const int p00 = p.perm[p0] + zi;
            const int p01 = p.perm[p0 + 1] + zi;
            const int p1 = xp1 + yi;
            const int p10 = p.perm[p1] + zi;
            const int p11 = p.perm[p1 + 1] + zi;
            q00 = lerp(xf, grad3(p.perm[p00], x, yf, z), grad3(p.perm[p10], x1, yf, z));
            q01 = lerp(xf, grad3(p.perm[p01], x, yfm1, z), grad3(p.perm[p11], x1, yfm1, z));
            q10 = lerp(xf, grad3(p.perm[p00 + 1], x, yf, z1), grad3(p.perm[p10 + 1], x1, yf, z1));
            q11 = lerp(xf, grad3(p.perm[p01 + 1], x, yfm1, z1), grad3(p.perm[p11 + 1], x1, yfm1, z1));
        }
        const double r0 = lerp(cache.yFade[y], q00, q01);
        const double r1 = lerp(cache.yFade[y], q10, q11);
        out6[k] = lerp(zf, r0, r1);
    }
}

/** Matches addNoise2DCachedAxesStrided for one exact point. */
P20_HD double perlin2(const PerlinState& p, double xCoord, double zCoord) {
    double x = xCoord + p.a;
    double z = zCoord + p.c;
    const int fx = javaFloor(x);
    const int fz = javaFloor(z);
    const int xi = fx & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double z1 = z - 1.0;
    const double xf = fade(x);
    const double zf = fade(z);

    const int xp0 = p.perm[xi];
    const int xp1 = p.perm[xi + 1];
    const int p00 = p.perm[xp0] + zi;
    const int p10 = p.perm[xp1] + zi;

    const double q0 = lerp(xf,
            grad2Legacy(p.perm[p00], x, z),
            grad3(p.perm[p10], x1, 0.0, z));
    const double q1 = lerp(xf,
            grad3(p.perm[p00 + 1], x, 0.0, z1),
            grad3(p.perm[p10 + 1], x1, 0.0, z1));
    return lerp(zf, q0, q1);
}

P20_HD int simplexFastFloor(double v) {
    return v > 0.0 ? static_cast<int>(v) : static_cast<int>(v) - 1;
}

P20_HD double simplexGrad2(int gradient, double x, double z) {
    switch (gradient) {
        case 0: return 1.0 * x + 1.0 * z;
        case 1: return -1.0 * x + 1.0 * z;
        case 2: return 1.0 * x + -1.0 * z;
        case 3: return -1.0 * x + -1.0 * z;
        case 4:
        case 6: return 1.0 * x + 0.0 * z;
        case 5:
        case 7: return -1.0 * x + 0.0 * z;
        case 8:
        case 10: return 0.0 * x + 1.0 * z;
        default: return 0.0 * x + -1.0 * z;
    }
}

P20_HD double simplex2(const PerlinState& s, double xCoord, double zCoord) {
    // NoiseGenerator2173 has identical constructor/permutation state layout to PerlinState.
    // Java computes these from sqrt(3); hard-coded literals preserve the exact double values.
    constexpr double F = 0.3660254037844386;
    constexpr double G = 0.21132486540518713;

    const double x = xCoord + s.a;
    const double z = zCoord + s.b;
    const double skew = (x + z) * F;
    const int i = simplexFastFloor(x + skew);
    const int j = simplexFastFloor(z + skew);
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
    const int g0 = s.perm[ii + s.perm[jj]] % 12;
    const int g1 = s.perm[ii + i1 + s.perm[jj + j1]] % 12;
    const int g2 = s.perm[ii + 1 + s.perm[jj + 1]] % 12;

    double t0 = 0.5 - x0 * x0 - z0 * z0;
    double n0;
    if (t0 < 0.0) n0 = 0.0;
    else { t0 *= t0; n0 = t0 * t0 * simplexGrad2(g0, x0, z0); }

    double t1 = 0.5 - x1 * x1 - z1 * z1;
    double n1;
    if (t1 < 0.0) n1 = 0.0;
    else { t1 *= t1; n1 = t1 * t1 * simplexGrad2(g1, x1, z1); }

    double t2 = 0.5 - x2 * x2 - z2 * z2;
    double n2;
    if (t2 < 0.0) n2 = 0.0;
    else { t2 *= t2; n2 = t2 * t2 * simplexGrad2(g2, x2, z2); }

    return 70.0 * (n0 + n1 + n2);
}

P20_HD std::uint64_t splitMixDeterministicSeed(std::uint64_t sequenceSeed, std::uint64_t attempt) {
    std::uint64_t z = sequenceSeed + attempt * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

inline std::uint64_t doubleBits(double v) {
    std::uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

} // namespace p20
