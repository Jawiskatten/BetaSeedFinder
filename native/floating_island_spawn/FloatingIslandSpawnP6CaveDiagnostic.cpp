#define main p6_raw_spawn_diag_embedded_main
#include "FloatingIslandSpawnP6SpawnDiagnostic.cpp"
#undef main

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace floating_island_spawn_p6_caves {
using namespace highest_pillar_spawn_p1;
namespace raw = floating_island_spawn_p6_diag;

static constexpr int AIR = 0;
static constexpr int STONE = 1;
static constexpr int GRASS = 2;
static constexpr int DIRT = 3;
static constexpr int WATER_MOVING = 8;
static constexpr int WATER_STILL = 9;
static constexpr int LAVA_MOVING = 10;
static constexpr int SAND = 12;
static constexpr int SANDSTONE = 24;
static constexpr std::uint64_t JR_MASK = (1ULL << 48) - 1ULL;

class JRandom {
public:
    JRandom() { setSeed(0); }
    explicit JRandom(std::int64_t seed) { setSeed(seed); }
    void setSeed(std::int64_t seed) {
        state_ = (static_cast<std::uint64_t>(seed) ^ 0x5DEECE66DULL) & JR_MASK;
    }
    std::uint32_t nextBits(int bits) {
        state_ = (state_ * 0x5DEECE66DULL + 0xBULL) & JR_MASK;
        return static_cast<std::uint32_t>(state_ >> (48 - bits));
    }
    int nextInt(int bound) {
        if (bound <= 0) throw std::runtime_error("Java Random bound must be positive");
        if ((bound & -bound) == bound) {
            return static_cast<int>((static_cast<std::int64_t>(bound) * static_cast<std::int64_t>(nextBits(31))) >> 31);
        }
        for (;;) {
            const std::int32_t bits = static_cast<std::int32_t>(nextBits(31));
            const std::int32_t val = bits % bound;
            const std::int32_t test = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(bits) - static_cast<std::uint32_t>(val) + static_cast<std::uint32_t>(bound - 1));
            if (test >= 0) return val;
        }
    }
    std::int64_t nextLong() {
        const std::int32_t hi = static_cast<std::int32_t>(nextBits(32));
        const std::int32_t lo = static_cast<std::int32_t>(nextBits(32));
        const std::uint64_t top = static_cast<std::uint64_t>(static_cast<std::int64_t>(hi)) << 32;
        const std::uint64_t sum = top + static_cast<std::uint64_t>(static_cast<std::int64_t>(lo));
        std::int64_t out;
        std::memcpy(&out, &sum, sizeof(out));
        return out;
    }
    float nextFloat() { return static_cast<float>(nextBits(24)) / 16777216.0f; }
    double nextDouble() {
        const std::uint64_t a = nextBits(26);
        const std::uint64_t b = nextBits(27);
        return static_cast<double>((a << 27) + b) / 9007199254740992.0;
    }
private:
    std::uint64_t state_ = 0;
};

static const std::array<float, 65536>& sinTable() {
    static const std::array<float, 65536> table = [] {
        std::array<float, 65536> t{};
        constexpr double pi = 3.14159265358979323846264338327950288;
        for (int i = 0; i < 65536; ++i) t[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(static_cast<double>(i) * pi * 2.0 / 65536.0));
        return t;
    }();
    return table;
}
static float mcSin(float v) { return sinTable()[static_cast<std::uint16_t>(static_cast<int>(v * 10430.378f))]; }
static float mcCos(float v) { return sinTable()[static_cast<std::uint16_t>(static_cast<int>(v * 10430.378f + 16384.0f))]; }
static int mcFloor(double v) { const int i = static_cast<int>(v); return v < static_cast<double>(i) ? i - 1 : i; }

static bool biomeIsDesert(double temperature, double rainfall) {
    int ti = static_cast<int>(temperature * 63.0);
    int ri = static_cast<int>(rainfall * 63.0);
    ti = std::max(0, std::min(63, ti));
    ri = std::max(0, std::min(63, ri));
    const float f = static_cast<float>(ti) / 63.0f;
    float wet = static_cast<float>(ri) / 63.0f;
    wet *= f;
    return wet < 0.2f && f >= 0.95f;
}

static std::array<int,128> buildOriginColumn(
        const double* density,
        double sandNoise,
        double stoneNoise,
        double temperature,
        double rainfall) {
    std::array<int,128> b{};
    for (int y = 0; y < 128; ++y) {
        if (raw::solidAtBlockHost(density, 0, y, 0)) b[static_cast<std::size_t>(y)] = STONE;
        else b[static_cast<std::size_t>(y)] = y < 64 ? WATER_STILL : AIR;
    }

    JRandom surface(0); // chunk (0,0): rand.setSeed(0*341873128712 + 0*132897987541)
    const bool sandPatch = sandNoise + surface.nextDouble() * 0.2 > 0.0;
    (void)surface.nextDouble(); // gravel jitter; sand wins on spawn-valid beach columns
    const int depth = static_cast<int>(stoneNoise / 3.0 + 3.0 + surface.nextDouble() * 0.25);
    const bool desert = biomeIsDesert(temperature, rainfall);
    int remaining = -1;
    int top = desert ? SAND : GRASS;
    int filler = desert ? SAND : DIRT;

    for (int y = 127; y >= 0; --y) {
        if (b[static_cast<std::size_t>(y)] == AIR || b[static_cast<std::size_t>(y)] == WATER_STILL) {
            remaining = -1;
            continue;
        }
        if (b[static_cast<std::size_t>(y)] != STONE) continue;
        if (remaining == -1) {
            top = desert ? SAND : GRASS;
            filler = desert ? SAND : DIRT;
            if (depth <= 0) {
                top = AIR;
                filler = STONE;
            } else if (y >= 60 && y <= 65 && sandPatch) {
                top = SAND;
                filler = SAND;
            }
            if (y < 64 && top == AIR) top = WATER_STILL;
            remaining = depth;
            b[static_cast<std::size_t>(y)] = y >= 63 ? top : filler;
        } else if (remaining > 0) {
            --remaining;
            b[static_cast<std::size_t>(y)] = filler;
            if (remaining == 0 && filler == SAND) {
                // Exact sandstone thickness is irrelevant for the spawn-height diagnostic;
                // sandstone is non-carvable by Beta MapGenCaves either way.
                filler = SANDSTONE;
            }
        }
    }
    return b;
}

static bool rawWaterAt(const double* density, int x, int y, int z) {
    if (y < 0 || y >= 64) return false;
    return !raw::solidAtBlockHost(density, x, y, z);
}
static bool fullCollisionBlock(int id) {
    return id != AIR && id != WATER_MOVING && id != WATER_STILL && id != LAVA_MOVING;
}

class CaveOriginSimulator {
public:
    CaveOriginSimulator(std::int64_t worldSeed, const double* density, std::array<int,128>& origin)
        : worldSeed_(worldSeed), density_(density), origin_(origin) {}

    void generateTargetChunkZero() {
        rand_.setSeed(worldSeed_);
        const std::int64_t a = odd(rand_.nextLong());
        const std::int64_t b = odd(rand_.nextLong());
        for (int sx = -8; sx <= 8; ++sx) {
            for (int sz = -8; sz <= 8; ++sz) {
                const std::uint64_t mixed = static_cast<std::uint64_t>(static_cast<std::int64_t>(sx)) * static_cast<std::uint64_t>(a)
                    + static_cast<std::uint64_t>(static_cast<std::int64_t>(sz)) * static_cast<std::uint64_t>(b)
                    ^ static_cast<std::uint64_t>(worldSeed_);
                std::int64_t sourceSeed;
                std::memcpy(&sourceSeed, &mixed, sizeof(sourceSeed));
                rand_.setSeed(sourceSeed);
                recursiveGenerate(sx, sz);
            }
        }
    }

    const std::vector<int>& carvedOriginY() const { return carved_; }

private:
    static std::int64_t odd(std::int64_t v) { return (v / 2) * 2 + 1; }

    void recursiveGenerate(int sourceChunkX, int sourceChunkZ) {
        int count = rand_.nextInt(rand_.nextInt(rand_.nextInt(40) + 1) + 1);
        if (rand_.nextInt(15) != 0) count = 0;
        for (int n = 0; n < count; ++n) {
            const double x = static_cast<double>(sourceChunkX * 16 + rand_.nextInt(16));
            const double y = static_cast<double>(rand_.nextInt(rand_.nextInt(120) + 8));
            const double z = static_cast<double>(sourceChunkZ * 16 + rand_.nextInt(16));
            int branches = 1;
            if (rand_.nextInt(4) == 0) {
                generateLarge(x, y, z);
                branches += rand_.nextInt(4);
            }
            for (int k = 0; k < branches; ++k) {
                const float yaw = rand_.nextFloat() * 3.1415927f * 2.0f;
                const float pitch = (rand_.nextFloat() - 0.5f) * 2.0f / 8.0f;
                const float width = rand_.nextFloat() * 2.0f + rand_.nextFloat();
                generateNode(x, y, z, width, yaw, pitch, 0, 0, 1.0);
            }
        }
    }

    void generateLarge(double x, double y, double z) {
        generateNode(x, y, z, 1.0f + rand_.nextFloat() * 6.0f, 0.0f, 0.0f, -1, -1, 0.5);
    }

    bool waterInBounds(int minX, int maxX, int minY, int maxY, int minZ, int maxZ) const {
        for (int x = minX; x < maxX; ++x) {
            for (int z = minZ; z < maxZ; ++z) {
                for (int y = maxY + 1; y >= minY - 1; --y) {
                    if (y >= 0 && y < 128 && rawWaterAt(density_, x, y, z)) return true;
                    if (y != minY - 1 && x != minX && x != maxX - 1 && z != minZ && z != maxZ - 1) y = minY;
                }
            }
        }
        return false;
    }

    void carveOriginAtStep(double cx, double cy, double cz, double horizontalRadius, double verticalRadius,
                           int minX, int maxX, int minY, int maxY, int minZ, int maxZ) {
        if (!(0 >= minX && 0 < maxX && 0 >= minZ && 0 < maxZ)) return;
        const double dx = (0.5 - cx) / horizontalRadius;
        const double dz = (0.5 - cz) / horizontalRadius;
        if (dx * dx + dz * dz >= 1.0) return;
        bool sawGrass = false;
        for (int y = maxY - 1; y >= minY; --y) {
            const double dy = (static_cast<double>(y) + 0.5 - cy) / verticalRadius;
            if (dy <= -0.7 || dx * dx + dy * dy + dz * dz >= 1.0) continue;
            int& id = origin_[static_cast<std::size_t>(y)];
            if (id == GRASS) sawGrass = true;
            if (id == STONE || id == DIRT || id == GRASS) {
                id = y < 10 ? LAVA_MOVING : AIR;
                if (std::find(carved_.begin(), carved_.end(), y) == carved_.end()) carved_.push_back(y);
                if (sawGrass && y > 0 && origin_[static_cast<std::size_t>(y - 1)] == DIRT) origin_[static_cast<std::size_t>(y - 1)] = GRASS;
            }
        }
    }

    void generateNode(double x, double y, double z, float width, float yaw, float pitch,
                      int step, int length, double verticalScale) {
        constexpr double centerX = 8.0;
        constexpr double centerZ = 8.0;
        float yawVel = 0.0f;
        float pitchVel = 0.0f;
        JRandom local(rand_.nextLong());
        if (length <= 0) {
            const int maxLen = 8 * 16 - 16;
            length = maxLen - local.nextInt(maxLen / 4);
        }
        bool large = false;
        if (step == -1) { step = length / 2; large = true; }
        const int split = local.nextInt(length / 2) + length / 4;
        const bool gentlePitch = local.nextInt(6) == 0;

        for (; step < length; ++step) {
            const double radius = 1.5 + static_cast<double>(mcSin(static_cast<float>(step) * 3.1415927f / static_cast<float>(length)) * width);
            const double vradius = radius * verticalScale;
            const float cp = mcCos(pitch);
            const float sp = mcSin(pitch);
            x += static_cast<double>(mcCos(yaw) * cp);
            y += static_cast<double>(sp);
            z += static_cast<double>(mcSin(yaw) * cp);
            pitch *= gentlePitch ? 0.92f : 0.7f;
            pitch += pitchVel * 0.1f;
            yaw += yawVel * 0.1f;
            pitchVel *= 0.9f;
            yawVel *= 0.75f;
            pitchVel += (local.nextFloat() - local.nextFloat()) * local.nextFloat() * 2.0f;
            yawVel += (local.nextFloat() - local.nextFloat()) * local.nextFloat() * 4.0f;

            if (!large && step == split && width > 1.0f) {
                generateNode(x, y, z, local.nextFloat() * 0.5f + 0.5f, yaw - 1.5707964f, pitch / 3.0f, step, length, 1.0);
                generateNode(x, y, z, local.nextFloat() * 0.5f + 0.5f, yaw + 1.5707964f, pitch / 3.0f, step, length, 1.0);
                return;
            }

            if (large || local.nextInt(4) != 0) {
                const double ddx = x - centerX;
                const double ddz = z - centerZ;
                const double remaining = static_cast<double>(length - step);
                const double reach = static_cast<double>(width + 18.0f);
                if (ddx * ddx + ddz * ddz - remaining * remaining > reach * reach) return;

                if (x >= centerX - 16.0 - radius * 2.0 && z >= centerZ - 16.0 - radius * 2.0 &&
                    x <= centerX + 16.0 + radius * 2.0 && z <= centerZ + 16.0 + radius * 2.0) {
                    int minX = mcFloor(x - radius) - 1;
                    int maxX = mcFloor(x + radius) + 1;
                    int minY = mcFloor(y - vradius) - 1;
                    int maxY = mcFloor(y + vradius) + 1;
                    int minZ = mcFloor(z - radius) - 1;
                    int maxZ = mcFloor(z + radius) + 1;
                    minX = std::max(0, minX); maxX = std::min(16, maxX);
                    minY = std::max(1, minY); maxY = std::min(120, maxY);
                    minZ = std::max(0, minZ); maxZ = std::min(16, maxZ);
                    if (!waterInBounds(minX, maxX, minY, maxY, minZ, maxZ)) {
                        carveOriginAtStep(x, y, z, radius, vradius, minX, maxX, minY, maxY, minZ, maxZ);
                        if (large) break;
                    }
                }
            }
        }
    }

    std::int64_t worldSeed_;
    const double* density_;
    std::array<int,128>& origin_;
    JRandom rand_;
    std::vector<int> carved_;
};

static std::string originRuns(const std::array<int,128>& b, int lo, int hi) {
    std::ostringstream out;
    bool first = true;
    for (int y = lo; y <= hi;) {
        if (!fullCollisionBlock(b[static_cast<std::size_t>(y)])) { ++y; continue; }
        const int a = y;
        while (y + 1 <= hi && fullCollisionBlock(b[static_cast<std::size_t>(y + 1)])) ++y;
        if (!first) out << ',';
        first = false;
        if (a == y) out << a; else out << a << '-' << y;
        ++y;
    }
    return first ? "none" : out.str();
}

static int firstUncovered(const std::array<int,128>& b) {
    int y = 63;
    while (y + 1 < 128 && b[static_cast<std::size_t>(y + 1)] != AIR) ++y;
    return y;
}
static int firstSolidAbove(const std::array<int,128>& b, int y0) {
    for (int y = y0 + 1; y < 128; ++y) if (fullCollisionBlock(b[static_cast<std::size_t>(y)])) return y;
    return -1;
}
static bool aabbCollision(const std::array<int,128>& b, int posY) {
    for (int y : {posY - 2, posY - 1, posY}) if (y >= 0 && y < 128 && fullCollisionBlock(b[static_cast<std::size_t>(y)])) return true;
    return false;
}
static int clearEntityPosY(const std::array<int,128>& b) {
    int y = 65;
    while (y < 130 && aabbCollision(b, y)) ++y;
    return y;
}
static int landingSupport(const std::array<int,128>& b, int clearY) {
    for (int y = std::min(127, clearY - 3); y >= 0; --y) if (fullCollisionBlock(b[static_cast<std::size_t>(y)])) return y;
    return -1;
}
static const char* blockName(int id) {
    switch (id) {
        case AIR: return "air"; case STONE: return "stone"; case GRASS: return "grass"; case DIRT: return "dirt";
        case WATER_MOVING: return "waterMoving"; case WATER_STILL: return "waterStill"; case LAVA_MOVING: return "lavaMoving";
        case SAND: return "sand"; case SANDSTONE: return "sandstone"; default: return "other";
    }
}

static void diagnoseOne(std::int64_t seed, const double* density, double sandNoise, double stoneNoise, double temp, double rain) {
    auto blocks = buildOriginColumn(density, sandNoise, stoneNoise, temp, rain);
    std::cout << "\nseed=" << seed << '\n';
    std::cout << "pre_cave_origin_fullblock_runs_y56_110=" << originRuns(blocks, 56, 110) << '\n';
    CaveOriginSimulator caves(seed, density, blocks);
    caves.generateTargetChunkZero();
    auto carved = caves.carvedOriginY();
    std::sort(carved.begin(), carved.end());
    std::cout << "cave_carved_origin_y=";
    if (carved.empty()) std::cout << "none";
    else for (std::size_t i = 0; i < carved.size(); ++i) { if (i) std::cout << ','; std::cout << carved[i]; }
    std::cout << '\n';
    std::cout << "post_cave_origin_fullblock_runs_y56_110=" << originRuns(blocks, 56, 110) << '\n';

    const int surface = firstUncovered(blocks);
    const int spawnBlock = blocks[static_cast<std::size_t>(surface)];
    const bool originSpawnValid = spawnBlock == SAND;
    const int upper = firstSolidAbove(blocks, surface);
    const int gap = upper >= 0 ? upper - surface - 1 : -1;
    const int clearY = clearEntityPosY(blocks);
    const int support = landingSupport(blocks, clearY);
    const bool onUpper = originSpawnValid && upper >= 0 && support >= upper;
    std::cout << "final_origin_first_uncovered_y=" << surface
              << " block=" << blockName(spawnBlock)
              << " originSpawnValid=" << (originSpawnValid ? "YES" : "NO") << '\n';
    std::cout << "final_first_upper_fullblock_y=" << upper << " final_air_gap=" << gap << '\n';
    std::cout << "vanilla_origin_AABB clearEntityPosY=" << clearY
              << " landingSupportY=" << support
              << " landingFeetY=" << (support >= 0 ? support + 1 : -1)
              << " landsOnFinalUpper=" << (onUpper ? "YES" : "NO") << '\n';
    if (!originSpawnValid) {
        std::cout << "NEXT: vanilla will random-walk away from (0,0); exact spawn-coordinate search is required for this seed.\n";
    }
}

static int run(const std::vector<std::int64_t>& seeds) {
    printDevice();
    DeviceBuffers b = allocateBuffers(static_cast<int>(seeds.size()));
    try {
        checkHip(hipMemcpy(b.seeds, seeds.data(), seeds.size() * sizeof(std::int64_t), hipMemcpyHostToDevice), "copy P6 cave diagnostic seeds");
        launchTerrain(b, static_cast<int>(seeds.size()), 64);
        std::vector<double> density(seeds.size() * static_cast<std::size_t>(coarsecore::CELLS));
        std::vector<double> sand(seeds.size()), stone(seeds.size()), temp(seeds.size()), rain(seeds.size());
        checkHip(hipMemcpy(density.data(), b.noise1, density.size() * sizeof(double), hipMemcpyDeviceToHost), "copy density");
        checkHip(hipMemcpy(sand.data(), b.originSandNoise, sand.size() * sizeof(double), hipMemcpyDeviceToHost), "copy sand noise");
        checkHip(hipMemcpy(stone.data(), b.originStoneNoise, stone.size() * sizeof(double), hipMemcpyDeviceToHost), "copy stone noise");
        checkHip(hipMemcpy(temp.data(), b.originTemperature, temp.size() * sizeof(double), hipMemcpyDeviceToHost), "copy temperature");
        checkHip(hipMemcpy(rain.data(), b.originRainfall, rain.size() * sizeof(double), hipMemcpyDeviceToHost), "copy rainfall");
        for (std::size_t i = 0; i < seeds.size(); ++i) diagnoseOne(seeds[i], density.data() + i * static_cast<std::size_t>(coarsecore::CELLS), sand[i], stone[i], temp[i], rain[i]);
    } catch (...) {
        freeBuffers(b);
        throw;
    }
    freeBuffers(b);
    std::cout << "\nP6 cave diagnostic models Beta MapGenCaves' RNG/path geometry for target chunk (0,0),"
                 " including its water-abort check from the generated base terrain. If originSpawnValid=NO,"
                 " the remaining missing piece is vanilla's random spawn-coordinate walk.\n";
    return 0;
}

} // namespace floating_island_spawn_p6_caves

int main(int argc, char** argv) {
    try {
        std::vector<std::int64_t> seeds;
        for (int i = 1; i < argc; ++i) seeds.push_back(std::stoll(argv[i]));
        if (seeds.empty()) { seeds.push_back(-3405360075020439777LL); seeds.push_back(6430576860599818994LL); }
        return floating_island_spawn_p6_caves::run(seeds);
    } catch (const std::exception& e) {
        std::cerr << "P6 cave diagnostic ERROR: " << e.what() << '\n';
        return 1;
    }
}
