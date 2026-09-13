#define main highest_pillar_spawn_p1_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace floating_island_spawn_p6_verify {
using namespace highest_pillar_spawn_p1;

struct Candidate {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    int spawnSurfaceY = -1;
    int firstUpperY = -1;
    int airGap = 0;
    int playerFeetY = -1;
    int supportY = -1;
    int sandReason = 0;
};

struct Gate {
    bool qualified = false;
    int spawnSurfaceY = -1;
    int firstUpperY = -1;
    int airGap = 0;
    int playerFeetY = -1;
    int supportY = -1;
    int sandReason = 0;
};

struct ComponentStats {
    bool contained = false;
    bool groundConnected = false;
    bool touchedBoundary = false;
    int blocks = 0;
    int footprint = 0;
    int topSurface = 0;
    int minY = 128;
    int maxY = -1;
    int minX = INT_MAX;
    int maxX = INT_MIN;
    int minZ = INT_MAX;
    int maxZ = INT_MIN;
    int spanX = 0;
    int spanZ = 0;
};

struct FloatingRecord {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    std::int64_t score = 0;
    int spawnSurfaceY = -1;
    int firstUpperY = -1;
    int airGap = 0;
    int playerFeetY = -1;
    int supportY = -1;
    int sandReason = 0;
    int blocks = 0;
    int footprint = 0;
    int topSurface = 0;
    int minY = -1;
    int maxY = -1;
    int minX = 0;
    int maxX = 0;
    int minZ = 0;
    int maxZ = 0;
    int spanX = 0;
    int spanZ = 0;
    int verifyChunkRadius = 0;
};

struct Config {
    std::filesystem::path input;
    std::filesystem::path outputDir;
    std::filesystem::path boundaryOut;
    int batch = 512;
    int terrainThreads = 64;
    int top = 500;
    int chunkRadius = 1;
    bool selfTest = false;
};

static int parseInt(const std::string& value, const char* name) {
    std::size_t used = 0;
    const int out = std::stoi(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}

static Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (++i >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[i];
        };
        if (arg == "--input") c.input = value("--input");
        else if (arg == "--output") c.outputDir = value("--output");
        else if (arg == "--boundary-out") c.boundaryOut = value("--boundary-out");
        else if (arg == "--batch") c.batch = parseInt(value("--batch"), "batch");
        else if (arg == "--terrain-threads") c.terrainThreads = parseInt(value("--terrain-threads"), "terrain-threads");
        else if (arg == "--top") c.top = parseInt(value("--top"), "top");
        else if (arg == "--chunk-radius") c.chunkRadius = parseInt(value("--chunk-radius"), "chunk-radius");
        else if (arg == "--self-test") c.selfTest = true;
        else throw std::invalid_argument("unknown argument: " + arg);
    }
    if (!c.selfTest && c.input.empty()) throw std::invalid_argument("--input is required");
    if (!c.selfTest && c.outputDir.empty()) throw std::invalid_argument("--output is required");
    if (!c.selfTest && c.boundaryOut.empty()) throw std::invalid_argument("--boundary-out is required");
    if (c.batch < 1 || c.batch > 4096) throw std::invalid_argument("--batch must be 1..4096");
    if (c.terrainThreads != 64 && c.terrainThreads != 128 && c.terrainThreads != 256) {
        throw std::invalid_argument("--terrain-threads must be 64, 128, or 256");
    }
    if (c.top < 1 || c.top > 10000) throw std::invalid_argument("--top must be 1..10000");
    if (c.chunkRadius < 0 || c.chunkRadius > 16) throw std::invalid_argument("--chunk-radius must be 0..16");
    return c;
}

static int floorDiv16(int v) {
    return v >= 0 ? v / 16 : -((-v + 15) / 16);
}
static int mod16(int v) {
    const int q = floorDiv16(v);
    return v - q * 16;
}

static void launchChunk(DeviceBuffers& b, int count, int terrainThreads, int chunkX, int chunkZ) {
    const int coarseOffsetX = chunkX * 4;
    const int coarseOffsetZ = chunkZ * 4;
#if defined(SKYBLOCK_COARSE_API_MODERN)
    hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel,
        dim3(count), dim3(terrainThreads), 0, 0,
        b.seeds, count,
        b.temp, b.rain, b.climateBlend,
        b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
        b.signs,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        coarseOffsetX, coarseOffsetZ,
        nullptr, nullptr, nullptr, nullptr, nullptr);
#else
#error "P6 chunk verifier requires modern coarse GPU API"
#endif
    checkHip(hipGetLastError(), "launch P6 chunk-local terrain");
    checkHip(hipDeviceSynchronize(), "finish P6 chunk-local terrain");
}

static double nodeDensity(const double* d, int x, int y, int z) {
    if (x < 0 || z < 0 || x >= 5 || z >= 5) return -10.0;
    if (y < 0) return 10.0;
    if (y >= coarsecore::Y_LEVELS) return -10.0;
    return d[coarsecore::index3(x, y, z)];
}

static double densityAtLocalBlock(const double* d, int localX, int worldY, int localZ) {
    if (worldY < coarsecore::Y_BASE * 8) return 10.0;
    if (worldY >= 128) return -10.0;
    const int ix = localX >> 2;
    const int iz = localZ >> 2;
    const int iy = (worldY >> 3) - coarsecore::Y_BASE;
    const double fx = static_cast<double>(localX & 3) * 0.25;
    const double fz = static_cast<double>(localZ & 3) * 0.25;
    const double fy = static_cast<double>(worldY & 7) * 0.125;
    const double d000 = nodeDensity(d, ix,     iy,     iz);
    const double d001 = nodeDensity(d, ix,     iy,     iz + 1);
    const double d100 = nodeDensity(d, ix + 1, iy,     iz);
    const double d101 = nodeDensity(d, ix + 1, iy,     iz + 1);
    const double d010 = nodeDensity(d, ix,     iy + 1, iz);
    const double d011 = nodeDensity(d, ix,     iy + 1, iz + 1);
    const double d110 = nodeDensity(d, ix + 1, iy + 1, iz);
    const double d111 = nodeDensity(d, ix + 1, iy + 1, iz + 1);
    const double a0 = d000 + (d100 - d000) * fx;
    const double a1 = d001 + (d101 - d001) * fx;
    const double b0 = d010 + (d110 - d010) * fx;
    const double b1 = d011 + (d111 - d011) * fx;
    const double low = a0 + (a1 - a0) * fz;
    const double high = b0 + (b1 - b0) * fz;
    return low + (high - low) * fy;
}

class TileView {
public:
    TileView(const std::vector<double>& tiles, int batchCount, int candidateIndex, int radius)
        : tiles_(tiles), n_(batchCount), i_(candidateIndex), r_(radius), tileSide_(radius * 2 + 1) {}

    bool solid(int worldX, int worldY, int worldZ) const {
        if (worldY < coarsecore::Y_BASE * 8) return true;
        if (worldY >= 128) return false;
        const int cx = floorDiv16(worldX);
        const int cz = floorDiv16(worldZ);
        if (cx < -r_ || cx > r_ || cz < -r_ || cz > r_) return false;
        const int tx = cx + r_;
        const int tz = cz + r_;
        const int tile = tz * tileSide_ + tx;
        const std::size_t base = (static_cast<std::size_t>(tile) * static_cast<std::size_t>(n_) + static_cast<std::size_t>(i_))
                               * static_cast<std::size_t>(coarsecore::CELLS);
        const int lx = mod16(worldX);
        const int lz = mod16(worldZ);
        return densityAtLocalBlock(tiles_.data() + base, lx, worldY, lz) > 0.0;
    }

private:
    const std::vector<double>& tiles_;
    int n_;
    int i_;
    int r_;
    int tileSide_;
};

static Gate gateOrigin(const double* originDensity, double sandNoise, double stoneNoise,
                       double originTemp, double originRain) {
    Gate g;
    auto solidY = [&](int y) { return densityAtLocalBlock(originDensity, 0, y, 0) > 0.0; };
    int spawnY = 63;
    while (spawnY + 1 < 128 && solidY(spawnY + 1)) ++spawnY;
    if (!solidY(spawnY)) return g;
    g.spawnSurfaceY = spawnY;

    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool desert = betaBiomeIsDesert(originTemp, originRain);
    const bool beachSand = sandNoise + sandJitter * 0.2 > 0.0;
    const int surfaceDepth = static_cast<int>(stoneNoise / 3.0 + 3.0 + depthJitter * 0.25);
    const bool beachBand = spawnY >= 60 && spawnY <= 65;
    g.sandReason = desert ? 2 : ((beachBand && beachSand) ? 1 : 0);
    if (!(spawnY >= 63 && surfaceDepth > 0 && g.sandReason != 0)) return g;

    int feetY = 65;
    while (feetY < 128 && (solidY(feetY) || solidY(feetY + 1))) ++feetY;
    g.playerFeetY = feetY;
    if (feetY <= 65) return g;
    int support = feetY - 1;
    while (support >= 0 && !solidY(support)) --support;
    g.supportY = support;
    if (support < 0 || support != feetY - 1) return g;
    int firstUpper = -1;
    for (int y = spawnY + 1; y <= support; ++y) {
        if (solidY(y)) { firstUpper = y; break; }
    }
    if (firstUpper < 0) return g;
    g.firstUpperY = firstUpper;
    g.airGap = firstUpper - spawnY - 1;
    if (g.airGap < 1 || g.airGap > 2) return g;
    g.qualified = true;
    return g;
}

class ComponentVerifier {
public:
    explicit ComponentVerifier(int chunkRadius)
        : r_(chunkRadius), minX_(-chunkRadius * 16), maxX_((chunkRadius + 1) * 16 - 1),
          minZ_(-chunkRadius * 16), maxZ_((chunkRadius + 1) * 16 - 1),
          sideX_(maxX_ - minX_ + 1), sideZ_(maxZ_ - minZ_ + 1), plane_(sideX_ * sideZ_),
          visited_(static_cast<std::size_t>(plane_) * static_cast<std::size_t>(128 - coarsecore::Y_BASE * 8), 0),
          columnSeen_(static_cast<std::size_t>(plane_), 0) {
        queue_.reserve(static_cast<std::size_t>(plane_) * 4u);
    }

    ComponentStats trace(const TileView& view, int startY) {
        ComponentStats s;
        if (startY < coarsecore::Y_BASE * 8 || startY >= 128 || !view.solid(0, startY, 0)) return s;
        std::fill(visited_.begin(), visited_.end(), static_cast<unsigned char>(0));
        std::fill(columnSeen_.begin(), columnSeen_.end(), static_cast<unsigned char>(0));
        queue_.clear();
        const int yBase = coarsecore::Y_BASE * 8;

        auto idxOf = [&](int x, int y, int z) -> int {
            return (y - yBase) * plane_ + (z - minZ_) * sideX_ + (x - minX_);
        };
        auto enqueue = [&](int x, int y, int z) {
            if (y < yBase) { s.groundConnected = true; return; }
            if (y >= 128) return;
            if (x < minX_ || x > maxX_ || z < minZ_ || z > maxZ_) { s.touchedBoundary = true; return; }
            const int idx = idxOf(x, y, z);
            if (visited_[static_cast<std::size_t>(idx)]) return;
            if (!view.solid(x, y, z)) return;
            visited_[static_cast<std::size_t>(idx)] = 1;
            queue_.push_back(idx);
        };

        enqueue(0, startY, 0);
        std::size_t head = 0;
        while (head < queue_.size() && !s.groundConnected && !s.touchedBoundary) {
            const int idx = queue_[head++];
            const int yRel = idx / plane_;
            const int rem = idx - yRel * plane_;
            const int zi = rem / sideX_;
            const int xi = rem - zi * sideX_;
            const int x = xi + minX_;
            const int z = zi + minZ_;
            const int y = yRel + yBase;

            ++s.blocks;
            s.minY = std::min(s.minY, y); s.maxY = std::max(s.maxY, y);
            s.minX = std::min(s.minX, x); s.maxX = std::max(s.maxX, x);
            s.minZ = std::min(s.minZ, z); s.maxZ = std::max(s.maxZ, z);
            const int col = zi * sideX_ + xi;
            if (!columnSeen_[static_cast<std::size_t>(col)]) {
                columnSeen_[static_cast<std::size_t>(col)] = 1;
                ++s.footprint;
            }
            if (!view.solid(x, y + 1, z)) ++s.topSurface;

            enqueue(x + 1, y, z); enqueue(x - 1, y, z);
            enqueue(x, y, z + 1); enqueue(x, y, z - 1);
            enqueue(x, y + 1, z); enqueue(x, y - 1, z);
        }

        if (!s.groundConnected && !s.touchedBoundary && s.blocks > 0) {
            s.contained = true;
            s.spanX = s.maxX - s.minX + 1;
            s.spanZ = s.maxZ - s.minZ + 1;
        }
        return s;
    }

private:
    int r_, minX_, maxX_, minZ_, maxZ_, sideX_, sideZ_, plane_;
    std::vector<unsigned char> visited_, columnSeen_;
    std::vector<int> queue_;
};

static std::vector<Candidate> readCandidates(const std::filesystem::path& path) {
    std::vector<Candidate> rows;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read candidate file: " + path.string());
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::array<std::string, 8> x{};
        bool ok = true;
        for (int i = 0; i < 8; ++i) if (!std::getline(ss, x[static_cast<std::size_t>(i)], ',')) { ok = false; break; }
        if (!ok) continue;
        try {
            Candidate c;
            c.seed = std::stoll(x[0]); c.sequenceIndex = std::stoull(x[1]);
            c.spawnSurfaceY = std::stoi(x[2]); c.firstUpperY = std::stoi(x[3]); c.airGap = std::stoi(x[4]);
            c.playerFeetY = std::stoi(x[5]); c.supportY = std::stoi(x[6]); c.sandReason = std::stoi(x[7]);
            rows.push_back(c);
        } catch (...) {}
    }
    return rows;
}

static void writeCandidateHeader(std::ofstream& f) {
    f << "seed,sequence_index,spawn_surface_y,first_upper_y,air_gap,player_feet_y,support_y,sand_reason\n";
}
static void writeCandidate(std::ofstream& f, const Candidate& c) {
    f << c.seed << ',' << c.sequenceIndex << ',' << c.spawnSurfaceY << ',' << c.firstUpperY << ','
      << c.airGap << ',' << c.playerFeetY << ',' << c.supportY << ',' << c.sandReason << '\n';
}

static std::int64_t makeScore(const FloatingRecord& r) {
    return static_cast<std::int64_t>(r.blocks) * 1000000000LL
         + static_cast<std::int64_t>(std::min(9999, r.footprint)) * 100000LL
         + static_cast<std::int64_t>(std::min(9999, r.topSurface)) * 100LL
         + std::max(0, std::min(99, r.playerFeetY));
}
static std::int64_t metricLargest(const FloatingRecord& r) { return static_cast<std::int64_t>(r.blocks) * 1000000LL + r.footprint; }
static std::int64_t metricFootprint(const FloatingRecord& r) { return static_cast<std::int64_t>(r.footprint) * 1000000LL + r.blocks; }
static std::int64_t metricSurface(const FloatingRecord& r) { return static_cast<std::int64_t>(r.topSurface) * 1000000LL + r.blocks; }
static std::int64_t metricHighest(const FloatingRecord& r) { return static_cast<std::int64_t>(r.playerFeetY) * 1000000000LL + r.blocks; }
static std::int64_t metricIslandLike(const FloatingRecord& r) {
    return static_cast<std::int64_t>(r.footprint) * std::max(1, std::min(r.spanX, r.spanZ)) * 1000000LL + r.blocks;
}
static std::int64_t metricOneByOne(const FloatingRecord& r) {
    const int h = r.maxY - r.minY + 1;
    return static_cast<std::int64_t>(h) * 1000000000LL + static_cast<std::int64_t>(r.playerFeetY) * 1000000LL + r.blocks;
}
static std::int64_t metricTallThin(const FloatingRecord& r) {
    const int h = r.maxY - r.minY + 1;
    return static_cast<std::int64_t>(h) * 1000000000LL / std::max(1, r.footprint) + r.blocks;
}
static std::int64_t metricLongSkinny(const FloatingRecord& r) {
    const int a = std::max(r.spanX, r.spanZ), b = std::max(1, std::min(r.spanX, r.spanZ));
    return static_cast<std::int64_t>(a) * 1000000000LL / b + r.blocks;
}
static std::int64_t metricFlatWide(const FloatingRecord& r) {
    const int h = std::max(1, r.maxY - r.minY + 1);
    return static_cast<std::int64_t>(r.footprint) * 1000000000LL / h + r.blocks;
}
static std::int64_t metricOutlier(const FloatingRecord& r) {
    const int h = std::max(1, r.maxY - r.minY + 1);
    const int a = std::max(r.spanX, r.spanZ), b = std::max(1, std::min(r.spanX, r.spanZ));
    const std::int64_t aspect = static_cast<std::int64_t>(a) * 1000000LL / b;
    const std::int64_t vertical = static_cast<std::int64_t>(h) * 1000000LL / std::max(1, b);
    const std::int64_t sparse = static_cast<std::int64_t>(std::max(1, r.spanX * r.spanZ)) * 1000000LL / std::max(1, r.footprint);
    return std::max({aspect, vertical, sparse}) * 1000000LL + r.blocks;
}
static std::int64_t metricSmallest(const FloatingRecord& r) { return -static_cast<std::int64_t>(r.blocks) * 1000000LL - r.footprint; }

using Metric = std::int64_t (*)(const FloatingRecord&);
enum class Filter { All, OneByOne, NonTrivial };
struct Board { std::string name; Metric metric; Filter filter; std::vector<FloatingRecord> rows; };

static bool accepts(Filter f, const FloatingRecord& r) {
    if (f == Filter::OneByOne) return r.footprint == 1 && r.spanX == 1 && r.spanZ == 1;
    if (f == Filter::NonTrivial) return r.footprint > 1;
    return true;
}
static void insertTop(Board& b, const FloatingRecord& r, int limit) {
    if (!accepts(b.filter, r)) return;
    auto it = std::lower_bound(b.rows.begin(), b.rows.end(), r, [&](const FloatingRecord& a, const FloatingRecord& v) {
        const auto av = b.metric(a), vv = b.metric(v);
        if (av != vv) return av > vv;
        return a.sequenceIndex < v.sequenceIndex;
    });
    b.rows.insert(it, r);
    if (static_cast<int>(b.rows.size()) > limit) b.rows.pop_back();
}

static void writeRecords(const std::filesystem::path& path, const std::vector<FloatingRecord>& rows) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f << "rank,seed,sequence_index,score,spawn_surface_y,first_upper_solid_y,air_gap,player_feet_y,support_y,sand_reason,component_blocks,footprint_columns,top_surface_blocks,min_y,max_y,min_x,max_x,min_z,max_z,span_x,span_z,verify_chunk_radius\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        f << i + 1 << ',' << r.seed << ',' << r.sequenceIndex << ',' << r.score << ','
          << r.spawnSurfaceY << ',' << r.firstUpperY << ',' << r.airGap << ',' << r.playerFeetY << ',' << r.supportY << ',' << r.sandReason << ','
          << r.blocks << ',' << r.footprint << ',' << r.topSurface << ',' << r.minY << ',' << r.maxY << ','
          << r.minX << ',' << r.maxX << ',' << r.minZ << ',' << r.maxZ << ',' << r.spanX << ',' << r.spanZ << ',' << r.verifyChunkRadius << '\n';
    }
}

static std::vector<FloatingRecord> readRecords(const std::filesystem::path& path) {
    std::vector<FloatingRecord> rows;
    std::ifstream f(path);
    if (!f) return rows;
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        std::vector<std::string> x; std::stringstream ss(line); std::string s;
        while (std::getline(ss, s, ',')) x.push_back(s);
        if (x.size() != 22) continue;
        try {
            FloatingRecord r;
            r.seed = std::stoll(x[1]); r.sequenceIndex = std::stoull(x[2]); r.score = std::stoll(x[3]);
            r.spawnSurfaceY = std::stoi(x[4]); r.firstUpperY = std::stoi(x[5]); r.airGap = std::stoi(x[6]);
            r.playerFeetY = std::stoi(x[7]); r.supportY = std::stoi(x[8]); r.sandReason = std::stoi(x[9]);
            r.blocks = std::stoi(x[10]); r.footprint = std::stoi(x[11]); r.topSurface = std::stoi(x[12]);
            r.minY = std::stoi(x[13]); r.maxY = std::stoi(x[14]); r.minX = std::stoi(x[15]); r.maxX = std::stoi(x[16]);
            r.minZ = std::stoi(x[17]); r.maxZ = std::stoi(x[18]); r.spanX = std::stoi(x[19]); r.spanZ = std::stoi(x[20]);
            r.verifyChunkRadius = std::stoi(x[21]); rows.push_back(r);
        } catch (...) {}
    }
    return rows;
}

static std::unordered_set<std::uint64_t> loadVerifiedIds(const std::filesystem::path& path) {
    std::unordered_set<std::uint64_t> ids;
    std::ifstream f(path); if (!f) return ids;
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        std::stringstream ss(line); std::string seed, seq;
        if (!std::getline(ss, seed, ',')) continue;
        if (!std::getline(ss, seq, ',')) continue;
        try { ids.insert(std::stoull(seq)); } catch (...) {}
    }
    return ids;
}

static void appendVerified(const std::filesystem::path& path, const FloatingRecord& r) {
    const bool newFile = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
    std::ofstream f(path, std::ios::app);
    if (!f) throw std::runtime_error("cannot append " + path.string());
    if (newFile) f << "seed,sequence_index,component_blocks,footprint_columns,top_surface_blocks,spawn_surface_y,first_upper_solid_y,air_gap,player_feet_y,support_y,sand_reason,min_y,max_y,min_x,max_x,min_z,max_z,span_x,span_z,verify_chunk_radius\n";
    f << r.seed << ',' << r.sequenceIndex << ',' << r.blocks << ',' << r.footprint << ',' << r.topSurface << ','
      << r.spawnSurfaceY << ',' << r.firstUpperY << ',' << r.airGap << ',' << r.playerFeetY << ',' << r.supportY << ',' << r.sandReason << ','
      << r.minY << ',' << r.maxY << ',' << r.minX << ',' << r.maxX << ',' << r.minZ << ',' << r.maxZ << ',' << r.spanX << ',' << r.spanZ << ',' << r.verifyChunkRadius << '\n';
    f.flush();
}

static FloatingRecord makeRecord(const Candidate& c, const ComponentStats& s, int radius) {
    FloatingRecord r;
    r.seed = c.seed; r.sequenceIndex = c.sequenceIndex; r.spawnSurfaceY = c.spawnSurfaceY;
    r.firstUpperY = c.firstUpperY; r.airGap = c.airGap; r.playerFeetY = c.playerFeetY; r.supportY = c.supportY; r.sandReason = c.sandReason;
    r.blocks = s.blocks; r.footprint = s.footprint; r.topSurface = s.topSurface; r.minY = s.minY; r.maxY = s.maxY;
    r.minX = s.minX; r.maxX = s.maxX; r.minZ = s.minZ; r.maxZ = s.maxZ; r.spanX = s.spanX; r.spanZ = s.spanZ;
    r.verifyChunkRadius = radius; r.score = makeScore(r); return r;
}

static std::vector<Board> loadBoards(const std::filesystem::path& dir, int top) {
    std::vector<Board> boards = {
        {"largest", metricLargest, Filter::All, readRecords(dir / "top_largest.csv")},
        {"footprint", metricFootprint, Filter::All, readRecords(dir / "top_footprint.csv")},
        {"top_surface", metricSurface, Filter::All, readRecords(dir / "top_top_surface.csv")},
        {"highest", metricHighest, Filter::All, readRecords(dir / "top_highest.csv")},
        {"island_like", metricIslandLike, Filter::All, readRecords(dir / "top_island_like.csv")},
        {"1x1", metricOneByOne, Filter::OneByOne, readRecords(dir / "top_1x1.csv")},
        {"tall_thin", metricTallThin, Filter::All, readRecords(dir / "top_tall_thin.csv")},
        {"long_skinny", metricLongSkinny, Filter::NonTrivial, readRecords(dir / "top_long_skinny.csv")},
        {"flat_wide", metricFlatWide, Filter::NonTrivial, readRecords(dir / "top_flat_wide.csv")},
        {"outlier_geometry", metricOutlier, Filter::All, readRecords(dir / "top_outlier_geometry.csv")},
        {"smallest", metricSmallest, Filter::All, readRecords(dir / "top_smallest.csv")}
    };
    for (auto& b : boards) if (static_cast<int>(b.rows.size()) > top) b.rows.resize(top);
    return boards;
}
static void saveBoards(std::vector<Board>& boards, const std::filesystem::path& dir) {
    for (auto& b : boards) writeRecords(dir / ("top_" + b.name + ".csv"), b.rows);
}

static bool gateMatches(const Gate& g, const Candidate& c) {
    return g.qualified && g.spawnSurfaceY == c.spawnSurfaceY && g.firstUpperY == c.firstUpperY &&
           g.airGap == c.airGap && g.playerFeetY == c.playerFeetY && g.supportY == c.supportY && g.sandReason == c.sandReason;
}

static int runSelfTest() {
    static_assert(coarsecore::SIZE == 5, "P6 verifier must compile against 5x5 chunk-local density lattice");
    static_assert(coarsecore::FROM_COARSE == 0, "P6 verifier chunk lattice must start at local node zero");
    printDevice();
    DeviceBuffers b = allocateBuffers(2);
    const std::int64_t seeds[2] = {6430576860599818994LL, -3405360075020439777LL};
    std::vector<double> density(2u * static_cast<std::size_t>(coarsecore::CELLS));
    std::vector<double> sand(2), stone(2), temp(2), rain(2);
    try {
        checkHip(hipMemcpy(b.seeds, seeds, sizeof(seeds), hipMemcpyHostToDevice), "copy P6 self-test seeds");
        launchChunk(b, 2, 64, 0, 0);
        checkHip(hipMemcpy(density.data(), b.noise1, density.size() * sizeof(double), hipMemcpyDeviceToHost), "copy P6 self-test density");
        checkHip(hipMemcpy(sand.data(), b.originSandNoise, 2 * sizeof(double), hipMemcpyDeviceToHost), "copy P6 self-test sand");
        checkHip(hipMemcpy(stone.data(), b.originStoneNoise, 2 * sizeof(double), hipMemcpyDeviceToHost), "copy P6 self-test stone");
        checkHip(hipMemcpy(temp.data(), b.originTemperature, 2 * sizeof(double), hipMemcpyDeviceToHost), "copy P6 self-test temp");
        checkHip(hipMemcpy(rain.data(), b.originRainfall, 2 * sizeof(double), hipMemcpyDeviceToHost), "copy P6 self-test rain");
        const auto good = gateOrigin(density.data(), sand[0], stone[0], temp[0], rain[0]);
        const auto bad = gateOrigin(density.data() + coarsecore::CELLS, sand[1], stone[1], temp[1], rain[1]);
        if (!good.qualified || good.spawnSurfaceY != 63 || good.firstUpperY != 65 || good.playerFeetY != 74 || good.supportY != 73) {
            throw std::runtime_error("known real 1x1 pillar failed P6 chunk gate");
        }
        if (bad.qualified) throw std::runtime_error("known two-air-block false positive still passes P6 chunk gate");
    } catch (...) { freeBuffers(b); throw; }
    freeBuffers(b);
    std::cout << "P6 CHUNK VERIFY SELFTEST OK good=PASS knownFalsePositive=REJECT\n";
    return 0;
}

static int run(const Config& c) {
    if (c.selfTest) return runSelfTest();
    static_assert(coarsecore::SIZE == 5, "P6 verifier requires SIZE=5");
    static_assert(coarsecore::FROM_COARSE == 0, "P6 verifier requires FROM_COARSE=0");
    printDevice();
    std::filesystem::create_directories(c.outputDir);
    auto candidates = readCandidates(c.input);
    std::cout << "P6 chunk verifier radius=" << c.chunkRadius << " chunks inputCandidates=" << candidates.size() << '\n';

    auto boards = loadBoards(c.outputDir, c.top);
    const auto verifiedPath = c.outputDir / "verified_all.csv";
    auto verifiedIds = loadVerifiedIds(verifiedPath);
    std::ofstream boundary(c.boundaryOut, std::ios::trunc);
    if (!boundary) throw std::runtime_error("cannot write boundary output: " + c.boundaryOut.string());
    writeCandidateHeader(boundary);

    DeviceBuffers b = allocateBuffers(c.batch);
    const int tileSide = c.chunkRadius * 2 + 1;
    const int tileCount = tileSide * tileSide;
    ComponentVerifier verifier(c.chunkRadius);
    std::uint64_t groundRejects = 0, boundaryDeferred = 0, verifiedNow = 0, gateMismatches = 0;

    try {
        for (std::size_t baseIndex = 0; baseIndex < candidates.size(); baseIndex += static_cast<std::size_t>(c.batch)) {
            const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(c.batch), candidates.size() - baseIndex));
            std::vector<std::int64_t> hostSeeds(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) hostSeeds[static_cast<std::size_t>(i)] = candidates[baseIndex + static_cast<std::size_t>(i)].seed;
            checkHip(hipMemcpy(b.seeds, hostSeeds.data(), static_cast<std::size_t>(n) * sizeof(std::int64_t), hipMemcpyHostToDevice), "copy P6 verifier seed batch");

            std::vector<double> tiles(static_cast<std::size_t>(tileCount) * static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS));
            std::vector<double> sand(static_cast<std::size_t>(n)), stone(static_cast<std::size_t>(n)), temp(static_cast<std::size_t>(n)), rain(static_cast<std::size_t>(n));

            // Generate origin first so we preserve exact spawn-gate auxiliary outputs.
            const int originTile = c.chunkRadius * tileSide + c.chunkRadius;
            launchChunk(b, n, c.terrainThreads, 0, 0);
            checkHip(hipMemcpy(tiles.data() + static_cast<std::size_t>(originTile) * static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS),
                               b.noise1, static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS) * sizeof(double), hipMemcpyDeviceToHost), "copy origin P6 density");
            checkHip(hipMemcpy(sand.data(), b.originSandNoise, static_cast<std::size_t>(n) * sizeof(double), hipMemcpyDeviceToHost), "copy P6 sand noise");
            checkHip(hipMemcpy(stone.data(), b.originStoneNoise, static_cast<std::size_t>(n) * sizeof(double), hipMemcpyDeviceToHost), "copy P6 stone noise");
            checkHip(hipMemcpy(temp.data(), b.originTemperature, static_cast<std::size_t>(n) * sizeof(double), hipMemcpyDeviceToHost), "copy P6 origin temp");
            checkHip(hipMemcpy(rain.data(), b.originRainfall, static_cast<std::size_t>(n) * sizeof(double), hipMemcpyDeviceToHost), "copy P6 origin rain");

            for (int cz = -c.chunkRadius; cz <= c.chunkRadius; ++cz) {
                for (int cx = -c.chunkRadius; cx <= c.chunkRadius; ++cx) {
                    if (cx == 0 && cz == 0) continue;
                    const int tile = (cz + c.chunkRadius) * tileSide + (cx + c.chunkRadius);
                    launchChunk(b, n, c.terrainThreads, cx, cz);
                    checkHip(hipMemcpy(tiles.data() + static_cast<std::size_t>(tile) * static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS),
                                       b.noise1, static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS) * sizeof(double), hipMemcpyDeviceToHost), "copy P6 tile density");
                }
            }

            for (int i = 0; i < n; ++i) {
                const Candidate& input = candidates[baseIndex + static_cast<std::size_t>(i)];
                const double* origin = tiles.data() + (static_cast<std::size_t>(originTile) * static_cast<std::size_t>(n) + static_cast<std::size_t>(i)) * static_cast<std::size_t>(coarsecore::CELLS);
                const auto g = gateOrigin(origin, sand[static_cast<std::size_t>(i)], stone[static_cast<std::size_t>(i)], temp[static_cast<std::size_t>(i)], rain[static_cast<std::size_t>(i)]);
                if (!gateMatches(g, input)) { ++gateMismatches; continue; }
                TileView view(tiles, n, i, c.chunkRadius);
                const auto s = verifier.trace(view, input.supportY);
                if (s.groundConnected) { ++groundRejects; continue; }
                if (s.touchedBoundary) { ++boundaryDeferred; writeCandidate(boundary, input); continue; }
                if (!s.contained || s.blocks <= 0) continue;
                if (verifiedIds.insert(input.sequenceIndex).second) {
                    const auto r = makeRecord(input, s, c.chunkRadius);
                    appendVerified(verifiedPath, r);
                    for (auto& board : boards) insertTop(board, r, c.top);
                    ++verifiedNow;
                    std::cout << "P6 VERIFIED FLOATING seed=" << r.seed << " blocks=" << r.blocks
                              << " footprint=" << r.footprint << " span=" << r.spanX << 'x' << r.spanZ
                              << " islandY=" << r.minY << ".." << r.maxY << " airGap=" << r.airGap
                              << " feetY=" << r.playerFeetY << " chunkR=" << r.verifyChunkRadius << '\n';
                }
            }
        }
    } catch (...) { freeBuffers(b); throw; }
    freeBuffers(b);
    boundary.flush();
    saveBoards(boards, c.outputDir);
    std::cout << "P6 verify done input=" << candidates.size() << " verifiedNew=" << verifiedNow
              << " groundRejects=" << groundRejects << " boundaryDeferred=" << boundaryDeferred
              << " gateMismatches=" << gateMismatches << '\n';
    if (gateMismatches != 0) throw std::runtime_error("P6 scout/chunk verifier mismatch detected; stopping to protect correctness");
    return 0;
}

} // namespace floating_island_spawn_p6_verify

int main(int argc, char** argv) {
    try {
        const auto c = floating_island_spawn_p6_verify::parseArgs(argc, argv);
        return floating_island_spawn_p6_verify::run(c);
    } catch (const std::exception& e) {
        std::cerr << "FloatingIslandSpawn P6 verify ERROR: " << e.what() << '\n';
        return 1;
    }
}
