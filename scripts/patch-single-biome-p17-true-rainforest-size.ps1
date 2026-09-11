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

if ($text.Contains('P17_TRUE_RAINFOREST_SIZE')) {
    Write-Host 'P17 true-size rainforest search is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('P16_LARGEST_RAINFOREST_COMPONENT')) {
    throw 'P17 expects P16 largest-rainforest-component source first.'
}
if (-not $text.Contains('RAINFOREST_P14_TARGET')) {
    throw 'P17 requires the exact Rainforest target predicate.'
}
if (-not $text.Contains('TUNDRA_P12_REPLAY_TAIL')) {
    throw 'P17 requires the optimized P12 RNG stack.'
}

$backupPath = $sourcePath + '.p16-before-p17-true-size.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P17_TRUE_RAINFOREST_SIZE
#
# Goal:
#   Find worlds containing the largest TRUE connected Beta 1.7.3 RAINFOREST.
#   The command-line --target square is now only the DISCOVERY WINDOW. A biome
#   is eligible if at least one of its blocks intersects that discovery square.
#   Exact measurement follows the connected component beyond that square by
#   repeatedly doubling the measurement envelope until every eligible component
#   is closed by non-rainforest blocks. The record metric is then the complete
#   connected-component block area, independent of the discovery edge.
#
# Why this definition is necessary:
#   Minecraft terrain is infinite. Without a finite discovery region, "largest
#   rainforest in the world" has no finite search domain. Searching a large
#   window around the origin while measuring components beyond it gives a clean,
#   reproducible objective without clipping the biome at an arbitrary border.
#
# Scout strategy:
#   P16's 8x8 connected sample proxy is kept, but the expensive rainfall state
#   is made lazy again. Every seed first evaluates 64 exact temperature samples
#   over the discovery window. Rainforest requires quantized temperature >= .97;
#   seeds with no hot sample never construct rain permutations. For surviving
#   seeds only the hot samples evaluate rain. This is especially useful when the
#   discovery window is enlarged (recommended target=1024 => 2048x2048).

# ---------------------------------------------------------------------------
# 1) ExactResult: retain P16 fields and add adaptive-measurement metadata.
#    touchesBoundary is repurposed as measurementIncomplete (1 only when the
#    hard safety cap is reached before all eligible components close).
# ---------------------------------------------------------------------------
$oldStructTail = @'
    int touchesBoundary;
};
'@
$newStructTail = @'
    int touchesBoundary; // P17: measurementIncomplete, not an objective edge flag
    int measurementHalfSize;
    int expansionCount;
};
'@
if (-not $text.Contains($oldStructTail)) {
    throw 'Could not find P16 ExactResult tail.'
}
$text = $text.Replace($oldStructTail, $newStructTail)

# ---------------------------------------------------------------------------
# 2) Replace P16 scout with temperature-first / lazy-rain discovery scout.
# ---------------------------------------------------------------------------
$kernelPattern = '(?s)// P16_LARGEST_RAINFOREST_COMPONENT.*?\r?\n// TUNDRA_P4_GPU_COMPACTION'
$kernelMatches = [regex]::Matches($text, $kernelPattern)
if ($kernelMatches.Count -ne 1) {
    throw "Expected exactly one P16 scout region, found $($kernelMatches.Count)."
}

$newKernel = @'
// P17_TRUE_RAINFOREST_SIZE
// Search --target is the discovery half-size, not a clipping boundary for exact area.

__device__ __forceinline__ void p17InitColdSearchClimate(
        SearchClimateState& s,
        std::int64_t seed,
        int laneInSeed
) {
    const bool tempOwner = laneInSeed < 4;
    const bool blendOwner = laneInSeed >= 4 && laneInSeed < 6;
    if (!tempOwner && !blendOwner) return;

    const int octave = tempOwner ? laneInSeed : (laneInSeed - 4);
    const std::uint64_t multiplier = tempOwner ? 9871ULL : 543321ULL;
    p20::JavaRandom rng;
    rng.setSeed(multipliedSeed(seed, multiplier));
    for (int prior = 0; prior < octave; ++prior) {
        p10ConsumeSearchPerlinRng(rng);
    }
    if (tempOwner) initSearchPerlin(rng, s.temp[octave]);
    else initSearchPerlin(rng, s.blend[octave]);
}

__device__ __forceinline__ bool p17RainforestTemperatureCandidate(
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
    return f >= 0.97f;
}

__device__ __forceinline__ int p17PopCount64(unsigned long long value) {
    return __popcll(value);
}

__device__ __forceinline__ int p17LargestSampleComponent(
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
        const int count = p17PopCount64(component);

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
    __shared__ double sampleD0[SEARCH_SEEDS_PER_BLOCK][64];
    __shared__ unsigned char sampleTi[SEARCH_SEEDS_PER_BLOCK][64];
    __shared__ unsigned long long hotMasks[SEARCH_SEEDS_PER_BLOCK];
    SearchClimateState& s = states[seedGroup];

    std::int64_t seed = 0;
    if (validSeed) {
        const std::uint64_t attempt = startAttempt + static_cast<std::uint64_t>(seedIndex);
        seed = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(sequence, attempt));
        p17InitColdSearchClimate(s, seed, lane);
    }
    __syncthreads();

    const int discoveryHalf = ringCount > 0 ? ringRadii[ringCount - 1] : 1024;
    const int sideMinusOne = 2 * discoveryHalf - 1;
    unsigned long long hotMask = 0ULL;

    // Stage A: exact temperature at 64 samples. This costs only temp+blend and
    // rejects the overwhelming majority of samples before rainfall exists.
    for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
        const int logical = base + lane;
        bool hot = false;
        double d0 = 0.0;
        int ti = 0;
        if (validSeed && logical < 64) {
            const int row = logical >> 3;
            const int col = logical & 7;
            const int dx = -discoveryHalf + (col * sideMinusOne) / 7;
            const int dz = -discoveryHalf + (row * sideMinusOne) / 7;
            hot = p17RainforestTemperatureCandidate(
                    s, centerX + dx, centerZ + dz, d0, ti);
            sampleD0[seedGroup][logical] = d0;
            sampleTi[seedGroup][logical] = static_cast<unsigned char>(ti);
        }

        const unsigned long long votes = __ballot(hot ? 1 : 0);
        const int laneInWave = globalLane % warpSize;
        const int seedBaseInWave = laneInWave - lane;
        const unsigned long long chunk = (votes >> seedBaseInWave) & 0xFFFFULL;
        if (lane == 0) hotMask |= chunk << base;
    }
    if (lane == 0) hotMasks[seedGroup] = validSeed ? hotMask : 0ULL;
    __syncthreads();

    // Stage B: only groups with at least one potentially-Rainforest sample pay
    // for four exact rain permutation tables. P10/P11/P12 replay remains exact.
    if (validSeed && hotMasks[seedGroup] != 0ULL) {
        p8InitRain(s, seed, lane);
    }
    __syncthreads();

    if (!validSeed) return;
    if (hotMasks[seedGroup] == 0ULL) {
        if (lane == 0) {
            probeRadius[seedIndex] = 0;
            baseBiome[seedIndex] = 255;
        }
        return;
    }

    unsigned long long rainforestMask = 0ULL;
    for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
        const int logical = base + lane;
        bool pass = false;
        if (logical < 64 && ((hotMasks[seedGroup] >> logical) & 1ULL) != 0ULL) {
            const int row = logical >> 3;
            const int col = logical & 7;
            const int dx = -discoveryHalf + (col * sideMinusOne) / 7;
            const int dz = -discoveryHalf + (row * sideMinusOne) / 7;
            pass = p8AmbiguousRainPasses(
                    s,
                    centerX + dx,
                    centerZ + dz,
                    sampleD0[seedGroup][logical],
                    static_cast<int>(sampleTi[seedGroup][logical]));
        }

        const unsigned long long votes = __ballot(pass ? 1 : 0);
        const int laneInWave = globalLane % warpSize;
        const int seedBaseInWave = laneInWave - lane;
        const unsigned long long chunk = (votes >> seedBaseInWave) & 0xFFFFULL;
        if (lane == 0) rainforestMask |= chunk << base;
    }

    if (lane == 0) {
        int bboxArea = 0;
        const int connectedSamples = p17LargestSampleComponent(rainforestMask, bboxArea);
        const int coarseScore = connectedSamples * 128 + bboxArea;
        probeRadius[seedIndex] = static_cast<unsigned short>(coarseScore);
        baseBiome[seedIndex] = connectedSamples > 0
                ? static_cast<unsigned char>(RAINFOREST) : 255;
    }
}

// TUNDRA_P4_GPU_COMPACTION
'@

$text = [regex]::Replace($text, $kernelPattern, $newKernel.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 3) Replace P16 finite-window exact flood fill with adaptive true-size measure.
#    We measure every Rainforest component that intersects the original discovery
#    square. If ANY such component reaches the current measurement boundary, the
#    map doubles and is regenerated. When no eligible component reaches the
#    boundary, every eligible component is closed and its area is exact.
# ---------------------------------------------------------------------------
$runExactPattern = '(?s)ExactResult runExact\(.*?return result;\r?\n\}'
$runMatches = [regex]::Matches($text, $runExactPattern)
if ($runMatches.Count -ne 1) {
    throw "Expected exactly one P16 runExact function, found $($runMatches.Count)."
}

$newRunExact = @'
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

    // Hard safety cap for pathological candidates. A normal result is returned
    // only after the component closes. If this cap is reached while still open,
    // touchesBoundary=1 marks the area as a lower bound and host ranking ignores it.
    constexpr int P17_MAX_MEASURE_HALF = 4096; // max exact envelope: 8192x8192

    const int discoveryMinX = centerX - target;
    const int discoveryMaxX = centerX + target - 1;
    const int discoveryMinZ = centerZ - target;
    const int discoveryMaxZ = centerZ + target - 1;

    int measureHalf = target;
    int expansionCount = 0;

    for (;;) {
        const int side = 2 * measureHalf;
        const int total = side * side;
        unsigned char* dMap = nullptr;
        HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dMap),
                            static_cast<std::size_t>(total) * sizeof(unsigned char)));

        hipLaunchKernelGGL(
                p16RainforestMapKernel,
                dim3(1), dim3(EXACT_THREADS), 0, 0,
                seed, centerX, centerZ, measureHalf, dMap);
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
        result.safeRadius = 0; // P17 semantic: TRUE connected component AREA
        result.firstMismatchD2 = -1;
        result.baseBiome = static_cast<int>(RAINFOREST);
        result.componentWidthX = 0;
        result.componentHeightZ = 0;
        result.minX = result.maxX = centerX;
        result.minZ = result.maxZ = centerZ;
        result.touchesBoundary = 0;
        result.measurementHalfSize = measureHalf;
        result.expansionCount = expansionCount;

        bool anyEligibleTouchesOuterBoundary = false;

        for (int start = 0; start < total; ++start) {
            if (map[static_cast<std::size_t>(start)] != 1) continue;

            int head = 0;
            int tail = 0;
            queue[static_cast<std::size_t>(tail++)] = start;
            map[static_cast<std::size_t>(start)] = 2;

            int area = 0;
            int minLX = side, maxLX = -1;
            int minLZ = side, maxLZ = -1;
            bool intersectsDiscovery = false;
            bool touchesOuterBoundary = false;

            while (head < tail) {
                const int idx = queue[static_cast<std::size_t>(head++)];
                const int x = idx % side;
                const int z = idx / side;
                const int worldX = centerX - measureHalf + x;
                const int worldZ = centerZ - measureHalf + z;
                ++area;

                if (x < minLX) minLX = x;
                if (x > maxLX) maxLX = x;
                if (z < minLZ) minLZ = z;
                if (z > maxLZ) maxLZ = z;

                if (worldX >= discoveryMinX && worldX <= discoveryMaxX &&
                    worldZ >= discoveryMinZ && worldZ <= discoveryMaxZ) {
                    intersectsDiscovery = true;
                }
                if (x == 0 || x == side - 1 || z == 0 || z == side - 1) {
                    touchesOuterBoundary = true;
                }

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

            if (!intersectsDiscovery) continue;
            if (touchesOuterBoundary) anyEligibleTouchesOuterBoundary = true;

            if (area > result.safeRadius) {
                result.safeRadius = area;
                result.componentWidthX = maxLX - minLX + 1;
                result.componentHeightZ = maxLZ - minLZ + 1;
                result.minX = centerX - measureHalf + minLX;
                result.maxX = centerX - measureHalf + maxLX;
                result.minZ = centerZ - measureHalf + minLZ;
                result.maxZ = centerZ - measureHalf + maxLZ;
            }
        }

        if (!anyEligibleTouchesOuterBoundary) {
            return result; // all eligible components are closed: TRUE size
        }

        if (measureHalf >= P17_MAX_MEASURE_HALF) {
            result.touchesBoundary = 1; // incomplete lower bound; do not rank
            return result;
        }

        const int nextHalf = std::min(P17_MAX_MEASURE_HALF, measureHalf * 2);
        if (nextHalf <= measureHalf) {
            result.touchesBoundary = 1;
            return result;
        }
        measureHalf = nextHalf;
        ++expansionCount;
    }
}
'@
$text = [regex]::Replace($text, $runExactPattern, $newRunExact.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 4) Ranking: incomplete measurements at the hard cap are lower bounds and are
#    never accepted as exact records. Also disable P16's finite-world jackpot;
#    a true component can legitimately be larger than the discovery square.
# ---------------------------------------------------------------------------
$recordGate = 'if (result.safeRadius > bestExact) {'
$recordGateMatches = [regex]::Matches($text, [regex]::Escape($recordGate))
if ($recordGateMatches.Count -ne 1) {
    throw "Expected exactly one record gate, found $($recordGateMatches.Count)."
}
$recordGateReplacement = @'
if (result.touchesBoundary != 0) {
                std::cout << "[MEASURE LIMIT] seed=" << seed
                          << " lowerBoundArea=" << result.safeRadius
                          << " length=" << std::max(result.componentWidthX, result.componentHeightZ)
                          << " width=" << std::min(result.componentWidthX, result.componentHeightZ)
                          << " measuredWindow=" << (2 * result.measurementHalfSize)
                          << 'x' << (2 * result.measurementHalfSize)
                          << " (not ranked; component still open)\n";
            }

            if (result.touchesBoundary == 0 && result.safeRadius > bestExact) {
'@
$text = $text.Replace($recordGate, $recordGateReplacement.TrimEnd())

$jackpotGate = 'if (result.safeRadius >= (2 * o.target) * (2 * o.target)) {'
if ($text.Contains($jackpotGate)) {
    $text = $text.Replace($jackpotGate, 'if (false) { // P17: no finite-world jackpot for true component area')
}

# ---------------------------------------------------------------------------
# 5) Output: remove edge/clipping semantics. Report complete area + dimensions,
#    plus how large an envelope was required to prove closure.
# ---------------------------------------------------------------------------
$oldTouchPrint = '              << " touchesBoundary=" << (result.touchesBoundary ? "YES" : "NO");'
if (-not $text.Contains($oldTouchPrint)) {
    throw 'Could not find P16 touchesBoundary output.'
}
$newTouchPrint = @'
              << " trueSize=" << (result.touchesBoundary ? "NO" : "YES")
              << " measuredWindow=" << (2 * result.measurementHalfSize)
              << 'x' << (2 * result.measurementHalfSize)
              << " expansions=" << result.expansionCount;
'@
$text = $text.Replace($oldTouchPrint, $newTouchPrint.TrimEnd())

$text = $text.Replace(' largestRainforestArea=', ' trueRainforestArea=')
$text = $text.Replace(' bestArea=', ' bestTrueArea=')
$text = $text.Replace('P16_LARGEST_RAINFOREST_COMPONENT | RAINFOREST area scout: 8x8 connected proxy | exact 4-neighbour area | tuned 4x16',
                     'P17_TRUE_RAINFOREST_SIZE | 8x8 discovery scout | lazy rain | adaptive exact connected area | tuned 4x16')
$text = $text.Replace('--target N             Half-size of finite search world; 450 = exact 900x900',
                     '--target N             Discovery half-size; 1024 = scan 2048x2048, true biome may extend beyond')

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied P17 TRUE Rainforest-size search.' -ForegroundColor Green
Write-Host 'Record metric: complete 4-neighbour-connected RAINFOREST block area.'
Write-Host 'The target square is now only the discovery window; it does NOT clip component size.'
Write-Host 'Exact measurement doubles outward until every Rainforest intersecting the discovery window is closed.'
Write-Host 'length/width are the full component bounding-box dimensions after closure.'
Write-Host 'Recommended first run: -Target 1024 (2048x2048 discovery window).'
Write-Host 'Scout keeps 64 spatial samples but restores temperature-first lazy rainfall for speed.'
Write-Host 'Hard exact-measure safety cap is 8192x8192; open-at-cap candidates are printed but never ranked.'
