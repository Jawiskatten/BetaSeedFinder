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

if ($text.Contains('TUNDRA_P9_BOUNDED_RAIN')) {
    Write-Host 'Tundra P9 bounded-rain optimization is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P8_LAZY_RAIN')) {
    throw 'P9 requires the P8 lazy-rain scout first.'
}
if (-not $text.Contains('TUNDRA_P4_GPU_COMPACTION')) {
    throw 'P9 requires P4 GPU compaction.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P9 requires exact 864x864 square semantics.'
}

$backupPath = $sourcePath + '.p8-before-p9.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# ---------------------------------------------------------------------------
# P9 strategy
# ---------------------------------------------------------------------------
# P8 already avoids constructing rain state for seeds rejected by the first
# temperature screen. For the remaining ambiguous points, P8 still evaluates
# all four rain simplex octaves unconditionally.
#
# P9 evaluates rain octaves from largest weight to smallest: 3,2,1,0.
# After octave 2 and octave 1 it asks whether every mathematically possible
# value of the unevaluated octaves gives the same quantized Beta biome result.
# If yes, it returns without doing the remaining simplex calls.
#
# The bound is deliberately rigorous rather than empirical. For one simplex
# corner, t = 0.5-r^2 and every legacy 2-D gradient has norm <= sqrt(2), so
#
#   |t^4 * grad.dot(delta)| <= sqrt(2) r (0.5-r^2)^4.
#
# The RHS is maximized at r^2=1/18. Multiplying that maximum by 3 possible
# corners and the legacy normalization 70 gives < 2.731291. We use 2.732 plus
# an additional floating-point guard. Thus no sampled Tundra point can be
# rejected because of the shortcut.
# ---------------------------------------------------------------------------

$searchPattern = '(?s)__device__ __forceinline__ bool searchIsTundraAt\(.*?\r?\n\}\r?\n\r?\n__device__ __forceinline__ void squareProbePoint864\('
$searchMatches = [regex]::Matches($text, $searchPattern)
if ($searchMatches.Count -ne 1) {
    throw "Expected exactly one searchIsTundraAt block, found $($searchMatches.Count)."
}

$newSearchBlock = @'
// TUNDRA_P9_BOUNDED_RAIN
// Proven absolute bound for one legacy searchSimplex2() result.
// Corner-wise Cauchy bound: 70 * 3 * max_r[sqrt(2)*r*(0.5-r^2)^4]
// with r^2=1/18 is ~2.731290962. 2.732 leaves a large FP safety margin.
static constexpr double P9_SIMPLEX_ABS_BOUND = 2.732;

__device__ __forceinline__ bool p9QuantizedRainPasses(int ti, int ri) {
    const float f = static_cast<float>(ti) / 63.0f;
    float f1 = static_cast<float>(ri) / 63.0f;
    f1 *= f;
    return f1 < 0.2f;
}

// Return +1 if the entire raw-noise interval is Tundra, 0 if the entire
// interval is non-Tundra, and -1 when another octave is required.
__device__ __forceinline__ int p9RainIntervalVerdict(
        double partialRaw,
        double remainingAbsRaw,
        double d0,
        int ti
) {
    // The analytic 2.732 simplex bound already has ~7e-4 headroom over the
    // proven corner bound. This extra guard also covers ordinary double
    // roundoff and the fact that the partial terms are accumulated heavy-first.
    const double guard = 1.0e-9 *
            (1.0 + fabs(partialRaw) + fabs(remainingAbsRaw));
    const double rawLo = partialRaw - remainingAbsRaw - guard;
    const double rawHi = partialRaw + remainingAbsRaw + guard;

    double rainLo = (rawLo * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    double rainHi = (rawHi * 0.15 + 0.5) * 0.998 + d0 * 0.002;

    // Expand the transformed interval as well before the legacy clamp/quantize.
    rainLo = clamp01(rainLo - 1.0e-9);
    rainHi = clamp01(rainHi + 1.0e-9);

    int riLo = static_cast<int>(rainLo * 63.0);
    int riHi = static_cast<int>(rainHi * 63.0);
    if (riLo < 0) riLo = 0;
    if (riLo > 63) riLo = 63;
    if (riHi < 0) riHi = 0;
    if (riHi > 63) riHi = 63;

    // Tundra classification is monotone in quantized rain for fixed ti.
    // If even the wettest possible endpoint passes, everything passes.
    if (p9QuantizedRainPasses(ti, riHi)) return 1;
    // If even the driest possible endpoint fails, everything fails.
    if (!p9QuantizedRainPasses(ti, riLo)) return 0;
    return -1;
}

__device__ __forceinline__ bool p9AmbiguousRainPassesBounded(
        const SearchClimateState& s,
        int blockX,
        int blockZ,
        double d0,
        int ti
) {
    const double x = static_cast<double>(blockX);
    const double z = static_cast<double>(blockZ);

    // Reproduce the exact scale/weight sequence used by searchOctaveNoise4().
    // We evaluate in a different order for early decisions, but retain each
    // weighted term separately so the all-four fallback can sum 0,1,2,3 in
    // the original order and preserve the normal exact floating-point result.
    double scale[4];
    double weight[4];
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 4; ++octave) {
        scale[octave] = (0.05000000074505806 / 1.5) * d7;
        weight[octave] = 0.55 / d6;
        d7 *= 0.3333333333333333;
        d6 *= 0.5;
    }

    double term[4];

    // Largest two weights first: octave 3 (4.4), then octave 2 (2.2).
    term[3] = searchSimplex2(s.rain[3], x * scale[3], z * scale[3]) * weight[3];
    term[2] = searchSimplex2(s.rain[2], x * scale[2], z * scale[2]) * weight[2];
    double partial = term[3] + term[2];

    // Remaining octaves 1+0 have total absolute weight 1.1+0.55.
    double remaining = P9_SIMPLEX_ABS_BOUND * (weight[1] + weight[0]);
    int verdict = p9RainIntervalVerdict(partial, remaining, d0, ti);
    if (verdict >= 0) return verdict != 0;

    // Still borderline: pay for octave 1. Many points become provably decided
    // here, allowing the smallest octave to be skipped.
    term[1] = searchSimplex2(s.rain[1], x * scale[1], z * scale[1]) * weight[1];
    partial += term[1];
    remaining = P9_SIMPLEX_ABS_BOUND * weight[0];
    verdict = p9RainIntervalVerdict(partial, remaining, d0, ti);
    if (verdict >= 0) return verdict != 0;

    // Truly close to a quantization boundary: evaluate octave 0 too.
    term[0] = searchSimplex2(s.rain[0], x * scale[0], z * scale[0]) * weight[0];

    // IMPORTANT: reconstruct the full sum in the original octave order rather
    // than using the heavy-first partial. This matches searchOctaveNoise4().
    double rainRaw = 0.0;
    rainRaw += term[0];
    rainRaw += term[1];
    rainRaw += term[2];
    rainRaw += term[3];

    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    rain = clamp01(rain);

    int ri = static_cast<int>(rain * 63.0);
    if (ri < 0) ri = 0;
    if (ri > 63) ri = 63;
    return p9QuantizedRainPasses(ti, ri);
}

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

    // Exact P3/P8 cold/hot shortcuts.
    if (f < 0.2f) return true;
    if (f >= 0.5f) return false;

    return p9AmbiguousRainPassesBounded(s, blockX, blockZ, d0, ti);
}

__device__ __forceinline__ void squareProbePoint864(
'@
$text = [regex]::Replace($text, $searchPattern, $newSearchBlock, 1)

# P8 has a second rain helper for the fused first temperature screen. Replace
# only that helper; all P8 lazy-state logic and the fused SIMD screen remain.
$p8RainPattern = '(?s)__device__ __forceinline__ bool p8AmbiguousRainPasses\(.*?\r?\n\}'
$p8RainMatches = [regex]::Matches($text, $p8RainPattern)
if ($p8RainMatches.Count -ne 1) {
    throw "Expected exactly one P8 ambiguous-rain helper, found $($p8RainMatches.Count)."
}
$p8RainWrapper = @'
__device__ __forceinline__ bool p8AmbiguousRainPasses(
        const SearchClimateState& s,
        int blockX,
        int blockZ,
        double d0,
        int ti
) {
    return p9AmbiguousRainPassesBounded(s, blockX, blockZ, d0, ti);
}
'@
$text = [regex]::Replace($text, $p8RainPattern, $p8RainWrapper.TrimEnd(), 1)

# Upgrade the banner while preserving the P8 marker needed for provenance and
# rollback checks. Handle either a literal \\n or a correctly escaped newline form.
$text = $text.Replace(
    'P8 scout: TUNDRA-only | lazy rain init | fused center+gate SIMD | warp votes | GPU compaction | tuned 4x16',
    'P9 scout: TUNDRA-only | lazy rain + bounded heavy-first octaves | fused SIMD | warp votes | GPU compaction | tuned 4x16'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P9 bounded progressive rain evaluation.' -ForegroundColor Green
Write-Host 'P8 lazy rain-state initialization is preserved.'
Write-Host 'Ambiguous rain now evaluates octaves 3 -> 2, then uses a proven remainder bound.'
Write-Host 'Only unresolved points pay for octave 1; only true boundary cases pay for octave 0.'
Write-Host 'If all four are needed, the final rain sum is rebuilt in legacy 0 -> 1 -> 2 -> 3 order.'
Write-Host 'The 2.732 simplex bound is analytic/conservative, not empirical.'
Write-Host 'Exact verifier, 864x864 semantics, GPU compaction, tuned batch and jackpot recall are unchanged.'
