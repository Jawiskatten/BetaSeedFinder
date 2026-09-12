#define main highest_pillar_spawn_p1_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <algorithm>
#include <array>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace floating_island_spawn_p4_verify {
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
    int spanX = 0;
    int spanZ = 0;
    int verifyRadius = 0;
};

struct Config {
    std::filesystem::path input;
    std::filesystem::path outputDir;
    std::filesystem::path boundaryOut;
    int batch = 1024;
    int terrainThreads = 64;
    int top = 250;
    bool selfTest = false;
};

static int parseInt(const std::string& value, const char* name) {
    std::size_t used = 0;
    const int out = std::stoi(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
    return out;
}

static Config parseArgsP4(int argc, char** argv) {
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
        else if (arg == "--self-test") c.selfTest = true;
        else throw std::invalid_argument("unknown argument: " + arg);
    }
    if (!c.selfTest && c.input.empty()) throw std::invalid_argument("--input is required");
    if (!c.selfTest && c.outputDir.empty()) throw std::invalid_argument("--output is required");
    if (!c.selfTest && c.boundaryOut.empty()) throw std::invalid_argument("--boundary-out is required");
    if (c.batch < 1 || c.batch > 8192) throw std::invalid_argument("--batch must be 1..8192");
    if (c.terrainThreads != 64 && c.terrainThreads != 128 && c.terrainThreads != 256) {
        throw std::invalid_argument("--terrain-threads must be 64, 128, or 256");
    }
    if (c.top < 1 || c.top > 10000) throw std::invalid_argument("--top must be 1..10000");
    return c;
}

static int floorDivPositiveStepHost(int value, int step) {
    return value >= 0 ? value / step : -((-value + step - 1) / step);
}
static double hostNodeDensity(const double* density, int x, int y, int z) {
    if (x < 0 || z < 0 || x >= coarsecore::SIZE || z >= coarsecore::SIZE) return -10.0;
    if (y < 0) return 10.0;
    if (y >= coarsecore::Y_LEVELS) return -10.0;
    return density[coarsecore::index3(x, y, z)];
}
static bool hostTerrainSolidAtBlock(const double* density, int worldX, int worldY, int worldZ) {
    if (worldY < coarsecore::Y_BASE * 8) return true;
    if (worldY >= 128) return false;
    const int coarseX = floorDivPositiveStepHost(worldX, 4);
    const int coarseZ = floorDivPositiveStepHost(worldZ, 4);
    const int ix = coarseX - coarsecore::FROM_COARSE;
    const int iz = coarseZ - coarsecore::FROM_COARSE;
    const int iy = (worldY >> 3) - coarsecore::Y_BASE;
    const double fx = static_cast<double>(worldX - coarseX * 4) * 0.25;
    const double fz = static_cast<double>(worldZ - coarseZ * 4) * 0.25;
    const double fy = static_cast<double>(worldY & 7) * 0.125;
    const double d000 = hostNodeDensity(density, ix,     iy,     iz);
    const double d001 = hostNodeDensity(density, ix,     iy,     iz + 1);
    const double d100 = hostNodeDensity(density, ix + 1, iy,     iz);
    const double d101 = hostNodeDensity(density, ix + 1, iy,     iz + 1);
    const double d010 = hostNodeDensity(density, ix,     iy + 1, iz);
    const double d011 = hostNodeDensity(density, ix,     iy + 1, iz + 1);
    const double d110 = hostNodeDensity(density, ix + 1, iy + 1, iz);
    const double d111 = hostNodeDensity(density, ix + 1, iy + 1, iz + 1);
    const double a0 = d000 + (d100 - d000) * fx;
    const double a1 = d001 + (d101 - d001) * fx;
    const double b0 = d010 + (d110 - d010) * fx;
    const double b1 = d011 + (d111 - d011) * fx;
    const double low = a0 + (a1 - a0) * fz;
    const double high = b0 + (b1 - b0) * fz;
    return low + (high - low) * fy > 0.0;
}

static int safeVerificationRadius() {
    const int minBlock = coarsecore::FROM_COARSE * 4;
    const int maxUsableCoarse = coarsecore::FROM_COARSE + coarsecore::SIZE - 2;
    const int maxBlock = maxUsableCoarse * 4 + 3;
    int safe = std::min(-minBlock - 1, maxBlock - 1);
    if (safe < 4) safe = 4;
    const int target = std::max(4, p14config::CHUNK_RADIUS * 12);
    return std::min(target, safe);
}

class ComponentVerifier {
public:
    explicit ComponentVerifier(int radius)
        : radius_(radius), side_(radius * 2 + 1), plane_(side_ * side_),
          visited_(static_cast<std::size_t>(plane_) * 128u, 0),
          columnSeen_(static_cast<std::size_t>(plane_), 0) {
        queue_.reserve(static_cast<std::size_t>(plane_) * 8u);
    }

    ComponentStats trace(const double* density, int startY) {
        ComponentStats s;
        if (startY < 0 || startY >= 128 || !hostTerrainSolidAtBlock(density, 0, startY, 0)) return s;
        std::fill(visited_.begin(), visited_.end(), static_cast<unsigned char>(0));
        std::fill(columnSeen_.begin(), columnSeen_.end(), static_cast<unsigned char>(0));
        queue_.clear();

        auto indexOf = [&](int x, int y, int z) -> int {
            return y * plane_ + (z + radius_) * side_ + (x + radius_);
        };
        auto enqueue = [&](int x, int y, int z) {
            if (x < -radius_ || x > radius_ || z < -radius_ || z > radius_ || y < 0 || y >= 128) return;
            const int idx = indexOf(x, y, z);
            if (visited_[static_cast<std::size_t>(idx)]) return;
            if (!hostTerrainSolidAtBlock(density, x, y, z)) return;
            visited_[static_cast<std::size_t>(idx)] = 1;
            queue_.push_back(idx);
        };

        enqueue(0, startY, 0);
        std::size_t head = 0;
        while (head < queue_.size()) {
            const int idx = queue_[head++];
            const int y = idx / plane_;
            const int rem = idx - y * plane_;
            const int zi = rem / side_;
            const int xi = rem - zi * side_;
            const int x = xi - radius_;
            const int z = zi - radius_;

            // The cropped exact terrain contract treats everything below Y56 as
            // solid. Reaching it proves this upper component joins the main world,
            // so reject immediately instead of flood-filling a giant ground slab.
            if (y < coarsecore::Y_BASE * 8) {
                s.groundConnected = true;
                return s;
            }
            if (x == -radius_ || x == radius_ || z == -radius_ || z == radius_) {
                s.touchedBoundary = true;
                return s;
            }

            ++s.blocks;
            s.minY = std::min(s.minY, y);
            s.maxY = std::max(s.maxY, y);
            s.minX = std::min(s.minX, x);
            s.maxX = std::max(s.maxX, x);
            s.minZ = std::min(s.minZ, z);
            s.maxZ = std::max(s.maxZ, z);

            const int col = zi * side_ + xi;
            if (!columnSeen_[static_cast<std::size_t>(col)]) {
                columnSeen_[static_cast<std::size_t>(col)] = 1;
                ++s.footprint;
            }
            if (!hostTerrainSolidAtBlock(density, x, y + 1, z)) ++s.topSurface;

            enqueue(x + 1, y, z);
            enqueue(x - 1, y, z);
            enqueue(x, y, z + 1);
            enqueue(x, y, z - 1);
            enqueue(x, y + 1, z);
            enqueue(x, y - 1, z);
        }

        s.contained = s.blocks > 0;
        if (s.contained) {
            s.spanX = s.maxX - s.minX + 1;
            s.spanZ = s.maxZ - s.minZ + 1;
        }
        return s;
    }

private:
    int radius_;
    int side_;
    int plane_;
    std::vector<unsigned char> visited_;
    std::vector<unsigned char> columnSeen_;
    std::vector<int> queue_;
};

__global__ void fullGateKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        DeviceResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;
    const int center = -coarsecore::FROM_COARSE;
    DeviceResult r{};
    r.seed = seeds[i];
    r.spawnSurfaceY = exactSpawnCheckYAtOrigin(density, base, center, center);
    const bool desert = betaBiomeIsDesert(originTemperature[i], originRainfall[i]);
    r.originBiome = desert ? 1 : 0;

    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool beachSand = originSandNoise[i] + sandJitter * 0.2 > 0.0;
    const int surfaceDepth = static_cast<int>(originStoneNoise[i] / 3.0 + 3.0 + depthJitter * 0.25);
    const bool beachBand = r.spawnSurfaceY >= 60 && r.spawnSurfaceY <= 65;
    r.sandReason = desert ? 2 : ((beachBand && beachSand) ? 1 : 0);
    r.spawnSand = (r.spawnSurfaceY >= 63 && surfaceDepth > 0 && r.sandReason != 0) ? 1 : 0;
    if (!r.spawnSand) { out[i] = r; return; }

    r.playerFeetY = actualPlayerFeetY(density, base);
    if (r.playerFeetY <= 65) { out[i] = r; return; }
    r.supportY = highestSolidAtOrBelow(density, base, 0, 0, r.playerFeetY - 1);
    if (r.supportY < 0 || r.supportY != r.playerFeetY - 1) { out[i] = r; return; }

    int firstUpper = -1;
    for (int y = r.spawnSurfaceY + 1; y <= r.supportY; ++y) {
        if (terrainSolidAtBlock(density, base, 0, y, 0)) { firstUpper = y; break; }
    }
    if (firstUpper < 0) { out[i] = r; return; }
    r.neighborMaxY = firstUpper;
    r.pillarDepth = firstUpper - r.spawnSurfaceY - 1;
    if (r.pillarDepth < 1 || r.pillarDepth > 2) { out[i] = r; return; }
    r.qualified = 1;
    out[i] = r;
}

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
        for (int i = 0; i < 8; ++i) {
            if (!std::getline(ss, x[static_cast<std::size_t>(i)], ',')) { ok = false; break; }
        }
        if (!ok) continue;
        Candidate c;
        try {
            c.seed = std::stoll(x[0]);
            c.sequenceIndex = std::stoull(x[1]);
            c.spawnSurfaceY = std::stoi(x[2]);
            c.firstUpperY = std::stoi(x[3]);
            c.airGap = std::stoi(x[4]);
            c.playerFeetY = std::stoi(x[5]);
            c.supportY = std::stoi(x[6]);
            c.sandReason = std::stoi(x[7]);
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
         + static_cast<std::int64_t>(std::max(0, std::min(99, r.playerFeetY)));
}
static std::int64_t islandLikeScore(const FloatingRecord& r) {
    const std::int64_t minSpan = std::min(r.spanX, r.spanZ);
    return static_cast<std::int64_t>(r.footprint) * minSpan * 1000000LL + r.blocks;
}

using Metric = std::int64_t (*)(const FloatingRecord&);
static std::int64_t metricLargest(const FloatingRecord& r) { return r.score; }
static std::int64_t metricFootprint(const FloatingRecord& r) { return static_cast<std::int64_t>(r.footprint) * 1000000LL + r.blocks; }
static std::int64_t metricSurface(const FloatingRecord& r) { return static_cast<std::int64_t>(r.topSurface) * 1000000LL + r.blocks; }
static std::int64_t metricIslandLike(const FloatingRecord& r) { return islandLikeScore(r); }

struct Board { std::string name; Metric metric; std::vector<FloatingRecord> rows; };

static void insertTop(Board& b, const FloatingRecord& r, int limit) {
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
    f << "rank,seed,sequence_index,score,spawn_surface_y,first_upper_solid_y,air_gap,player_feet_y,support_y,sand_reason,component_blocks,footprint_columns,top_surface_blocks,min_y,max_y,span_x,span_z,verify_radius\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        f << i + 1 << ',' << r.seed << ',' << r.sequenceIndex << ',' << r.score << ','
          << r.spawnSurfaceY << ',' << r.firstUpperY << ',' << r.airGap << ',' << r.playerFeetY << ',' << r.supportY << ','
          << r.sandReason << ',' << r.blocks << ',' << r.footprint << ',' << r.topSurface << ',' << r.minY << ',' << r.maxY << ','
          << r.spanX << ',' << r.spanZ << ',' << r.verifyRadius << '\n';
    }
}

static std::vector<FloatingRecord> readRecords(const std::filesystem::path& path) {
    std::vector<FloatingRecord> rows;
    std::ifstream f(path);
    if (!f) return rows;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::vector<std::string> x;
        std::stringstream ss(line);
        std::string s;
        while (std::getline(ss, s, ',')) x.push_back(s);
        if (x.size() != 18) continue;
        try {
            FloatingRecord r;
            r.seed = std::stoll(x[1]); r.sequenceIndex = std::stoull(x[2]); r.score = std::stoll(x[3]);
            r.spawnSurfaceY = std::stoi(x[4]); r.firstUpperY = std::stoi(x[5]); r.airGap = std::stoi(x[6]);
            r.playerFeetY = std::stoi(x[7]); r.supportY = std::stoi(x[8]); r.sandReason = std::stoi(x[9]);
            r.blocks = std::stoi(x[10]); r.footprint = std::stoi(x[11]); r.topSurface = std::stoi(x[12]);
            r.minY = std::stoi(x[13]); r.maxY = std::stoi(x[14]); r.spanX = std::stoi(x[15]); r.spanZ = std::stoi(x[16]);
            r.verifyRadius = std::stoi(x[17]); rows.push_back(r);
        } catch (...) {}
    }
    return rows;
}

static FloatingRecord makeRecord(const Candidate& c, const ComponentStats& s, int radius) {
    FloatingRecord r;
    r.seed = c.seed; r.sequenceIndex = c.sequenceIndex; r.spawnSurfaceY = c.spawnSurfaceY;
    r.firstUpperY = c.firstUpperY; r.airGap = c.airGap; r.playerFeetY = c.playerFeetY;
    r.supportY = c.supportY; r.sandReason = c.sandReason; r.blocks = s.blocks;
    r.footprint = s.footprint; r.topSurface = s.topSurface; r.minY = s.minY; r.maxY = s.maxY;
    r.spanX = s.spanX; r.spanZ = s.spanZ; r.verifyRadius = radius; r.score = makeScore(r);
    return r;
}

static std::unordered_set<std::uint64_t> loadVerifiedIds(const std::filesystem::path& path) {
    std::unordered_set<std::uint64_t> ids;
    std::ifstream f(path);
    if (!f) return ids;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        std::stringstream ss(line);
        std::string seed, seq;
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
    if (newFile) f << "seed,sequence_index,component_blocks,footprint_columns,top_surface_blocks,player_feet_y,min_y,max_y,span_x,span_z,verify_radius\n";
    f << r.seed << ',' << r.sequenceIndex << ',' << r.blocks << ',' << r.footprint << ',' << r.topSurface << ','
      << r.playerFeetY << ',' << r.minY << ',' << r.maxY << ',' << r.spanX << ',' << r.spanZ << ',' << r.verifyRadius << '\n';
}

static void saveBoards(std::vector<Board>& boards, const std::filesystem::path& dir) {
    for (auto& b : boards) writeRecords(dir / ("top_" + b.name + ".csv"), b.rows);
}

static int runSelfTest() {
    printDevice();
    const Candidate c{6430576860599818994LL, 0, 63, 65, 1, 74, 73, 1};
    DeviceBuffers b = allocateBuffers(1);
    std::vector<double> density(static_cast<std::size_t>(coarsecore::CELLS));
    try {
        checkHip(hipMemcpy(b.seeds, &c.seed, sizeof(c.seed), hipMemcpyHostToDevice), "copy P4 verify self-test seed");
        launchTerrain(b, 1, 64);
        hipLaunchKernelGGL(fullGateKernel, dim3(1), dim3(1), 0, 0,
            b.seeds, b.noise1, b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall, 1, b.results);
        checkHip(hipGetLastError(), "launch P4 verify self-test gate");
        checkHip(hipDeviceSynchronize(), "finish P4 verify self-test gate");
        DeviceResult g{};
        checkHip(hipMemcpy(&g, b.results, sizeof(g), hipMemcpyDeviceToHost), "copy P4 verify self-test gate");
        if (!g.qualified || g.playerFeetY != 74 || g.supportY != 73 || g.pillarDepth != 1) {
            throw std::runtime_error("P4 full verifier gate disagrees with known seed");
        }
        checkHip(hipMemcpy(density.data(), b.noise1, density.size() * sizeof(double), hipMemcpyDeviceToHost), "copy P4 self-test density");
        ComponentVerifier verifier(safeVerificationRadius());
        const auto s = verifier.trace(density.data(), 73);
        if (!s.contained || s.groundConnected || s.touchedBoundary || s.blocks != 9 || s.footprint != 1) {
            throw std::runtime_error("P4 component verifier disagrees with known 9-block pillar");
        }
        std::cout << "P4 VERIFY SELFTEST OK radius=" << safeVerificationRadius()
                  << " blocks=" << s.blocks << " footprint=" << s.footprint << '\n';
    } catch (...) { freeBuffers(b); throw; }
    freeBuffers(b);
    return 0;
}

static int run(const Config& c) {
    if (c.selfTest) return runSelfTest();
    printDevice();
    std::filesystem::create_directories(c.outputDir);
    const int verifyRadius = safeVerificationRadius();
    auto candidates = readCandidates(c.input);
    std::cout << "P4 verify radius=+/-" << verifyRadius << " inputCandidates=" << candidates.size() << '\n';

    std::vector<Board> boards = {
        {"largest", metricLargest, readRecords(c.outputDir / "top_largest.csv")},
        {"footprint", metricFootprint, readRecords(c.outputDir / "top_footprint.csv")},
        {"top_surface", metricSurface, readRecords(c.outputDir / "top_top_surface.csv")},
        {"island_like", metricIslandLike, readRecords(c.outputDir / "top_island_like.csv")}
    };
    for (auto& b : boards) if (static_cast<int>(b.rows.size()) > c.top) b.rows.resize(c.top);

    const auto verifiedPath = c.outputDir / "verified_all.csv";
    auto verifiedIds = loadVerifiedIds(verifiedPath);
    std::ofstream boundary(c.boundaryOut, std::ios::trunc);
    if (!boundary) throw std::runtime_error("cannot write boundary output: " + c.boundaryOut.string());
    writeCandidateHeader(boundary);

    DeviceBuffers b = allocateBuffers(c.batch);
    std::vector<std::int64_t> hostSeeds;
    std::vector<DeviceResult> gates;
    std::vector<double> allDensity;
    ComponentVerifier verifier(verifyRadius);
    std::uint64_t groundRejects = 0, boundaryDeferred = 0, verifiedNow = 0, gateMismatches = 0;

    try {
        for (std::size_t baseIndex = 0; baseIndex < candidates.size(); baseIndex += static_cast<std::size_t>(c.batch)) {
            const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(c.batch), candidates.size() - baseIndex));
            hostSeeds.resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) hostSeeds[static_cast<std::size_t>(i)] = candidates[baseIndex + static_cast<std::size_t>(i)].seed;
            checkHip(hipMemcpy(b.seeds, hostSeeds.data(), static_cast<std::size_t>(n) * sizeof(std::int64_t), hipMemcpyHostToDevice), "copy P4 verify seed batch");
            launchTerrain(b, n, c.terrainThreads);
            const int threads = 128;
            const int blocks = (n + threads - 1) / threads;
            hipLaunchKernelGGL(fullGateKernel, dim3(blocks), dim3(threads), 0, 0,
                b.seeds, b.noise1, b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall, n, b.results);
            checkHip(hipGetLastError(), "launch P4 full re-gate");
            checkHip(hipDeviceSynchronize(), "finish P4 full re-gate");
            gates.resize(static_cast<std::size_t>(n));
            checkHip(hipMemcpy(gates.data(), b.results, static_cast<std::size_t>(n) * sizeof(DeviceResult), hipMemcpyDeviceToHost), "copy P4 full gates");

            allDensity.resize(static_cast<std::size_t>(n) * static_cast<std::size_t>(coarsecore::CELLS));
            checkHip(hipMemcpy(allDensity.data(), b.noise1, allDensity.size() * sizeof(double), hipMemcpyDeviceToHost), "bulk copy P4 candidate density");

            for (int i = 0; i < n; ++i) {
                const Candidate& input = candidates[baseIndex + static_cast<std::size_t>(i)];
                const auto& g = gates[static_cast<std::size_t>(i)];
                if (!g.qualified || g.seed != input.seed || g.spawnSurfaceY != input.spawnSurfaceY ||
                    g.neighborMaxY != input.firstUpperY || g.pillarDepth != input.airGap ||
                    g.playerFeetY != input.playerFeetY || g.supportY != input.supportY || g.sandReason != input.sandReason) {
                    ++gateMismatches;
                    continue;
                }
                const double* density = allDensity.data() + static_cast<std::size_t>(i) * static_cast<std::size_t>(coarsecore::CELLS);
                const auto s = verifier.trace(density, input.supportY);
                if (s.groundConnected) { ++groundRejects; continue; }
                if (s.touchedBoundary) { ++boundaryDeferred; writeCandidate(boundary, input); continue; }
                if (!s.contained || s.blocks <= 0) continue;

                if (verifiedIds.insert(input.sequenceIndex).second) {
                    const auto r = makeRecord(input, s, verifyRadius);
                    appendVerified(verifiedPath, r);
                    for (auto& boardRef : boards) insertTop(boardRef, r, c.top);
                    ++verifiedNow;
                    std::cout << "P4 VERIFIED FLOATING seed=" << r.seed
                              << " blocks=" << r.blocks << " footprint=" << r.footprint
                              << " span=" << r.spanX << 'x' << r.spanZ
                              << " islandY=" << r.minY << ".." << r.maxY
                              << " airGap=" << r.airGap << " feetY=" << r.playerFeetY
                              << " verifyR=" << r.verifyRadius << '\n';
                }
            }
        }
    } catch (...) { freeBuffers(b); throw; }
    freeBuffers(b);
    boundary.flush();
    saveBoards(boards, c.outputDir);

    std::cout << "P4 verify done input=" << candidates.size()
              << " verifiedNew=" << verifiedNow
              << " groundRejects=" << groundRejects
              << " boundaryDeferred=" << boundaryDeferred
              << " gateMismatches=" << gateMismatches << '\n';
    if (gateMismatches != 0) throw std::runtime_error("origin scout/full verifier mismatch detected; stopping to protect correctness");
    return 0;
}

} // namespace floating_island_spawn_p4_verify

int main(int argc, char** argv) {
    try {
        const auto c = floating_island_spawn_p4_verify::parseArgsP4(argc, argv);
        return floating_island_spawn_p4_verify::run(c);
    } catch (const std::exception& e) {
        std::cerr << "FloatingIslandSpawn P4 verify ERROR: " << e.what() << '\n';
        return 1;
    }
}
