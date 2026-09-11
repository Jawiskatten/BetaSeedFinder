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

if ($text.Contains('TUNDRA_P12_REPLAY_TAIL')) {
    Write-Host 'Tundra P12 replay-tail optimization is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P11_FAST_PERMUTATION_RNG')) {
    throw 'P12 requires P11 fast permutation RNG first.'
}
if (-not $text.Contains('TUNDRA_P10_PARALLEL_OCTAVE_INIT')) {
    throw 'P12 requires P10 parallel octave initialization.'
}
if (-not $text.Contains('TUNDRA_P8_LAZY_RAIN')) {
    throw 'P12 requires P8 lazy rain.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P12 requires exact 864x864 square semantics.'
}

$backupPath = $sourcePath + '.p11-before-p12.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# ---------------------------------------------------------------------------
# P12 strategy
# ---------------------------------------------------------------------------
# P10/P11 parallel octave owners must replay every preceding octave's Java RNG
# stream so they arrive at the exact start state for their assigned octave.
# During replay the permutation values are irrelevant: only RNG state matters.
#
# Java Random.nextInt(bound), bound <= 256, accepts a 31-bit draw iff
#
#   bits < 2^31 - (2^31 mod bound)
#
# and otherwise consumes another draw and retries. Therefore RNG-only replay
# does NOT need to calculate bits % bound at all. P11's exact reciprocal modulo
# is still used by real Fisher-Yates builds, where the sampled value is needed.
# P12 uses an 8-bit table of the rejection-tail size (2^31 mod bound), so each
# replay draw is just the normal LCG step plus one compare in the overwhelmingly
# common accepted case. Rejections consume additional draws exactly as Java does.
# ---------------------------------------------------------------------------

$consumePattern = '(?s)__device__ __forceinline__ void p10ConsumeSearchPerlinRng\(p20::JavaRandom& random\) \{.*?\r?\n\}'
$matches = [regex]::Matches($text, $consumePattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one p10ConsumeSearchPerlinRng helper, found $($matches.Count)."
}

$replacement = @'
// TUNDRA_P12_REPLAY_TAIL
// For bounds 1..256 this stores 2^31 % bound. Values fit in one byte because
// the remainder is always smaller than bound. Entry 0 is unused.
__device__ __constant__ unsigned char P12_NEXTINT_REJECT_TAIL[257] = {
    0, 0, 0, 2, 0, 3, 2, 2, 0, 2, 8, 2, 8, 11, 2, 8,
    0, 9, 2, 3, 8, 2, 2, 6, 8, 23, 24, 11, 16, 8, 8, 2,
    0, 2, 26, 23, 20, 22, 22, 11, 8, 39, 2, 8, 24, 38, 6, 21,
    32, 44, 48, 26, 24, 21, 38, 13, 16, 41, 8, 55, 8, 59, 2, 2,
    0, 63, 2, 50, 60, 29, 58, 40, 56, 16, 22, 23, 60, 2, 50, 25,
    48, 65, 80, 80, 44, 43, 8, 8, 24, 67, 38, 37, 52, 2, 68, 3,
    32, 66, 44, 2, 48, 34, 26, 83, 24, 23, 74, 68, 92, 92, 68, 59,
    16, 8, 98, 98, 8, 11, 114, 9, 8, 90, 120, 80, 64, 23, 2, 8,
    0, 8, 128, 124, 68, 79, 50, 38, 128, 17, 98, 90, 128, 68, 40, 24,
    128, 8, 16, 44, 96, 139, 98, 2, 136, 128, 2, 33, 128, 125, 104, 74,
    128, 121, 146, 50, 80, 68, 80, 87, 128, 141, 128, 155, 8, 48, 8, 23,
    112, 173, 156, 63, 128, 98, 128, 59, 144, 133, 2, 145, 68, 65, 98, 169,
    128, 54, 66, 128, 44, 44, 2, 23, 48, 50, 34, 37, 128, 203, 186, 29,
    128, 79, 128, 131, 180, 182, 68, 8, 200, 2, 92, 89, 68, 128, 170, 115,
    128, 173, 8, 88, 212, 195, 98, 2, 8, 4, 128, 68, 232, 104, 128, 55,
    128, 128, 90, 65, 120, 93, 80, 193, 64, 80, 148, 187, 128, 167, 8, 128,
    0
};

// Consume one Java nextInt(bound) call while discarding its returned value.
// Acceptance depends only on the raw 31-bit draw and the rejection-tail size;
// no modulo/remainder calculation is required for RNG-only replay.
__device__ __forceinline__ void p12ConsumeNextIntOnlyExact(
        p20::JavaRandom& random,
        int bound
) {
    const unsigned int tail = static_cast<unsigned int>(P12_NEXTINT_REJECT_TAIL[bound]);
    const unsigned int limit = 0x80000000u - tail;
    unsigned int bits;
    do {
        bits = random.nextBits(31);
    } while (bits >= limit);
}

__device__ __forceinline__ void p10ConsumeSearchPerlinRng(p20::JavaRandom& random) {
    // Exact affine six-step jump over discarded a/b/c nextDouble values.
    p11AdvanceJava6(random);

    // Replay Fisher-Yates RNG consumption only. The permutation itself is not
    // built here, so the sampled remainder is useless; only Java's rejection
    // count can affect the state seen by the following octave.
    for (int i = 0; i < 256; ++i) {
        p12ConsumeNextIntOnlyExact(random, 256 - i);
    }
}
'@

$text = [regex]::Replace($text, $consumePattern, $replacement.TrimEnd(), 1)

$text = $text.Replace(
    'P11 scout: TUNDRA-only | fast exact permutation RNG | parallel octave init | lazy bounded rain | warp votes | GPU compaction | tuned 4x16',
    'P12 scout: TUNDRA-only | zero-mod replay | fast exact permutation RNG | parallel octave init | lazy bounded rain | warp votes | tuned 4x16'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P12 zero-modulo exact RNG replay.' -ForegroundColor Green
Write-Host 'Preceding-octave RNG replay no longer computes nextInt remainders.'
Write-Host 'Replay uses the exact Java acceptance threshold 2^31 - (2^31 mod bound).'
Write-Host 'Rare Java rejection retries still consume the same additional RNG draws.'
Write-Host 'Real permutation construction still uses P11 exact sampled nextInt values.'
Write-Host 'Biome math, permutation contents, probes, acceptance set and exact verifier are unchanged.'
Write-Host 'P11/P10/P9/P8 optimizations and true 864x864 Tundra jackpot recall are preserved.'
