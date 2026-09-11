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

if ($text.Contains('TU4_WATER_P4_PARALLEL_TAIL_INIT')) {
    Write-Host 'TU4 Water P4 parallel tail init is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P3_JUMP_SCOUT')) {
    throw 'P4 requires TU4 Water P3 jump scout first.'
}

$backupPath = $sourcePath + '.p3-before-water-p4.bak'
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

# P3 still constructs the four retained noise5 Perlin permutations serially in
# lane 0. Each constructor has an independent Java RNG start state on the common
# no-nextInt-rejection path. P4 precomputes the affine LCG jump to each of those
# four starts and lets lanes 0..3 build them concurrently in separate shared
# PerlinState slots. All 32 sample lanes then evaluate the same four tail octaves.
#
# As in P3, this is a scout-only approximation around extremely rare Java
# nextInt rejection retries. The full 864x864 exact finalist evaluator is not
# changed and remains authoritative for every reported land/water record.
$oldHelper = @'
__device__ __forceinline__ void p3JumpToNoise5Tail(p20::JavaRandom& random) {
    random.state =
            (random.state * 0xDC270C086991ULL + 0x3E75AD5FE1A4ULL) & p20::JAVA_MASK;
}
'@
$newHelper = @'
__device__ __forceinline__ void p3JumpToNoise5Tail(p20::JavaRandom& random) {
    random.state =
            (random.state * 0xDC270C086991ULL + 0x3E75AD5FE1A4ULL) & p20::JAVA_MASK;
}

// TU4_WATER_P4_PARALLEL_TAIL_INIT
// Common-path affine start states for noise5 octaves 12..15. Each prior Perlin
// constructor consumes 262 Java-LCG draws when no nextInt rejection occurs.
__device__ __forceinline__ void p4JumpToTailOctave(
        p20::JavaRandom& random,
        int tailIndex
) {
    const std::uint64_t s = random.state;
    if (tailIndex == 0) {
        random.state = (s * 0xDC270C086991ULL + 0x3E75AD5FE1A4ULL) & p20::JAVA_MASK;
    } else if (tailIndex == 1) {
        random.state = (s * 0xAC6F30F60909ULL + 0x158AEFA61AE2ULL) & p20::JAVA_MASK;
    } else if (tailIndex == 2) {
        random.state = (s * 0x142C7D7F4CC1ULL + 0x366237F93230ULL) & p20::JAVA_MASK;
    } else {
        random.state = (s * 0xF3A8380212B9ULL + 0x7BAF38DCF70EULL) & p20::JAVA_MASK;
    }
}
'@
Replace-Once $oldHelper.TrimEnd() $newHelper.TrimEnd() 'P3 jump helper'

Replace-Once `
    '    __shared__ p20::PerlinState perlin;' `
    '    __shared__ p20::PerlinState tailPerlin[4];' `
    'single shared scout Perlin state'

$oldScout = @'
    p20::JavaRandom rng;
    if (lane == 0) {
        rng.setSeed(seed);
        // P3: jump directly to noise5 octave 12 on the overwhelmingly common
        // no-nextInt-rejection path instead of serially replaying 18,340 draws.
        p3JumpToNoise5Tail(rng);
    }
    __syncthreads();

    double noise5 = 0.0;
    // octave 12 starts at amplitude 2^-12. The four retained octaves dominate
    // the legacy weighted sum while cutting point evaluations by 4x.
    double amplitude = 1.0 / 4096.0;
    for (int octave = SCOUT_FIRST_NOISE5_OCTAVE; octave < 16; ++octave) {
        if (lane == 0) p20::initPerlin(rng, perlin);
        __syncthreads();

        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        noise5 += p20::perlin2(perlin, coarseX * scale, coarseZ * scale) * weight;

        __syncthreads();
        amplitude /= 2.0;
    }
'@
$newScout = @'
    // Four lanes construct the retained tail-octave permutations concurrently.
    // This removes the remaining serial permutation-build chain from P3.
    if (lane < 4) {
        p20::JavaRandom octaveRng;
        octaveRng.setSeed(seed);
        p4JumpToTailOctave(octaveRng, lane);
        p20::initPerlin(octaveRng, tailPerlin[lane]);
    }
    __syncthreads();

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
'@
Replace-Once $oldScout.TrimEnd() $newScout.TrimEnd() 'P3 serial retained-octave scout block'

$text = $text.Replace(
    'Scout P3: 32-point tail4 + affine RNG jump; rank by continuous d7 sum; exact finalists unchanged.',
    'Scout P4: 32-point tail4 + parallel 4-octave init + affine RNG jumps; d7 ranking; exact finalists unchanged.'
)

# Show the actual continuous scout score beside exact records so the next tuning
# pass can compare d7 ranking against real land count directly from pasted logs.
$oldRecordTail = '<< " scoutLow=" << hLow[idx] << "/32\n";'
$newRecordTail = @'
<< " scoutLow=" << hLow[idx] << "/32"
                              << " scoutD7=" << std::setprecision(3) << hSum[idx] << "\n";
'@.TrimEnd()
if ($text.Contains($oldRecordTail)) {
    $text = $text.Replace($oldRecordTail, $newRecordTail)
}

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P4 parallel tail-octave initialization.' -ForegroundColor Green
Write-Host 'Retained noise5 permutations now build concurrently in four lanes instead of serially in lane 0.'
Write-Host 'Spatial scout stays 32 points and tail4; ranking stays continuous d7Sum.'
Write-Host 'Exact 864x864 terrain measurement and record metric are unchanged.'
