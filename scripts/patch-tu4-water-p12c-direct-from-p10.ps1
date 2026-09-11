param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\tu4_water_finder.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "TU4 water source not found: $sourcePath"
}

$text = [System.IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")

if ($text.Contains('TU4_WATER_P12_DIRECT_SCREEN')) {
    Write-Host 'TU4 Water P12 direct screen is already applied.' -ForegroundColor Green
    exit 0
}
if ($text.Contains('TU4_WATER_P12_EXACT_DENSITY_SCREEN')) {
    Write-Host 'TU4 Water P12 exact-density screen is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P10_RESTORE_PARTIAL_SORT')) {
    throw 'P12c requires the local source to be at P10.'
}
if (-not $text.Contains('TU4_WATER_P8_BATCHED_EXACT')) {
    throw 'P12c expects the P8 batched exact evaluator underneath P10.'
}
if ($text.Contains('TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND')) {
    throw 'P12c is the direct-from-P10 path. P11 is already present; use the normal P12 patch instead.'
}

$backupPath = $sourcePath + '.p10-before-water-p12c.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P11 did not materially improve throughput and its patcher is brittle against
# this local P10 source. P12c skips P11 entirely and applies the important
# architectural change directly on top of the proven P10/P8 path:
#   24 P6 diverse finalists -> 16x16 full-density screen -> 8 full exacts.
# The final 800x800 metric is unchanged. --verify-seed still full-exacts n=1.

$markerPos = $text.IndexOf('// TU4_WATER_P10_RESTORE_PARTIAL_SORT')
if ($markerPos -lt 0) { throw 'Could not locate P10 marker.' }
$marker = @'
// TU4_WATER_P12_DIRECT_SCREEN
// Direct P10 -> P12 path: sparse 16x16 full-density screen of all P6 finalists,
// then full exact only the strongest third plus two portfolio safety slots.
// Authoritative final 800x800 land metric remains unchanged.
'@
$text = $text.Insert($markerPos, $marker + "`n")

# Add screen constants once.
$exactThreads = 'static constexpr int EXACT_THREADS = 256;'
$pos = $text.IndexOf($exactThreads)
if ($pos -lt 0) { throw 'Could not locate EXACT_THREADS constant.' }
$insert = @'
static constexpr int EXACT_THREADS = 256;
static constexpr int P12_SCREEN_SIDE = 16;
static constexpr int P12_SCREEN_POINTS = P12_SCREEN_SIDE * P12_SCREEN_SIDE;
'@.TrimEnd()
$text = $text.Remove($pos, $exactThreads.Length).Insert($pos, $insert)

# Generalize the P8/P10 exact-density kernel structurally. Do not touch any of
# the exact terrain math below the coordinate prologue.
$densityStart = $text.IndexOf('__global__ void exactSeaDensityKernel(')
if ($densityStart -lt 0) { throw 'Could not locate exactSeaDensityKernel.' }
$densityBrace = $text.IndexOf('{', $densityStart)
if ($densityBrace -lt 0) { throw 'Could not locate exactSeaDensityKernel body.' }
$densitySignature = $text.Substring($densityStart, $densityBrace - $densityStart)
if (-not $densitySignature.Contains('candidateCount')) {
    throw 'Expected P8/P10 batched exactSeaDensityKernel with candidateCount.'
}
if ($densitySignature.Contains('pointSide')) {
    throw 'exactSeaDensityKernel already appears screen-generalized.'
}
$needle = 'int candidateCount'
if (-not $densitySignature.Contains($needle)) {
    throw 'Could not locate candidateCount parameter in exactSeaDensityKernel.'
}
$newDensitySignature = $densitySignature.Replace(
    $needle,
    "int candidateCount,`n        int pointSide,`n        bool sparseGrid"
)
$text = $text.Remove($densityStart, $densityBrace - $densityStart).Insert($densityStart, $newDensitySignature)

$densityStart = $text.IndexOf('__global__ void exactSeaDensityKernel(')
$prologueStartNeedle = '    const int globalIdx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);'
$prologueStart = $text.IndexOf($prologueStartNeedle, $densityStart)
if ($prologueStart -lt 0) { throw 'Could not locate P8 exact-density prologue.' }
$coarseXNeedle = '    const double coarseX ='
$coarseXPos = $text.IndexOf($coarseXNeedle, $prologueStart)
if ($coarseXPos -lt 0) { throw 'Could not locate exact-density coarseX line.' }
$newPrologue = @'
    const int globalIdx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int pointsPerCandidate = pointSide * pointSide;
    const int totalPoints = candidateCount * pointsPerCandidate;
    if (globalIdx >= totalPoints) return;

    const int candidate = globalIdx / pointsPerCandidate;
    const int idx = globalIdx - candidate * pointsPerCandidate;
    terrain += static_cast<std::size_t>(candidate) * TERRAIN_STATE_COUNT;
    tempStates += static_cast<std::size_t>(candidate) * 4;
    rainStates += static_cast<std::size_t>(candidate) * 4;
    blendStates += static_cast<std::size_t>(candidate) * 2;
    seaDensity += static_cast<std::size_t>(candidate) * pointsPerCandidate;

    const int sampleX = idx / pointSide;
    const int sampleZ = idx - sampleX * pointSide;
    const int ix = sparseGrid
            ? ((2 * sampleX + 1) * COARSE_CELLS) / (2 * pointSide)
            : sampleX;
    const int iz = sparseGrid
            ? ((2 * sampleZ + 1) * COARSE_CELLS) / (2 * pointSide)
            : sampleZ;

'@
$text = $text.Remove($prologueStart, $coarseXPos - $prologueStart).Insert($prologueStart, $newPrologue)

# Add persistent host readback storage for the sparse screen.
$hLand = '    std::vector<int> hLand;'
$pos = $text.IndexOf($hLand)
if ($pos -lt 0) { throw 'Could not locate ExactWorkspace hLand.' }
$text = $text.Insert($pos + $hLand.Length, "`n    std::vector<double> hScreenDensity;")

$resizeLand = '        hLand.resize(n);'
$pos = $text.IndexOf($resizeLand)
if ($pos -lt 0) { throw 'Could not locate ExactWorkspace hLand resize.' }
$text = $text.Insert($pos + $resizeLand.Length,
    "`n        hScreenDensity.resize(n * P12_SCREEN_POINTS);")

# Replace P8/P10 runExactBatch as one unit. countLandKernel itself remains the
# proven P10 flattened batched kernel; only the number of full candidates drops.
$runStart = $text.IndexOf('std::vector<ExactWaterResult> runExactBatch(')
if ($runStart -lt 0) { throw 'Could not locate runExactBatch.' }
$runEnd = $text.IndexOf("`nExactWaterResult runExact(", $runStart)
if ($runEnd -lt 0) { throw 'Could not locate end of runExactBatch.' }

$newRun = @'
std::vector<ExactWaterResult> runExactBatch(
        const std::vector<std::int64_t>& seeds,
        ExactWorkspace& w
) {
    const int n = static_cast<int>(seeds.size());
    if (n == 0) return {};
    if (n > w.capacity) {
        throw std::runtime_error("Exact batch exceeds preallocated workspace capacity.");
    }

    std::vector<p20::PerlinState> terrainOne;
    std::vector<p20::PerlinState> tempOne;
    std::vector<p20::PerlinState> rainOne;
    std::vector<p20::PerlinState> blendOne;
    terrainOne.reserve(TERRAIN_STATE_COUNT);
    tempOne.reserve(4);
    rainOne.reserve(4);
    blendOne.reserve(2);

    auto buildIntoSlot = [&](std::int64_t seed, int slot) {
        buildExactStates(seed, terrainOne, tempOne, rainOne, blendOne);
        std::copy(terrainOne.begin(), terrainOne.end(),
                  w.hTerrain.begin() + static_cast<std::size_t>(slot) * TERRAIN_STATE_COUNT);
        std::copy(tempOne.begin(), tempOne.end(),
                  w.hTemp.begin() + static_cast<std::size_t>(slot) * 4);
        std::copy(rainOne.begin(), rainOne.end(),
                  w.hRain.begin() + static_cast<std::size_t>(slot) * 4);
        std::copy(blendOne.begin(), blendOne.end(),
                  w.hBlend.begin() + static_cast<std::size_t>(slot) * 2);
    };

    auto uploadStates = [&](int count) {
        const std::size_t nn = static_cast<std::size_t>(count);
        HIP_CHECK(hipMemcpy(w.dTerrain, w.hTerrain.data(),
                            nn * TERRAIN_STATE_COUNT * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(w.dTemp, w.hTemp.data(),
                            nn * 4 * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(w.dRain, w.hRain.data(),
                            nn * 4 * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(w.dBlend, w.hBlend.data(),
                            nn * 2 * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    };

    // Stage 2: all pre-finalists receive a sparse sample using the SAME exact
    // climate + noise1/2/3/4/5 density math as the full evaluator.
    for (int i = 0; i < n; ++i) buildIntoSlot(seeds[static_cast<std::size_t>(i)], i);
    uploadStates(n);

    const int screenPointCount = n * P12_SCREEN_POINTS;
    const int screenBlocks = (screenPointCount + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            exactSeaDensityKernel,
            dim3(screenBlocks), dim3(EXACT_THREADS), 0, 0,
            w.dTerrain, w.dTemp, w.dRain, w.dBlend, w.dDensity,
            n, P12_SCREEN_SIDE, true);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(w.hScreenDensity.data(), w.dDensity,
                        static_cast<std::size_t>(screenPointCount) * sizeof(double),
                        hipMemcpyDeviceToHost));

    struct ScreenRank {
        int source = 0;
        int landish = 0;
        double positiveMass = 0.0;
    };
    std::vector<ScreenRank> screen(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        ScreenRank r;
        r.source = i;
        const double* values = w.hScreenDensity.data()
                + static_cast<std::size_t>(i) * P12_SCREEN_POINTS;
        for (int p = 0; p < P12_SCREEN_POINTS; ++p) {
            const double v = values[p];
            if (v > 0.0) {
                ++r.landish;
                r.positiveMass += v;
            }
        }
        screen[static_cast<std::size_t>(i)] = r;
    }
    std::sort(screen.begin(), screen.end(), [](const ScreenRank& a, const ScreenRank& b) {
        if (a.landish != b.landish) return a.landish < b.landish;
        if (a.positiveMass != b.positiveMass) return a.positiveMass < b.positiveMass;
        return a.source < b.source;
    });

    // At TopExact=24 this sends 8 candidates to the full 201x201 exact grid.
    // Six slots follow the strong full-density screen; two are P6 safety slots.
    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 2) / 3));
    std::vector<int> chosen;
    chosen.reserve(static_cast<std::size_t>(fullN));
    auto addUnique = [&](int source) {
        if (source < 0 || source >= n || static_cast<int>(chosen.size()) >= fullN) return;
        if (std::find(chosen.begin(), chosen.end(), source) == chosen.end()) chosen.push_back(source);
    };

    const int screenTarget = std::max(0, fullN - 2);
    for (int i = 0; i < n && static_cast<int>(chosen.size()) < screenTarget; ++i) {
        addUnique(screen[static_cast<std::size_t>(i)].source);
    }
    addUnique(0);
    const int diverseStart = std::min(n - 1, (n * 2 + 2) / 3);
    addUnique(diverseStart);
    for (int i = 0; i < n && static_cast<int>(chosen.size()) < fullN; ++i) {
        addUnique(screen[static_cast<std::size_t>(i)].source);
    }

    // Compact chosen candidates into front slots and run the unchanged full exact.
    for (int j = 0; j < fullN; ++j) {
        buildIntoSlot(seeds[static_cast<std::size_t>(chosen[static_cast<std::size_t>(j)])], j);
    }
    uploadStates(fullN);

    const int totalPoints = fullN * COARSE_POINT_COUNT;
    const int pointBlocks = (totalPoints + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            exactSeaDensityKernel,
            dim3(pointBlocks), dim3(EXACT_THREADS), 0, 0,
            w.dTerrain, w.dTemp, w.dRain, w.dBlend, w.dDensity,
            fullN, COARSE_POINTS, false);
    HIP_CHECK(hipGetLastError());

    const std::size_t fullNN = static_cast<std::size_t>(fullN);
    HIP_CHECK(hipMemset(w.dLand, 0, fullNN * sizeof(int)));
    const int totalCells = fullN * COARSE_CELL_COUNT;
    const int cellBlocks = (totalCells + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            countLandKernel,
            dim3(cellBlocks), dim3(EXACT_THREADS), 0, 0,
            w.dDensity, w.dLand, fullN);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    HIP_CHECK(hipMemcpy(w.hLand.data(), w.dLand,
                        fullNN * sizeof(int), hipMemcpyDeviceToHost));

    std::vector<ExactWaterResult> results(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        results[static_cast<std::size_t>(i)].landColumns = SEARCH_COLUMNS + 1;
        results[static_cast<std::size_t>(i)].waterColumns = 0;
    }
    for (int j = 0; j < fullN; ++j) {
        const int source = chosen[static_cast<std::size_t>(j)];
        const int land = w.hLand[static_cast<std::size_t>(j)];
        results[static_cast<std::size_t>(source)].landColumns = land;
        results[static_cast<std::size_t>(source)].waterColumns = TOTAL_COLUMNS - land;
    }
    return results;
}
'@
$text = $text.Remove($runStart, $runEnd - $runStart).Insert($runStart, $newRun)

# No stale old-signature exact-density launch should remain.
$oldLaunchNeedle = 'w.dTerrain, w.dTemp, w.dRain, w.dBlend, w.dDensity, n);'
if ($text.Contains($oldLaunchNeedle)) {
    throw 'Found an old exactSeaDensityKernel launch signature after P12c rewrite.'
}

$text = $text.Replace(
    'Scout P10: P8 batched exact + restored fast partial_sort pools; P6 portfolio and exact metric unchanged.',
    'Scout P12c: P10/P8 + 16x16 full-density screen; 8/24 full exact; final metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P12c direct exact-density screen.' -ForegroundColor Green
Write-Host 'Skipped P11 entirely; P10/P8 exact math and flattened land kernel remain.'
Write-Host '24 diverse P6 pre-finalists -> 16x16 full-density screen -> 8 full exacts.'
Write-Host '--verify-seed still full-exacts n=1; authoritative 800x800 metric is unchanged.'
Write-Host 'VERIFY 96617 land before benchmarking.'
