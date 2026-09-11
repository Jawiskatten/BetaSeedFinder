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

function Replace-Once([string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`r`n", "`n")
    $new = $new.Replace("`r`n", "`n")
    $count = ([regex]::Matches($script:text, [regex]::Escape($old))).Count
    if ($count -ne 1) {
        throw "Expected exactly one $label, found $count."
    }
    $script:text = $script:text.Replace($old, $new)
}

# P3/P4 ranked candidates by raw d7 sum. The pasted exact records show why that
# is not the right proxy for MIN LAND: a sample far below sea level keeps making
# raw d7Sum look better even though, for the final objective, an underwater
# column already counts as water and extra depth buys nothing. Conversely, one
# tall sampled peak can make d7Sum look bad even if almost everything else is
# ocean. P5 ranks a soft approximation to the sea-level threshold instead.
#
# We also recover the missing half of P4's 8x8 lattice WITHOUT increasing the
# block size. Each of the same 32 lanes evaluates its checkerboard point plus the
# adjacent complementary point, so every seed gets all 64 spatial samples while
# keeping one 32-lane wave/block on RDNA.

$heightHelper = @'
__device__ __forceinline__ double heightCenterFromNoise5(double noise5) {
    double d6 = noise5 / 8000.0;
    if (d6 < 0.0) d6 = -d6 * 0.3;
    d6 = d6 * 3.0 - 2.0;
    if (d6 < 0.0) {
        d6 /= 2.0;
        if (d6 < -1.0) d6 = -1.0;
        d6 /= 1.4;
        d6 /= 2.0;
    } else {
        if (d6 > 1.0) d6 = 1.0;
        d6 /= 8.0;
    }
    d6 *= 17.0 / 16.0;
    return 17.0 / 2.0 + d6 * 4.0;
}
'@
$heightHelperP5 = @'
__device__ __forceinline__ double heightCenterFromNoise5(double noise5) {
    double d6 = noise5 / 8000.0;
    if (d6 < 0.0) d6 = -d6 * 0.3;
    d6 = d6 * 3.0 - 2.0;
    if (d6 < 0.0) {
        d6 /= 2.0;
        if (d6 < -1.0) d6 = -1.0;
        d6 /= 1.4;
        d6 /= 2.0;
    } else {
        if (d6 > 1.0) d6 = 1.0;
        d6 /= 8.0;
    }
    d6 *= 17.0 / 16.0;
    return 17.0 / 2.0 + d6 * 4.0;
}

// TU4_WATER_P5_FULL64_SOFT_WATER
// Sea surface is coarse Y 7.875. Treat clearly submerged macro samples as zero
// land penalty and clearly elevated samples as one, with a six-block-wide soft
// transition (coarse 7.5..8.25) around sea level. Lower total penalty is better.
__device__ __forceinline__ double p5SeaLandPenalty(double d7) {
    if (d7 <= 7.5) return 0.0;
    if (d7 >= 8.25) return 1.0;
    return (d7 - 7.5) / 0.75;
}
'@
Replace-Once $heightHelper.TrimEnd() $heightHelperP5.TrimEnd() 'heightCenterFromNoise5 helper'

$oldCoords = @'
    // 32 samples chosen as an alternating checkerboard of the original 8x8
    // cell-center lattice. Every row is represented and adjacent rows sample
    // opposite columns, avoiding the directional bias of a plain 4x8 grid.
    const int row = lane >> 2;
    const int col = ((lane & 3) << 1) + (row & 1);
    const int qx = -108 + ((2 * col + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const int qz = -108 + ((2 * row + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const double coarseX = static_cast<double>(qx);
    const double coarseZ = static_cast<double>(qz);
'@
$newCoords = @'
    // Full 8x8 coverage with only 32 lanes: each lane owns one checkerboard
    // point plus its missing horizontal neighbor. Together the block evaluates
    // all 64 original cell-center samples while retaining P4's 32-thread block.
    const int row = lane >> 2;
    const int colA = ((lane & 3) << 1) + (row & 1);
    const int colB = colA + ((row & 1) ? -1 : 1);
    const int qxA = -108 + ((2 * colA + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const int qxB = -108 + ((2 * colB + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const int qz = -108 + ((2 * row + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const double coarseXA = static_cast<double>(qxA);
    const double coarseXB = static_cast<double>(qxB);
    const double coarseZ = static_cast<double>(qz);
'@
Replace-Once $oldCoords.TrimEnd() $newCoords.TrimEnd() 'P4 checkerboard coordinate block'

$oldEval = @'
    double noise5 = 0.0;
    // octave 12 starts at amplitude 2^-12. Keep all four dominant tail octaves
    // and the same 32 spatial samples as P3; only their initialization changed.
    double amplitude = 1.0 / 4096.0;
#pragma unroll
    for (int tail = 0; tail < 4; ++tail) {
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        noise5 += p20::perlin2(
                tailPerlin[tail], coarseX * scale, coarseZ * scale) * weight;
        amplitude /= 2.0;
    }

    const double d7 = heightCenterFromNoise5(noise5);
    // Sea-surface y=63 is coarse vertical coordinate 7+7/8 = 7.875.
    lowScratch[lane] = d7 < 7.875 ? 1 : 0;
    sumScratch[lane] = d7;
'@
$newEval = @'
    double noise5A = 0.0;
    double noise5B = 0.0;
    // Same dominant tail4 as P4, now evaluated at both complementary points.
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
    // Keep an easy-to-read hard count, but rank by the soft threshold penalty.
    lowScratch[lane] = (d7A < 7.875 ? 1 : 0) + (d7B < 7.875 ? 1 : 0);
    sumScratch[lane] = p5SeaLandPenalty(d7A) + p5SeaLandPenalty(d7B);
'@
Replace-Once $oldEval.TrimEnd() $newEval.TrimEnd() 'P4 single-point tail4 evaluation'

# hSum is now a land-penalty sum, so P3's ascending comparator is already the
# correct ordering. Update all human-readable labels and 32->64 sample counts.
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
Write-Host 'Spatial coverage: 32 checkerboard samples -> all 64 8x8 samples, still using 32 lanes.'
Write-Host 'Ranking: raw d7 sum -> soft sea-level land penalty (lower is better).'
Write-Host 'Tail4 parallel init and exact 864x864 finalist measurement are unchanged.'
Write-Host 'Keep Batch=131072 TopExact=16 for the first apples-to-apples benchmark.'
