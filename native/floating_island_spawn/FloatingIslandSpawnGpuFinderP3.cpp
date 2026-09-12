#define main highest_pillar_spawn_p1_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <array>
#include <climits>

namespace floating_island_spawn_p3 {

using namespace highest_pillar_spawn_p1;

// P3 is deliberately two-stage:
//  1) GPU: exact Beta sand-spawn gate + prove an upper disconnected-looking mass
//     intersects the initial player AABB and pushes the player upward.
//  2) CPU: copy only those rare candidates' density lattices and flood-fill the
//     exact block component the player lands on. We only keep components that
//     are fully contained inside a large verification box, so ground-connected
//     terrain cannot masquerade as a floating island.

__global__ void scoreFloatingSpawnGateKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        std::uint64_t baseIndex,
        DeviceResult* out) {
    const int seedIndex = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (seedIndex >= count) return;
    const std::size_t base = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;
    const int center = -coarsecore::FROM_COARSE;

    DeviceResult r{};
    r.seed = seeds[seedIndex];
    r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(seedIndex);
    r.neighborMaxY = -1;
    r.dropR1 = r.dropR2Sample = r.dropR4Sample = -999;

    const int spawnSurfaceY = exactSpawnCheckYAtOrigin(density, base, center, center);
    r.spawnSurfaceY = spawnSurfaceY;
    const bool desert = betaBiomeIsDesert(originTemperature[seedIndex], originRainfall[seedIndex]);
    r.originBiome = desert ? 1 : 0;

    // Exact chunk-(0,0), column-(0,0) surface replacement RNG draws.
    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool beachSand = originSandNoise[seedIndex] + sandJitter * 0.2 > 0.0;
    const int surfaceDepth = static_cast<int>(originStoneNoise[seedIndex] / 3.0 + 3.0 + depthJitter * 0.25);
    const bool beachBand = spawnSurfaceY >= 60 && spawnSurfaceY <= 65;
    const int reason = desert ? 2 : ((beachBand && beachSand) ? 1 : 0);
    r.sandReason = reason;
    const bool spawnSand = spawnSurfaceY >= 63 && surfaceDepth > 0 && reason != 0;
    r.spawnSand = spawnSand ? 1 : 0;
    if (!spawnSand) {
        out[seedIndex] = r;
        return;
    }

    // Simulate the physical placement separately from the sand eligibility test.
    // A floating mass at Y65/Y66 can intersect the initial AABB even though an
    // air gap at Y64 kept the spawn-valid sand surface below it.
    const int feetY = actualPlayerFeetY(density, base);
    r.playerFeetY = feetY;
    if (feetY <= 65) {
        out[seedIndex] = r;
        return;
    }

    const int supportY = highestSolidAtOrBelow(density, base, 0, 0, feetY - 1);
    r.supportY = supportY;
    if (supportY < 0 || supportY != feetY - 1) {
        out[seedIndex] = r;
        return;
    }

    int firstUpperSolidY = -1;
    for (int y = spawnSurfaceY + 1; y <= supportY; ++y) {
        if (terrainSolidAtBlock(density, base, 0, y, 0)) {
            firstUpperSolidY = y;
            break;
        }
    }
    if (firstUpperSolidY < 0) {
        out[seedIndex] = r;
        return;
    }

    const int airGap = firstUpperSolidY - spawnSurfaceY - 1;
    if (airGap < 1) {
        out[seedIndex] = r;
        return;
    }

    // Reuse fields that are no longer needed after the P1 gate.
    r.pillarDepth = airGap;
    r.neighborMaxY = firstUpperSolidY;
    r.qualified = 1;
    out[seedIndex] = r;
}

static int hostFloorDivPositiveStep(int value, int step) {
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
    const int coarseX = hostFloorDivPositiveStep(worldX, 4);
    const int coarseZ = hostFloorDivPositiveStep(worldZ, 4);
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
    // terrainSolidAtBlock needs ix and ix+1. Compute a symmetric world-block
    // radius guaranteed to remain inside the generated coarse lattice.
    const int minBlock = coarsecore::FROM_COARSE * 4;
    const int maxUsableCoarse = coarsecore::FROM_COARSE + coarsecore::SIZE - 2;
    const int maxBlock = maxUsableCoarse * 4 + 3;
    int safe = std::min(-minBlock - 1, maxBlock - 1);
    if (safe < 4) safe = 4;
    return std::min(48, safe);
}

struct ComponentStats {
    bool contained = false;
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

class ComponentVerifier {
public:
    explicit ComponentVerifier(int radius)
        : radius_(radius), side_(radius * 2 + 1), plane_(side_ * side_),
          visited_(static_cast<std::size_t>(plane_) * 128u, 0),
          columnSeen_(static_cast<std::size_t>(plane_), 0) {
        queue_.reserve(static_cast<std::size_t>(plane_) * 32u);
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

            if (x == -radius_ || x == radius_ || z == -radius_ || z == radius_) {
                s.touchedBoundary = true;
                return s; // cannot prove this component is isolated with this lattice window
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

    int radius() const { return radius_; }

private:
    int radius_;
    int side_;
    int plane_;
    std::vector<unsigned char> visited_;
    std::vector<unsigned char> columnSeen_;
    std::vector<int> queue_;
};

struct FloatingRecord {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    std::int64_t score = 0;
    int spawnSurfaceY = -1;
    int firstUpperSolidY = -1;
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

static std::int64_t makeScore(const FloatingRecord& r) {
    // Strict objective order: total component blocks first; footprint second;
    // exposed top area third; physical spawn height only breaks deep ties.
    return static_cast<std::int64_t>(r.blocks) * 1000000000LL
         + static_cast<std::int64_t>(std::min(9999, r.footprint)) * 100000LL
         + static_cast<std::int64_t>(std::min(9999, r.topSurface)) * 100LL
         + static_cast<std::int64_t>(std::max(0, std::min(99, r.playerFeetY)));
}

static void launchGateBatch(DeviceBuffers& b, const Config& c, std::uint64_t baseIndex, int count,
                            std::vector<DeviceResult>& host) {
    if (c.singleSeedSet) {
        checkHip(hipMemcpy(b.seeds, &c.singleSeed, sizeof(c.singleSeed), hipMemcpyHostToDevice), "copy single seed");
    } else {
        const int threads = 256;
        const int blocks = (count + threads - 1) / threads;
        hipLaunchKernelGGL(generateRandomSeedsKernel, dim3(blocks), dim3(threads), 0, 0,
            b.seeds, count, c.randomKey, baseIndex, static_cast<int>(c.seedMode));
        checkHip(hipGetLastError(), "generate random seeds");
    }
    launchTerrain(b, count, c.terrainThreads);
    const int threads = 128;
    const int blocks = (count + threads - 1) / threads;
    hipLaunchKernelGGL(scoreFloatingSpawnGateKernel, dim3(blocks), dim3(threads), 0, 0,
        b.seeds, b.noise1,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        count, baseIndex, b.results);
    checkHip(hipGetLastError(), "launch floating-island spawn gate");
    checkHip(hipDeviceSynchronize(), "finish floating-island spawn gate");
    host.resize(static_cast<std::size_t>(count));
    checkHip(hipMemcpy(host.data(), b.results, static_cast<std::size_t>(count) * sizeof(DeviceResult), hipMemcpyDeviceToHost),
             "copy floating-island spawn gate results");
}

static void copyCandidateDensity(DeviceBuffers& b, int seedIndex, std::vector<double>& hostDensity) {
    hostDensity.resize(static_cast<std::size_t>(coarsecore::CELLS));
    const double* src = b.noise1 + static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;
    checkHip(hipMemcpy(hostDensity.data(), src,
        static_cast<std::size_t>(coarsecore::CELLS) * sizeof(double), hipMemcpyDeviceToHost),
        "copy candidate density lattice");
}

using HostMetric = std::int64_t (*)(const FloatingRecord&);
static std::int64_t metricLargest(const FloatingRecord& r) { return r.score; }
static std::int64_t metricBlocks(const FloatingRecord& r) { return r.blocks; }
static std::int64_t metricFootprint(const FloatingRecord& r) { return r.footprint; }
static std::int64_t metricSurface(const FloatingRecord& r) { return r.topSurface; }
static std::int64_t metricHighest(const FloatingRecord& r) { return r.playerFeetY; }

struct HostBoard {
    std::string name;
    HostMetric metric;
    std::vector<FloatingRecord> rows;
};

static void insertTop(HostBoard& b, const FloatingRecord& r, int limit) {
    const std::int64_t value = b.metric(r);
    auto it = std::lower_bound(b.rows.begin(), b.rows.end(), value,
        [&](const FloatingRecord& a, std::int64_t v) {
            const auto av = b.metric(a);
            if (av != v) return av > v;
            return a.score > r.score;
        });
    if (static_cast<int>(b.rows.size()) < limit || it != b.rows.end()) {
        b.rows.insert(it, r);
        if (static_cast<int>(b.rows.size()) > limit) b.rows.pop_back();
    }
}

static void writeCsv(const std::filesystem::path& path, const std::vector<FloatingRecord>& rows) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f << "rank,seed,sequence_index,score,spawn_surface_y,first_upper_solid_y,air_gap,player_feet_y,support_y,sand_reason,component_blocks,footprint_columns,top_surface_blocks,min_y,max_y,span_x,span_z,verify_radius\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        f << (i + 1) << ',' << r.seed << ',' << r.sequenceIndex << ',' << r.score << ','
          << r.spawnSurfaceY << ',' << r.firstUpperSolidY << ',' << r.airGap << ','
          << r.playerFeetY << ',' << r.supportY << ',' << r.sandReason << ','
          << r.blocks << ',' << r.footprint << ',' << r.topSurface << ','
          << r.minY << ',' << r.maxY << ',' << r.spanX << ',' << r.spanZ << ',' << r.verifyRadius << '\n';
    }
}

static std::vector<FloatingRecord> readCsv(const std::filesystem::path& path) {
    std::vector<FloatingRecord> rows;
    std::ifstream f(path);
    if (!f) return rows;
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) fields.push_back(field);
        if (fields.size() != 18) continue;
        try {
            FloatingRecord r;
            r.seed = std::stoll(fields[1]);
            r.sequenceIndex = std::stoull(fields[2]);
            r.score = std::stoll(fields[3]);
            r.spawnSurfaceY = std::stoi(fields[4]);
            r.firstUpperSolidY = std::stoi(fields[5]);
            r.airGap = std::stoi(fields[6]);
            r.playerFeetY = std::stoi(fields[7]);
            r.supportY = std::stoi(fields[8]);
            r.sandReason = std::stoi(fields[9]);
            r.blocks = std::stoi(fields[10]);
            r.footprint = std::stoi(fields[11]);
            r.topSurface = std::stoi(fields[12]);
            r.minY = std::stoi(fields[13]);
            r.maxY = std::stoi(fields[14]);
            r.spanX = std::stoi(fields[15]);
            r.spanZ = std::stoi(fields[16]);
            r.verifyRadius = std::stoi(fields[17]);
            rows.push_back(r);
        } catch (...) {}
    }
    return rows;
}

static const char* sandReasonNameP3(int code) {
    if (code == 2) return "DESERT_TOP";
    if (code == 1) return "BEACH_NOISE";
    return "NONE";
}

static void printRecord(const char* prefix, const FloatingRecord& r) {
    std::cout << prefix
              << " seed=" << r.seed
              << " blocks=" << r.blocks
              << " footprint=" << r.footprint
              << " topSurface=" << r.topSurface
              << " span=" << r.spanX << 'x' << r.spanZ
              << " islandY=" << r.minY << ".." << r.maxY
              << " airGap=" << r.airGap
              << " firstUpperY=" << r.firstUpperSolidY
              << " playerFeetY=" << r.playerFeetY
              << " supportY=" << r.supportY
              << " sand=" << sandReasonNameP3(r.sandReason)
              << " score=" << r.score << '\n';
}

static void saveState(const Config& c, std::uint64_t completed, const std::vector<HostBoard>& boards, int verifyRadius) {
    std::filesystem::create_directories(c.outputDir);
    for (const auto& b : boards) writeCsv(c.outputDir / ("top_" + b.name + ".csv"), b.rows);
    std::ofstream ck(c.outputDir / "checkpoint.txt", std::ios::trunc);
    ck << "VERSION=FloatingIslandSpawnP3\n"
       << "SPAWN_X=0\nSAVED_SPAWN_Y=64\nSPAWN_Z=0\n"
       << "VERIFY_RADIUS=" << verifyRadius << "\n"
       << "START_INDEX=" << c.startIndex << "\n"
       << "COMPLETED=" << completed << "\n"
       << "NEXT_INDEX=" << (c.startIndex + completed) << "\n"
       << "COUNT=" << c.count << "\n"
       << "RANDOM_KEY=" << c.randomKey << "\n"
       << "SEED_MODE=" << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64") << "\n";
    if (!boards.empty() && !boards[0].rows.empty()) {
        const auto& r = boards[0].rows.front();
        ck << "BEST_SEED=" << r.seed
           << "\nBEST_COMPONENT_BLOCKS=" << r.blocks
           << "\nBEST_FOOTPRINT=" << r.footprint
           << "\nBEST_TOP_SURFACE=" << r.topSurface
           << "\nBEST_PLAYER_FEET_Y=" << r.playerFeetY
           << "\nBEST_AIR_GAP=" << r.airGap << "\n";
    }
}

static FloatingRecord makeRecord(const DeviceResult& g, const ComponentStats& s, int verifyRadius) {
    FloatingRecord r;
    r.seed = g.seed;
    r.sequenceIndex = g.sequenceIndex;
    r.spawnSurfaceY = g.spawnSurfaceY;
    r.firstUpperSolidY = g.neighborMaxY;
    r.airGap = g.pillarDepth;
    r.playerFeetY = g.playerFeetY;
    r.supportY = g.supportY;
    r.sandReason = g.sandReason;
    r.blocks = s.blocks;
    r.footprint = s.footprint;
    r.topSurface = s.topSurface;
    r.minY = s.minY;
    r.maxY = s.maxY;
    r.spanX = s.spanX;
    r.spanZ = s.spanZ;
    r.verifyRadius = verifyRadius;
    r.score = makeScore(r);
    return r;
}

static int runSelfTest(Config c) {
    printDevice();
    c.singleSeedSet = true;
    c.singleSeed = 6430576860599818994LL;
    c.count = 1;
    c.batch = 1;

    DeviceBuffers b = allocateBuffers(1);
    std::vector<DeviceResult> gate;
    std::vector<double> density;
    try {
        launchGateBatch(b, c, 0, 1, gate);
        if (gate.empty() || !gate[0].qualified) {
            freeBuffers(b);
            throw std::runtime_error("known floating-pillar seed failed the P3 GPU gate");
        }
        copyCandidateDensity(b, 0, density);
    } catch (...) {
        freeBuffers(b);
        throw;
    }
    freeBuffers(b);

    ComponentVerifier verifier(safeVerificationRadius());
    const ComponentStats stats = verifier.trace(density.data(), gate[0].supportY);
    if (!stats.contained || stats.blocks < 1) {
        throw std::runtime_error("known floating-pillar seed failed exact component verification");
    }
    const FloatingRecord r = makeRecord(gate[0], stats, verifier.radius());
    printRecord("SELFTEST OK", r);
    return 0;
}

static int run(Config c) {
    if (c.selfTest) return runSelfTest(c);
    printDevice();
    const int verifyRadius = safeVerificationRadius();
    ComponentVerifier verifier(verifyRadius);

    std::cout << "FloatingIslandSpawn P3 | sand -> air gap -> upper component -> collision-pushed player\n";
    std::cout << "GPU gate: exact Beta sand spawn eligibility plus upward player displacement.\n";
    std::cout << "CPU verification: exact 6-connected solid component at the landing block; ground-connected/boundary-touching components are rejected.\n";
    std::cout << "Ranking: verified floating component block count FIRST, footprint second, top surface third.\n";
    std::cout << "Verification radius: +/-" << verifyRadius << " blocks around spawn (component must fit fully inside).\n";
    std::cout << "Seed mode: " << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64")
              << " | randomKey=" << c.randomKey << '\n';

    std::vector<HostBoard> boards = {
        {"largest", metricLargest, {}},
        {"blocks", metricBlocks, {}},
        {"footprint", metricFootprint, {}},
        {"top_surface", metricSurface, {}},
        {"highest_spawn", metricHighest, {}}
    };

    std::uint64_t completed = 0;
    if (c.resumeExisting) {
        for (auto& b : boards) {
            b.rows = readCsv(c.outputDir / ("top_" + b.name + ".csv"));
            if (static_cast<int>(b.rows.size()) > c.top) b.rows.resize(static_cast<std::size_t>(c.top));
        }
        std::ifstream ck(c.outputDir / "checkpoint.txt");
        std::string line;
        while (std::getline(ck, line)) {
            if (line.rfind("COMPLETED=", 0) == 0) completed = std::stoull(line.substr(10));
            else if (line.rfind("RANDOM_KEY=", 0) == 0) c.randomKey = std::stoull(line.substr(11));
        }
        if (completed > c.count) completed = c.count;
        std::cout << "Resuming at completed=" << completed << '\n';
    }

    DeviceBuffers b = allocateBuffers(c.batch);
    std::vector<DeviceResult> gate;
    std::vector<double> density;
    auto start = std::chrono::steady_clock::now();
    auto lastProgress = start;
    auto lastCheckpoint = start;
    std::int64_t bestPrintedScore = boards[0].rows.empty() ? -1 : boards[0].rows.front().score;
    std::uint64_t gateCandidates = 0;
    std::uint64_t verifiedFloating = 0;
    std::uint64_t boundaryRejects = 0;

    try {
        while (completed < c.count) {
            const std::uint64_t left = c.count - completed;
            const int batchCount = static_cast<int>(std::min<std::uint64_t>(left, static_cast<std::uint64_t>(c.batch)));
            launchGateBatch(b, c, c.startIndex + completed, batchCount, gate);

            for (int i = 0; i < batchCount; ++i) {
                const auto& g = gate[static_cast<std::size_t>(i)];
                if (!g.qualified) continue;
                ++gateCandidates;
                copyCandidateDensity(b, i, density);
                const ComponentStats stats = verifier.trace(density.data(), g.supportY);
                if (stats.touchedBoundary) {
                    ++boundaryRejects;
                    continue;
                }
                if (!stats.contained || stats.blocks <= 0) continue;

                ++verifiedFloating;
                FloatingRecord r = makeRecord(g, stats, verifyRadius);
                for (auto& board : boards) insertTop(board, r, c.top);
                if (r.score > bestPrintedScore) {
                    bestPrintedScore = r.score;
                    printRecord("NEW FLOATING ISLAND SPAWN", r);
                }
            }

            completed += static_cast<std::uint64_t>(batchCount);
            const auto now = std::chrono::steady_clock::now();
            const auto progressElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgress).count();
            const auto checkpointElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheckpoint).count();
            if (progressElapsed >= c.progressMs || completed == c.count) {
                const double seconds = std::chrono::duration<double>(now - start).count();
                const double rate = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
                std::cout << "progress checked=" << completed << '/' << c.count
                          << " rate=" << std::fixed << std::setprecision(1) << rate << " seeds/s"
                          << " gpuGate=" << gateCandidates
                          << " verifiedFloating=" << verifiedFloating
                          << " boundaryRejects=" << boundaryRejects;
                if (!boards[0].rows.empty()) {
                    const auto& best = boards[0].rows.front();
                    std::cout << " bestBlocks=" << best.blocks
                              << " bestFootprint=" << best.footprint
                              << " bestFeetY=" << best.playerFeetY
                              << " bestSeed=" << best.seed;
                }
                std::cout << '\n';
                lastProgress = now;
            }
            if (checkpointElapsed >= c.checkpointMs || completed == c.count) {
                saveState(c, completed, boards, verifyRadius);
                lastCheckpoint = now;
            }
        }
    } catch (...) {
        try { saveState(c, completed, boards, verifyRadius); } catch (...) {}
        freeBuffers(b);
        throw;
    }

    freeBuffers(b);
    saveState(c, completed, boards, verifyRadius);
    if (!boards[0].rows.empty()) printRecord("FINAL FLOATING ISLAND SPAWN BEST", boards[0].rows.front());
    std::cout << "Results: " << c.outputDir.string() << '\n';
    return 0;
}

} // namespace floating_island_spawn_p3

int main(int argc, char** argv) {
    try {
        const auto c = highest_pillar_spawn_p1::parseArgs(argc, argv);
        return floating_island_spawn_p3::run(c);
    } catch (const std::exception& e) {
        std::cerr << "FloatingIslandSpawn P3 ERROR: " << e.what() << '\n';
        return 1;
    }
}
