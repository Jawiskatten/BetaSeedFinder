param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\single_biome_radius.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "Source file not found: $sourcePath"
}

$text = [System.IO.File]::ReadAllText($sourcePath)

if ($text.Contains('RAINFOREST_P14_TARGET')) {
    Write-Host 'Rainforest P14 is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('SNOW_P13_TUNDRA_TAIGA')) {
    throw 'Rainforest P14 expects the current Snow P13 source first.'
}
if (-not $text.Contains('TUNDRA_P12_REPLAY_TAIL')) {
    throw 'Rainforest P14 requires the P12 optimized scout stack.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'Rainforest P14 requires the generic exact square semantics.'
}

$backupPath = $sourcePath + '.p13-before-rainforest-p14.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# Beta 1.7.3 quantized biome lookup:
# RAINFOREST iff
#   f >= 0.97
#   and f1 = quantizedRain * f >= 0.9
# Since f = ti/63 after the game's exact 64-level quantization, only the
# hottest temperature cells can possibly pass. P14 therefore keeps P8's lazy
# rain design: reject cold points from temp+blend alone, and only build/evaluate
# rain for seed groups that survive the fused first temperature screen.

# ---------------------------------------------------------------------------
# 1) Normal scout predicate -> exact RAINFOREST classification.
# ---------------------------------------------------------------------------
$searchPattern = '(?s)__device__ __forceinline__ bool searchIsTundraAt\(.*?\r?\n\}'
$searchMatches = [regex]::Matches($text, $searchPattern)
if ($searchMatches.Count -ne 1) {
    throw "Expected exactly one searchIsTundraAt helper, found $($searchMatches.Count)."
}

$newSearch = @'
// RAINFOREST_P14_TARGET
// Name kept for minimal kernel churn; semantics are now exact RAINFOREST-only.
__device__ __forceinline__ bool searchIsTundraAt(
        const SearchClimateState& s,
        int blockX,
        int blockZ
) {
    const double x = static_cast<double>(blockX);
    const double z = static_cast<double>(blockZ);

    const double blendRaw = searchOctaveNoise2(s.blend, x, z);
    const double d0 = blendRaw * 1.1 + 0.5;
    const double tempRaw = searchOctaveNoise4(
            s.temp, x, z, 0.02500000037252903, 0.25);

    double temperature = (tempRaw * 0.15 + 0.7) * 0.99 + d0 * 0.01;
    temperature = 1.0 - (1.0 - temperature) * (1.0 - temperature);
    temperature = clamp01(temperature);

    int ti = static_cast<int>(temperature * 63.0);
    if (ti < 0) ti = 0;
    if (ti > 63) ti = 63;
    const float f = static_cast<float>(ti) / 63.0f;

    if (f < 0.97f) return false;

    const double rainRaw = searchOctaveNoise4(
            s.rain, x, z, 0.05000000074505806, 0.3333333333333333);
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    rain = clamp01(rain);

    int ri = static_cast<int>(rain * 63.0);
    if (ri < 0) ri = 0;
    if (ri > 63) ri = 63;
    float f1 = static_cast<float>(ri) / 63.0f;
    f1 *= f;
    return f1 >= 0.9f;
}
'@
$text = [regex]::Replace($text, $searchPattern, $newSearch.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 2) Fused first screen. 2 = impossible (too cold), 1 = needs rain.
#    RAINFOREST has no temperature-only guaranteed-pass class.
# ---------------------------------------------------------------------------
$tempPattern = '(?s)__device__ __forceinline__ int p8TemperatureClassAt\(.*?\r?\n\}'
$tempMatches = [regex]::Matches($text, $tempPattern)
if ($tempMatches.Count -ne 1) {
    throw "Expected exactly one p8TemperatureClassAt helper, found $($tempMatches.Count)."
}

$newTemp = @'
__device__ __forceinline__ int p8TemperatureClassAt(
        const SearchClimateState& s,
        int blockX,
        int blockZ,
        double& d0Out,
        int& tiOut
) {
    const double x = static_cast<double>(blockX);
    const double z = static_cast<double>(blockZ);

    const double blendRaw = searchOctaveNoise2(s.blend, x, z);
    const double d0 = blendRaw * 1.1 + 0.5;
    const double tempRaw = searchOctaveNoise4(
            s.temp, x, z, 0.02500000037252903, 0.25);

    double temperature = (tempRaw * 0.15 + 0.7) * 0.99 + d0 * 0.01;
    temperature = 1.0 - (1.0 - temperature) * (1.0 - temperature);
    temperature = clamp01(temperature);

    int ti = static_cast<int>(temperature * 63.0);
    if (ti < 0) ti = 0;
    if (ti > 63) ti = 63;
    const float f = static_cast<float>(ti) / 63.0f;

    d0Out = d0;
    tiOut = ti;
    return f < 0.97f ? 2 : 1;
}
'@
$text = [regex]::Replace($text, $tempPattern, $newTemp.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 3) Rain half for first-screen points -> exact RAINFOREST wetness test.
# ---------------------------------------------------------------------------
$rainHelperPattern = '(?s)__device__ __forceinline__ bool p8AmbiguousRainPasses\(.*?\r?\n\}'
$rainHelperMatches = [regex]::Matches($text, $rainHelperPattern)
if ($rainHelperMatches.Count -ne 1) {
    throw "Expected exactly one p8AmbiguousRainPasses helper, found $($rainHelperMatches.Count)."
}

$newRainHelper = @'
__device__ __forceinline__ bool p8AmbiguousRainPasses(
        const SearchClimateState& s,
        int blockX,
        int blockZ,
        double d0,
        int ti
) {
    const double x = static_cast<double>(blockX);
    const double z = static_cast<double>(blockZ);
    const double rainRaw = searchOctaveNoise4(
            s.rain, x, z, 0.05000000074505806, 0.3333333333333333);
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    rain = clamp01(rain);

    int ri = static_cast<int>(rain * 63.0);
    if (ri < 0) ri = 0;
    if (ri > 63) ri = 63;
    const float f = static_cast<float>(ti) / 63.0f;
    float f1 = static_cast<float>(ri) / 63.0f;
    f1 *= f;
    return f >= 0.97f && f1 >= 0.9f;
}
'@
$text = [regex]::Replace($text, $rainHelperPattern, $newRainHelper.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 4) Re-enable P8/P10 lazy rain construction and first-point rain check.
# ---------------------------------------------------------------------------
$rainDisabled = '    // P13: rain state is not built; TUNDRA+TAIGA is temperature-only.'
if (-not $text.Contains($rainDisabled)) {
    throw 'Could not find the P13 disabled-rain marker.'
}
$text = $text.Replace($rainDisabled, @'
    if (alive) {
        p8InitRain(s, seed, lane);
    }
'@.TrimEnd())

$oldFirstRain = '    bool firstRainFailed = false; // P13: no ambiguous rain class.'
if (-not $text.Contains($oldFirstRain)) {
    throw 'Could not find the P13 first-rain marker.'
}
$newFirstRain = @'
    bool firstRainFailed = false;
    if (alive && firstClass == 1) {
        firstRainFailed = !p8AmbiguousRainPasses(
                s, centerX + firstDx, centerZ + firstDz, firstD0, firstTi);
    }
'@
$text = $text.Replace($oldFirstRain, $newFirstRain.TrimEnd())

# Surviving center label in the compact scout output.
$oldBase = 'baseBiome[seedIndex] = centerPass ? static_cast<unsigned char>(TUNDRA) : 255;'
if (-not $text.Contains($oldBase)) {
    throw 'Could not find the surviving-center biome label.'
}
$text = $text.Replace(
    $oldBase,
    'baseBiome[seedIndex] = centerPass ? static_cast<unsigned char>(RAINFOREST) : 255;'
)

# ---------------------------------------------------------------------------
# 5) Exact verifier -> mismatch is anything except RAINFOREST.
# ---------------------------------------------------------------------------
$oldExact = @'
if (b != static_cast<unsigned char>(TUNDRA) &&
                b != static_cast<unsigned char>(TAIGA)) atomicMin(&s.groupMinD2, p.d2);
'@.TrimEnd()
if (-not $text.Contains($oldExact)) {
    throw 'Could not find the P13 exact-verifier allowed-set comparison.'
}
$text = $text.Replace(
    $oldExact,
    'if (b != static_cast<unsigned char>(RAINFOREST)) atomicMin(&s.groupMinD2, p.d2);'
)

# Exact verify mode should also reject a non-rainforest center immediately.
$centerInit = @'
        result->baseBiome = s.baseBiome;
    }
    __syncthreads();

    for (int base = 0; base < pointCount; base += EXACT_THREADS) {
'@
if ($text.Contains($centerInit)) {
    $centerReplacement = @'
        result->baseBiome = s.baseBiome;
        if (s.baseBiome != static_cast<int>(RAINFOREST)) {
            result->firstMismatchD2 = 0;
        }
    }
    __syncthreads();

    if (s.baseBiome != static_cast<int>(RAINFOREST)) return;

    for (int base = 0; base < pointCount; base += EXACT_THREADS) {
'@
    $text = $text.Replace($centerInit, $centerReplacement)
}

# ---------------------------------------------------------------------------
# 6) Coverage -> count RAINFOREST only.
# ---------------------------------------------------------------------------
$oldCoverage = @'
if (b == static_cast<unsigned char>(TUNDRA) ||
            b == static_cast<unsigned char>(TAIGA)) ++localSame;
'@.TrimEnd()
if (-not $text.Contains($oldCoverage)) {
    throw 'Could not find the P13 coverage allowed-set comparison.'
}
$text = $text.Replace(
    $oldCoverage,
    'if (b == static_cast<unsigned char>(RAINFOREST)) ++localSame;'
)

$oldCenterCoverage = 'sameCount = (s.baseBiome == TUNDRA || s.baseBiome == TAIGA) ? 1 : 0; // center'
if ($text.Contains($oldCenterCoverage)) {
    $text = $text.Replace(
        $oldCenterCoverage,
        'sameCount = (s.baseBiome == RAINFOREST) ? 1 : 0; // center'
    )
}

# ---------------------------------------------------------------------------
# 7) Console wording.
# ---------------------------------------------------------------------------
$text = $text.Replace(
    'P13 scout: TUNDRA+TAIGA | exact temp-only snow predicate | zero rain work | fast permutation RNG | warp votes | tuned 4x16',
    'P14 scout: RAINFOREST-only | hot-temp gate + lazy exact rain | fast permutation RNG | warp votes | tuned 4x16'
)
$text = $text.Replace('snowBiomeBlocks=', 'rainforestBlocks=')
$text = $text.Replace('contains only TUNDRA/TAIGA.', 'contains only RAINFOREST.')
$text = $text.Replace('TUNDRA+TAIGA', 'RAINFOREST')

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied Rainforest P14.' -ForegroundColor Green
Write-Host 'Target rule: every accepted position must be exact Beta 1.7.3 RAINFOREST.'
Write-Host 'Scout first rejects quantized temperature f < 0.97, then lazily evaluates rain.'
Write-Host 'Exact verifier and realCoverage now count RAINFOREST only.'
Write-Host 'Square size stays generic: use -Target 450 for an exact 900x900 search.'
