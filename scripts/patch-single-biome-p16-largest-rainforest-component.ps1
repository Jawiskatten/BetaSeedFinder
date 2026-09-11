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

if ($text.Contains('P16_LARGEST_RAINFOREST_COMPONENT')) {
    Write-Host 'P16 largest-rainforest-component search is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('P15_BLOCK_COUNT_OUTPUT')) {
    throw 'P16 expects P15/P14 rainforest source first.'
}
if (-not $text.Contains('RAINFOREST_P14_TARGET')) {
    throw 'P16 requires the Rainforest P14 target predicate.'
}
if (-not $text.Contains('TUNDRA_P12_REPLAY_TAIL')) {
    throw 'P16 requires the optimized P12 RNG stack.'
}

$backupPath = $sourcePath + '.p15-before-p16-component.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P16_LARGEST_RAINFOREST_COMPONENT
# Exact objective:
#   largest 4-neighbour-connected RAINFOREST component inside the target square.
# For target=450 that square is exactly 900x900: offsets [-450,+449]^2.
# Record ranking is component BLOCK AREA, not total rainforest coverage and not
# a centered safe radius. Length/width are the long/short sides of the exact
# component bounding box. Components may touch the finite world boundary.
#
# Search/scout objective:
#   every seed is sampled on a fixed 8x8 grid spanning the whole target square.
#   The GPU finds the largest 4-neighbour-connected component in that 8x8 sample
#   mask. coarseScore = connectedSamples*128 + sampleBBoxArea, so connected
#   sample count dominates and bbox only breaks ties. One best coarse candidate
#   per normal GPU batch is then exact-scanned over all blocks. This is a
#   heuristic scout (not a mathematical global-optimum proof), but unlike the old
#   centered-radius probe it can discover a huge rainforest anywhere in the map.

# ---------------------------------------------------------------------------
# 1) ExactResult now carries component geometry.
# ---------------------------------------------------------------------------
$oldStruct = @'
struct ExactResult {
    int safeRadius;
    int firstMismatchD2;
    int baseBiome;
};
'@
$newStruct = @'
struct ExactResult {
    // P16: safeRadius is retained as the host-side record integer for minimal
    // churn, but its semantic value is now largest connected component AREA.
    int safeRadius;
    int firstMismatchD2;
    int baseBiome;
    int componentWidthX;
    int componentHeightZ;
    int minX;
    int maxX;
    int minZ;
    int maxZ;
    int touchesBoundary;
};
'@
if (-not $text.Contains($oldStruct)) {
    throw 'Could not find ExactResult struct.'
}
$text = $text.Replace($oldStruct, $newStruct)

# ---------------------------------------------------------------------------
# 2) Replace the centered ring scout with an 8x8 whole-world connected proxy.
#    Build temp/blend/rain octaves concurrently across ten lanes per seed.
# ---------------------------------------------------------------------------
$kernelPattern = '(?s)__global__ void searchKernel\(.*?\r?\n\}\r?\n\r?\n// TUNDRA_P4_GPU_COMPACTION'
$kernelMatches = [regex]::Matches($text, $kernelPattern)
if ($kernelMatches.Count -ne 1) {
    throw "Expected one searchKernel before P4 compaction, found $($kernelMatches.Count)."
}

$newKernel = @'
// P16_LARGEST_RAINFOREST_COMPONENT
__device__ __forceinline__ void p16InitAllSearchClimate(
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

__device__ __forceinline__ int p16PopCount64(unsigned long long value) {
    return __popcll(value);
}

__device__ __forceinline__ int p16LargestSampleComponent(
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
        const int count = p16PopCount64(component);

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
        p16InitAllSearchClimate(s, seed, lane);
    }
    __syncthreads();

    if (!validSeed) return;

    const int target = ringCount > 0 ? ringRadii[ringCount - 1] : 450;
    const int sideMinusOne = 2 * target - 1;
    unsigned long long sampleMask = 0ULL;

    // 8x8 fixed lattice across [-target, target-1]^2. Each 16-lane seed group
    // handles sixteen samples at a time, four chunks total.
    for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
        const int logical = base + lane;
        const int row = logical >> 3;
        const int col = logical & 7;
        const int dx = -target + (col * sideMinusOne) / 7;
        const int dz = -target + (row * sideMinusOne) / 7;
        const bool pass = searchIsTundraAt(s, centerX + dx, centerZ + dz);

        const unsigned long long votes = __ballot(pass ? 1 : 0);
        const int laneInWave = globalLane % warpSize;
        const int seedBaseInWave = laneInWave - lane;
        const unsigned long long chunk = (votes >> seedBaseInWave) & 0xFFFFULL;
        if (lane == 0) sampleMask |= chunk << base;
    }

    if (lane == 0) {
        int bboxArea = 0;
        const int connectedSamples = p16LargestSampleComponent(sampleMask, bboxArea);
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
# 3) Exact map kernel: one block builds the exact 900x900 (or generic target)
#    rainforest bitmap, reusing one exact ClimateState for the whole map.
# ---------------------------------------------------------------------------
$parseMarker = 'std::uint64_t parseU64(const std::string& value, const char* name) {'
if (-not $text.Contains($parseMarker)) {
    throw 'Could not find parseU64 insertion point.'
}
$mapKernel = @'
__global__ void p16RainforestMapKernel(
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
        map[idx] = biomeAt(s, blockX, blockZ) == static_cast<unsigned char>(RAINFOREST)
                ? 1 : 0;
    }
}

'@
$text = $text.Replace($parseMarker, $mapKernel + $parseMarker)

# ---------------------------------------------------------------------------
# 4) runExact becomes exact connected-component analysis. We intentionally do
#    the flood fill on CPU after one 810k-byte D2H copy: only one candidate per
#    large production batch reaches this path, while the expensive climate map
#    generation stays on the GPU.
# ---------------------------------------------------------------------------
$runExactPattern = '(?s)ExactResult runExact\(.*?return result;\r?\n\}'
$runMatches = [regex]::Matches($text, $runExactPattern)
if ($runMatches.Count -ne 1) {
    throw "Expected exactly one runExact function, found $($runMatches.Count)."
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

    const int side = 2 * target;
    const int total = side * side;
    unsigned char* dMap = nullptr;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dMap),
                        static_cast<std::size_t>(total) * sizeof(unsigned char)));

    hipLaunchKernelGGL(
            p16RainforestMapKernel,
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
    result.safeRadius = 0; // P16 semantic: AREA
    result.firstMismatchD2 = -1;
    result.baseBiome = static_cast<int>(RAINFOREST);
    result.componentWidthX = 0;
    result.componentHeightZ = 0;
    result.minX = result.maxX = centerX;
    result.minZ = result.maxZ = centerZ;
    result.touchesBoundary = 0;

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
$text = [regex]::Replace($text, $runExactPattern, $newRunExact.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 5) P4's production path must exact-check the best coarse seed every batch.
#    Exact AREA and coarse sample score are different units, so never compare
#    batchBestProbe against bestExact.
# ---------------------------------------------------------------------------
$oldCandidateGate = 'if (o.topExact == 1 && batchBestIndex >= 0 && batchBestProbe > bestExact) {'
if (-not $text.Contains($oldCandidateGate)) {
    throw 'Could not find P4 topExact=1 candidate gate.'
}
$text = $text.Replace(
    $oldCandidateGate,
    'if (o.topExact == 1 && batchBestIndex >= 0 && batchBestProbe > 0) {'
)

# ---------------------------------------------------------------------------
# 6) Preserve exact component geometry for periodic status output.
# ---------------------------------------------------------------------------
$bestVars = @'
    CoverageResult bestCoverage{};
    bool stop = false;
'@
if (-not $text.Contains($bestVars)) {
    throw 'Could not find bestCoverage variable block.'
}
$text = $text.Replace($bestVars, @'
    CoverageResult bestCoverage{};
    ExactResult bestComponent{};
    bool stop = false;
'@)

$bestAssign = '                bestCoverage = coverage;'
if (-not $text.Contains($bestAssign)) {
    throw 'Could not find bestCoverage assignment.'
}
$text = $text.Replace($bestAssign, @'
                bestCoverage = coverage;
                bestComponent = result;
'@.TrimEnd())

# ---------------------------------------------------------------------------
# 7) Output semantics: area is the record metric; length/width are exact bbox
#    dimensions, with the longer side reported as length.
# ---------------------------------------------------------------------------
$oldSafePrint = '              << " safeRadius=" << result.safeRadius;'
if (-not $text.Contains($oldSafePrint)) {
    throw 'Could not find safeRadius record output.'
}
$newSafePrint = @'
              << " largestRainforestArea=" << result.safeRadius
              << " length=" << std::max(result.componentWidthX, result.componentHeightZ)
              << " width=" << std::min(result.componentWidthX, result.componentHeightZ)
              << " bboxX=" << result.minX << ".." << result.maxX
              << " bboxZ=" << result.minZ << ".." << result.maxZ
              << " touchesBoundary=" << (result.touchesBoundary ? "YES" : "NO");
'@
$text = $text.Replace($oldSafePrint, $newSafePrint.TrimEnd())

# P15's old total-rainforest count is no longer the objective; suppress it.
$text = $text.Replace('        std::cout << " rainforestBlocks=" << coverage->sameCount;',
                     '        (void)coverage; // P16: total coverage is intentionally not the metric.')

# Remove the old firstDifferent/square-layer suffix from record output.
$mismatchPrintPattern = '(?s)    if \(result\.firstMismatchD2 < 0\) \{.*?\r?\n    \}\r?\n    std::cout << " center="'
$mismatchMatches = [regex]::Matches($text, $mismatchPrintPattern)
if ($mismatchMatches.Count -eq 1) {
    $text = [regex]::Replace(
        $text,
        $mismatchPrintPattern,
        '    std::cout << " center="',
        1
    )
}

# Periodic P15 status: swap total rainforest blocks for exact best component.
$statusOld = @'
                std::cout << " rainforestBlocks=" << bestCoverage.sameCount
                          << " bestSeed=" << bestExactSeed
'@
if (-not $text.Contains($statusOld)) {
    throw 'Could not find P15 periodic rainforestBlocks status block.'
}
$statusNew = @'
                std::cout << " bestArea=" << bestExact
                          << " length=" << std::max(bestComponent.componentWidthX, bestComponent.componentHeightZ)
                          << " width=" << std::min(bestComponent.componentWidthX, bestComponent.componentHeightZ)
                          << " bestSeed=" << bestExactSeed
'@
$text = $text.Replace($statusOld, $statusNew.TrimEnd())

$text = $text.Replace(' exactBest=', ' bestArea=')
$text = $text.Replace('[PROBE RECORD]', '[COARSE RECORD]')
$text = $text.Replace('passedProbeRadius=', 'coarseScore=')
$text = $text.Replace(' probeBest=', ' coarseBest=')

# A component AREA >= target would be trivial, so only call the full-world case
# a jackpot and only stop automatically when all target blocks are one component.
$text = $text.Replace(
    'if (result.safeRadius >= o.target) {',
    'if (result.safeRadius >= (2 * o.target) * (2 * o.target)) {'
)
$text = $text.Replace(
    'return result.safeRadius >= o.target ? 0 : 2;',
    'return result.safeRadius > 0 ? 0 : 2;'
)

# Cosmetic help/banner wording.
$text = $text.Replace(
    'P15_BLOCK_COUNT_OUTPUT | P14 scout: RAINFOREST-only | hot-temp gate + lazy exact rain | fast permutation RNG | warp votes | tuned 4x16',
    'P16_LARGEST_RAINFOREST_COMPONENT | RAINFOREST area scout: 8x8 connected proxy | exact 4-neighbour area | tuned 4x16'
)
$text = $text.Replace(
    '--target N             Required same-biome disk radius (default 432)',
    '--target N             Half-size of finite search world; 450 = exact 900x900'
)
$text = $text.Replace('safe_radius', 'largest_component_area')

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied P16 largest connected Rainforest search.' -ForegroundColor Green
Write-Host 'Exact record metric: largest 4-neighbour-connected RAINFOREST block area.'
Write-Host 'length/width: long/short side of that component''s exact bounding box.'
Write-Host 'touchesBoundary tells you whether the component reaches the finite world edge.'
Write-Host 'Scout: 8x8 whole-world connected sample proxy; exact full map for one best seed/batch.'
Write-Host 'Use -Target 450 for an exact 900x900 world.'
