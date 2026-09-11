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

if ($text.Contains('TU4_WATER_P3_JUMP_SCOUT')) {
    Write-Host 'TU4 Water P3 jump scout is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P2_FAST_SCOUT')) {
    throw 'P3 requires TU4 Water P2 fast scout first.'
}

$backupPath = $sourcePath + '.p2-before-water-p3.bak'
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

# The P2 scout spends most of its time serially replaying 70 complete Perlin
# constructor RNG streams in lane 0 before it reaches noise5 octave 12. Each
# constructor normally consumes 262 Java-LCG draws (three nextDouble calls and
# 256 nextInt draws). Java nextInt rejection is extraordinarily rare for bounds
# <=256. P3 therefore jumps over the common no-rejection path in one affine LCG
# operation. This is ONLY a scout optimization: the exact finalist evaluator is
# untouched and remains bit-exact. A rare skipped-stream rejection can perturb a
# seed's scout rank, never its reported land/water result.
$insertAfter = @'
__device__ __forceinline__ void consumePerlinRngOnly(p20::JavaRandom& random) {
    advanceJava6(random); // a,b,c: three nextDouble() calls = six LCG draws
    for (int i = 0; i < 256; ++i) {
        consumeNextIntOnlyExact(random, 256 - i);
    }
}
'@
$helper = @'

// TU4_WATER_P3_JUMP_SCOUT
// Common-path affine jump over 70 discarded Perlin constructors:
// 70 * (6 nextDouble LCG draws + 256 nextInt LCG draws) = 18,340 steps.
// Constants are the exact composition of Java's 48-bit LCG for 18,340 steps.
__device__ __forceinline__ void p3JumpToNoise5Tail(p20::JavaRandom& random) {
    random.state =
            (random.state * 0xDC270C086991ULL + 0x3E75AD5FE1A4ULL) & p20::JAVA_MASK;
}
'@
Replace-Once $insertAfter.TrimEnd() ($insertAfter.TrimEnd() + $helper) 'consumePerlinRngOnly helper'

$oldReplay = @'
        for (int i = 0; i < 58; ++i) consumePerlinRngOnly(rng);
        // Skip the first twelve noise5 octaves without constructing their
        // permutations. Exact Java RNG consumption is preserved.
        for (int i = 0; i < SCOUT_FIRST_NOISE5_OCTAVE; ++i) {
            consumePerlinRngOnly(rng);
        }
'@
$newReplay = @'
        // P3: jump directly to noise5 octave 12 on the overwhelmingly common
        // no-nextInt-rejection path instead of serially replaying 18,340 draws.
        p3JumpToNoise5Tail(rng);
'@
Replace-Once $oldReplay.TrimEnd() $newReplay.TrimEnd() 'P2 discarded Perlin replay loops'

# P2's 31/32 and 32/32 low-sample counts saturate quickly. The pasted runs also
# show strong exact records at 29/32 and 30/32. Rank by the continuous macro
# height sum first; use low-count only as a tie breaker.
$oldComparator = @'
            auto betterScout = [&](int a, int b) {
                if (hLow[a] != hLow[b]) return hLow[a] > hLow[b];
                return hSum[a] < hSum[b];
            };
'@
$newComparator = @'
            auto betterScout = [&](int a, int b) {
                if (hSum[a] != hSum[b]) return hSum[a] < hSum[b];
                return hLow[a] > hLow[b];
            };
'@
Replace-Once $oldComparator.TrimEnd() $newComparator.TrimEnd() 'P2 scout comparator'

$oldGlobal = @'
            if (hLow[batchBest] > globalScoutLow ||
                (hLow[batchBest] == globalScoutLow && hSum[batchBest] < globalScoutSum)) {
                globalScoutLow = hLow[batchBest];
                globalScoutSum = hSum[batchBest];
'@
$newGlobal = @'
            if (hSum[batchBest] < globalScoutSum ||
                (hSum[batchBest] == globalScoutSum && hLow[batchBest] > globalScoutLow)) {
                globalScoutLow = hLow[batchBest];
                globalScoutSum = hSum[batchBest];
'@
Replace-Once $oldGlobal.TrimEnd() $newGlobal.TrimEnd() 'global scout record comparison'

# Larger batches amortize launch/copy/sort overhead after the replay jump. Keep
# the same exact-check density as P2: 16 / 131072 == 4 / 32768 == 1 / 8192.
$text = $text.Replace('int batch = 32768;', 'int batch = 131072;')
$text = $text.Replace('int topExact = 4;', 'int topExact = 16;')
$text = $text.Replace('(default 32768)', '(default 131072)')
$text = $text.Replace('(default 4)', '(default 16)')

$text = $text.Replace(
    'Scout P2: 32-point checkerboard + dominant noise5 tail4; full exact terrain for top candidates.',
    'Scout P3: 32-point tail4 + affine RNG jump; rank by continuous d7 sum; exact finalists unchanged.'
)

$text = $text.Replace(
    '<< " scoutBest=" << globalScoutLow << "/32";',
    '<< " scoutBestD7=" << std::fixed << std::setprecision(3) << globalScoutSum\n                          << " scoutLow=" << globalScoutLow << "/32";'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P3 jump-ahead scout.' -ForegroundColor Green
Write-Host 'Removed the 70-Perlin serial RNG replay from the hot scout path.'
Write-Host 'Scout ranking now uses continuous d7Sum first; low sample count is only a tie-breaker.'
Write-Host 'Default batch: 131072; exact candidates: 16 (same exact-check density as P2).'
Write-Host 'Exact 864x864 terrain measurement and record metric are unchanged.'
