$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
. (Join-Path $ProjectRoot 'scripts\cursed-spawn-origin-p1-common.ps1')

$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot

# Start from the already-proven cropped-Y P14 generator, then specialize it to
# one real Beta chunk: 5x17x5 density nodes, retaining nodes 7..15.
$generated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' 4
$build = Join-Path $ProjectRoot 'build\floating-island-spawn-p6-chunk-exact'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$corePath = Join-Path $generated 'coarse_exact_core.hpp'
$gpuPath = Join-Path $generated 'coarse_exact_gpu.hpp'
$configPath = Join-Path $generated 'skyblock_p14_config.hpp'
$core = [System.IO.File]::ReadAllText($corePath)
$core = [regex]::Replace($core, 'static constexpr int SIZE\s*=\s*\d+\s*;', 'static constexpr int SIZE = 5;', 1)
$core = [regex]::Replace($core, 'static constexpr int FROM_COARSE\s*=\s*-?\d+\s*;', 'static constexpr int FROM_COARSE = 0;', 1)
[System.IO.File]::WriteAllText($corePath, $core, [System.Text.UTF8Encoding]::new($false))

$gpu = [System.IO.File]::ReadAllText($gpuPath)
$oldCoordinates = @'
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
'@
$newCoordinates = @'
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    // Beta 1.7.3 func_4061_a samples a 16x16 climate array with
    // step = 16 / 5 = 3 and center offset = 3 / 2 = 1.
    // coarseOffset is in 4-block density-node units, so *4 is the chunk/block origin.
    climateX = static_cast<double>(coarseOffsetX * 4 + x * 3 + 1);
    climateZ = static_cast<double>(coarseOffsetZ * 4 + z * 3 + 1);
'@
if (-not $gpu.Contains($oldCoordinates)) { throw 'Could not find offset climate-coordinate block in generated GPU header.' }
$gpu = $gpu.Replace($oldCoordinates, $newCoordinates)
[System.IO.File]::WriteAllText($gpuPath, $gpu, [System.Text.UTF8Encoding]::new($false))

$config = [System.IO.File]::ReadAllText($configPath)
$config = $config.Replace('static constexpr int CHUNK_RADIUS = 4;', 'static constexpr int CHUNK_RADIUS = 0;')
[System.IO.File]::WriteAllText($configPath, $config, [System.Text.UTF8Encoding]::new($false))

$cpp = Join-Path $build 'FloatingIslandSpawnP6ChunkTerrainRegression.cpp'
$exe = Join-Path $build 'FloatingIslandSpawnP6ChunkTerrainRegression_AMD.exe'
$cppText = @'
#define main highest_pillar_spawn_p1_embedded_main
#include "HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace p6_chunk_regression {
using namespace highest_pillar_spawn_p1;

static void launchChunk(DeviceBuffers& b, int count, int threads, int chunkX, int chunkZ) {
    const int coarseOffsetX = chunkX * 4;
    const int coarseOffsetZ = chunkZ * 4;
    hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel,
        dim3(count), dim3(threads), 0, 0,
        b.seeds, count,
        b.temp, b.rain, b.climateBlend,
        b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
        b.signs,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        coarseOffsetX, coarseOffsetZ,
        nullptr, nullptr, nullptr, nullptr, nullptr);
    checkHip(hipGetLastError(), "launch P6 chunk-local terrain");
    checkHip(hipDeviceSynchronize(), "finish P6 chunk-local terrain");
}

static double node(const double* d, int x, int y, int z) {
    if (y < 0) return 10.0;
    if (y >= coarsecore::Y_LEVELS) return -10.0;
    return d[coarsecore::index3(x, y, z)];
}

static double originDensityAtBlock(const double* d, int worldY) {
    if (worldY < coarsecore::Y_BASE * 8) return 10.0;
    if (worldY >= 128) return -10.0;
    const int iy = (worldY >> 3) - coarsecore::Y_BASE;
    const double fy = static_cast<double>(worldY & 7) * 0.125;
    const double a = node(d, 0, iy, 0);
    const double b = node(d, 0, iy + 1, 0);
    return a + (b - a) * fy;
}

static bool solid(const double* d, int y) { return originDensityAtBlock(d, y) > 0.0; }

static std::string solidRuns(const double* d, int fromY, int toY) {
    std::ostringstream out;
    bool first = true;
    int y = fromY;
    while (y <= toY) {
        if (!solid(d, y)) { ++y; continue; }
        const int start = y;
        while (y + 1 <= toY && solid(d, y + 1)) ++y;
        if (!first) out << ',';
        first = false;
        if (start == y) out << start; else out << start << '-' << y;
        ++y;
    }
    return first ? std::string("none") : out.str();
}

static void runOne(DeviceBuffers& b, std::int64_t seed, bool expectY81Solid) {
    checkHip(hipMemcpy(b.seeds, &seed, sizeof(seed), hipMemcpyHostToDevice), "copy P6 regression seed");
    launchChunk(b, 1, 64, 0, 0);
    std::vector<double> density(static_cast<std::size_t>(coarsecore::CELLS));
    checkHip(hipMemcpy(density.data(), b.noise1, density.size() * sizeof(double), hipMemcpyDeviceToHost),
             "copy P6 chunk density");

    std::cout << "\nseed=" << seed << '\n';
    std::cout << "chunk_local_origin_solid_runs_y56_100=" << solidRuns(density.data(), 56, 100) << '\n';
    std::cout << std::scientific << std::setprecision(17);
    for (int y = 79; y <= 82; ++y) {
        const double v = originDensityAtBlock(density.data(), y);
        std::cout << "  y=" << y << " density=" << v << " solid=" << (v > 0.0 ? "YES" : "NO") << '\n';
    }
    const bool y81 = solid(density.data(), 81);
    if (y81 != expectY81Solid) {
        throw std::runtime_error("Y81 solidity did not match vanilla regression expectation");
    }
}

int run() {
    static_assert(coarsecore::SIZE == 5, "P6 chunk tile must be 5x5 density nodes");
    static_assert(coarsecore::FROM_COARSE == 0, "P6 chunk tile must start at local node 0");
    printDevice();
    DeviceBuffers b = allocateBuffers(1);
    try {
        // Ground-truth save: this seed has air at Y80 and Y81, stone at Y82.
        runOne(b, -3405360075020439777LL, false);
        // Known real pillar: Y65..73 remains a solid upper run.
        runOne(b, 6430576860599818994LL, true);
    } catch (...) {
        freeBuffers(b);
        throw;
    }
    freeBuffers(b);
    std::cout << "\nP6 CHUNK-LOCAL TERRAIN REGRESSION OK\n";
    return 0;
}
} // namespace p6_chunk_regression

int main() {
    try { return p6_chunk_regression::run(); }
    catch (const std::exception& e) {
        std::cerr << "P6 CHUNK TERRAIN ERROR: " << e.what() << '\n';
        return 1;
    }
}
'@
[System.IO.File]::WriteAllText($cpp, $cppText, [System.Text.UTF8Encoding]::new($false))

Write-Host "Compiling P6 chunk-local vanilla terrain regression for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$generated" "-I$nativeSourceDir" "-I$(Join-Path $ProjectRoot 'native\highest_pillar_spawn')" $cpp -o $exe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'P6 chunk-local terrain regression compilation failed.' }

Write-Host ''
Write-Host '=== P6 CHUNK-LOCAL VANILLA TERRAIN REGRESSION ==='
& $exe
if ($LASTEXITCODE -ne 0) { throw 'P6 chunk-local terrain regression failed.' }
