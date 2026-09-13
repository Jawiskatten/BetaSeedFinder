#define main highest_pillar_spawn_p1_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace floating_island_spawn_visual_p6 {
using namespace highest_pillar_spawn_p1;

struct Row {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    int blocks = 0, footprint = 0, topSurface = 0;
    int spawnSurfaceY = -1, firstUpperY = -1, airGap = 0, playerFeetY = -1, supportY = -1, sandReason = 0;
    int minY = -1, maxY = -1, minX = 0, maxX = 0, minZ = 0, maxZ = 0, spanX = 0, spanZ = 0, verifyChunkRadius = 0;

    double visualScore = 0.0;
    double sameHeightClearPct = 0.0;
    double highClearPct = 0.0;
    double baseClearPct = 0.0;
    double nearestSameHeight = 0.0;
    double nearestHigh = 0.0;
    int sameHeightColumnsR16 = 0;
    int highColumnsR32 = 0;
    int sampledColumnsR32 = 0;
    int lowerOneByOneDepth = 0;
};

struct Config {
    std::filesystem::path input;
    std::filesystem::path outputDir;
    int chunkRadius = 4;
    int batch = 256;
    int terrainThreads = 64;
    int top = 500;
};

static int parseInt(const std::string& s, const char* name) {
    std::size_t used = 0;
    const int v = std::stoi(s, &used, 0);
    if (used != s.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + s);
    return v;
}

static Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* name) -> std::string {
            if (++i >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[i];
        };
        if (a == "--input") c.input = value("--input");
        else if (a == "--output") c.outputDir = value("--output");
        else if (a == "--chunk-radius") c.chunkRadius = parseInt(value("--chunk-radius"), "chunk-radius");
        else if (a == "--batch") c.batch = parseInt(value("--batch"), "batch");
        else if (a == "--terrain-threads") c.terrainThreads = parseInt(value("--terrain-threads"), "terrain-threads");
        else if (a == "--top") c.top = parseInt(value("--top"), "top");
        else throw std::invalid_argument("unknown argument: " + a);
    }
    if (c.input.empty()) throw std::invalid_argument("--input is required");
    if (c.outputDir.empty()) throw std::invalid_argument("--output is required");
    if (c.chunkRadius < 4 || c.chunkRadius > 8) throw std::invalid_argument("--chunk-radius must be 4..8");
    if (c.batch < 1 || c.batch > 1024) throw std::invalid_argument("--batch must be 1..1024");
    if (c.terrainThreads != 64 && c.terrainThreads != 128 && c.terrainThreads != 256) throw std::invalid_argument("--terrain-threads must be 64, 128, or 256");
    if (c.top < 1 || c.top > 10000) throw std::invalid_argument("--top must be 1..10000");
    return c;
}

static std::vector<Row> readRows(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path.string());
    std::vector<Row> rows;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> x;
        std::stringstream ss(line);
        std::string s;
        while (std::getline(ss, s, ',')) x.push_back(s);
        if (x.size() < 20) continue;
        try {
            Row r;
            r.seed = std::stoll(x[0]); r.sequenceIndex = std::stoull(x[1]);
            r.blocks = std::stoi(x[2]); r.footprint = std::stoi(x[3]); r.topSurface = std::stoi(x[4]);
            r.spawnSurfaceY = std::stoi(x[5]); r.firstUpperY = std::stoi(x[6]); r.airGap = std::stoi(x[7]);
            r.playerFeetY = std::stoi(x[8]); r.supportY = std::stoi(x[9]); r.sandReason = std::stoi(x[10]);
            r.minY = std::stoi(x[11]); r.maxY = std::stoi(x[12]); r.minX = std::stoi(x[13]); r.maxX = std::stoi(x[14]);
            r.minZ = std::stoi(x[15]); r.maxZ = std::stoi(x[16]); r.spanX = std::stoi(x[17]); r.spanZ = std::stoi(x[18]);
            r.verifyChunkRadius = std::stoi(x[19]);
            rows.push_back(r);
        } catch (...) {}
    }
    return rows;
}

static int floorDiv16(int v) { return v >= 0 ? v / 16 : -((-v + 15) / 16); }
static int mod16(int v) { const int q = floorDiv16(v); return v - q * 16; }

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
#error "P6 visual ranker requires modern coarse GPU API"
#endif
    checkHip(hipGetLastError(), "launch P6 visual terrain");
    checkHip(hipDeviceSynchronize(), "finish P6 visual terrain");
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
    const int ix = localX >> 2, iz = localZ >> 2, iy = (worldY >> 3) - coarsecore::Y_BASE;
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
        : tiles_(tiles), n_(batchCount), i_(candidateIndex), r_(radius), side_(radius * 2 + 1) {}
    bool solid(int worldX, int worldY, int worldZ) const {
        if (worldY < coarsecore::Y_BASE * 8) return true;
        if (worldY >= 128) return false;
        const int cx = floorDiv16(worldX), cz = floorDiv16(worldZ);
        if (cx < -r_ || cx > r_ || cz < -r_ || cz > r_) return false;
        const int tile = (cz + r_) * side_ + (cx + r_);
        const std::size_t base = (static_cast<std::size_t>(tile) * static_cast<std::size_t>(n_) + static_cast<std::size_t>(i_)) * static_cast<std::size_t>(coarsecore::CELLS);
        return densityAtLocalBlock(tiles_.data() + base, mod16(worldX), worldY, mod16(worldZ)) > 0.0;
    }
private:
    const std::vector<double>& tiles_;
    int n_, i_, r_, side_;
};

static bool inExpandedComponentXZ(const Row& r, int x, int z, int pad) {
    return x >= r.minX - pad && x <= r.maxX + pad && z >= r.minZ - pad && z <= r.maxZ + pad;
}

static void analyze(Row& r, const TileView& view) {
    constexpr int nearR = 32;
    constexpr int sameR = 16;
    constexpr int farR = 64;
    const int ySame = std::max(56, r.playerFeetY - 2);
    const int yHigh = std::max(56, r.playerFeetY - 8);
    const int yBase = std::max(56, r.minY - 2);

    int sameCols = 0, sameTotal = 0;
    int highCols = 0, baseCols = 0, nearTotal = 0;
    double nearestSame = static_cast<double>(farR + 1);
    double nearestHigh = static_cast<double>(farR + 1);

    for (int z = -nearR; z <= nearR; ++z) {
        for (int x = -nearR; x <= nearR; ++x) {
            if (x * x + z * z > nearR * nearR) continue;
            if (inExpandedComponentXZ(r, x, z, 2)) continue;
            const double dist = std::sqrt(static_cast<double>(x * x + z * z));
            const bool high = view.solid(x, yHigh, z);
            const bool base = view.solid(x, yBase, z);
            ++nearTotal;
            if (high) { ++highCols; nearestHigh = std::min(nearestHigh, dist); }
            if (base) ++baseCols;
            if (x * x + z * z <= sameR * sameR) {
                ++sameTotal;
                const bool same = view.solid(x, ySame, z);
                if (same) { ++sameCols; nearestSame = std::min(nearestSame, dist); }
            }
        }
    }

    // Extend the isolation-distance search to 64 blocks. Sampling every second
    // column is enough for ranking while keeping the 23k-hit pass cheap.
    for (int z = -farR; z <= farR; z += 2) {
        for (int x = -farR; x <= farR; x += 2) {
            if (x * x + z * z > farR * farR || x * x + z * z <= nearR * nearR) continue;
            if (inExpandedComponentXZ(r, x, z, 2)) continue;
            const double dist = std::sqrt(static_cast<double>(x * x + z * z));
            if (dist >= nearestHigh) continue;
            if (view.solid(x, yHigh, z)) nearestHigh = dist;
        }
    }

    r.sameHeightColumnsR16 = sameCols;
    r.highColumnsR32 = highCols;
    r.sampledColumnsR32 = nearTotal;
    r.sameHeightClearPct = sameTotal > 0 ? 100.0 * (1.0 - static_cast<double>(sameCols) / sameTotal) : 100.0;
    r.highClearPct = nearTotal > 0 ? 100.0 * (1.0 - static_cast<double>(highCols) / nearTotal) : 100.0;
    r.baseClearPct = nearTotal > 0 ? 100.0 * (1.0 - static_cast<double>(baseCols) / nearTotal) : 100.0;
    r.nearestSameHeight = nearestSame;
    r.nearestHigh = nearestHigh;

    // Detect the visual "lower 1x1 pedestal" effect: starting at the vanilla
    // spawn-surface block, count consecutive levels where the origin is solid
    // and all eight neighboring columns are air at that same Y.
    int lowerDepth = 0;
    for (int y = r.spawnSurfaceY; y >= 56; --y) {
        if (!view.solid(0, y, 0)) break;
        bool clean = true;
        for (int dz = -1; dz <= 1 && clean; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                if (view.solid(dx, y, dz)) { clean = false; break; }
            }
        }
        if (!clean) break;
        ++lowerDepth;
    }
    r.lowerOneByOneDepth = lowerDepth;

    const int componentHeight = std::max(1, r.maxY - r.minY + 1);
    const int thinAxis = std::min(r.spanX, r.spanZ);
    const int longAxis = std::max(r.spanX, r.spanZ);
    const double altitude = static_cast<double>(std::max(0, r.playerFeetY - 72));
    double shapeBonus = 0.0;
    if (r.footprint == 1 && r.spanX == 1 && r.spanZ == 1) shapeBonus += std::min(32, componentHeight) * 2.5;
    else if (thinAxis == 1) shapeBonus += std::min(80, longAxis) * 0.65 + std::min(32, componentHeight) * 0.5;
    else if (thinAxis <= 2) shapeBonus += std::min(48, componentHeight) * 0.25;

    // This deliberately rewards clean surroundings and relative isolation more
    // than raw absolute Y. A Y119 spawn beside a Y118 mountain should lose to a
    // lower but visually isolated freak structure.
    r.visualScore =
          0.45 * r.sameHeightClearPct
        + 0.35 * r.highClearPct
        + 0.15 * r.baseClearPct
        + 1.15 * std::min(65.0, r.nearestSameHeight)
        + 0.85 * std::min(65.0, r.nearestHigh)
        + 4.5 * static_cast<double>(r.lowerOneByOneDepth)
        + 0.55 * altitude
        + shapeBonus;
}

static void writeHeader(std::ofstream& f) {
    f << "rank,seed,sequence_index,visual_score,component_blocks,footprint_columns,span_x,span_z,min_y,max_y,player_feet_y,spawn_surface_y,air_gap,nearest_same_height,nearest_high,same_height_clear_pct_r16,high_clear_pct_r32,base_clear_pct_r32,same_height_columns_r16,high_columns_r32,lower_1x1_depth,verify_chunk_radius\n";
}
static void writeRow(std::ofstream& f, const Row& r, std::size_t rank) {
    f << rank << ',' << r.seed << ',' << r.sequenceIndex << ',' << std::fixed << std::setprecision(3) << r.visualScore << ','
      << r.blocks << ',' << r.footprint << ',' << r.spanX << ',' << r.spanZ << ',' << r.minY << ',' << r.maxY << ','
      << r.playerFeetY << ',' << r.spawnSurfaceY << ',' << r.airGap << ',' << std::setprecision(2) << r.nearestSameHeight << ',' << r.nearestHigh << ','
      << r.sameHeightClearPct << ',' << r.highClearPct << ',' << r.baseClearPct << ',' << r.sameHeightColumnsR16 << ',' << r.highColumnsR32 << ','
      << r.lowerOneByOneDepth << ',' << r.verifyChunkRadius << '\n';
}

template <typename Compare, typename Filter>
static void writeTop(const std::filesystem::path& path, const std::vector<Row>& rows, int limit, Compare cmp, Filter filter) {
    std::vector<const Row*> v;
    v.reserve(rows.size());
    for (const auto& r : rows) if (filter(r)) v.push_back(&r);
    std::sort(v.begin(), v.end(), [&](const Row* a, const Row* b) {
        if (cmp(*a, *b)) return true;
        if (cmp(*b, *a)) return false;
        return a->sequenceIndex < b->sequenceIndex;
    });
    if (static_cast<int>(v.size()) > limit) v.resize(static_cast<std::size_t>(limit));
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    writeHeader(f);
    for (std::size_t i = 0; i < v.size(); ++i) writeRow(f, *v[i], i + 1);
}

static void saveOutputs(const Config& c, const std::vector<Row>& rows) {
    auto all = [](const Row&) { return true; };
    auto oneXN = [](const Row& r) { return std::min(r.spanX, r.spanZ) == 1; };
    auto trueOne = [](const Row& r) { return r.spanX == 1 && r.spanZ == 1 && r.footprint == 1; };

    writeTop(c.outputDir / "top_visual_insanity.csv", rows, c.top,
        [](const Row& a, const Row& b) { return a.visualScore > b.visualScore; }, all);
    writeTop(c.outputDir / "top_visual_isolation.csv", rows, c.top,
        [](const Row& a, const Row& b) {
            if (a.nearestSameHeight != b.nearestSameHeight) return a.nearestSameHeight > b.nearestSameHeight;
            if (a.nearestHigh != b.nearestHigh) return a.nearestHigh > b.nearestHigh;
            return a.highClearPct > b.highClearPct;
        }, all);
    writeTop(c.outputDir / "top_lower_1x1_pedestal.csv", rows, c.top,
        [](const Row& a, const Row& b) {
            if (a.lowerOneByOneDepth != b.lowerOneByOneDepth) return a.lowerOneByOneDepth > b.lowerOneByOneDepth;
            return a.visualScore > b.visualScore;
        }, all);
    writeTop(c.outputDir / "top_clean_1x1.csv", rows, c.top,
        [](const Row& a, const Row& b) { return a.visualScore > b.visualScore; }, trueOne);
    writeTop(c.outputDir / "top_1xN_biggest.csv", rows, c.top,
        [](const Row& a, const Row& b) {
            if (a.blocks != b.blocks) return a.blocks > b.blocks;
            if (a.footprint != b.footprint) return a.footprint > b.footprint;
            return std::max(a.spanX, a.spanZ) > std::max(b.spanX, b.spanZ);
        }, oneXN);
    writeTop(c.outputDir / "top_1xN_longest.csv", rows, c.top,
        [](const Row& a, const Row& b) {
            const int al = std::max(a.spanX, a.spanZ), bl = std::max(b.spanX, b.spanZ);
            if (al != bl) return al > bl;
            if (a.blocks != b.blocks) return a.blocks > b.blocks;
            return a.playerFeetY > b.playerFeetY;
        }, oneXN);
    writeTop(c.outputDir / "top_1xN_visual.csv", rows, c.top,
        [](const Row& a, const Row& b) { return a.visualScore > b.visualScore; }, oneXN);

    std::vector<const Row*> ranked;
    ranked.reserve(rows.size());
    for (const auto& r : rows) ranked.push_back(&r);
    std::sort(ranked.begin(), ranked.end(), [](const Row* a, const Row* b) {
        if (a->visualScore != b->visualScore) return a->visualScore > b->visualScore;
        return a->sequenceIndex < b->sequenceIndex;
    });
    std::ofstream f(c.outputDir / "visual_rank_all.csv", std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write visual_rank_all.csv");
    writeHeader(f);
    for (std::size_t i = 0; i < ranked.size(); ++i) writeRow(f, *ranked[i], i + 1);
}

static int run(const Config& c) {
    static_assert(coarsecore::SIZE == 5, "P6 visual ranker requires 5x5 chunk density nodes");
    static_assert(coarsecore::FROM_COARSE == 0, "P6 visual ranker requires chunk-local FROM_COARSE=0");
    printDevice();
    std::filesystem::create_directories(c.outputDir);
    auto rows = readRows(c.input);
    std::cout << "P6 visual ranker inputHits=" << rows.size() << " chunkRadius=" << c.chunkRadius << " analysisRadius=64\n";
    if (rows.empty()) { saveOutputs(c, rows); return 0; }

    DeviceBuffers b = allocateBuffers(c.batch);
    const int tileSide = c.chunkRadius * 2 + 1;
    const int tileCount = tileSide * tileSide;
    std::size_t done = 0;
    try {
        for (std::size_t base = 0; base < rows.size(); base += static_cast<std::size_t>(c.batch)) {
            const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(c.batch), rows.size() - base));
            std::vector<std::int64_t> seeds(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) seeds[static_cast<std::size_t>(i)] = rows[base + static_cast<std::size_t>(i)].seed;
            checkHip(hipMemcpy(b.seeds, seeds.data(), static_cast<std::size_t>(n) * sizeof(std::int64_t), hipMemcpyHostToDevice), "copy P6 visual seeds");

            std::vector<double> tiles(static_cast<std::size_t>(tileCount) * static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS));
            for (int cz = -c.chunkRadius; cz <= c.chunkRadius; ++cz) {
                for (int cx = -c.chunkRadius; cx <= c.chunkRadius; ++cx) {
                    const int tile = (cz + c.chunkRadius) * tileSide + (cx + c.chunkRadius);
                    launchChunk(b, n, c.terrainThreads, cx, cz);
                    double* dst = tiles.data() + static_cast<std::size_t>(tile) * static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS);
                    checkHip(hipMemcpy(dst, b.noise1, static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS) * sizeof(double), hipMemcpyDeviceToHost), "copy P6 visual tile");
                }
            }
            for (int i = 0; i < n; ++i) {
                TileView view(tiles, n, i, c.chunkRadius);
                analyze(rows[base + static_cast<std::size_t>(i)], view);
            }
            done += static_cast<std::size_t>(n);
            std::cout << "P6 visual progress " << done << '/' << rows.size() << '\n';
        }
    } catch (...) { freeBuffers(b); throw; }
    freeBuffers(b);
    saveOutputs(c, rows);
    std::cout << "P6 VISUAL RANKING COMPLETE hits=" << rows.size() << '\n';
    std::cout << "  " << (c.outputDir / "top_visual_insanity.csv").string() << '\n';
    std::cout << "  " << (c.outputDir / "top_1xN_biggest.csv").string() << '\n';
    std::cout << "  " << (c.outputDir / "visual_rank_all.csv").string() << '\n';
    return 0;
}

} // namespace floating_island_spawn_visual_p6

int main(int argc, char** argv) {
    try { return floating_island_spawn_visual_p6::run(floating_island_spawn_visual_p6::parseArgs(argc, argv)); }
    catch (const std::exception& e) {
        std::cerr << "FloatingIslandSpawn P6 visual rank ERROR: " << e.what() << '\n';
        return 1;
    }
}
