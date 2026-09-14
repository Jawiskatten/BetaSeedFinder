#define main lava_spawn_origin_embedded_main
#include "CursedSpawnOriginGpuFinder.cpp"
#undef main

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace ghost_floor_p1 {
using namespace lava_spawn_origin_p2;

struct LakeInfo {
    int waterAttempt;
    int attemptX, attemptY, attemptZ;
    int baseX, baseZ;
    int localX, localZ;
    int maskBits; // bits 1..6 are the exact WorldGenLakes mask at x=z=0
};

struct RngResult {
    std::int64_t seed;
    std::uint64_t sequenceIndex;
    int pass;
};

struct SandResult {
    std::int64_t seed;
    std::uint64_t sequenceIndex;
    int pass;
    int originY;
    int sandReason;
};

struct FinalResult {
    std::int64_t seed;
    std::uint64_t sequenceIndex;
    int qualified;
    int originY;
    int sandReason;
    int attemptX, attemptY, attemptZ;
    int lakeBaseCornerX, lakeBaseCornerZ;
    int lakeDescendedY;
    int lakeBaseY;
    int originLakeLocalY;
    int maskBits;
    int dryCarvePredicted;
};

__device__ __forceinline__ LakeInfo waterLakeInfo(std::int64_t seed) {
    LakeInfo li{};
    li.waterAttempt = 0;
    li.attemptX = li.attemptY = li.attemptZ = -999;
    li.baseX = li.baseZ = -999;
    li.localX = li.localZ = -1;
    li.maskBits = 0;

    // Population chunk (-1,-1) is the only water-lake population chunk whose
    // 16x16 lake carving box can include world X=0,Z=0.
    p20::JavaRandom wr;
    wr.setSeed(seed);
    const std::int64_t oddX = javaOddLong(javaNextLong(wr));
    const std::int64_t oddZ = javaOddLong(javaNextLong(wr));
    std::uint64_t popSeed = 0ULL - static_cast<std::uint64_t>(oddX);
    popSeed += 0ULL - static_cast<std::uint64_t>(oddZ);
    popSeed ^= static_cast<std::uint64_t>(seed);

    p20::JavaRandom pr;
    pr.setSeed(static_cast<std::int64_t>(popSeed));
    if (pr.nextInt(4) != 0) return li;
    li.waterAttempt = 1;

    li.attemptX = -16 + pr.nextInt(16) + 8;
    li.attemptY = pr.nextInt(128);
    li.attemptZ = -16 + pr.nextInt(16) + 8;

    // WorldGenLakes subtracts 8 from X/Z before descending and carving.
    li.baseX = li.attemptX - 8;
    li.baseZ = li.attemptZ - 8;
    li.localX = -li.baseX;
    li.localZ = -li.baseZ;
    if (li.localX < 1 || li.localX > 14 || li.localZ < 1 || li.localZ > 14) return li;

    const int ellipsoids = pr.nextInt(4) + 4;
    int bits = 0;
    for (int e = 0; e < ellipsoids; ++e) {
        const double sx = pr.nextDouble() * 6.0 + 3.0;
        const double sy = pr.nextDouble() * 4.0 + 2.0;
        const double sz = pr.nextDouble() * 6.0 + 3.0;
        const double cx = pr.nextDouble() * (16.0 - sx - 2.0) + 1.0 + sx / 2.0;
        const double cy = pr.nextDouble() * (8.0 - sy - 4.0) + 2.0 + sy / 2.0;
        const double cz = pr.nextDouble() * (16.0 - sz - 2.0) + 1.0 + sz / 2.0;
        const double dx = (static_cast<double>(li.localX) - cx) / (sx / 2.0);
        const double dz = (static_cast<double>(li.localZ) - cz) / (sz / 2.0);
        for (int y = 1; y < 7; ++y) {
            const double dy = (static_cast<double>(y) - cy) / (sy / 2.0);
            if (dx * dx + dy * dy + dz * dz < 1.0) bits |= (1 << y);
        }
    }
    li.maskBits = bits;
    return li;
}

__global__ void rngPrefilterKernel(const std::int64_t* seeds, int count,
                                   std::uint64_t baseIndex, RngResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    RngResult r{};
    r.seed = seeds[i];
    r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(i);
    r.pass = 0;
    const LakeInfo li = waterLakeInfo(r.seed);
    if (!li.waterAttempt) { out[i] = r; return; }
    // A surface ghost-floor needs the lake attempt high enough to descend onto
    // surface terrain, and the origin column must be in the air-carving half.
    if (li.attemptY < 56) { out[i] = r; return; }
    if ((li.maskBits & ((1 << 4) | (1 << 5) | (1 << 6))) == 0) { out[i] = r; return; }
    r.pass = 1;
    out[i] = r;
}

__global__ void sandGateKernel(const std::int64_t* seeds, const std::uint64_t* sequenceIndices,
                               const double* density,
                               const double* originSandNoise,
                               const double* originStoneNoise,
                               const double* originTemperature,
                               const double* originRainfall,
                               int count, SandResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    SandResult r{};
    r.seed = seeds[i];
    r.sequenceIndex = sequenceIndices[i];
    r.pass = 0;
    r.originY = -1;
    r.sandReason = 0;

    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;
    const int originY = exactSpawnCheckYAtOrigin(density, base, 0, 0);
    r.originY = originY;

    const bool desert = betaBiomeIsDesert(originTemperature[i], originRainfall[i]);
    p20::JavaRandom surfaceRandom;
    surfaceRandom.setSeed(0);
    const double sandJitter = surfaceRandom.nextDouble();
    (void)surfaceRandom.nextDouble();
    const double depthJitter = surfaceRandom.nextDouble();
    const bool beachSand = originSandNoise[i] + sandJitter * 0.2 > 0.0;
    const int depth = static_cast<int>(originStoneNoise[i] / 3.0 + 3.0 + depthJitter * 0.25);
    const int reason = desert ? 2 : ((originY >= 60 && originY <= 65 && beachSand) ? 1 : 0);
    r.sandReason = reason;
    if (originY >= 63 && depth > 0 && reason != 0) r.pass = 1;
    out[i] = r;
}

__device__ __forceinline__ bool chunkMinusOneTerrainSolid(const double* density, std::size_t base,
                                                           int worldX, int worldY, int worldZ) {
    if (worldY < coarsecore::Y_BASE * 8) return true;
    if (worldY >= 128) return false;
    const int localX = worldX + 16;
    const int localZ = worldZ + 16;
    if (localX < 0 || localX > 15 || localZ < 0 || localZ > 15) return false;
    const int ix = localX >> 2;
    const int iz = localZ >> 2;
    const double fx = static_cast<double>(localX & 3) * 0.25;
    const double fz = static_cast<double>(localZ & 3) * 0.25;
    const int iy = (worldY >> 3) - coarsecore::Y_BASE;
    const double fy = static_cast<double>(worldY & 7) * 0.125;
    const double d000 = nodeDensity(density, base, ix,     iy,     iz);
    const double d001 = nodeDensity(density, base, ix,     iy,     iz + 1);
    const double d100 = nodeDensity(density, base, ix + 1, iy,     iz);
    const double d101 = nodeDensity(density, base, ix + 1, iy,     iz + 1);
    const double d010 = nodeDensity(density, base, ix,     iy + 1, iz);
    const double d011 = nodeDensity(density, base, ix,     iy + 1, iz + 1);
    const double d110 = nodeDensity(density, base, ix + 1, iy + 1, iz);
    const double d111 = nodeDensity(density, base, ix + 1, iy + 1, iz + 1);
    const double a0 = d000 + (d100 - d000) * fx;
    const double a1 = d001 + (d101 - d001) * fx;
    const double b0 = d010 + (d110 - d010) * fx;
    const double b1 = d011 + (d111 - d011) * fx;
    const double low = a0 + (a1 - a0) * fz;
    const double high = b0 + (b1 - b0) * fz;
    return low + (high - low) * fy > 0.0;
}

__device__ __forceinline__ bool chunkMinusOneNonAir(const double* density, std::size_t base,
                                                     int x, int y, int z) {
    return y < 64 || chunkMinusOneTerrainSolid(density, base, x, y, z);
}

__global__ void verticalAlignmentKernel(const std::int64_t* seeds,
                                        const std::uint64_t* sequenceIndices,
                                        const int* originYs,
                                        const int* sandReasons,
                                        const double* density,
                                        int count, FinalResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    FinalResult r{};
    r.seed = seeds[i];
    r.sequenceIndex = sequenceIndices[i];
    r.qualified = 0;
    r.originY = originYs[i];
    r.sandReason = sandReasons[i];
    r.lakeDescendedY = -1;
    r.lakeBaseY = -1;
    r.originLakeLocalY = -1;

    const LakeInfo li = waterLakeInfo(r.seed);
    r.attemptX = li.attemptX; r.attemptY = li.attemptY; r.attemptZ = li.attemptZ;
    r.lakeBaseCornerX = li.baseX; r.lakeBaseCornerZ = li.baseZ;
    r.maskBits = li.maskBits;
    r.dryCarvePredicted = ((li.maskBits & ((1 << 1) | (1 << 2) | (1 << 3))) == 0) ? 1 : 0;

    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;
    int descended = li.attemptY;
    while (descended > 0 && !chunkMinusOneNonAir(density, base, li.baseX, descended, li.baseZ)) --descended;
    const int lakeBaseY = descended - 4;
    const int localY = r.originY - lakeBaseY;
    r.lakeDescendedY = descended;
    r.lakeBaseY = lakeBaseY;
    r.originLakeLocalY = localY;

    // This is the important prediction: using exact Beta RNG shape and the raw
    // terrain descent at the lake corner, the sand block that approved spawn is
    // itself in WorldGenLakes' upper (air-carving) half.
    if (localY >= 4 && localY <= 6 && (li.maskBits & (1 << localY)) != 0) {
        r.qualified = 1;
    }
    out[i] = r;
}

static void launchTerrainAt(DeviceBuffers& b, int count, int terrainThreads, int offX, int offZ) {
#if defined(SKYBLOCK_COARSE_API_MODERN)
    hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel,
        dim3(count), dim3(terrainThreads), 0, 0,
        b.seeds, count,
        b.temp, b.rain, b.climateBlend,
        b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
        b.signs,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        offX, offZ,
        nullptr, nullptr, nullptr, nullptr, nullptr);
#else
#error "GhostFloor P1 requires SKYBLOCK_COARSE_API_MODERN"
#endif
    checkHip(hipGetLastError(), "launch exact Beta terrain generation");
}

static void writeHeader(std::ofstream& f) {
    f << "seed,sequence_index,prepop_sand_y,sand_reason,water_attempt_x,water_attempt_y,water_attempt_z,"
         "lake_base_corner_x,lake_base_corner_z,lake_descended_y,lake_base_y,origin_lake_local_y,mask_bits,dry_carve_predicted\n";
}

static int runScout(int argc, char** argv) {
    Config c = parseArgs(argc, argv);
    if (c.outputDir.empty()) throw std::invalid_argument("--output is required");
    std::filesystem::create_directories(c.outputDir);
    printDevice();

    DeviceBuffers b = allocateBuffers(c.batch);
    RngResult* dRng = nullptr;
    SandResult* dSand = nullptr;
    FinalResult* dFinal = nullptr;
    std::uint64_t* dSeq = nullptr;
    int* dOriginY = nullptr;
    int* dSandReason = nullptr;
    allocateArray(dRng, static_cast<std::size_t>(c.batch), "allocate ghost RNG results");
    allocateArray(dSand, static_cast<std::size_t>(c.batch), "allocate ghost sand results");
    allocateArray(dFinal, static_cast<std::size_t>(c.batch), "allocate ghost final results");
    allocateArray(dSeq, static_cast<std::size_t>(c.batch), "allocate ghost sequence indices");
    allocateArray(dOriginY, static_cast<std::size_t>(c.batch), "allocate origin Y metadata");
    allocateArray(dSandReason, static_cast<std::size_t>(c.batch), "allocate sand reason metadata");

    std::vector<RngResult> rngHost(static_cast<std::size_t>(c.batch));
    std::vector<SandResult> sandHost(static_cast<std::size_t>(c.batch));
    std::vector<FinalResult> finalHost(static_cast<std::size_t>(c.batch));
    std::vector<std::int64_t> packedSeeds(static_cast<std::size_t>(c.batch));
    std::vector<std::uint64_t> packedSeq(static_cast<std::size_t>(c.batch));
    std::vector<int> packedY(static_cast<std::size_t>(c.batch));
    std::vector<int> packedReason(static_cast<std::size_t>(c.batch));

    const std::filesystem::path csvPath = c.outputDir / ("candidates_" + std::to_string(c.startIndex) + ".csv");
    std::ofstream csv(csvPath, std::ios::trunc);
    if (!csv) throw std::runtime_error("cannot write " + csvPath.string());
    writeHeader(csv);

    std::uint64_t checked = 0, rngSurvivors = 0, sandSurvivors = 0, hits = 0;
    const auto start = std::chrono::steady_clock::now();
    auto lastPrint = start;

    try {
        while (checked < c.count) {
            const int n = static_cast<int>(std::min<std::uint64_t>(static_cast<std::uint64_t>(c.batch), c.count - checked));
            const std::uint64_t sequenceBase = c.startIndex + checked;
            if (c.singleSeedSet) {
                checkHip(hipMemcpy(b.seeds, &c.singleSeed, sizeof(c.singleSeed), hipMemcpyHostToDevice), "copy single seed");
            } else {
                const int threads = 256, blocks = (n + threads - 1) / threads;
                hipLaunchKernelGGL(generateRandomSeedsKernel, dim3(blocks), dim3(threads), 0, 0,
                    b.seeds, n, c.randomKey, sequenceBase, static_cast<int>(c.seedMode));
                checkHip(hipGetLastError(), "generate ghost-floor seeds");
            }

            const int threads = 256;
            int blocks = (n + threads - 1) / threads;
            hipLaunchKernelGGL(rngPrefilterKernel, dim3(blocks), dim3(threads), 0, 0,
                b.seeds, n, sequenceBase, dRng);
            checkHip(hipGetLastError(), "launch ghost RNG prefilter");
            checkHip(hipDeviceSynchronize(), "finish ghost RNG prefilter");
            checkHip(hipMemcpy(rngHost.data(), dRng, static_cast<std::size_t>(n) * sizeof(RngResult), hipMemcpyDeviceToHost),
                "copy ghost RNG prefilter");

            int m = 0;
            for (int j = 0; j < n; ++j) if (rngHost[static_cast<std::size_t>(j)].pass) {
                packedSeeds[static_cast<std::size_t>(m)] = rngHost[static_cast<std::size_t>(j)].seed;
                packedSeq[static_cast<std::size_t>(m)] = rngHost[static_cast<std::size_t>(j)].sequenceIndex;
                ++m;
            }
            rngSurvivors += static_cast<std::uint64_t>(m);

            int s = 0;
            if (m > 0) {
                checkHip(hipMemcpy(b.seeds, packedSeeds.data(), static_cast<std::size_t>(m) * sizeof(std::int64_t), hipMemcpyHostToDevice), "upload RNG survivors");
                checkHip(hipMemcpy(dSeq, packedSeq.data(), static_cast<std::size_t>(m) * sizeof(std::uint64_t), hipMemcpyHostToDevice), "upload RNG survivor sequences");
                launchTerrainAt(b, m, c.terrainThreads, 0, 0);
                blocks = (m + threads - 1) / threads;
                hipLaunchKernelGGL(sandGateKernel, dim3(blocks), dim3(threads), 0, 0,
                    b.seeds, dSeq, b.noise1, b.originSandNoise, b.originStoneNoise,
                    b.originTemperature, b.originRainfall, m, dSand);
                checkHip(hipGetLastError(), "launch ghost sand gate");
                checkHip(hipDeviceSynchronize(), "finish ghost sand gate");
                checkHip(hipMemcpy(sandHost.data(), dSand, static_cast<std::size_t>(m) * sizeof(SandResult), hipMemcpyDeviceToHost), "copy ghost sand results");
                for (int j = 0; j < m; ++j) if (sandHost[static_cast<std::size_t>(j)].pass) {
                    const auto& x = sandHost[static_cast<std::size_t>(j)];
                    packedSeeds[static_cast<std::size_t>(s)] = x.seed;
                    packedSeq[static_cast<std::size_t>(s)] = x.sequenceIndex;
                    packedY[static_cast<std::size_t>(s)] = x.originY;
                    packedReason[static_cast<std::size_t>(s)] = x.sandReason;
                    ++s;
                }
            }
            sandSurvivors += static_cast<std::uint64_t>(s);

            if (s > 0) {
                checkHip(hipMemcpy(b.seeds, packedSeeds.data(), static_cast<std::size_t>(s) * sizeof(std::int64_t), hipMemcpyHostToDevice), "upload sand survivors");
                checkHip(hipMemcpy(dSeq, packedSeq.data(), static_cast<std::size_t>(s) * sizeof(std::uint64_t), hipMemcpyHostToDevice), "upload sand sequences");
                checkHip(hipMemcpy(dOriginY, packedY.data(), static_cast<std::size_t>(s) * sizeof(int), hipMemcpyHostToDevice), "upload origin Ys");
                checkHip(hipMemcpy(dSandReason, packedReason.data(), static_cast<std::size_t>(s) * sizeof(int), hipMemcpyHostToDevice), "upload sand reasons");
                // Chunk (-1,-1) coarse lattice coordinates are -4..0.
                launchTerrainAt(b, s, c.terrainThreads, -4, -4);
                blocks = (s + threads - 1) / threads;
                hipLaunchKernelGGL(verticalAlignmentKernel, dim3(blocks), dim3(threads), 0, 0,
                    b.seeds, dSeq, dOriginY, dSandReason, b.noise1, s, dFinal);
                checkHip(hipGetLastError(), "launch ghost vertical alignment");
                checkHip(hipDeviceSynchronize(), "finish ghost vertical alignment");
                checkHip(hipMemcpy(finalHost.data(), dFinal, static_cast<std::size_t>(s) * sizeof(FinalResult), hipMemcpyDeviceToHost), "copy ghost final results");
                for (int j = 0; j < s; ++j) {
                    const auto& r = finalHost[static_cast<std::size_t>(j)];
                    if (!r.qualified) continue;
                    ++hits;
                    csv << r.seed << ',' << r.sequenceIndex << ',' << r.originY << ',' << r.sandReason << ','
                        << r.attemptX << ',' << r.attemptY << ',' << r.attemptZ << ','
                        << r.lakeBaseCornerX << ',' << r.lakeBaseCornerZ << ',' << r.lakeDescendedY << ','
                        << r.lakeBaseY << ',' << r.originLakeLocalY << ',' << r.maskBits << ',' << r.dryCarvePredicted << '\n';
                }
            }

            checked += static_cast<std::uint64_t>(n);
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() >= c.progressMs || checked == c.count) {
                csv.flush();
                const double sec = std::chrono::duration<double>(now - start).count();
                std::cout << "progress checked=" << checked << '/' << c.count
                          << " rate=" << static_cast<std::uint64_t>(checked / std::max(0.001, sec))
                          << " rng=" << rngSurvivors << " sand=" << sandSurvivors << " candidates=" << hits << "\n";
                lastPrint = now;
            }
            if (c.singleSeedSet) break;
        }
    } catch (...) {
        if (dRng) (void)hipFree(dRng); if (dSand) (void)hipFree(dSand); if (dFinal) (void)hipFree(dFinal);
        if (dSeq) (void)hipFree(dSeq); if (dOriginY) (void)hipFree(dOriginY); if (dSandReason) (void)hipFree(dSandReason);
        freeBuffers(b); throw;
    }

    if (dRng) checkHip(hipFree(dRng), "free ghost RNG");
    if (dSand) checkHip(hipFree(dSand), "free ghost sand");
    if (dFinal) checkHip(hipFree(dFinal), "free ghost final");
    if (dSeq) checkHip(hipFree(dSeq), "free ghost seq");
    if (dOriginY) checkHip(hipFree(dOriginY), "free ghost origin Y");
    if (dSandReason) checkHip(hipFree(dSandReason), "free ghost sand reason");
    freeBuffers(b);
    std::cout << "DONE checked=" << checked << " rng=" << rngSurvivors << " sand=" << sandSurvivors
              << " candidates=" << hits << " file=" << csvPath.string() << "\n";
    std::cout << "P1 searches water-lake attempts from population chunk (-1,-1) whose exact RNG shape is predicted to carve the origin spawn-gate sand. Actual client startup generation is verified in Java.\n";
    return 0;
}
} // namespace ghost_floor_p1

int main(int argc, char** argv) {
    try { return ghost_floor_p1::runScout(argc, argv); }
    catch (const std::exception& e) { std::cerr << "ERROR: " << e.what() << '\n'; return 1; }
}
