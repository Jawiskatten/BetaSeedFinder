#define main highest_pillar_spawn_p1_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

namespace tallest_pillar_spawn_p2 {

using namespace highest_pillar_spawn_p1;

// True pillar height: start at the support block under the player and walk
// downward while the center block remains solid AND every one of the eight
// neighboring blocks at the same Y is air. The first Y where the column joins
// surrounding terrain ends the 1x1 pillar. No arbitrary 64-block cap.
__device__ __forceinline__ int trueOneByOnePillarHeight(
        const double* density, std::size_t base, int supportY) {
    int height = 0;
    for (int y = supportY; y >= 0; --y) {
        if (!terrainSolidAtBlock(density, base, 0, y, 0)) break;
        if (!adjacentEightAirAt(density, base, y)) break;
        ++height;
    }
    return height;
}

__global__ void scoreTallestPillarKernel(
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

    // Keep the exact Beta 1.7.3 origin-spawn sand gate from P1.
    const int spawnSurfaceY = exactSpawnCheckYAtOrigin(density, base, center, center);
    r.spawnSurfaceY = spawnSurfaceY;
    const bool desert = betaBiomeIsDesert(originTemperature[seedIndex], originRainfall[seedIndex]);
    r.originBiome = desert ? 1 : 0;

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

    // Actual physical spawn after Beta pushes the player upward through the
    // solid origin column. The pillar top must be exactly the support beneath
    // the resulting player feet position.
    const int feetY = actualPlayerFeetY(density, base);
    const int supportY = feetY > 0 ? highestSolidAtOrBelow(density, base, 0, 0, feetY - 1) : -1;
    r.playerFeetY = feetY;
    r.supportY = supportY;
    if (supportY < 0 || supportY != feetY - 1) {
        out[seedIndex] = r;
        return;
    }

    const bool isolatedAtTop = adjacentEightAirAt(density, base, supportY);
    r.isolatedR1 = isolatedAtTop ? 1 : 0;
    if (!isolatedAtTop) {
        out[seedIndex] = r;
        return;
    }

    r.pillarDepth = trueOneByOnePillarHeight(density, base, supportY);
    r.clearR2AtTop = clearCountAtTop(density, base, supportY, 2);
    r.clearR4AtTop = clearCountAtTop(density, base, supportY, 4);

    const int n1 = exactNeighborMaxY(density, base, supportY);
    const int n2 = sampleRingMaxY(density, base, supportY, 2);
    const int n4 = sampleRingMaxY(density, base, supportY, 4);
    r.neighborMaxY = n1;
    r.dropR1 = supportY - n1;
    r.dropR2Sample = supportY - n2;
    r.dropR4Sample = supportY - n4;

    r.qualified = r.pillarDepth > 0 ? 1 : 0;
    if (r.qualified) {
        // P2 OBJECTIVE ORDER:
        //   1. TRUE consecutive 1x1 pillar height (dominant)
        //   2. local vertical drop
        //   3. actual spawn height only as a tie-breaker
        // This deliberately prevents a high mountain tip of height 1 from
        // beating a lower but genuinely tall 1x1 needle.
        const std::int64_t pillar = static_cast<std::int64_t>(std::min(999, r.pillarDepth));
        const std::int64_t drop = static_cast<std::int64_t>(std::max(0, std::min(999, r.dropR1)));
        const std::int64_t feet = static_cast<std::int64_t>(std::max(0, std::min(999, r.playerFeetY)));
        const std::int64_t clear4 = static_cast<std::int64_t>(std::max(0, std::min(99, r.clearR4AtTop)));
        r.score = pillar * 1000000000000LL
                + drop   * 1000000000LL
                + feet   * 1000000LL
                + clear4;
    }
    out[seedIndex] = r;
}

static void launchBatchP2(DeviceBuffers& b, const Config& c, std::uint64_t baseIndex, int count,
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
    const int blocks = (count + SCORE_THREADS - 1) / SCORE_THREADS;
    hipLaunchKernelGGL(scoreTallestPillarKernel, dim3(blocks), dim3(SCORE_THREADS), 0, 0,
        b.seeds, b.noise1,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        count, baseIndex, b.results);
    checkHip(hipGetLastError(), "launch tallest 1x1 spawn pillar scoring");
    checkHip(hipDeviceSynchronize(), "finish tallest 1x1 spawn pillar batch");
    host.resize(static_cast<std::size_t>(count));
    checkHip(hipMemcpy(host.data(), b.results, static_cast<std::size_t>(count) * sizeof(DeviceResult), hipMemcpyDeviceToHost),
             "copy tallest pillar results");
}

static void printP2(const char* prefix, const DeviceResult& r) {
    const int baseY = r.supportY - r.pillarDepth + 1;
    std::cout << prefix << " seed=" << r.seed
              << " pillarHeight=" << r.pillarDepth
              << " pillarTopY=" << r.supportY
              << " pillarBaseY=" << baseY
              << " playerFeetY=" << r.playerFeetY
              << " dropR1=" << r.dropR1
              << " dropR2Sample=" << r.dropR2Sample
              << " dropR4Sample=" << r.dropR4Sample
              << " clearR4=" << r.clearR4AtTop << "/80"
              << " sand=" << sandReasonName(r.sandReason)
              << " score=" << r.score << "\n";
}

static void saveStateP2(const Config& c, std::uint64_t completed, const std::vector<Board>& boards) {
    std::filesystem::create_directories(c.outputDir);
    for (const auto& b : boards) writeCsv(c.outputDir / ("top_" + b.name + ".csv"), b.rows);
    std::ofstream ck(c.outputDir / "checkpoint.txt", std::ios::trunc);
    ck << "VERSION=Tallest1x1SpawnPillarP2\n"
       << "SPAWN_X=0\nSAVED_SPAWN_Y=64\nSPAWN_Z=0\n"
       << "RADIUS=" << p14config::CHUNK_RADIUS << "\n"
       << "START_INDEX=" << c.startIndex << "\n"
       << "COMPLETED=" << completed << "\n"
       << "NEXT_INDEX=" << (c.startIndex + completed) << "\n"
       << "COUNT=" << c.count << "\n"
       << "RANDOM_KEY=" << c.randomKey << "\n"
       << "SEED_MODE=" << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64") << "\n";
    if (!boards.empty() && !boards[0].rows.empty()) {
        const auto& r = boards[0].rows.front();
        ck << "BEST_SEED=" << r.seed
           << "\nBEST_SCORE=" << r.score
           << "\nBEST_PILLAR_HEIGHT=" << r.pillarDepth
           << "\nBEST_PILLAR_TOP_Y=" << r.supportY
           << "\nBEST_PILLAR_BASE_Y=" << (r.supportY - r.pillarDepth + 1)
           << "\nBEST_PLAYER_FEET_Y=" << r.playerFeetY
           << "\nBEST_DROP_R1=" << r.dropR1 << "\n";
    }
}

static int runSelfTestP2(Config c) {
    printDevice();
    c.singleSeedSet = true;
    c.singleSeed = 8734788222889465725LL;
    c.count = 1;
    c.batch = 1;
    DeviceBuffers b = allocateBuffers(1);
    std::vector<DeviceResult> host;
    try {
        launchBatchP2(b, c, 0, 1, host);
    } catch (...) {
        freeBuffers(b);
        throw;
    }
    freeBuffers(b);
    const auto& r = host.front();
    if (!r.spawnSand || r.spawnSurfaceY != 86 || r.playerFeetY != 87) {
        std::ostringstream msg;
        msg << "known-seed self-test mismatch: spawnSand=" << r.spawnSand
            << " spawnSurfaceY=" << r.spawnSurfaceY
            << " playerFeetY=" << r.playerFeetY
            << " (expected 1,86,87 before population edits)";
        throw std::runtime_error(msg.str());
    }
    printP2("SELFTEST OK", r);
    return 0;
}

static int runP2(Config c) {
    if (c.selfTest) return runSelfTestP2(c);
    printDevice();
    std::cout << "Tallest1x1SpawnPillar P2 | TRUE pillar-height objective\n";
    std::cout << "Hard gate: Beta can select X=0,Z=0 as spawn; player lands on the pillar top; all 8 adjacent blocks are air at every counted pillar Y.\n";
    std::cout << "Ranking: TRUE 1x1 pillar height FIRST. Local drop second. Spawn Y is only a tie-breaker.\n";
    std::cout << "Seed mode: " << (c.seedMode == SeedMode::Unique48 ? "unique48" : "splitmix64")
              << " | randomKey=" << c.randomKey << "\n";

    std::vector<Board> boards = {
        {"tallest_pillar", metricOverall, {}},
        {"pillar_height", metricDepth, {}},
        {"drop_r1", metricDrop, {}},
        {"highest_spawn_tiebreak", metricHighest, {}}
    };

    std::uint64_t completed = 0;
    if (c.resumeExisting) {
        for (auto& board : boards) {
            board.rows = readCsv(c.outputDir / ("top_" + board.name + ".csv"));
            if (static_cast<int>(board.rows.size()) > c.top) board.rows.resize(static_cast<std::size_t>(c.top));
        }
        std::ifstream ck(c.outputDir / "checkpoint.txt");
        std::string line;
        while (std::getline(ck, line)) {
            if (line.rfind("COMPLETED=", 0) == 0) completed = std::stoull(line.substr(10));
            else if (line.rfind("RANDOM_KEY=", 0) == 0) c.randomKey = std::stoull(line.substr(11));
        }
        if (completed > c.count) completed = c.count;
        std::cout << "Resuming at completed=" << completed << "\n";
    }

    DeviceBuffers b = allocateBuffers(c.batch);
    std::vector<DeviceResult> host;
    auto start = std::chrono::steady_clock::now();
    auto lastProgress = start;
    auto lastCheckpoint = start;
    std::int64_t bestPrintedScore = boards[0].rows.empty() ? -1 : boards[0].rows.front().score;
    std::uint64_t pillarCandidates = 0;

    try {
        while (completed < c.count) {
            const std::uint64_t left = c.count - completed;
            const int batchCount = static_cast<int>(std::min<std::uint64_t>(left, static_cast<std::uint64_t>(c.batch)));
            launchBatchP2(b, c, c.startIndex + completed, batchCount, host);
            for (const auto& r : host) {
                if (!r.qualified) continue;
                ++pillarCandidates;
                for (auto& board : boards) insertTop(board, r, c.top);
                if (r.score > bestPrintedScore) {
                    bestPrintedScore = r.score;
                    printP2("NEW TALLEST 1x1 SPAWN PILLAR", r);
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
                          << " pillarCandidates=" << pillarCandidates;
                if (!boards[0].rows.empty()) {
                    const auto& best = boards[0].rows.front();
                    std::cout << " bestPillarHeight=" << best.pillarDepth
                              << " bestTopY=" << best.supportY
                              << " bestFeetY=" << best.playerFeetY
                              << " bestSeed=" << best.seed;
                }
                std::cout << "\n";
                lastProgress = now;
            }
            if (checkpointElapsed >= c.checkpointMs || completed == c.count) {
                saveStateP2(c, completed, boards);
                lastCheckpoint = now;
            }
        }
    } catch (...) {
        try { saveStateP2(c, completed, boards); } catch (...) {}
        freeBuffers(b);
        throw;
    }

    freeBuffers(b);
    saveStateP2(c, completed, boards);
    if (!boards[0].rows.empty()) printP2("FINAL TALLEST 1x1 SPAWN PILLAR", boards[0].rows.front());
    std::cout << "Results: " << c.outputDir.string() << "\n";
    return 0;
}

} // namespace tallest_pillar_spawn_p2

int main(int argc, char** argv) {
    try {
        const auto c = highest_pillar_spawn_p1::parseArgs(argc, argv);
        return tallest_pillar_spawn_p2::runP2(c);
    } catch (const std::exception& e) {
        std::cerr << "Tallest1x1SpawnPillar P2 ERROR: " << e.what() << "\n";
        return 1;
    }
}
