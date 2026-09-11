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

# P8 keeps the P7 metric/scout/finalist portfolio unchanged. It only batches the
# expensive exact evaluation so all finalists share transfers, launches and one
# synchronization. This v2 patcher finds function regions structurally instead
# of requiring byte-identical generated P7 source.

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
# 1) Batch exactSeaDensityKernel structurally.
# -------------------------------------------------------------------------
$densityStart = $text.IndexOf('__global__ void exactSeaDensityKernel(')
if ($densityStart -lt 0) {
    throw 'Could not locate exactSeaDensityKernel.'
}
$densityBrace = $text.IndexOf('{', $densityStart)
if ($densityBrace -lt 0) {
    throw 'Could not locate exactSeaDensityKernel body.'
}
$densitySignature = $text.Substring($densityStart, $densityBrace - $densityStart)
if ($densitySignature.Contains('candidateCount')) {
    throw 'exactSeaDensityKernel already appears batched but P8 marker is missing.'
}
if (-not $densitySignature.Contains('double* seaDensity')) {
    throw 'Could not locate seaDensity parameter in exactSeaDensityKernel.'
}
$newDensitySignature = $densitySignature.Replace(
    'double* seaDensity',
    "double* seaDensity,`n        int candidateCount"
)
$text = $text.Remove($densityStart, $densityBrace - $densityStart).Insert($densityStart, $newDensitySignature)

# Replace only the old single-candidate index prologue, leaving the exact math.
$densityStart = $text.IndexOf('__global__ void exactSeaDensityKernel(')
$oldDensityIdx = '    const int idx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);'
$densityIdxPos = $text.IndexOf($oldDensityIdx, $densityStart)
if ($densityIdxPos -lt 0) {
    throw 'Could not locate exactSeaDensityKernel single-candidate index line.'
}
$densityIxMarker = '    const int ix = idx / COARSE_POINTS;'
$densityIxPos = $text.IndexOf($densityIxMarker, $densityIdxPos)
if ($densityIxPos -lt 0) {
    throw 'Could not locate exactSeaDensityKernel ix line.'
}
$newDensityPrologue = @'
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

'@
$text = $text.Remove($densityIdxPos, $densityIxPos - $densityIdxPos).Insert($densityIdxPos, $newDensityPrologue)

# -------------------------------------------------------------------------
# 2) Batch countLandKernel structurally.
# -------------------------------------------------------------------------
$countStart = $text.IndexOf('__global__ void countLandKernel(')
if ($countStart -lt 0) {
    throw 'Could not locate countLandKernel.'
}
$countBrace = $text.IndexOf('{', $countStart)
if ($countBrace -lt 0) {
    throw 'Could not locate countLandKernel body.'
}
$countSignature = $text.Substring($countStart, $countBrace - $countStart)
if ($countSignature.Contains('candidateCount')) {
    throw 'countLandKernel already appears batched but P8 marker is missing.'
}
if (-not $countSignature.Contains('int* landCount')) {
    throw 'Could not locate landCount parameter in countLandKernel.'
}
$newCountSignature = $countSignature.Replace(
    'int* landCount',
    "int* landCount,`n        int candidateCount"
)
$text = $text.Remove($countStart, $countBrace - $countStart).Insert($countStart, $newCountSignature)

$countStart = $text.IndexOf('__global__ void countLandKernel(')
$oldCellIdx = '    const int cell = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);'
$cellIdxPos = $text.IndexOf($oldCellIdx, $countStart)
if ($cellIdxPos -lt 0) {
    throw 'Could not locate countLandKernel single-candidate index line.'
}
$cxMarker = '    const int cx = cell / COARSE_CELLS;'
$cxPos = $text.IndexOf($cxMarker, $cellIdxPos)
if ($cxPos -lt 0) {
    throw 'Could not locate countLandKernel cx line.'
}
$newCountPrologue = @'
    const int globalCell = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int totalCells = candidateCount * COARSE_CELL_COUNT;
    if (globalCell >= totalCells) return;

    const int candidate = globalCell / COARSE_CELL_COUNT;
    const int cell = globalCell - candidate * COARSE_CELL_COUNT;
    seaDensity += static_cast<std::size_t>(candidate) * COARSE_POINT_COUNT;
    landCount += candidate;

'@
$text = $text.Remove($cellIdxPos, $cxPos - $cellIdxPos).Insert($cellIdxPos, $newCountPrologue)

# -------------------------------------------------------------------------
# 3) Replace ExactWorkspace with a persistent multi-candidate workspace.
# -------------------------------------------------------------------------
$workspaceStart = $text.IndexOf('struct ExactWorkspace {')
if ($workspaceStart -lt 0) {
    throw 'Could not locate ExactWorkspace.'
}
$buildStart = $text.IndexOf("`nvoid buildExactStates(", $workspaceStart)
if ($buildStart -lt 0) {
    throw 'Could not locate buildExactStates after ExactWorkspace.'
}
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
$text = $text.Remove($workspaceStart, $buildStart - $workspaceStart).Insert($workspaceStart, $newWorkspace.TrimEnd())

# -------------------------------------------------------------------------
# 4) Replace one-at-a-time runExact with runExactBatch. buildExactStates stays
#    untouched, so every finalist gets exactly the same Perlin states as P7.
# -------------------------------------------------------------------------
$runStart = $text.IndexOf('ExactWaterResult runExact(')
if ($runStart -lt 0) {
    throw 'Could not locate runExact.'
}
$parseStart = $text.IndexOf('std::uint64_t parseU64(', $runStart)
if ($parseStart -lt 0) {
    throw 'Could not locate parseU64 after runExact.'
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
$text = $text.Remove($runStart, $parseStart - $runStart).Insert($runStart, $newRun)

# -------------------------------------------------------------------------
# 5) Give the workspace TopExact capacity and exact-check the selected P6
#    portfolio in one call.
# -------------------------------------------------------------------------
$workspaceCreatePos = $text.IndexOf('ExactWorkspace exactWorkspace;')
if ($workspaceCreatePos -lt 0) {
    throw 'Could not locate ExactWorkspace construction in main.'
}
$text = $text.Remove($workspaceCreatePos, 'ExactWorkspace exactWorkspace;'.Length).Insert(
    $workspaceCreatePos,
    'ExactWorkspace exactWorkspace(std::max(1, o.topExact));'
)

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

$callPos = $text.IndexOf('runExact(seed, exactWorkspace)', $exactLoopPos + $batchPrelude.Length)
if ($callPos -lt 0) {
    throw 'Could not locate per-finalist runExact call.'
}
$beforeCall = $text.Substring(0, $callPos)
$lineStart = $beforeCall.LastIndexOf("`n") + 1
$lineEnd = $text.IndexOf("`n", $callPos)
if ($lineEnd -lt 0) { $lineEnd = $text.Length }
$replacementLine = '                const ExactWaterResult& r = exactResults[static_cast<std::size_t>(rank)];'
$text = $text.Remove($lineStart, $lineEnd - $lineStart).Insert($lineStart, $replacementLine)

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
Write-Host 'P8 patcher v2: exact kernels/workspace/run loop located structurally.'
Write-Host 'Scout, P6 finalist portfolio, P7 800x800 metric, and exact terrain math are unchanged.'
Write-Host 'All TopExact finalists now share one state-upload set, two GPU launches, one sync, and one result copy.'
Write-Host 'Verify the existing 96617-land record before benchmarking throughput.'
