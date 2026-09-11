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

$text = [System.IO.File]::ReadAllText($sourcePath)
$text = $text.Replace("`r`n", "`n")

if ($text.Contains('TU4_WATER_P8_BATCHED_EXACT')) {
    Write-Host 'TU4 Water P8 batched exact evaluator is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P7_800_INTERIOR')) {
    throw 'P8 requires TU4 Water P7 800x800 interior first.'
}
if (-not $text.Contains('TU4_WATER_P6_DIVERSE_FINALISTS')) {
    throw 'P8 expects the P6 diverse-finalist selection used by P7.'
}

$backupPath = $sourcePath + '.p7-before-water-p8.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P8 does not change the scout, finalist portfolio, 800x800 metric, or exact
# terrain math. It only evaluates all selected finalists in one GPU batch.
# P7/P6 previously did 24 separate state uploads, density launches, land-count
# launches, synchronizations, and result copies per scout batch. P8 turns those
# into one upload set + two launches + one synchronization/copy for all finalists.

$markerPos = $text.IndexOf('// TU4_WATER_P7_800_INTERIOR')
if ($markerPos -lt 0) {
    throw 'Could not locate P7 marker.'
}
$marker = @'
// TU4_WATER_P8_BATCHED_EXACT
// Exact finalists are evaluated together to amortize transfers, launches, and
// synchronization. Per-seed terrain math and final land counts are unchanged.
'@
$text = $text.Insert($markerPos, $marker + "`n")

# -------------------------------------------------------------------------
# 1) Batch the density kernel over candidateCount independent exact seeds.
# -------------------------------------------------------------------------
$oldDensityHead = @'
__global__ void exactSeaDensityKernel(
        const p20::PerlinState* terrain,
        const p20::PerlinState* tempStates,
        const p20::PerlinState* rainStates,
        const p20::PerlinState* blendStates,
        double* seaDensity
) {
    const int idx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (idx >= COARSE_POINT_COUNT) return;

    const int ix = idx / COARSE_POINTS;
    const int iz = idx - ix * COARSE_POINTS;
'@
$newDensityHead = @'
__global__ void exactSeaDensityKernel(
        const p20::PerlinState* terrain,
        const p20::PerlinState* tempStates,
        const p20::PerlinState* rainStates,
        const p20::PerlinState* blendStates,
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
'@
$count = ([regex]::Matches($text, [regex]::Escape($oldDensityHead.TrimEnd()))).Count
if ($count -ne 1) {
    throw "Expected exactly one exactSeaDensityKernel header, found $count."
}
$text = $text.Replace($oldDensityHead.TrimEnd(), $newDensityHead.TrimEnd())

# -------------------------------------------------------------------------
# 2) Batch the land-count kernel. Each candidate owns one atomic accumulator.
# -------------------------------------------------------------------------
$oldCountHead = @'
__global__ void countLandKernel(
        const double* seaDensity,
        int* landCount
) {
    const int cell = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (cell >= COARSE_CELL_COUNT) return;

    const int cx = cell / COARSE_CELLS;
'@
$newCountHead = @'
__global__ void countLandKernel(
        const double* seaDensity,
        int* landCount,
        int candidateCount
) {
    const int globalCell = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int totalCells = candidateCount * COARSE_CELL_COUNT;
    if (globalCell >= totalCells) return;

    const int candidate = globalCell / COARSE_CELL_COUNT;
    const int cell = globalCell - candidate * COARSE_CELL_COUNT;
    seaDensity += static_cast<std::size_t>(candidate) * COARSE_POINT_COUNT;
    landCount += candidate;

    const int cx = cell / COARSE_CELLS;
'@
$count = ([regex]::Matches($text, [regex]::Escape($oldCountHead.TrimEnd()))).Count
if ($count -ne 1) {
    throw "Expected exactly one countLandKernel header, found $count."
}
$text = $text.Replace($oldCountHead.TrimEnd(), $newCountHead.TrimEnd())

# -------------------------------------------------------------------------
# 3) Replace the one-candidate workspace with a persistent N-candidate workspace.
#    Host buffers are persistent too, avoiding per-batch large allocations.
# -------------------------------------------------------------------------
$workspaceStart = $text.IndexOf('struct ExactWorkspace {')
if ($workspaceStart -lt 0) {
    throw 'Could not locate ExactWorkspace.'
}
$workspaceEndMarker = "`n};`n`nvoid buildExactStates("
$workspaceEnd = $text.IndexOf($workspaceEndMarker, $workspaceStart)
if ($workspaceEnd -lt 0) {
    throw 'Could not locate end of ExactWorkspace.'
}
$workspaceEnd += 4 # include newline + }; + newline

$newWorkspace = @'
struct ExactWorkspace {
    int capacity = 1;
    p20::PerlinState* dTerrain = nullptr;
    p20::PerlinState* dTemp = nullptr;
    p20::PerlinState* dRain = nullptr;
    p20::PerlinState* dBlend = nullptr;
    double* dDensity = nullptr;
    int* dLand = nullptr;

    std::vector<p20::PerlinState> hTerrain;
    std::vector<p20::PerlinState> hTemp;
    std::vector<p20::PerlinState> hRain;
    std::vector<p20::PerlinState> hBlend;
    std::vector<int> hLand;

    explicit ExactWorkspace(int requestedCapacity) {
        capacity = std::max(1, requestedCapacity);
        const std::size_t n = static_cast<std::size_t>(capacity);
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dTerrain),
                            n * TERRAIN_STATE_COUNT * sizeof(p20::PerlinState)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dTemp),
                            n * 4 * sizeof(p20::PerlinState)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dRain),
                            n * 4 * sizeof(p20::PerlinState)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dBlend),
                            n * 2 * sizeof(p20::PerlinState)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dDensity),
                            n * COARSE_POINT_COUNT * sizeof(double)));
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dLand),
                            n * sizeof(int)));

        hTerrain.resize(n * TERRAIN_STATE_COUNT);
        hTemp.resize(n * 4);
        hRain.resize(n * 4);
        hBlend.resize(n * 2);
        hLand.resize(n);
    }

    ~ExactWorkspace() {
        if (dTerrain) (void)hipFree(dTerrain);
        if (dTemp) (void)hipFree(dTemp);
        if (dRain) (void)hipFree(dRain);
        if (dBlend) (void)hipFree(dBlend);
        if (dDensity) (void)hipFree(dDensity);
        if (dLand) (void)hipFree(dLand);
    }
};
'@
$text = $text.Remove($workspaceStart, $workspaceEnd - $workspaceStart).Insert($workspaceStart, $newWorkspace)

# -------------------------------------------------------------------------
# 4) Replace runExact with batched exact evaluation + a one-seed wrapper used by
#    --verify-seed. buildExactStates itself is unchanged/authoritative.
# -------------------------------------------------------------------------
$runStart = $text.IndexOf('ExactWaterResult runExact(std::int64_t seed, ExactWorkspace& w) {')
if ($runStart -lt 0) {
    throw 'Could not locate runExact.'
}
$runEnd = $text.IndexOf("`nstd::uint64_t parseU64(", $runStart)
if ($runEnd -lt 0) {
    throw 'Could not locate end of runExact.'
}

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

    for (int i = 0; i < n; ++i) {
        buildExactStates(seeds[static_cast<std::size_t>(i)],
                         terrainOne, tempOne, rainOne, blendOne);
        std::copy(terrainOne.begin(), terrainOne.end(),
                  w.hTerrain.begin() + static_cast<std::size_t>(i) * TERRAIN_STATE_COUNT);
        std::copy(tempOne.begin(), tempOne.end(),
                  w.hTemp.begin() + static_cast<std::size_t>(i) * 4);
        std::copy(rainOne.begin(), rainOne.end(),
                  w.hRain.begin() + static_cast<std::size_t>(i) * 4);
        std::copy(blendOne.begin(), blendOne.end(),
                  w.hBlend.begin() + static_cast<std::size_t>(i) * 2);
    }

    const std::size_t nn = static_cast<std::size_t>(n);
    HIP_CHECK(hipMemcpy(w.dTerrain, w.hTerrain.data(),
                        nn * TERRAIN_STATE_COUNT * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(w.dTemp, w.hTemp.data(),
                        nn * 4 * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(w.dRain, w.hRain.data(),
                        nn * 4 * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(w.dBlend, w.hBlend.data(),
                        nn * 2 * sizeof(p20::PerlinState), hipMemcpyHostToDevice));

    const int totalPoints = n * COARSE_POINT_COUNT;
    const int pointBlocks = (totalPoints + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            exactSeaDensityKernel,
            dim3(pointBlocks), dim3(EXACT_THREADS), 0, 0,
            w.dTerrain, w.dTemp, w.dRain, w.dBlend, w.dDensity, n);
    HIP_CHECK(hipGetLastError());

    HIP_CHECK(hipMemset(w.dLand, 0, nn * sizeof(int)));
    const int totalCells = n * COARSE_CELL_COUNT;
    const int cellBlocks = (totalCells + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            countLandKernel,
            dim3(cellBlocks), dim3(EXACT_THREADS), 0, 0,
            w.dDensity, w.dLand, n);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    HIP_CHECK(hipMemcpy(w.hLand.data(), w.dLand,
                        nn * sizeof(int), hipMemcpyDeviceToHost));

    std::vector<ExactWaterResult> results(nn);
    for (int i = 0; i < n; ++i) {
        const int land = w.hLand[static_cast<std::size_t>(i)];
        results[static_cast<std::size_t>(i)].landColumns = land;
        results[static_cast<std::size_t>(i)].waterColumns = TOTAL_COLUMNS - land;
    }
    return results;
}

ExactWaterResult runExact(std::int64_t seed, ExactWorkspace& w) {
    const std::vector<std::int64_t> seeds{seed};
    return runExactBatch(seeds, w)[0];
}
'@
$text = $text.Remove($runStart, $runEnd - $runStart).Insert($runStart, $newRun)

# -------------------------------------------------------------------------
# 5) Allocate the workspace for TopExact candidates and execute the selected P6
#    portfolio as one exact batch instead of calling runExact 24 times.
# -------------------------------------------------------------------------
$workspaceCreateOld = '        ExactWorkspace exactWorkspace;'
$workspaceCreateNew = '        ExactWorkspace exactWorkspace(std::max(1, o.topExact));'
$count = ([regex]::Matches($text, [regex]::Escape($workspaceCreateOld))).Count
if ($count -ne 1) {
    throw "Expected exactly one ExactWorkspace construction, found $count."
}
$text = $text.Replace($workspaceCreateOld, $workspaceCreateNew)

$exactLoopNeedle = '            for (int rank = 0; rank < exactN; ++rank) {'
$exactLoopPos = $text.IndexOf($exactLoopNeedle)
if ($exactLoopPos -lt 0) {
    throw 'Could not locate exact finalist loop.'
}

$batchPrelude = @'
            std::vector<std::int64_t> exactSeeds(static_cast<std::size_t>(exactN));
            for (int rank = 0; rank < exactN; ++rank) {
                const int idx = selectedIndices[static_cast<std::size_t>(rank)];
                const std::uint64_t attempt = nextAttempt + static_cast<std::uint64_t>(idx);
                exactSeeds[static_cast<std::size_t>(rank)] = static_cast<std::int64_t>(
                        p20::splitMixDeterministicSeed(o.sequence, attempt));
            }
            const std::vector<ExactWaterResult> exactResults =
                    runExactBatch(exactSeeds, exactWorkspace);

'@
$text = $text.Insert($exactLoopPos, $batchPrelude)

$runExactOld = '                const ExactWaterResult r = runExact(seed, exactWorkspace);'
$runExactNew = '                const ExactWaterResult& r = exactResults[static_cast<std::size_t>(rank)];'
$count = ([regex]::Matches($text, [regex]::Escape($runExactOld))).Count
if ($count -ne 1) {
    throw "Expected exactly one per-finalist runExact call, found $count."
}
$text = $text.Replace($runExactOld, $runExactNew)

$text = $text.Replace(
    'Scout P7: P6 diverse finalists + 800x800 variable-interior exact scan; outer TU4 ring counted as forced water.',
    'Scout P8: P7 800x800 + P6 diverse finalists + batched exact GPU evaluation; exact metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P8 batched exact finalist evaluator.' -ForegroundColor Green
Write-Host 'Scout, P6 finalist portfolio, P7 800x800 area, and exact terrain math are unchanged.'
Write-Host 'All TopExact finalists now share one set of GPU uploads, two kernel launches, and one synchronization.'
Write-Host 'Workspace is preallocated for TopExact candidates; host/device bulk buffers are reused across batches.'
Write-Host 'Use TopExact=24 for the direct P7 vs P8 throughput benchmark.'
