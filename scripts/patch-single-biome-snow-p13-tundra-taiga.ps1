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

if ($text.Contains('SNOW_P13_TUNDRA_TAIGA')) {
    Write-Host 'Snow P13 (Tundra + Taiga) is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P12_REPLAY_TAIL')) {
    throw 'Snow P13 requires the current P12 scout first.'
}
if (-not $text.Contains('TUNDRA_P8_LAZY_RAIN')) {
    throw 'Snow P13 requires the P8 fused scout layout.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'Snow P13 requires exact 864x864 square semantics.'
}
if (-not $text.Contains('realCoveragePercent')) {
    throw 'Snow P13 expects the true full-square coverage patch.'
}

$backupPath = $sourcePath + '.p12-before-snow-p13.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# Beta 1.7.3 exact identity used here:
# after 64-level quantization, {TUNDRA, TAIGA} is exactly temperature f < 0.5.
# Rain cannot change membership in this union, so the scout can skip rain fully.

# ---------------------------------------------------------------------------
# 1) Normal scout point predicate -> exact Tundra-or-Taiga temperature test.
# ---------------------------------------------------------------------------
$searchPattern = '(?s)__device__ __forceinline__ bool searchIsTundraAt\(.*?\r?\n\}'
$searchMatches = [regex]::Matches($text, $searchPattern)
if ($searchMatches.Count -ne 1) {
    throw "Expected exactly one searchIsTundraAt helper, found $($searchMatches.Count)."
}

$newSearch = @'
// SNOW_P13_TUNDRA_TAIGA
// Exact Beta 1.7.3 predicate for {TUNDRA, TAIGA}.
// This union is exactly quantized temperature f < 0.5; rain is irrelevant.
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
    return f < 0.5f;
}
'@
$text = [regex]::Replace($text, $searchPattern, $newSearch.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 2) Fused first screen -> no ambiguous/rain class. 0=pass, 2=fail.
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
    return f < 0.5f ? 0 : 2;
}
'@
$text = [regex]::Replace($text, $tempPattern, $newTemp.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 3) Rain is now provably irrelevant in the scout hot path.
# ---------------------------------------------------------------------------
$rainInitPattern = '(?m)^    if \(alive\) \{\r?\n        p8InitRain\(s, seed, lane\);\r?\n    \}'
$rainInitMatches = [regex]::Matches($text, $rainInitPattern)
if ($rainInitMatches.Count -ne 1) {
    throw "Expected exactly one p8InitRain call block, found $($rainInitMatches.Count)."
}
$text = [regex]::Replace(
    $text,
    $rainInitPattern,
    '    // P13: rain state is not built; TUNDRA+TAIGA is temperature-only.',
    1
)

$firstRainPattern = '(?s)    bool firstRainFailed = false;\r?\n    if \(alive && firstClass == 1\) \{.*?\r?\n    \}'
$firstRainMatches = [regex]::Matches($text, $firstRainPattern)
if ($firstRainMatches.Count -ne 1) {
    throw "Expected exactly one first-screen rain block, found $($firstRainMatches.Count)."
}
$text = [regex]::Replace(
    $text,
    $firstRainPattern,
    '    bool firstRainFailed = false; // P13: no ambiguous rain class.',
    1
)

# ---------------------------------------------------------------------------
# 4) Exact verifier. P12 leaves this simple comparison untouched, so patch the
#    actual mismatch predicate directly instead of depending on function order.
# ---------------------------------------------------------------------------
$oldExactMismatch = 'if (static_cast<int>(b) != s.baseBiome) atomicMin(&s.groupMinD2, p.d2);'
$exactMismatchCount = ([regex]::Matches($text, [regex]::Escape($oldExactMismatch))).Count
if ($exactMismatchCount -ne 1) {
    throw "Expected exactly one exact-verifier biome mismatch comparison, found $exactMismatchCount."
}
$newExactMismatch = @'
if (b != static_cast<unsigned char>(TUNDRA) &&
                b != static_cast<unsigned char>(TAIGA)) atomicMin(&s.groupMinD2, p.d2);
'@.TrimEnd()
$text = $text.Replace($oldExactMismatch, $newExactMismatch)

# ---------------------------------------------------------------------------
# 5) Full-square coverage. Count TUNDRA + TAIGA together.
# ---------------------------------------------------------------------------
$oldCoverageCompare = 'if (static_cast<int>(b) == s.baseBiome) ++localSame;'
$coverageCompareCount = ([regex]::Matches($text, [regex]::Escape($oldCoverageCompare))).Count
if ($coverageCompareCount -ne 1) {
    throw "Expected exactly one coverage biome comparison, found $coverageCompareCount."
}
$newCoverageCompare = @'
if (b == static_cast<unsigned char>(TUNDRA) ||
            b == static_cast<unsigned char>(TAIGA)) ++localSame;
'@.TrimEnd()
$text = $text.Replace($oldCoverageCompare, $newCoverageCompare)

$oldCenterCount = 'sameCount = 1; // center block itself'
$centerCountMatches = ([regex]::Matches($text, [regex]::Escape($oldCenterCount))).Count
if ($centerCountMatches -eq 1) {
    $text = $text.Replace(
        $oldCenterCount,
        'sameCount = (s.baseBiome == TUNDRA || s.baseBiome == TAIGA) ? 1 : 0; // center'
    )
} elseif ($centerCountMatches -gt 1) {
    throw "Expected at most one coverage center count initializer, found $centerCountMatches."
}

# ---------------------------------------------------------------------------
# 6) Console wording only. Data/log structures remain compatible.
# ---------------------------------------------------------------------------
$text = $text.Replace(
    'P12 scout: TUNDRA-only | zero-mod replay | fast exact permutation RNG | parallel octave init | lazy bounded rain | warp votes | tuned 4x16',
    'P13 scout: TUNDRA+TAIGA | exact temp-only snow predicate | zero rain work | fast permutation RNG | warp votes | tuned 4x16'
)
$text = $text.Replace('sameBiomeBlocks=', 'snowBiomeBlocks=')
$text = $text.Replace('is one biome.', 'contains only TUNDRA/TAIGA.')
$text = $text.Replace(
    '<< " biome=" << (bestProbe > 0 ? "TUNDRA" : "UNKNOWN")',
    '<< " allowed=" << (bestProbe > 0 ? "TUNDRA+TAIGA" : "UNKNOWN")'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied Snow P13: TUNDRA + TAIGA are both allowed.' -ForegroundColor Green
Write-Host 'Exact rule: every position in the 864x864 square may be TUNDRA or TAIGA.'
Write-Host 'Scout rule: exact quantized temperature f < 0.5; rain work is skipped.'
Write-Host 'Exact verifier and full-square coverage now treat TUNDRA+TAIGA as one allowed set.'
Write-Host 'P12 RNG/temp optimizations, square probes, GPU compaction and jackpot recall are preserved.'
