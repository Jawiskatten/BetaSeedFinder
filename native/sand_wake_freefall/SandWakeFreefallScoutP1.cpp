#define main p6_cave_diagnostic_embedded_main
#include "../floating_island_spawn/FloatingIslandSpawnP6CaveDiagnostic.cpp"
#undef main

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace sand_wake_freefall_p1 {
using namespace highest_pillar_spawn_p1;
namespace cave = floating_island_spawn_p6_caves;
namespace raw = floating_island_spawn_p6_diag;

struct BasicResult {
    std::int64_t seed;
    std::uint64_t sequenceIndex;
    int pass;
    int spawnSurfaceY;
    int desert;
    int sandPatch;
    int sandReason;
    int surfaceDepth;
};

struct CandidateResult {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    int gateY = -1;
    int sandBottomY = -1;
    int sandBlocks = 0;
    int sandstoneBlocks = 0;
    int caveFloorY = -1;
    int relocatedTopY = -1;
    int landingFeetY = -1;
    int potentialDrop = -1;
    int caveCarvedBlocks = 0;
    int desert = 0;
    int sandReason = 0;
    int surfaceDepth = 0;
};

struct Config {
    std::filesystem::path outputDir;
    std::filesystem::path bestState;
    std::uint64_t count = 1000000;
    std::uint64_t startIndex = 0;
    int batch = 32768;
    int terrainThreads = 64;
    int minPotentialDrop = 5;
    int progressMs = 1000;
    std::uint64_t randomKey = 0;
    bool randomKeySet = false;
    SeedMode seedMode = SeedMode::Unique48;
};

struct BestState {
    int drop = -1;
    std::int64_t seed = 0;
    int floorY = -1;
    int sandBlocks = 0;
};

__global__ void basicSandGateKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        std::uint64_t baseIndex,
        BasicResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;
    const int center = -coarsecore::FROM_COARSE;

    BasicResult r{};
    r.seed = seeds[i];
    r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(i);
    r.spawnSurfaceY = exactSpawnCheckYAtOrigin(density, base, center, center);
    if (r.spawnSurfaceY != 63) { out[i] = r; return; }

    p20::JavaRandom surface;
    surface.setSeed(0);
    const double sandJitter = surface.nextDouble();
    (void)surface.nextDouble(); // gravel jitter; exact Java oracle rejects rare gravel override false positives.
    const double depthJitter = surface.nextDouble();
    const bool sandPatch = originSandNoise[i] + sandJitter * 0.2 > 0.0;
    const int depth = static_cast<int>(originStoneNoise[i] / 3.0 + 3.0 + depthJitter * 0.25);
    const bool desert = betaBiomeIsDesert(originTemperature[i], originRainfall[i]);
    const int reason = desert ? 2 : (sandPatch ? 1 : 0);

    r.desert = desert ? 1 : 0;
    r.sandPatch = sandPatch ? 1 : 0;
    r.sandReason = reason;
    r.surfaceDepth = depth;
    if (depth <= 0 || reason == 0) { out[i] = r; return; }

    // Saved spawn Y is 64 and the player starts with feet/bbox bottom at Y65.
    // We only want a clean freefall setup, not a pre-existing collision push.
    if (terrainSolidAtBlock(density, base, 0, 65, 0)
            || terrainSolidAtBlock(density, base, 0, 66, 0)) {
        out[i] = r;
        return;
    }

    r.pass = 1;
    out[i] = r;
}

static std::array<int,128> buildExactOriginColumn(
        const double* density,
        double sandNoise,
        double stoneNoise,
        double temperature,
        double rainfall,
        int& sandstoneBlocksOut) {
    std::array<int,128> b{};
    for (int y = 0; y < 128; ++y) {
        if (raw::solidAtBlockHost(density, 0, y, 0)) b[static_cast<std::size_t>(y)] = cave::STONE;
        else b[static_cast<std::size_t>(y)] = y < 64 ? cave::WATER_STILL : cave::AIR;
    }

    cave::JRandom surface(0);
    const bool sandPatch = sandNoise + surface.nextDouble() * 0.2 > 0.0;
    // We intentionally do not have origin gravel noise in the compact GPU API.
    // For beach candidates sandPatch overrides gravel exactly. Desert-only false
    // positives are harmless because the authoritative Java world rejects them.
    (void)surface.nextDouble();
    const int depth = static_cast<int>(stoneNoise / 3.0 + 3.0 + surface.nextDouble() * 0.25);
    const bool desert = cave::biomeIsDesert(temperature, rainfall);

    int remaining = -1;
    int top = desert ? cave::SAND : cave::GRASS;
    int filler = desert ? cave::SAND : cave::DIRT;
    sandstoneBlocksOut = 0;

    for (int y = 127; y >= 0; --y) {
        const int bedrockRoll = surface.nextInt(5);
        if (y <= bedrockRoll) {
            b[static_cast<std::size_t>(y)] = 7; // bedrock
            continue;
        }
        const int rawId = b[static_cast<std::size_t>(y)];
        if (rawId == cave::AIR || rawId == cave::WATER_STILL) {
            remaining = -1;
            continue;
        }
        if (rawId != cave::STONE) continue;

        if (remaining == -1) {
            top = desert ? cave::SAND : cave::GRASS;
            filler = desert ? cave::SAND : cave::DIRT;
            if (depth <= 0) {
                top = cave::AIR;
                filler = cave::STONE;
            } else if (y >= 60 && y <= 65 && sandPatch) {
                top = cave::SAND;
                filler = cave::SAND;
            }
            if (y < 64 && top == cave::AIR) top = cave::WATER_STILL;
            remaining = depth;
            b[static_cast<std::size_t>(y)] = y >= 63 ? top : filler;
        } else if (remaining > 0) {
            --remaining;
            b[static_cast<std::size_t>(y)] = filler;
            if (remaining == 0 && filler == cave::SAND) {
                remaining = surface.nextInt(4);
                filler = cave::SANDSTONE;
            }
        }
    }

    for (int y = 0; y < 63; ++y) if (b[static_cast<std::size_t>(y)] == cave::SANDSTONE) ++sandstoneBlocksOut;
    return b;
}

static bool isFallThrough(int id) {
    return id == cave::AIR || id == cave::WATER_MOVING || id == cave::WATER_STILL || id == cave::LAVA_MOVING;
}

static bool analyzeCandidate(const BasicResult& basic,
                             const double* density,
                             double sandNoise,
                             double stoneNoise,
                             double temp,
                             double rain,
                             int minPotentialDrop,
                             CandidateResult& out) {
    int sandstoneBlocks = 0;
    auto blocks = buildExactOriginColumn(density, sandNoise, stoneNoise, temp, rain, sandstoneBlocks);
    cave::CaveOriginSimulator caves(basic.seed, density, blocks);
    caves.generateTargetChunkZero();
    const auto& carved = caves.carvedOriginY();

    const int gateY = cave::firstUncovered(blocks);
    if (gateY != 63 || blocks[63] != cave::SAND) return false;

    int bottom = 63;
    while (bottom - 1 >= 0 && blocks[static_cast<std::size_t>(bottom - 1)] == cave::SAND) --bottom;
    const int sandBlocks = 64 - bottom;
    if (sandBlocks < 1 || bottom <= 0) return false;

    // The whole point of this mechanism is dormant, unsupported sand over a
    // DRY cave. Water immediately below would only make the player splash.
    if (blocks[static_cast<std::size_t>(bottom - 1)] != cave::AIR) return false;

    int floorY = bottom - 1;
    while (floorY >= 0 && isFallThrough(blocks[static_cast<std::size_t>(floorY)])) {
        // Reject wet/lava columns: they are not the dramatic dry freefall target.
        const int id = blocks[static_cast<std::size_t>(floorY)];
        if (id != cave::AIR) return false;
        --floorY;
    }
    if (floorY < 0 || !cave::fullCollisionBlock(blocks[static_cast<std::size_t>(floorY)])) return false;

    // If the sand stack wakes while BlockSand.fallInstantly=true, the bottom
    // sand lands at floor+1 and the remaining sand cascades onto it.
    const int relocatedTop = floorY + sandBlocks;
    const int landingFeet = relocatedTop + 1;
    const int potentialDrop = 65 - landingFeet;
    if (potentialDrop < minPotentialDrop) return false;

    out.seed = basic.seed;
    out.sequenceIndex = basic.sequenceIndex;
    out.gateY = gateY;
    out.sandBottomY = bottom;
    out.sandBlocks = sandBlocks;
    out.sandstoneBlocks = sandstoneBlocks;
    out.caveFloorY = floorY;
    out.relocatedTopY = relocatedTop;
    out.landingFeetY = landingFeet;
    out.potentialDrop = potentialDrop;
    out.caveCarvedBlocks = static_cast<int>(carved.size());
    out.desert = basic.desert;
    out.sandReason = basic.sandReason;
    out.surfaceDepth = basic.surfaceDepth;
    return true;
}

static std::uint64_t parseU64(const std::string& v, const char* name) {
    std::size_t used = 0; const auto x = std::stoull(v, &used, 0);
    if (used != v.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + v);
    return x;
}
static int parseInt(const std::string& v, const char* name) {
    std::size_t used = 0; const int x = std::stoi(v, &used, 0);
    if (used != v.size()) throw std::invalid_argument(std::string("invalid ") + name + ": " + v);
    return x;
}

static Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* n) { if (++i >= argc) throw std::invalid_argument(std::string("missing ") + n); return std::string(argv[i]); };
        if (a == "--output") c.outputDir = value("--output");
        else if (a == "--best-state") c.bestState = value("--best-state");
        else if (a == "--count") c.count = parseU64(value("--count"), "count");
        else if (a == "--start-index") c.startIndex = parseU64(value("--start-index"), "start-index");
        else if (a == "--batch") c.batch = parseInt(value("--batch"), "batch");
        else if (a == "--terrain-threads") c.terrainThreads = parseInt(value("--terrain-threads"), "terrain-threads");
        else if (a == "--min-potential-drop") c.minPotentialDrop = parseInt(value("--min-potential-drop"), "min-potential-drop");
        else if (a == "--progress-ms") c.progressMs = parseInt(value("--progress-ms"), "progress-ms");
        else if (a == "--random-key") { c.randomKey = parseU64(value("--random-key"), "random-key"); c.randomKeySet = true; }
        else if (a == "--seed-mode") {
            const std::string m = value("--seed-mode");
            if (m == "unique48") c.seedMode = SeedMode::Unique48;
            else if (m == "splitmix64") c.seedMode = SeedMode::SplitMix64;
            else throw std::invalid_argument("--seed-mode must be unique48 or splitmix64");
        } else throw std::invalid_argument("unknown argument: " + a);
    }
    if (c.outputDir.empty()) throw std::invalid_argument("--output is required");
    if (c.batch < 256 || c.batch > 32768) throw std::invalid_argument("--batch must be 256..32768");
    if (c.terrainThreads != 64 && c.terrainThreads != 128 && c.terrainThreads != 256) throw std::invalid_argument("--terrain-threads must be 64,128,256");
    if (c.minPotentialDrop < 1 || c.minPotentialDrop > 60) throw std::invalid_argument("--min-potential-drop must be 1..60");
    if (c.seedMode == SeedMode::Unique48 && (c.startIndex >= JAVA_SEED_PERIOD || c.count > JAVA_SEED_PERIOD - c.startIndex)) throw std::invalid_argument("unique48 range exceeds 2^48");
    if (!c.randomKeySet) {
        std::random_device rd;
        c.randomKey = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd()) ^ static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    }
    return c;
}

static BestState loadBest(const std::filesystem::path& p) {
    BestState b;
    if (p.empty()) return b;
    std::ifstream f(p); std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('='); if (eq == std::string::npos) continue;
        const auto k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "DROP") b.drop = std::stoi(v);
            else if (k == "SEED") b.seed = std::stoll(v);
            else if (k == "FLOOR_Y") b.floorY = std::stoi(v);
            else if (k == "SAND_BLOCKS") b.sandBlocks = std::stoi(v);
        } catch (...) {}
    }
    return b;
}
static void saveBest(const std::filesystem::path& p, const BestState& b) {
    if (p.empty()) return;
    std::ofstream f(p, std::ios::trunc);
    f << "DROP=" << b.drop << "\nSEED=" << b.seed << "\nFLOOR_Y=" << b.floorY << "\nSAND_BLOCKS=" << b.sandBlocks << "\n";
}

static int run(const Config& c) {
    printDevice();
    std::filesystem::create_directories(c.outputDir);
    const auto csvPath = c.outputDir / ("candidates_" + std::to_string(c.startIndex) + ".csv");
    std::ofstream csv(csvPath, std::ios::trunc);
    if (!csv) throw std::runtime_error("cannot write " + csvPath.string());
    csv << "seed,sequence_index,gate_y,sand_bottom_y,sand_blocks,sandstone_blocks,cave_floor_y,relocated_top_y,landing_feet_y,potential_drop,cave_carved_origin_blocks,desert,sand_reason,surface_depth\n";

    DeviceBuffers b = allocateBuffers(c.batch);
    BasicResult* dBasic = nullptr;
    allocateArray(dBasic, static_cast<std::size_t>(c.batch), "allocate sand-wake basic results");
    std::vector<BasicResult> basic(static_cast<std::size_t>(c.batch));
    std::vector<std::int64_t> compactSeeds(static_cast<std::size_t>(c.batch));
    std::vector<BasicResult> compactBasic(static_cast<std::size_t>(c.batch));

    BestState best = loadBest(c.bestState);
    std::uint64_t checked = 0, basicHits = 0, caveOpenHits = 0, candidates = 0;
    const auto started = std::chrono::steady_clock::now();
    auto lastPrint = started;

    try {
        while (checked < c.count) {
            const int n = static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint64_t>(c.batch), c.count - checked));
            const std::uint64_t seqBase = c.startIndex + checked;
            const int seedThreads = 256, seedBlocks = (n + seedThreads - 1) / seedThreads;
            hipLaunchKernelGGL(generateRandomSeedsKernel, dim3(seedBlocks), dim3(seedThreads), 0, 0,
                b.seeds, n, c.randomKey, seqBase, static_cast<int>(c.seedMode));
            checkHip(hipGetLastError(), "generate sand-wake seeds");
            launchTerrain(b, n, c.terrainThreads);

            const int scoreThreads = 128, scoreBlocks = (n + scoreThreads - 1) / scoreThreads;
            hipLaunchKernelGGL(basicSandGateKernel, dim3(scoreBlocks), dim3(scoreThreads), 0, 0,
                b.seeds, b.noise1, b.originSandNoise, b.originStoneNoise,
                b.originTemperature, b.originRainfall, n, seqBase, dBasic);
            checkHip(hipGetLastError(), "launch sand-wake basic gate");
            checkHip(hipDeviceSynchronize(), "finish sand-wake basic gate");
            checkHip(hipMemcpy(basic.data(), dBasic, static_cast<std::size_t>(n) * sizeof(BasicResult), hipMemcpyDeviceToHost), "copy sand-wake basic results");

            int m = 0;
            for (int i = 0; i < n; ++i) if (basic[static_cast<std::size_t>(i)].pass) {
                compactSeeds[static_cast<std::size_t>(m)] = basic[static_cast<std::size_t>(i)].seed;
                compactBasic[static_cast<std::size_t>(m)] = basic[static_cast<std::size_t>(i)];
                ++m;
            }
            basicHits += static_cast<std::uint64_t>(m);

            if (m > 0) {
                checkHip(hipMemcpy(b.seeds, compactSeeds.data(), static_cast<std::size_t>(m) * sizeof(std::int64_t), hipMemcpyHostToDevice), "upload sand-wake compact seeds");
                launchTerrain(b, m, c.terrainThreads);
                std::vector<double> density(static_cast<std::size_t>(m) * coarsecore::CELLS);
                std::vector<double> sand(static_cast<std::size_t>(m)), stone(static_cast<std::size_t>(m)), temp(static_cast<std::size_t>(m)), rain(static_cast<std::size_t>(m));
                checkHip(hipMemcpy(density.data(), b.noise1, density.size() * sizeof(double), hipMemcpyDeviceToHost), "copy sand-wake density");
                checkHip(hipMemcpy(sand.data(), b.originSandNoise, static_cast<std::size_t>(m) * sizeof(double), hipMemcpyDeviceToHost), "copy sand-wake sand noise");
                checkHip(hipMemcpy(stone.data(), b.originStoneNoise, static_cast<std::size_t>(m) * sizeof(double), hipMemcpyDeviceToHost), "copy sand-wake stone noise");
                checkHip(hipMemcpy(temp.data(), b.originTemperature, static_cast<std::size_t>(m) * sizeof(double), hipMemcpyDeviceToHost), "copy sand-wake temp");
                checkHip(hipMemcpy(rain.data(), b.originRainfall, static_cast<std::size_t>(m) * sizeof(double), hipMemcpyDeviceToHost), "copy sand-wake rain");

                for (int j = 0; j < m; ++j) {
                    CandidateResult r;
                    if (!analyzeCandidate(compactBasic[static_cast<std::size_t>(j)],
                        density.data() + static_cast<std::size_t>(j) * coarsecore::CELLS,
                        sand[static_cast<std::size_t>(j)], stone[static_cast<std::size_t>(j)],
                        temp[static_cast<std::size_t>(j)], rain[static_cast<std::size_t>(j)],
                        c.minPotentialDrop, r)) continue;
                    ++caveOpenHits;
                    ++candidates;
                    csv << r.seed << ',' << r.sequenceIndex << ',' << r.gateY << ',' << r.sandBottomY << ','
                        << r.sandBlocks << ',' << r.sandstoneBlocks << ',' << r.caveFloorY << ',' << r.relocatedTopY << ','
                        << r.landingFeetY << ',' << r.potentialDrop << ',' << r.caveCarvedBlocks << ',' << r.desert << ','
                        << r.sandReason << ',' << r.surfaceDepth << '\n';
                    if (r.potentialDrop > best.drop) {
                        best.drop = r.potentialDrop; best.seed = r.seed; best.floorY = r.caveFloorY; best.sandBlocks = r.sandBlocks;
                        saveBest(c.bestState, best);
                        std::cout << "NEW BEST SAND-WAKE POTENTIAL seed=" << r.seed
                                  << " drop=" << r.potentialDrop << " floorY=" << r.caveFloorY
                                  << " sandBlocks=" << r.sandBlocks << " bottomSandY=" << r.sandBottomY << "\n";
                    }
                }
            }

            checked += static_cast<std::uint64_t>(n);
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() >= c.progressMs || checked == c.count) {
                csv.flush();
                const double sec = std::chrono::duration<double>(now - started).count();
                std::cout << "progress checked=" << checked << '/' << c.count
                          << " rate=" << static_cast<std::uint64_t>(checked / std::max(0.001, sec)) << " seeds/s"
                          << " sandGate=" << basicHits << " dryCaveCandidates=" << caveOpenHits;
                if (best.drop >= 0) std::cout << " runBestPotential=" << best.drop << " seed=" << best.seed;
                else std::cout << " runBestPotential=NONE";
                std::cout << "\n";
                lastPrint = now;
            }
        }
    } catch (...) {
        if (dBasic) (void)hipFree(dBasic);
        freeBuffers(b);
        throw;
    }

    if (dBasic) checkHip(hipFree(dBasic), "free sand-wake basic results");
    freeBuffers(b);
    saveBest(c.bestState, best);
    std::cout << "DONE checked=" << checked << " sandGate=" << basicHits << " candidates=" << candidates
              << " runBestPotential=" << (best.drop >= 0 ? std::to_string(best.drop) : std::string("NONE"))
              << " file=" << csvPath.string() << "\n";
    std::cout << "P1 geometry proof: exact chunk-0 surface replacement + exact Beta cave path at origin. Candidate means dormant sand over a dry cave could fall >= threshold if population wakes it. Java verifies the natural wake during real client startup.\n";
    return 0;
}

} // namespace sand_wake_freefall_p1

int main(int argc, char** argv) {
    try { return sand_wake_freefall_p1::run(sand_wake_freefall_p1::parseArgs(argc, argv)); }
    catch (const std::exception& e) { std::cerr << "SandWakeFreefall P1 ERROR: " << e.what() << '\n'; return 1; }
}
