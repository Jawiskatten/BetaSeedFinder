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

# ---------------------------------------------------------------------------
# Why this can be temperature-only and still be EXACT:
# ---------------------------------------------------------------------------
# Beta 1.7.3 first quantizes temperature/rain to the 64x64 biome lookup table.
# Let f = quantized temperature and f1 = quantized rain * f.
#
#   f < 0.1                        -> TUNDRA
#   f1 < 0.2 && f < 0.5            -> TUNDRA
#   otherwise, if f < 0.5          -> TAIGA
#
# The SWAMPLAND branch requires f1 > 0.5, which is impossible when f < 0.5
# because quantized rain <= 1 and therefore f1 <= f. Conversely, neither
# TUNDRA nor TAIGA can occur at f >= 0.5.
#
# Therefore the exact union {TUNDRA, TAIGA} is simply:
#
#                       quantized f < 0.5
#
# Rain is irrelevant for this target set. P13 keeps the exact temperature/blend
# math and exact 64-level quantization, but removes all rain work from the hot
# search path. Exact verification/coverage below still classify the real biome
# and accept either TUNDRA or TAIGA.
# ---------------------------------------------------------------------------

# 1) Replace P9's normal point predicate with the exact Tundra-or-Taiga test.
$searchPattern = '(?s)__device__ __forceinline__ bool searchIsTundraAt\(.*?\r?\n\}'
$searchMatches = [regex]::Matches($text, $searchPattern)
if ($searchMatches.Count -ne 1) {
    throw "Expected exactly one searchIsTundraAt helper, found $($searchMatches.Count)."
}

$newSearch = @'
// SNOW_P13_TUNDRA_TAIGA
// Exact Beta 1.7.3 target predicate for the union {TUNDRA, TAIGA}.
// After the game's 64-level temperature quantization this union is exactly
// f < 0.5, so rainfall never needs to be initialized or evaluated.
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

# 2) The fused first-stage classifier uses the same exact temperature rule.
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

    // Keep P8's existing return convention. There is no ambiguous/rain class
    // for the snow-biome union: 0 = pass, 2 = fail.
    return f < 0.5f ? 0 : 2;
}
'@
$text = [regex]::Replace($text, $tempPattern, $newTemp.TrimEnd(), 1)

# 3) Rain state is now provably irrelevant. Do not build it for survivors.
$rainInitBlock = @'
    if (alive) {
        p8InitRain(s, seed, lane);
    }
'@
if (-not $text.Contains($rainInitBlock)) {
    throw 'Could not find the P8 lazy-rain initialization call.'
}
$text = $text.Replace($rainInitBlock, @'
    // P13: no rain initialization. TUNDRA+TAIGA depends only on quantized temp.
'@)

# Remove the now-unreachable first-point rain check as well. This also makes it
# impossible for a future compiler decision to read the intentionally unbuilt rain state.
$firstRainPattern = '(?s)    bool firstRainFailed = false;\r?\n    if \(alive && firstClass == 1\) \{.*?\r?\n    \}'
$firstRainMatches = [regex]::Matches($text, $firstRainPattern)
if ($firstRainMatches.Count -ne 1) {
    throw "Expected exactly one first-screen rain block, found $($firstRainMatches.Count)."
}
$text = [regex]::Replace($text, $firstRainPattern, '    bool firstRainFailed = false; // P13: no ambiguous rain class.', 1)

# 4) Exact verifier: a mismatch means a biome outside {TUNDRA, TAIGA}, not a
#    biome different from the center. The center itself must also be in the set.
$exactPattern = '(?s)__global__ void exactKernel\(.*?\r?\n\}(?=\r?\n\r?\n__global__ void coverageKernel\()'
$exactMatches = [regex]::Matches($text, $exactPattern)
if ($exactMatches.Count -ne 1) {
    throw "Expected exactly one exactKernel before coverageKernel, found $($exactMatches.Count)."
}

$newExact = @'
__global__ void exactKernel(
        std::int64_t seed,
        int centerX,
        int centerZ,
        const ExactPoint* points,
        int pointCount,
        int target,
        ExactResult* result
) {
    const int lane = static_cast<int>(threadIdx.x);
    __shared__ ClimateState s;
    initClimate(s, seed);

    if (lane == 0) {
        s.baseBiome = static_cast<int>(biomeAt(s, centerX, centerZ));
        result->safeRadius = 0;
        result->firstMismatchD2 = -1;
        result->baseBiome = s.baseBiome;
        if (s.baseBiome != static_cast<int>(TUNDRA) &&
            s.baseBiome != static_cast<int>(TAIGA)) {
            result->firstMismatchD2 = 0;
        }
    }
    __syncthreads();

    if (s.baseBiome != static_cast<int>(TUNDRA) &&
        s.baseBiome != static_cast<int>(TAIGA)) {
        return;
    }

    for (int base = 0; base < pointCount; base += EXACT_THREADS) {
        if (lane == 0) s.groupMinD2 = 0x7fffffff;
        __syncthreads();

        const int idx = base + lane;
        if (idx < pointCount) {
            const ExactPoint p = points[idx];
            const unsigned char b = biomeAt(s, centerX + p.dx, centerZ + p.dz);
            if (b != static_cast<unsigned char>(TUNDRA) &&
                b != static_cast<unsigned char>(TAIGA)) {
                atomicMin(&s.groupMinD2, p.d2);
            }
        }
        __syncthreads();

        if (s.groupMinD2 != 0x7fffffff) {
            if (lane == 0) {
                const int d2 = s.groupMinD2;
                int root = static_cast<int>(sqrt(static_cast<double>(d2)));
                while ((root + 1) * (root + 1) <= d2) ++root;
                while (root * root > d2) --root;
                const int ceilRoot = root * root == d2 ? root : root + 1;
                int safe = ceilRoot - 1;
                if (safe < 0) safe = 0;
                if (safe > target) safe = target;
                result->safeRadius = safe;
                result->firstMismatchD2 = d2;
            }
            return;
        }
    }

    if (lane == 0) {
        result->safeRadius = target;
        result->firstMismatchD2 = -1;
    }
}
'@
$text = [regex]::Replace($text, $exactPattern, $newExact.TrimEnd(), 1)

# 5) Full-square coverage: count every TUNDRA or TAIGA position.
$coveragePattern = '(?s)__global__ void coverageKernel\(.*?\r?\n\}(?=\r?\n\r?\nstd::uint64_t parseU64\()'
$coverageMatches = [regex]::Matches($text, $coveragePattern)
if ($coverageMatches.Count -ne 1) {
    throw "Expected exactly one coverageKernel, found $($coverageMatches.Count)."
}

$newCoverage = @'
__global__ void coverageKernel(
        std::int64_t seed,
        int centerX,
        int centerZ,
        const ExactPoint* points,
        int pointCount,
        CoverageResult* result
) {
    const int lane = static_cast<int>(threadIdx.x);
    __shared__ ClimateState s;
    __shared__ int sameCount;
    initClimate(s, seed);

    if (lane == 0) {
        const unsigned char centerBiome = biomeAt(s, centerX, centerZ);
        sameCount = (centerBiome == static_cast<unsigned char>(TUNDRA) ||
                     centerBiome == static_cast<unsigned char>(TAIGA)) ? 1 : 0;
    }
    __syncthreads();

    int localSame = 0;
    for (int idx = lane; idx < pointCount; idx += EXACT_THREADS) {
        const ExactPoint p = points[idx];
        const unsigned char b = biomeAt(s, centerX + p.dx, centerZ + p.dz);
        if (b == static_cast<unsigned char>(TUNDRA) ||
            b == static_cast<unsigned char>(TAIGA)) {
            ++localSame;
        }
    }
    atomicAdd(&sameCount, localSame);
    __syncthreads();

    if (lane == 0) {
        result->sameCount = sameCount;
        result->totalCount = pointCount + 1;
    }
}
'@
$text = [regex]::Replace($text, $coveragePattern, $newCoverage.TrimEnd(), 1)

# 6) Make the console semantics explicit. Keep the data structures/log format
#    compatible so all existing host-side selection/checkpoint behavior survives.
$text = $text.Replace(
    'P12 scout: TUNDRA-only | zero-mod replay | fast exact permutation RNG | parallel octave init | lazy bounded rain | warp votes | tuned 4x16',
    'P13 scout: TUNDRA+TAIGA | exact temp-only snow predicate | zero rain work | fast permutation RNG | warp votes | tuned 4x16'
)
$text = $text.Replace('sameBiomeBlocks=', 'snowBiomeBlocks=')
$text = $text.Replace('is one biome.', 'contains only TUNDRA/TAIGA.')
$text = $text.Replace('all-Tundra', 'Tundra-or-Taiga')

# P4's fast probe-record path hard-coded the old target label.
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
Write-Host 'Exact target rule: every position in the 864x864 square must be TUNDRA or TAIGA.'
Write-Host 'Scout is now exact temperature-only: quantized f < 0.5.'
Write-Host 'Rain permutation construction/evaluation is skipped completely in the search hot path.'
Write-Host 'Exact verifier and realCoverage now treat TUNDRA+TAIGA as one allowed set.'
Write-Host 'All P12 RNG/temp optimizations, square probes, GPU compaction and jackpot recall are preserved.'
