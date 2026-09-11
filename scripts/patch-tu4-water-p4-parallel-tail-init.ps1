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

# Insert the P4 direct-jump helper immediately after the P3 helper. This avoids
# brittle full-block matching against a locally generated P3/P3b source.
$p3HelperStart = $text.IndexOf('__device__ __forceinline__ void p3JumpToNoise5Tail(')
if ($p3HelperStart -lt 0) {
    throw 'Could not locate p3JumpToNoise5Tail.'
}
$p3HelperEnd = $text.IndexOf("`n}`n", $p3HelperStart)
if ($p3HelperEnd -lt 0) {
    throw 'Could not locate end of p3JumpToNoise5Tail.'
}
$p3HelperEnd += 3

$p4Helper = @'

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
$text = $text.Insert($p3HelperEnd, $p4Helper)

# Scope all remaining edits to the scout kernel. The old P4 patcher tried to
# match the whole generated P3 block byte-for-byte and failed on harmless local
# formatting differences. Here we replace the unique region between the scout
# RNG declaration and the d7 calculation instead.
$kernelStart = $text.IndexOf('__global__ void waterScoutKernel(')
if ($kernelStart -lt 0) {
    throw 'Could not locate waterScoutKernel.'
}

$sharedOld = '    __shared__ p20::PerlinState perlin;'
$sharedPos = $text.IndexOf($sharedOld, $kernelStart)
if ($sharedPos -lt 0) {
    throw 'Could not locate scout shared Perlin state.'
}
$sharedNew = '    __shared__ p20::PerlinState tailPerlin[4];'
$text = $text.Remove($sharedPos, $sharedOld.Length).Insert($sharedPos, $sharedNew)

# Re-find positions after the shared-state edit.
$kernelStart = $text.IndexOf('__global__ void waterScoutKernel(')
$replaceStart = $text.IndexOf("    p20::JavaRandom rng;`n", $kernelStart)
if ($replaceStart -lt 0) {
    throw 'Could not locate P3 scout RNG block start.'
}
$replaceEndMarker = '    const double d7 = heightCenterFromNoise5(noise5);'
$replaceEnd = $text.IndexOf($replaceEndMarker, $replaceStart)
if ($replaceEnd -lt 0) {
    throw 'Could not locate P3 scout RNG block end.'
}

$newScout = @'
    // Four lanes construct the retained tail-octave permutations concurrently.
    // Each lane starts from the seed and jumps straight to its own octave.
    if (lane < 4) {
        p20::JavaRandom octaveRng;
        octaveRng.setSeed(seed);
        p4JumpToTailOctave(octaveRng, lane);
        p20::initPerlin(octaveRng, tailPerlin[lane]);
    }
    __syncthreads();

    double noise5 = 0.0;
    // Same four dominant tail octaves and same 32 spatial samples as P3.
    // Only permutation initialization is parallelized.
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
$text = $text.Remove($replaceStart, $replaceEnd - $replaceStart).Insert($replaceStart, $newScout)

$text = $text.Replace(
    'Scout P3: 32-point tail4 + affine RNG jump; rank by continuous d7 sum; exact finalists unchanged.',
    'Scout P4: 32-point tail4 + parallel 4-octave init + affine RNG jumps; d7 ranking; exact finalists unchanged.'
)

# Add the continuous scout score to exact record lines when the P3 record tail
# is present. This is diagnostic only and does not change ranking or exactness.
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
Write-Host 'P4 patcher v2: scout block located structurally instead of exact full-text matching.'
Write-Host 'Retained noise5 permutations now build concurrently in four lanes.'
Write-Host 'Spatial scout stays 32 points and tail4; ranking stays continuous d7Sum.'
Write-Host 'Exact 864x864 terrain measurement and record metric are unchanged.'
