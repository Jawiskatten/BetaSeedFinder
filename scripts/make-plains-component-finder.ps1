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

# Keep the existing optimized Rainforest finder untouched.  We generate a
# separate source file from the locally-patched P17 stack so the old search can
# still be resumed later.
if (-not $text.Contains('P17_TRUE_RAINFOREST_SIZE')) {
    throw 'Plains P18 generator expects the local single_biome_radius.cpp to have P17_TRUE_RAINFOREST_SIZE applied.'
}
if (-not $text.Contains('TUNDRA_P12_REPLAY_TAIL')) {
    throw 'Plains P18 generator expects the optimized P12 search RNG stack.'
}

# ---------------------------------------------------------------------------
# 1) Replace the P17 Rainforest-specific scout with an exact-biome 8x8 Plains
#    connected-component proxy over the finite square.  The search square is
#    target=400 => offsets [-400,+399]^2, exactly 800x800 blocks.
# ---------------------------------------------------------------------------
$scoutPattern = '(?s)// P17_TRUE_RAINFOREST_SIZE.*?\n// TUNDRA_P4_GPU_COMPACTION'
$scoutMatches = [regex]::Matches($text, $scoutPattern)
if ($scoutMatches.Count -ne 1) {
    throw "Expected exactly one P17 scout region, found $($scoutMatches.Count)."
}

$newScout = @'
// P18_LARGEST_PLAINS_400_SQUARE
// Objective: largest 4-neighbour-connected PLAINS component clipped to the
// finite target square. Default target=400 => [-400,+399]^2 = 800x800 blocks.
// Scout: exact Beta climate at an 8x8 cell-centered lattice (100-block spacing
// at target 400), ranked by largest connected Plains sample component.

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

    return classifyQuantizedBiome(temperature, rain) == static_cast<unsigned char>(PLAINS);
}

__device__ __forceinline__ int p18PopCount64(unsigned long long value) {
    return __popcll(value);
}

__device__ __forceinline__ int p18LargestSampleComponent(
        unsigned long long mask,
        int& bestBBoxArea
) {
    constexpr unsigned long long COL0 = 0x0101010101010101ULL;
    constexpr unsigned long long COL7 = 0x8080808080808080ULL;
    unsigned long long remaining = mask;
    int bestCount = 0;
    bestBBoxArea = 0;

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
        const int bboxArea = (maxCol - minCol + 1) * (maxRow - minRow + 1);
        if (count > bestCount || (count == bestCount && bboxArea > bestBBoxArea)) {
            bestCount = count;
            bestBBoxArea = bboxArea;
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
    unsigned long long plainsMask = 0ULL;

    // One exact-climate sample at the center of every 8x8 cell. At target=400
    // these are spaced by 100 blocks across the 800x800 objective square.
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
        if (lane == 0) plainsMask |= chunk << base;
    }

    if (lane == 0) {
        int bboxArea = 0;
        const int connectedSamples = p18LargestSampleComponent(plainsMask, bboxArea);
        const int coarseScore = connectedSamples * 128 + bboxArea;
        probeRadius[seedIndex] = static_cast<unsigned short>(coarseScore);
        baseBiome[seedIndex] = connectedSamples > 0
                ? static_cast<unsigned char>(PLAINS) : 255;
    }
}

// TUNDRA_P4_GPU_COMPACTION
'@
$text = [regex]::Replace($text, $scoutPattern, $newScout.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 2) Replace the old exact Rainforest bitmap kernel with an exact Plains map.
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
        map[idx] = biomeAt(s, blockX, blockZ) == static_cast<unsigned char>(PLAINS)
                ? 1 : 0;
    }
}
'@
$text = [regex]::Replace($text, $mapPattern, $newMapKernel.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 3) Exact objective: largest Plains component INSIDE the finite target square.
#    We intentionally clip at the square boundary; touching it is diagnostic and
#    does not disqualify a component.
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
    result.safeRadius = 0; // P18 semantic: connected PLAINS AREA inside square
    result.firstMismatchD2 = -1;
    result.baseBiome = static_cast<int>(PLAINS);
    result.componentWidthX = 0;
    result.componentHeightZ = 0;
    result.minX = result.maxX = centerX;
    result.minZ = result.maxZ = centerZ;
    result.touchesBoundary = 0;
    result.measurementHalfSize = target;
    result.expansionCount = 0;

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

        if (area > result.safeRadius) {
            result.safeRadius = area;
            result.componentWidthX = maxLX - minLX + 1;
            result.componentHeightZ = maxLZ - minLZ + 1;
            result.minX = centerX - target + minLX;
            result.maxX = centerX - target + maxLX;
            result.minZ = centerZ - target + minLZ;
            result.maxZ = centerZ - target + maxLZ;
            result.touchesBoundary =
                    (minLX == 0 || maxLX == side - 1 ||
                     minLZ == 0 || maxLZ == side - 1) ? 1 : 0;
        }
    }

    return result;
}
'@
$text = [regex]::Replace($text, $runPattern, $newRun.TrimEnd(), 1)

# P17 rejected open-at-cap components. P18 is intentionally clipped to the
# square, so a boundary-touching component is still a valid record.
$recordPattern = '(?s)if \(result\.touchesBoundary != 0\) \{.*?if \(result\.touchesBoundary == 0 && result\.safeRadius > bestExact\) \{'
$recordMatches = [regex]::Matches($text, $recordPattern)
if ($recordMatches.Count -ne 1) {
    throw "Expected exactly one P17 record gate block, found $($recordMatches.Count)."
}
$text = [regex]::Replace($text, $recordPattern, 'if (result.safeRadius > bestExact) {', 1)

# ---------------------------------------------------------------------------
# 4) Defaults/output: fixed 400-block radius square, Plains terminology.
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

$oldP17Touch = @'
              << " trueSize=" << (result.touchesBoundary ? "NO" : "YES")
              << " measuredWindow=" << (2 * result.measurementHalfSize)
              << 'x' << (2 * result.measurementHalfSize)
              << " expansions=" << result.expansionCount;
'@
$newP18Touch = @'
              << " touchesSquareBoundary=" << (result.touchesBoundary ? "YES" : "NO");
'@
if ($text.Contains($oldP17Touch.TrimEnd())) {
    $text = $text.Replace($oldP17Touch.TrimEnd(), $newP18Touch.TrimEnd())
}

$text = $text.Replace(' trueRainforestArea=', ' plainsArea=')
$text = $text.Replace(' bestTrueArea=', ' bestPlainsArea=')
$text = $text.Replace(
    'P17_TRUE_RAINFOREST_SIZE | 8x8 discovery scout | lazy rain | adaptive exact connected area | tuned 4x16',
    'P18_LARGEST_PLAINS_400_SQUARE | 8x8 Plains connected scout | exact 4-neighbour 800x800 area | tuned 4x16'
)
$text = $text.Replace(
    '--target N             Discovery half-size; 1024 = scan 2048x2048, true biome may extend beyond',
    '--target N             Square half-size; default 400 = exact [-400,+399]^2 objective'
)
$text = $text.Replace('single_biome_radius_hits.csv', 'plains_component_hits.csv')

# P17 disabled the finite-square jackpot. Restore it for the logically complete
# case where all 640,000 blocks are one connected Plains component.
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
if (-not $verify.Contains('P18_LARGEST_PLAINS_400_SQUARE')) {
    throw 'Generated Plains source is missing the P18 marker.'
}
if (-not $verify.Contains('p18PlainsMapKernel')) {
    throw 'Generated Plains source is missing the exact Plains map kernel.'
}

Write-Host "Generated: $outSource" -ForegroundColor Green
Write-Host 'Objective: largest 4-neighbour-connected PLAINS component inside an 800x800 square.'
Write-Host 'Square coordinates at center 0,0: X/Z -400..399.'
Write-Host 'Original optimized Rainforest source was left untouched.'
