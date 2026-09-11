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

if ($text.Contains('TU4_WATER_P5_FULL64_SOFT_WATER')) {
    Write-Host 'TU4 Water P5 full-64 soft-water scout is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P4_PARALLEL_TAIL_INIT')) {
    throw 'P5 requires TU4 Water P4 parallel tail init first.'
}

$backupPath = $sourcePath + '.p4-before-water-p5.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# -------------------------------------------------------------------------
# P5 goal
# -------------------------------------------------------------------------
# P3/P4 rank candidates by raw d7 sum. That is only loosely aligned with the
# actual objective (minimum exact land columns): once a sampled macro-height is
# comfortably under sea level, making it even deeper should not buy unlimited
# score. P5 therefore uses a soft sea-level land penalty and restores the full
# 8x8 spatial lattice without increasing the 32-thread scout block size.
#
# This v2 patcher intentionally locates regions structurally instead of matching
# whole generated P4 blocks byte-for-byte. P4 itself is locally generated, so
# comment/whitespace differences must not prevent the patch from applying.

# -------------------------------------------------------------------------
# 1) Add sea-level penalty helper after heightCenterFromNoise5.
# -------------------------------------------------------------------------
$heightStart = $text.IndexOf('__device__ __forceinline__ double heightCenterFromNoise5(')
if ($heightStart -lt 0) {
    throw 'Could not locate heightCenterFromNoise5.'
}
$heightEnd = $text.IndexOf("`n}`n", $heightStart)
if ($heightEnd -lt 0) {
    throw 'Could not locate end of heightCenterFromNoise5.'
}
$heightEnd += 3

$penaltyHelper = @'

// TU4_WATER_P5_FULL64_SOFT_WATER
// Sea surface is coarse Y 7.875. Samples clearly underwater pay zero land
// penalty, clearly elevated samples pay one, and values around sea level get a
// smooth fractional penalty. Lower total penalty is better.
__device__ __forceinline__ double p5SeaLandPenalty(double d7) {
    if (d7 <= 7.5) return 0.0;
    if (d7 >= 8.25) return 1.0;
    return (d7 - 7.5) / 0.75;
}
'@
$text = $text.Insert($heightEnd, $penaltyHelper)

# -------------------------------------------------------------------------
# 2) Scope the remaining edits to waterScoutKernel.
# -------------------------------------------------------------------------
$kernelStart = $text.IndexOf('__global__ void waterScoutKernel(')
if ($kernelStart -lt 0) {
    throw 'Could not locate waterScoutKernel.'
}

# Replace the P4 single checkerboard coordinate with a complementary pair.
$coordStart = $text.IndexOf('    const int row = lane >> 2;', $kernelStart)
if ($coordStart -lt 0) {
    throw 'Could not locate P4 scout coordinate start.'
}
$coordEndMarker = '    const double coarseZ = static_cast<double>(qz);'
$coordEnd = $text.IndexOf($coordEndMarker, $coordStart)
if ($coordEnd -lt 0) {
    throw 'Could not locate P4 scout coordinate end.'
}
$coordEnd += $coordEndMarker.Length

$newCoords = @'
    // Full 8x8 coverage with 32 lanes. Each lane evaluates its P4 checkerboard
    // point plus the missing horizontal neighbor, covering all 64 cell centers.
    const int row = lane >> 2;
    const int colA = ((lane & 3) << 1) + (row & 1);
    const int colB = colA + ((row & 1) ? -1 : 1);
    const int qxA = -108 + ((2 * colA + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const int qxB = -108 + ((2 * colB + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const int qz = -108 + ((2 * row + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const double coarseXA = static_cast<double>(qxA);
    const double coarseXB = static_cast<double>(qxB);
    const double coarseZ = static_cast<double>(qz);
'@.TrimEnd()
$text = $text.Remove($coordStart, $coordEnd - $coordStart).Insert($coordStart, $newCoords)

# Re-find the kernel after the coordinate replacement, then replace everything
# from the P4 noise5 accumulator through its old sumScratch assignment.
$kernelStart = $text.IndexOf('__global__ void waterScoutKernel(')
$evalStart = $text.IndexOf('    double noise5 = 0.0;', $kernelStart)
if ($evalStart -lt 0) {
    throw 'Could not locate P4 noise5 evaluation start.'
}
$evalEndMarker = '    sumScratch[lane] = d7;'
$evalEnd = $text.IndexOf($evalEndMarker, $evalStart)
if ($evalEnd -lt 0) {
    throw 'Could not locate P4 noise5 evaluation end.'
}
$evalEnd += $evalEndMarker.Length

$newEval = @'
    double noise5A = 0.0;
    double noise5B = 0.0;
    // Same P4 dominant tail4, evaluated at both complementary spatial points.
    double amplitude = 1.0 / 4096.0;
#pragma unroll
    for (int tail = 0; tail < 4; ++tail) {
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        noise5A += p20::perlin2(
                tailPerlin[tail], coarseXA * scale, coarseZ * scale) * weight;
        noise5B += p20::perlin2(
                tailPerlin[tail], coarseXB * scale, coarseZ * scale) * weight;
        amplitude /= 2.0;
    }

    const double d7A = heightCenterFromNoise5(noise5A);
    const double d7B = heightCenterFromNoise5(noise5B);
    // Hard underwater count stays diagnostic; the continuous soft penalty is
    // the actual scout ranking score stored in hSum.
    lowScratch[lane] = (d7A < 7.875 ? 1 : 0) + (d7B < 7.875 ? 1 : 0);
    sumScratch[lane] = p5SeaLandPenalty(d7A) + p5SeaLandPenalty(d7B);
'@.TrimEnd()
$text = $text.Remove($evalStart, $evalEnd - $evalStart).Insert($evalStart, $newEval)

# hSum now means land penalty rather than raw d7 sum. P3/P4 already sort hSum
# ascending, which is exactly what P5 wants. Only labels/sample counts change.
$text = $text.Replace(
    'Scout P4: 32-point tail4 + parallel 4-octave init + affine RNG jumps; d7 ranking; exact finalists unchanged.',
    'Scout P5: full 64-point tail4 via 32 lanes + soft sea-level land penalty; exact finalists unchanged.'
)
$text = $text.Replace('d7Sum=', 'landPenalty64=')
$text = $text.Replace('scoutBestD7=', 'scoutPenalty64=')
$text = $text.Replace('scoutD7=', 'scoutPenalty64=')
$text = $text.Replace('scout_d7_sum', 'scout_land_penalty64')
$text = $text.Replace('lowHeightSamples=" << globalScoutLow << "/32', 'lowHeightSamples=" << globalScoutLow << "/64')
$text = $text.Replace('scoutLow=" << hLow[idx] << "/32', 'scoutLow=" << hLow[idx] << "/64')
$text = $text.Replace('scoutLow=" << globalScoutLow << "/32', 'scoutLow=" << globalScoutLow << "/64')

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P5 full-64 soft-water scout.' -ForegroundColor Green
Write-Host 'P5 patcher v2: P4 scout regions located structurally, not by exact full-text matching.'
Write-Host 'Spatial coverage: 32 checkerboard samples -> all 64 8x8 samples, still using 32 lanes.'
Write-Host 'Ranking: raw d7 sum -> soft sea-level land penalty (lower is better).'
Write-Host 'Tail4 parallel init and exact 864x864 finalist measurement are unchanged.'
Write-Host 'Keep Batch=131072 TopExact=16 for the first apples-to-apples benchmark.'
