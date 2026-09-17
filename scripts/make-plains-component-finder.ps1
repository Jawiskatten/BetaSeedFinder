param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$baseSource = Join-Path $ProjectRoot 'native\src\single_biome_radius.cpp'
$outSource = Join-Path $ProjectRoot 'native\src\plains_component_finder.cpp'
if (-not (Test-Path $baseSource -PathType Leaf)) {
    throw "Single-biome source not found: $baseSource"
}

$text = [System.IO.File]::ReadAllText($baseSource).Replace("`r`n", "`n")

# Keep the optimized Rainforest finder untouched. Generate a dedicated source
# from the locally-patched P17 stack.
if (-not $text.Contains('P17_TRUE_RAINFOREST_SIZE')) {
    throw 'Plains generator expects local single_biome_radius.cpp to have P17_TRUE_RAINFOREST_SIZE applied.'
}
if (-not $text.Contains('TUNDRA_P12_REPLAY_TAIL')) {
    throw 'Plains generator expects the optimized P12 search RNG stack.'
}

# ---------------------------------------------------------------------------
# 1) Scout: PLAINS + SEASONAL_FOREST are one allowed connected region.
#    Search square target=400 => [-400,+399]^2 = 800x800 blocks.
#    The 8x8 proxy rewards connected sample count AND compact shape.
# ---------------------------------------------------------------------------
$scoutPattern = '(?s)// P17_TRUE_RAINFOREST_SIZE.*?\n// TUNDRA_P4_GPU_COMPACTION'
$scoutMatches = [regex]::Matches($text, $scoutPattern)
if ($scoutMatches.Count -ne 1) {
    throw "Expected exactly one P17 scout region, found $($scoutMatches.Count)."
}

$newScout = @'
// P22_PLAINS_SEASONAL_SHAPE
// P18_LARGEST_PLAINS_400_SQUARE
// Allowed climate biomes: PLAINS or SEASONAL_FOREST.
// Objective after exact terrain masking: largest useful connected dry region,
// with compactness rewarded so thin/holey shapes rank below similarly-sized
// broad, filled regions.

__device__ __forceinline__ void p18InitAllSearchClimate(
        SearchClimateState& s,
        std::int64_t seed,
        int laneInSeed
) {
    bool active = false;
    int octave = 0;
    std::uint64_t multiplier = 0ULL;
    SearchPerlinState* destination = nullptr;

    if (laneInSeed < 4) {
        active = true;
        octave = laneInSeed;
        multiplier = 9871ULL;
        destination = &s.temp[octave];
    } else if (laneInSeed < 6) {
        active = true;
        octave = laneInSeed - 4;
        multiplier = 543321ULL;
        destination = &s.blend[octave];
    } else if (laneInSeed < 10) {
        active = true;
        octave = laneInSeed - 6;
        multiplier = 39811ULL;
        destination = &s.rain[octave];
    }
    if (!active) return;

    p20::JavaRandom rng;
    rng.setSeed(multipliedSeed(seed, multiplier));
    for (int prior = 0; prior < octave; ++prior) {
        p10ConsumeSearchPerlinRng(rng);
    }
    initSearchPerlin(rng, *destination);
}

__device__ __forceinline__ bool p18IsPlainsAt(
        const SearchClimateState& s,
        int blockX,
        int blockZ
) {
    const double x = static_cast<double>(blockX);
    const double z = static_cast<double>(blockZ);

    const double tempRaw = searchOctaveNoise4(
            s.temp, x, z, 0.02500000037252903, 0.25);
    const double rainRaw = searchOctaveNoise4(
            s.rain, x, z, 0.05000000074505806, 0.3333333333333333);
    const double blendRaw = searchOctaveNoise2(s.blend, x, z);

    const double d0 = blendRaw * 1.1 + 0.5;
    double temperature = (tempRaw * 0.15 + 0.7) * 0.99 + d0 * 0.01;
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    temperature = 1.0 - (1.0 - temperature) * (1.0 - temperature);
    temperature = clamp01(temperature);
    rain = clamp01(rain);

    const unsigned char biome = classifyQuantizedBiome(temperature, rain);
    return biome == static_cast<unsigned char>(PLAINS) ||
           biome == static_cast<unsigned char>(SEASONAL_FOREST);
}

__device__ __forceinline__ int p18PopCount64(unsigned long long value) {
    return __popcll(value);
}

__device__ __forceinline__ int p18LargestSampleComponent(
        unsigned long long mask,
        int& bestShapeWeight
) {
    constexpr unsigned long long COL0 = 0x0101010101010101ULL;
    constexpr unsigned long long COL7 = 0x8080808080808080ULL;
    unsigned long long remaining = mask;
    int bestCount = 0;
    int bestScore = -1;
    bestShapeWeight = 0;

    while (remaining != 0ULL) {
        const unsigned long long seedBit = remaining & (~remaining + 1ULL);
        unsigned long long component = 0ULL;
        unsigned long long frontier = seedBit;
        while (frontier != 0ULL) {
            component |= frontier;
            unsigned long long expanded = 0ULL;
            expanded |= (frontier & ~COL0) >> 1;
            expanded |= (frontier & ~COL7) << 1;
            expanded |= frontier >> 8;
            expanded |= frontier << 8;
            frontier = expanded & mask & ~component;
        }
        remaining &= ~component;

        const int count = p18PopCount64(component);
        int minCol = 8, maxCol = -1, minRow = 8, maxRow = -1;
        unsigned long long bits = component;
        while (bits != 0ULL) {
            const int bit = __ffsll(static_cast<long long>(bits)) - 1;
            const int row = bit >> 3;
            const int col = bit & 7;
            if (col < minCol) minCol = col;
            if (col > maxCol) maxCol = col;
            if (row < minRow) minRow = row;
            if (row > maxRow) maxRow = row;
            bits &= bits - 1ULL;
        }

        const int width = maxCol - minCol + 1;
        const int height = maxRow - minRow + 1;
        const int bboxArea = width * height;
        const int fillPermille = bboxArea > 0 ? (count * 1000) / bboxArea : 0;
        const int longSide = width > height ? width : height;
        const int shortSide = width < height ? width : height;
        const int aspectPermille = longSide > 0 ? (shortSide * 1000) / longSide : 0;

        // 70% area base + up to 20% bbox fill + up to 10% balanced aspect.
        const int shapeWeight = 700 + fillPermille / 5 + aspectPermille / 10;
        const int score = count * shapeWeight; // <= 64,000, fits ushort
        if (score > bestScore || (score == bestScore && count > bestCount)) {
            bestScore = score;
            bestCount = count;
            bestShapeWeight = shapeWeight;
        }
    }
    return bestCount;
}

__global__ void searchKernel(
        std::uint64_t sequence,
        std::uint64_t startAttempt,
        int count,
        int centerX,
        int centerZ,
        const int* probeDx,
        const int* probeDz,
        const int* ringRadii,
        int ringCount,
        int minRequiredRadius,
        unsigned short* probeRadius,
        unsigned char* baseBiome
) {
    (void)probeDx;
    (void)probeDz;
    (void)minRequiredRadius;

    const int globalLane = static_cast<int>(threadIdx.x);
    const int seedGroup = globalLane / SEARCH_LANES_PER_SEED;
    const int lane = globalLane - seedGroup * SEARCH_LANES_PER_SEED;
    const int seedIndex = static_cast<int>(blockIdx.x) * SEARCH_SEEDS_PER_BLOCK + seedGroup;
    const bool validSeed = seedIndex < count;

    __shared__ SearchClimateState states[SEARCH_SEEDS_PER_BLOCK];
    SearchClimateState& s = states[seedGroup];

    std::int64_t seed = 0;
    if (validSeed) {
        const std::uint64_t attempt = startAttempt + static_cast<std::uint64_t>(seedIndex);
        seed = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(sequence, attempt));
        p18InitAllSearchClimate(s, seed, lane);
    }
    __syncthreads();

    if (!validSeed) return;

    const int target = ringCount > 0 ? ringRadii[ringCount - 1] : 400;
    unsigned long long allowedMask = 0ULL;

    for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
        const int logical = base + lane;
        bool pass = false;
        if (logical < 64) {
            const int row = logical >> 3;
            const int col = logical & 7;
            const int dx = -target + ((2 * col + 1) * target) / 8;
            const int dz = -target + ((2 * row + 1) * target) / 8;
            pass = p18IsPlainsAt(s, centerX + dx, centerZ + dz);
        }

        const unsigned long long votes = __ballot(pass ? 1 : 0);
        const int laneInWave = globalLane % warpSize;
        const int seedBaseInWave = laneInWave - lane;
        const unsigned long long chunk = (votes >> seedBaseInWave) & 0xFFFFULL;
        if (lane == 0) allowedMask |= chunk << base;
    }

    if (lane == 0) {
        int shapeWeight = 0;
        const int connectedSamples = p18LargestSampleComponent(allowedMask, shapeWeight);
        const int coarseScore = connectedSamples * shapeWeight;
        probeRadius[seedIndex] = static_cast<unsigned short>(coarseScore);
        baseBiome[seedIndex] = connectedSamples > 0
                ? static_cast<unsigned char>(PLAINS) : 255;
    }
}

// TUNDRA_P4_GPU_COMPACTION
'@
$text = [regex]::Replace($text, $scoutPattern, $newScout.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 2) Exact climate bitmap: either PLAINS or SEASONAL_FOREST is allowed.
# ---------------------------------------------------------------------------
$mapPattern = '(?s)__global__ void p16RainforestMapKernel\(.*?\n\}'
$mapMatches = [regex]::Matches($text, $mapPattern)
if ($mapMatches.Count -ne 1) {
    throw "Expected exactly one p16RainforestMapKernel, found $($mapMatches.Count)."
}
$newMapKernel = @'
__global__ void p18PlainsMapKernel(
        std::int64_t seed,
        int centerX,
        int centerZ,
        int target,
        unsigned char* map
) {
    const int lane = static_cast<int>(threadIdx.x);
    __shared__ ClimateState s;
    initClimate(s, seed);

    const int side = 2 * target;
    const int total = side * side;
    for (int idx = lane; idx < total; idx += EXACT_THREADS) {
        const int localX = idx % side;
        const int localZ = idx / side;
        const int blockX = centerX - target + localX;
        const int blockZ = centerZ - target + localZ;
        const unsigned char biome = biomeAt(s, blockX, blockZ);
        map[idx] = (biome == static_cast<unsigned char>(PLAINS) ||
                    biome == static_cast<unsigned char>(SEASONAL_FOREST)) ? 1 : 0;
    }
}
'@
$text = [regex]::Replace($text, $mapPattern, $newMapKernel.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 3) Exact finite-square flood fill. P20 later masks ocean/sea columns to zero.
#    Ranking uses connected area weighted by compactness, while safeRadius keeps
#    the actual connected block area for reporting/jackpot semantics.
# ---------------------------------------------------------------------------
$runPattern = '(?s)ExactResult runExact\(.*?return result;\r?\n\}'
$runMatches = [regex]::Matches($text, $runPattern)
if ($runMatches.Count -ne 1) {
    throw "Expected exactly one runExact function, found $($runMatches.Count)."
}
$newRun = @'
ExactResult runExact(
        std::int64_t seed,
        int centerX,
        int centerZ,
        int target,
        const ExactPoint* dPoints,
        int pointCount,
        ExactResult* dResult
) {
    (void)dPoints;
    (void)pointCount;
    (void)dResult;

    const int side = 2 * target;
    const int total = side * side;
    unsigned char* dMap = nullptr;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dMap),
                        static_cast<std::size_t>(total) * sizeof(unsigned char)));

    hipLaunchKernelGGL(
            p18PlainsMapKernel,
            dim3(1), dim3(EXACT_THREADS), 0, 0,
            seed, centerX, centerZ, target, dMap);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    std::vector<unsigned char> map(static_cast<std::size_t>(total));
    HIP_CHECK(hipMemcpy(
            map.data(), dMap,
            static_cast<std::size_t>(total) * sizeof(unsigned char),
            hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(dMap));

    std::vector<int> queue(static_cast<std::size_t>(total));
    ExactResult result{};
    result.safeRadius = 0;          // actual connected block area
    result.firstMismatchD2 = -1;   // P22 shape score
    result.baseBiome = static_cast<int>(PLAINS);
    result.componentWidthX = 0;
    result.componentHeightZ = 0;
    result.minX = result.maxX = centerX;
    result.minZ = result.maxZ = centerZ;
    result.touchesBoundary = 0;
    result.measurementHalfSize = 0; // bbox fill permille
    result.expansionCount = 0;      // aspect permille

    for (int start = 0; start < total; ++start) {
        if (map[static_cast<std::size_t>(start)] != 1) continue;

        int head = 0;
        int tail = 0;
        queue[static_cast<std::size_t>(tail++)] = start;
        map[static_cast<std::size_t>(start)] = 2;

        int area = 0;
        int minLX = side, maxLX = -1;
        int minLZ = side, maxLZ = -1;

        while (head < tail) {
            const int idx = queue[static_cast<std::size_t>(head++)];
            const int x = idx % side;
            const int z = idx / side;
            ++area;
            if (x < minLX) minLX = x;
            if (x > maxLX) maxLX = x;
            if (z < minLZ) minLZ = z;
            if (z > maxLZ) maxLZ = z;

            if (x > 0) {
                const int n = idx - 1;
                if (map[static_cast<std::size_t>(n)] == 1) {
                    map[static_cast<std::size_t>(n)] = 2;
                    queue[static_cast<std::size_t>(tail++)] = n;
                }
            }
            if (x + 1 < side) {
                const int n = idx + 1;
                if (map[static_cast<std::size_t>(n)] == 1) {
                    map[static_cast<std::size_t>(n)] = 2;
                    queue[static_cast<std::size_t>(tail++)] = n;
                }
            }
            if (z > 0) {
                const int n = idx - side;
                if (map[static_cast<std::size_t>(n)] == 1) {
                    map[static_cast<std::size_t>(n)] = 2;
                    queue[static_cast<std::size_t>(tail++)] = n;
                }
            }
            if (z + 1 < side) {
                const int n = idx + side;
                if (map[static_cast<std::size_t>(n)] == 1) {
                    map[static_cast<std::size_t>(n)] = 2;
                    queue[static_cast<std::size_t>(tail++)] = n;
                }
            }
        }

        const int widthX = maxLX - minLX + 1;
        const int heightZ = maxLZ - minLZ + 1;
        const int bboxArea = widthX * heightZ;
        const int fillPermille = bboxArea > 0 ? (area * 1000) / bboxArea : 0;
        const int longSide = widthX > heightZ ? widthX : heightZ;
        const int shortSide = widthX < heightZ ? widthX : heightZ;
        const int aspectPermille = longSide > 0 ? (shortSide * 1000) / longSide : 0;
        const int shapeWeight = 700 + fillPermille / 5 + aspectPermille / 10;
        const int shapeScore = static_cast<int>(
                (static_cast<long long>(area) * shapeWeight) / 1000LL);

        if (shapeScore > result.firstMismatchD2 ||
            (shapeScore == result.firstMismatchD2 && area > result.safeRadius)) {
            result.safeRadius = area;
            result.firstMismatchD2 = shapeScore;
            result.componentWidthX = widthX;
            result.componentHeightZ = heightZ;
            result.minX = centerX - target + minLX;
            result.maxX = centerX - target + maxLX;
            result.minZ = centerZ - target + minLZ;
            result.maxZ = centerZ - target + maxLZ;
            result.touchesBoundary =
                    (minLX == 0 || maxLX == side - 1 ||
                     minLZ == 0 || maxLZ == side - 1) ? 1 : 0;
            result.measurementHalfSize = fillPermille;
            result.expansionCount = aspectPermille;
        }
    }

    return result;
}
'@
$text = [regex]::Replace($text, $runPattern, $newRun.TrimEnd(), 1)

# P17 rejected open-at-cap components. This objective is clipped to the square,
# so boundary-touching components are valid. Host records compare shapeScore.
$recordPattern = '(?s)if \(result\.touchesBoundary != 0\) \{.*?if \(result\.touchesBoundary == 0 && result\.safeRadius > bestExact\) \{'
$recordMatches = [regex]::Matches($text, $recordPattern)
if ($recordMatches.Count -ne 1) {
    throw "Expected exactly one P17 record gate block, found $($recordMatches.Count)."
}
$text = [regex]::Replace($text, $recordPattern, 'if (result.firstMismatchD2 > bestExact) {', 1)

$bestAssign = '                bestExact = result.safeRadius;'
$bestAssignCount = ([regex]::Matches($text, [regex]::Escape($bestAssign))).Count
if ($bestAssignCount -ne 1) {
    throw "Expected exactly one bestExact assignment, found $bestAssignCount."
}
$text = $text.Replace($bestAssign, '                bestExact = result.firstMismatchD2;')

# ---------------------------------------------------------------------------
# 4) Defaults/output.
# ---------------------------------------------------------------------------
$targetReplaced = $false
foreach ($old in @('    int target = 432;', '    int target = 450;', '    int target = 1024;')) {
    if ($text.Contains($old)) {
        $text = $text.Replace($old, '    int target = 400;')
        $targetReplaced = $true
        break
    }
}
if (-not $targetReplaced -and -not $text.Contains('    int target = 400;')) {
    throw 'Could not set Options target default to 400.'
}

# Replace P17 true-size diagnostics with finite-square shape diagnostics.
$legacyGeomPattern = '(?s)\s*<< " trueSize=" << \(result\.touchesBoundary \? "NO" : "YES"\)\s*<< " measuredWindow=" << \(2 \* result\.measurementHalfSize\)\s*<< ''x'' << \(2 \* result\.measurementHalfSize\)\s*<< " expansions=" << result\.expansionCount;'
if ([regex]::IsMatch($text, $legacyGeomPattern)) {
    $geomReplacement = @'
              << " touchesSquareBoundary=" << (result.touchesBoundary ? "YES" : "NO")
              << " shapeScore=" << result.firstMismatchD2
              << " bboxFill=" << std::fixed << std::setprecision(1)
              << (static_cast<double>(result.measurementHalfSize) / 10.0) << "%"
              << " aspect=" << (static_cast<double>(result.expansionCount) / 10.0) << "%";
'@
    $text = [regex]::Replace($text, $legacyGeomPattern, "`n" + $geomReplacement.TrimEnd(), 1)
}

# firstMismatchD2 now means shapeScore, so remove old firstDifferent diagnostics.
$mismatchPattern = '(?s)\s*if \(result\.firstMismatchD2 < 0\) \{.*?\n\s*\}\n\s*std::cout << " center="'
if ([regex]::IsMatch($text, $mismatchPattern)) {
    $centerReplacement = @'
    std::cout << " center="
'@
    $text = [regex]::Replace($text, $mismatchPattern, "`n" + $centerReplacement.TrimEnd(), 1)
}

$text = $text.Replace(' trueRainforestArea=', ' connectedArea=')
$text = $text.Replace(' bestTrueArea=', ' bestShapeScore=')
$text = $text.Replace(
    'P17_TRUE_RAINFOREST_SIZE | 8x8 discovery scout | lazy rain | adaptive exact connected area | tuned 4x16',
    'P22_PLAINS_SEASONAL_SHAPE | PLAINS+SEASONAL_FOREST | compact connected scout | exact finite-square shape score'
)
$text = $text.Replace(
    '--target N             Discovery half-size; 1024 = scan 2048x2048, true biome may extend beyond',
    '--target N             Square half-size; default 400 = exact [-400,+399]^2 objective'
)
$text = $text.Replace('single_biome_radius_hits.csv', 'plains_seasonal_shape_hits.csv')

# Coarse record label from the Rainforest stack is misleading for the two-biome objective.
$text = $text.Replace(
    '                          << " biome=" << biomeName(hBiome[static_cast<std::size_t>(i)])',
    '                          << " allowed=PLAINS+SEASONAL_FOREST"'
)

# Restore only the true full-square jackpot; shapeScore is not used here.
$text = $text.Replace(
    'if (false) { // P17: no finite-world jackpot for true component area',
    'if (result.safeRadius >= (2 * o.target) * (2 * o.target)) {'
)

[System.IO.File]::WriteAllText(
    $outSource,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$verify = [System.IO.File]::ReadAllText($outSource)
if (-not $verify.Contains('P22_PLAINS_SEASONAL_SHAPE')) {
    throw 'Generated source is missing P22 marker.'
}
if (-not $verify.Contains('SEASONAL_FOREST')) {
    throw 'Generated source is missing SEASONAL_FOREST allowance.'
}
if (-not $verify.Contains('const int shapeScore')) {
    throw 'Generated source is missing exact shape scoring.'
}
if (-not $verify.Contains('if (result.firstMismatchD2 > bestExact) {')) {
    throw 'Generated source is not ranking host records by shapeScore.'
}

Write-Host "Generated: $outSource" -ForegroundColor Green
Write-Host 'Objective: dry connected PLAINS + SEASONAL_FOREST inside an 800x800 square.'
Write-Host 'Ranking: connected area weighted by bbox fill and aspect ratio.'
Write-Host 'Square coordinates at center 0,0: X/Z -400..399.'
Write-Host 'Original optimized Rainforest source was left untouched.'