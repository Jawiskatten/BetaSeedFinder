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

if ($text.Contains('TU4_WATER_P12_EXACT_DENSITY_SCREEN')) {
    Write-Host 'TU4 Water P12 exact-density screen is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND')) {
    throw 'P12 requires TU4 Water P11 first.'
}
if (-not $text.Contains('TU4_WATER_P8_BATCHED_EXACT')) {
    throw 'P12 expects the P8 batched exact evaluator underneath P11.'
}

$backupPath = $sourcePath + '.p11-before-water-p12.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P11 showed that micro-optimizing the full exact kernels does not materially
# move whole-search throughput: the run stayed around ~2.70M seeds/s. P12 makes
# an architectural change instead. The P6/P10 scout still contributes 24
# diverse finalists per 131072-seed batch, but all 24 first receive a sparse
# 16x16 FULL-density screen using the exact climate + noise1/2/3/4/5 equations.
# Only one third (8 at TopExact=24) proceed to the expensive 201x201 full exact
# density grid. Six are chosen by the strong sparse exact-density score, with
# two safety/diversity slots preserving one primary and one diverse P6 candidate.
#
# This screen evaluates 256 exact-density points/candidate versus 40401 for a
# full candidate, so its cost is tiny compared with the 16 full exact evaluations
# it eliminates. Any candidate that passes the screen is still measured with the
# unchanged exact 800x800 metric. --verify-seed always has n=1 and therefore
# bypasses filtering in practice: the requested seed is fully exacted.

$markerPos = $text.IndexOf('// TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND')
if ($markerPos -lt 0) { throw 'Could not locate P11 marker.' }
$marker = @'
// TU4_WATER_P12_EXACT_DENSITY_SCREEN
// Stage 2: sparse 16x16 full-density screen of all P6 finalists, then full exact
// only the strongest third plus two portfolio safety slots. Final metric unchanged.
'@
$text = $text.Insert($markerPos, $marker + "`n")

# Add screen constants near the exact thread constant.
$exactThreads = 'static constexpr int EXACT_THREADS = 256;'
$pos = $text.IndexOf($exactThreads)
if ($pos -lt 0) { throw 'Could not locate EXACT_THREADS constant.' }
$insert = @'
static constexpr int EXACT_THREADS = 256;
static constexpr int P12_SCREEN_SIDE = 16;
static constexpr int P12_SCREEN_POINTS = P12_SCREEN_SIDE * P12_SCREEN_SIDE;
'@.TrimEnd()
$text = $text.Remove($pos, $exactThreads.Length).Insert($pos, $insert)

# Generalize the existing exact-density kernel so the exact same math can run on
# either the full 201x201 lattice or an evenly-spaced 16x16 sparse lattice.
$oldSigTail = @'
        double* seaDensity,
        int candidateCount
) {
    const int globalIdx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int totalPoints = candidateCount * COARSE_POINT_COUNT;
    if (globalIdx >= totalPoints) return;

    const int candidate = globalIdx / COARSE_POINT_COUNT;
    const int idx = globalIdx - candidate * COARSE_POINT_COUNT;
    terrain += static_cast<std::size_t>(candidate) * TERRAIN_STATE_COUNT;
    tempStates += static_cast<std::size_t>(candidate) * 4;
    rainStates += static_cast<std::size_t>(candidate) * 4;
    blendStates += static_cast<std::size_t>(candidate) * 2;
    seaDensity += static_cast<std::size_t>(candidate) * COARSE_POINT_COUNT;

    const int ix = idx / COARSE_POINTS;
    const int iz = idx - ix * COARSE_POINTS;
'@.TrimEnd()
$newSigTail = @'
        double* seaDensity,
        int candidateCount,
        int pointSide,
        bool sparseGrid
) {
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
'@.TrimEnd()
$count = ([regex]::Matches($text, [regex]::Escape($oldSigTail))).Count
if ($count -ne 1) {
    throw "Expected exactly one P8/P11 exact-density kernel head, found $count."
}
$text = $text.Replace($oldSigTail, $newSigTail)

# Give ExactWorkspace persistent host space for the sparse screen readback.
$hLand = '    std::vector<int> hLand;'
$pos = $text.IndexOf($hLand)
if ($pos -lt 0) { throw 'Could not locate ExactWorkspace hLand.' }
$text = $text.Insert($pos + $hLand.Length, "`n    std::vector<double> hScreenDensity;")

$resizeLand = '        hLand.resize(n);'
$pos = $text.IndexOf($resizeLand)
if ($pos -lt 0) { throw 'Could not locate ExactWorkspace hLand resize.' }
$text = $text.Insert($pos + $resizeLand.Length,
    "`n        hScreenDensity.resize(n * P12_SCREEN_POINTS);")

# Replace runExactBatch as one unit. The exact state constructor and P11 kernels
# remain authoritative; this function only adds sparse screening and compacts the
# selected candidates before the unchanged full exact pass.
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

    // Stage 2A: build/upload all P6 finalists, then measure 16x16 evenly-spaced
    // sea-density samples using the full exact terrain equations.
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

    // Full-exact one third of the pre-finalists (8/24). Most slots follow the
    // much stronger full-density screen. Two safety slots retain one original
    // primary P6 leader and one candidate from the diverse part of the portfolio.
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
    addUnique(0); // best original primary P6 finalist
    const int diverseStart = std::min(n - 1, (n * 2 + 2) / 3);
    addUnique(diverseStart);
    for (int i = 0; i < n && static_cast<int>(chosen.size()) < fullN; ++i) {
        addUnique(screen[static_cast<std::size_t>(i)].source);
    }

    // Compact/rebuild only selected exact seeds into the front of the workspace.
    // Rebuilding <=8 state sets is cheaper and simpler than carrying an indirection
    // through every full-grid density thread.
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
    const int cellBlocksPerCandidate =
            (COARSE_CELL_COUNT + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            countLandKernel,
            dim3(static_cast<unsigned int>(fullN), static_cast<unsigned int>(cellBlocksPerCandidate)),
            dim3(EXACT_THREADS), 0, 0,
            w.dDensity, w.dLand, fullN);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    HIP_CHECK(hipMemcpy(w.hLand.data(), w.dLand,
                        fullNN * sizeof(int), hipMemcpyDeviceToHost));

    // Unscreened finalists receive a sentinel that cannot become a record. Every
    // result that can update bestLand is still from the unchanged full exact metric.
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

# Update any remaining exact-density launch in the one-seed/full path is already
# inside the new runExactBatch. There should be no old P8/P11 launch signature left.
$oldLaunchNeedle = 'w.dTerrain, w.dTemp, w.dRain, w.dBlend, w.dDensity, n);'
if ($text.Contains($oldLaunchNeedle)) {
    throw 'Found an old exactSeaDensityKernel launch signature after P12 rewrite.'
}

$text = $text.Replace(
    'Scout P11: paired Perlin scout/exact + reduced-atomic land count + P8 batching; exact metric unchanged.',
    'Scout P12: P11 + 16x16 full-density screen; full-exact strongest third + safety slots; final metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P12 exact-density finalist screen.' -ForegroundColor Green
Write-Host 'P6/P10 still supplies 24 diverse pre-finalists per batch.'
Write-Host 'All 24 get a 16x16 FULL exact-density screen (256 points each).'
Write-Host 'Only 8/24 run the expensive 201x201 full exact grid: 6 screen leaders + 2 safety/diversity slots.'
Write-Host '--verify-seed still full-exacts the requested seed because n=1.'
Write-Host 'This changes finalist routing, not the authoritative final 800x800 land metric.'
Write-Host 'P11 throughput was ~2.70M/s; P12 is designed for a materially larger jump.'
