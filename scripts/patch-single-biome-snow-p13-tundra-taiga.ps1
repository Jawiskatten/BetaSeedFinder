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
# Exact Beta 1.7.3 identity used by this patch
# ---------------------------------------------------------------------------
# After the game's 64-level climate quantization, let f be temperature and
# f1 = rain * f. The lookup does:
#   f < 0.1                     -> TUNDRA
#   f1 < 0.2 && f < 0.5         -> TUNDRA
#   otherwise f < 0.5           -> TAIGA
# The SWAMPLAND branch needs f1 > 0.5, impossible when f < 0.5 because
# rain <= 1, so f1 <= f. Conversely neither TUNDRA nor TAIGA occurs at f>=0.5.
# Therefore {TUNDRA,TAIGA} is EXACTLY quantized f < 0.5 and rain is irrelevant.
# ---------------------------------------------------------------------------

# Normal scout point predicate: exact Tundra-or-Taiga test, temperature only.
$searchPattern = '(?s)__device__ __forceinline__ bool searchIsTundraAt\(.*?\r?\n\}'
$searchMatches = [regex]::Matches($text, $searchPattern)
if ($searchMatches.Count -ne 1) {
    throw "Expected exactly one searchIsTundraAt helper, found $($searchMatches.Count)."
}
$newSearch = @'
// SNOW_P13_TUNDRA_TAIGA
// Exact Beta 1.7.3 predicate for the union {TUNDRA, TAIGA}.
// The union is exactly quantized temperature f < 0.5, so rain is irrelevant.
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

# Fused first screen: there is no longer an ambiguous/rain class.
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
    return f < 0.5f ? 0 : 2; // 0 pass, 2 fail; class 1 no longer exists.
}
'@
$text = [regex]::Replace($text, $tempPattern, $newTemp.TrimEnd(), 1)

# Do not construct lazy rain state for survivors anymore.
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

# Remove the unreachable first-point rain evaluation too.
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

# Exact verifier: failure means a biome outside {TUNDRA,TAIGA}.
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

# True 864x864 coverage now counts both snow biomes.
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

# Console wording only; host selection/log structures stay compatible.
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
Write-Host 'Exact rule: every position in the 864x864 square must be TUNDRA or TAIGA.'
Write-Host 'Scout rule is exact quantized temperature f < 0.5; rain is skipped entirely.'
Write-Host 'Exact verifier and full-square coverage now treat TUNDRA+TAIGA as one allowed set.'
Write-Host 'P12 temperature/RNG optimizations, square probes, GPU compaction and jackpot recall are preserved.'
