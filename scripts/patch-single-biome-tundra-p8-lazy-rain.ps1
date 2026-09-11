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

if ($text.Contains('TUNDRA_P8_LAZY_RAIN')) {
    Write-Host 'Tundra P8 lazy-rain scout is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P7_WARP_VOTE')) {
    throw 'P8 requires the P7 warp-vote scout first.'
}
if (-not $text.Contains('TUNDRA_P4_GPU_COMPACTION')) {
    throw 'P8 requires P4 GPU compaction.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P8 requires exact 864x864 square semantics.'
}
if (-not $text.Contains('static constexpr int SEARCH_SEEDS_PER_BLOCK = 4; // TUNDRA_P3_GROUPED_SEARCH')) {
    throw 'P8 is specialized for the tuned 4-seeds/block winner.'
}

$backupPath = $sourcePath + '.p7-before-p8.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# Replace P7's helper+kernel region only. Exact verification, coverage, P4
# compaction, tuned batch and all host-side behavior are intentionally untouched.
$kernelPattern = '(?s)// TUNDRA_P7_WARP_VOTE.*?\r?\n\}\r?\n\r?\n// TUNDRA_P4_GPU_COMPACTION'
$matches = [regex]::Matches($text, $kernelPattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one P7 helper/searchKernel region, found $($matches.Count)."
}

$newKernel = @'
// TUNDRA_P8_LAZY_RAIN
// Tuned RX 7800 XT path: four seeds/block, sixteen contiguous lanes per seed.
// P8 removes rainfall-state construction from the unconditional per-seed cost.
// Temperature+blend are initialized first. Center + the first 15 spatially
// spread gate/ring points then run together across the otherwise under-used
// 16-lane seed group. Only groups that survive this exact temperature screen
// build the four rainfall permutation tables.
static_assert(SEARCH_SEEDS_PER_BLOCK == 4, "P8 requires tuned 4 seeds/block");
static_assert(SEARCH_LANES_PER_SEED == 16, "P8 requires 16 lanes/seed");

__device__ __forceinline__ bool p8SeedAnyFailed(
        bool failed,
        int globalLane,
        int laneInSeed
) {
    const unsigned long long votes = __ballot(failed ? 1 : 0);
    const int laneInWave = globalLane % warpSize;
    const int seedBaseInWave = laneInWave - laneInSeed;
    const unsigned long long seedMask = 0xFFFFULL << seedBaseInWave;
    return (votes & seedMask) != 0ULL;
}

__device__ __forceinline__ bool p8WaveHasLiveSeed(bool alive) {
    return __ballot(alive ? 1 : 0) != 0ULL;
}

// Build only the six Perlin states needed to know the quantized temperature.
// Rain is deliberately omitted here. The two independent RNG streams execute
// concurrently in lanes 0 and 1 of each 16-lane seed group.
__device__ __forceinline__ void p8InitColdClimate(
        SearchClimateState& s,
        std::int64_t seed,
        int laneInSeed
) {
    if (laneInSeed == 0) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 9871ULL));
        for (int i = 0; i < 4; ++i) initSearchPerlin(rng, s.temp[i]);
    } else if (laneInSeed == 1) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 543321ULL));
        for (int i = 0; i < 2; ++i) initSearchPerlin(rng, s.blend[i]);
    }
}

// Called only for groups that survived the first temperature-only SIMD screen.
// A single lane owns the Java RNG stream because Fisher-Yates/JavaRandom order
// is inherently sequential and must remain bit-identical to Beta 1.7.3.
__device__ __forceinline__ void p8InitRain(
        SearchClimateState& s,
        std::int64_t seed,
        int laneInSeed
) {
    if (laneInSeed == 2) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 39811ULL));
        for (int i = 0; i < 4; ++i) initSearchPerlin(rng, s.rain[i]);
    }
}

// Exact temperature half of searchIsTundraAt(). Return values:
//   0 = definitely TUNDRA (quantized f < 0.2, rain can never change it)
//   1 = ambiguous       (0.2 <= f < 0.5, rain is required)
//   2 = definitely fail (f >= 0.5, cannot be TUNDRA)
// d0 + ti are retained in registers so an ambiguous first point can be checked
// after lazy rain initialization without recomputing temp/blend noise.
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
    if (f < 0.2f) return 0;
    if (f >= 0.5f) return 2;
    return 1;
}

// Exact rain half for a point already proven to be in the ambiguous temp band.
__device__ __forceinline__ bool p8AmbiguousRainPasses(
        const SearchClimateState& s,
        int blockX,
        int blockZ,
        double d0,
        int ti
) {
    const double x = static_cast<double>(blockX);
    const double z = static_cast<double>(blockZ);
    const double rainRaw = searchOctaveNoise4(
            s.rain, x, z, 0.05000000074505806, 0.3333333333333333);
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    rain = clamp01(rain);

    int ri = static_cast<int>(rain * 63.0);
    if (ri < 0) ri = 0;
    if (ri > 63) ri = 63;
    const float f = static_cast<float>(ti) / 63.0f;
    float f1 = static_cast<float>(ri) / 63.0f;
    f1 *= f;
    return f1 < 0.2f;
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
        p8InitColdClimate(s, seed, lane);
    }

    // Barrier #1: only temp+blend are live at this point. Rain bytes are still
    // untouched for most seeds, avoiding four full Fisher-Yates tables per seed.
    __syncthreads();

    const int target = ringRadii[ringCount - 1];
    const int denseMin = target > 96 ? target - 96 : 1;
    const bool bootstrap = minRequiredRadius <= 0;

    if (validSeed && lane == 0) {
        probeRadius[seedIndex] = 0;
        baseBiome[seedIndex] = 255;
    }

    // ---------------- fused first temperature screen ----------------
    // Lane 0 checks center. Lanes 1..15 simultaneously check the first fifteen
    // spatially spread points from the record gate (or ring 0 during bootstrap).
    // On SIMD hardware this fills lanes that P7 left idle during center testing.
    int firstDx = 0;
    int firstDz = 0;
    if (validSeed) {
        if (lane == 0) {
            firstDx = 0;
            firstDz = 0;
        } else {
            const int logical = lane - 1; // 0..14; logical 15 remains for next chunk
            const int sample = (logical & 15) * 4 + (logical >> 4);
            if (bootstrap) {
                const int pointIndex = sample; // ring 0 * SEARCH_THREADS
                firstDx = probeDx[pointIndex];
                firstDz = probeDz[pointIndex];
            } else {
                squareProbePoint864(minRequiredRadius, sample, firstDx, firstDz);
            }
        }
    }

    double firstD0 = 0.0;
    int firstTi = 0;
    int firstClass = 2;
    if (validSeed) {
        firstClass = p8TemperatureClassAt(
                s, centerX + firstDx, centerZ + firstDz, firstD0, firstTi);
    }

    const bool temperatureHot = validSeed && firstClass == 2;
    const bool groupTemperatureHot = p8SeedAnyFailed(temperatureHot, globalLane, lane);
    bool alive = validSeed && !groupTemperatureHot;

    // Do NOT wave-return before the next block barrier: surviving groups need a
    // safely published lazy rain table. Only those groups pay this setup cost.
    if (alive) {
        p8InitRain(s, seed, lane);
    }

    // Barrier #2 is the final block-wide barrier in P8. It publishes rain state
    // for surviving groups. All later control flow is warp-local exactly as P7.
    __syncthreads();

    // Resolve only ambiguous first-screen points. Cold points need no rain work;
    // hot groups were already rejected before rain initialization.
    bool firstRainFailed = false;
    if (alive && firstClass == 1) {
        firstRainFailed = !p8AmbiguousRainPasses(
                s, centerX + firstDx, centerZ + firstDz, firstD0, firstTi);
    }

    // Preserve base-biome reporting for surviving centers. If another point in
    // the fused screen killed the group before rain setup, baseBiome=255 is fine
    // because that seed cannot be a production candidate.
    if (validSeed && lane == 0 && !temperatureHot) {
        const bool centerPass = firstClass == 0 || (firstClass == 1 && !firstRainFailed);
        baseBiome[seedIndex] = centerPass ? static_cast<unsigned char>(TUNDRA) : 255;
    }

    if (p8SeedAnyFailed(firstRainFailed, globalLane, lane)) alive = false;
    if (!p8WaveHasLiveSeed(alive)) return;

    // Finish the first gate/ring. The fused screen consumed logical points
    // 0..14; logical 15..63 are identical P7 points and keep 16-point ballots.
    for (int base = 15; base < 64; base += SEARCH_LANES_PER_SEED) {
        bool failed = false;
        if (alive) {
            const int logical = base + lane;
            if (logical < 64) {
                const int sample = (logical & 15) * 4 + (logical >> 4);
                int dx = 0;
                int dz = 0;
                if (bootstrap) {
                    const int pointIndex = sample;
                    dx = probeDx[pointIndex];
                    dz = probeDz[pointIndex];
                } else {
                    squareProbePoint864(minRequiredRadius, sample, dx, dz);
                }
                failed = !searchIsTundraAt(s, centerX + dx, centerZ + dz);
            }
        }
        if (p8SeedAnyFailed(failed, globalLane, lane)) alive = false;
    }

    const int firstRadius = bootstrap ? ringRadii[0] : minRequiredRadius;
    if (alive && lane == 0 && firstRadius < target) {
        probeRadius[seedIndex] = static_cast<unsigned short>(firstRadius);
    }
    if (!p8WaveHasLiveSeed(alive)) return;

    // ---------------- bootstrap remainder ----------------
    // Ring 0 was consumed by the fused stage above; continue at ring 1.
    if (bootstrap) {
        for (int ring = 1; ring < ringCount; ++ring) {
            const int r = ringRadii[ring];
            for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
                bool failed = false;
                if (alive) {
                    const int logical = base + lane;
                    if (logical < 64) {
                        const int sample = (logical & 15) * 4 + (logical >> 4);
                        const int pointIndex = ring * SEARCH_THREADS + sample;
                        failed = !searchIsTundraAt(
                                s,
                                centerX + probeDx[pointIndex],
                                centerZ + probeDz[pointIndex]);
                    }
                }
                if (p8SeedAnyFailed(failed, globalLane, lane)) alive = false;
            }
            if (alive && lane == 0 && r < target) {
                probeRadius[seedIndex] = static_cast<unsigned short>(r);
            }
            if (!p8WaveHasLiveSeed(alive)) return;
        }
    }

    // ---------------- outer-band coarse pass ----------------
    // Identical P7/P6 sample set and 16-point early-exit boundaries.
    for (int r = target; r >= denseMin; r -= 8) {
        if (r <= minRequiredRadius) continue;

        for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
            bool failed = false;
            if (alive) {
                const int logical = base + lane;
                if (logical < 64) {
                    const int coarse = (logical & 15) * 4 + (logical >> 4);
                    const int side = coarse / 16;
                    const int t = coarse - side * 16;
                    const int denseSample = side * 64 + t * 4;
                    int dx = 0;
                    int dz = 0;
                    denseSquareProbePoint864(r, denseSample, dx, dz);
                    failed = !searchIsTundraAt(s, centerX + dx, centerZ + dz);
                }
            }
            if (p8SeedAnyFailed(failed, globalLane, lane)) alive = false;
        }
        if (!p8WaveHasLiveSeed(alive)) return;
    }

    // ---------------- dense outer remainder ----------------
    // Same 192 no-duplicate points per useful layer as P7.
    for (int r = target; r >= denseMin; r -= 8) {
        if (r <= minRequiredRadius) continue;

        for (int base = 0; base < 192; base += SEARCH_LANES_PER_SEED) {
            bool failed = false;
            if (alive) {
                const int logical = base + lane;
                if (logical < 192) {
                    const int side = logical & 3;
                    const int local = logical >> 2;
                    const int t = local + 1 + local / 3;
                    const int denseSample = side * 64 + t;
                    int dx = 0;
                    int dz = 0;
                    denseSquareProbePoint864(r, denseSample, dx, dz);
                    failed = !searchIsTundraAt(s, centerX + dx, centerZ + dz);
                }
            }
            if (p8SeedAnyFailed(failed, globalLane, lane)) alive = false;
        }
        if (!p8WaveHasLiveSeed(alive)) return;
    }

    // ---------------- sparse interior ----------------
    // Same outside-in interior probes as P7, only above the exact-record gate.
    for (int ring = ringCount - 1; ring >= 0; --ring) {
        const int r = ringRadii[ring];
        if (r <= minRequiredRadius || r >= denseMin) continue;

        for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
            bool failed = false;
            if (alive) {
                const int logical = base + lane;
                if (logical < 64) {
                    const int sample = (logical & 15) * 4 + (logical >> 4);
                    const int pointIndex = ring * SEARCH_THREADS + sample;
                    failed = !searchIsTundraAt(
                            s,
                            centerX + probeDx[pointIndex],
                            centerZ + probeDz[pointIndex]);
                }
            }
            if (p8SeedAnyFailed(failed, globalLane, lane)) alive = false;
        }
        if (!p8WaveHasLiveSeed(alive)) return;
    }

    // Acceptance set remains the same sampled P7 set. P4 compaction still sends
    // every target survivor to exact block-by-block verification.
    if (alive && lane == 0) {
        probeRadius[seedIndex] = static_cast<unsigned short>(target);
    }
}

// TUNDRA_P4_GPU_COMPACTION
'@

$text = [regex]::Replace($text, $kernelPattern, $newKernel, 1)

# P7 accidentally emitted a literal "\n" in the banner on some patched trees.
# Replace the whole statement and intentionally write a single C++ \n escape.
$bannerPattern = 'std::cout\s*<<\s*"P7 scout:[^"\r\n]*";'
if ([regex]::Matches($text, $bannerPattern).Count -eq 1) {
    $text = [regex]::Replace(
        $text,
        $bannerPattern,
        'std::cout << "P8 scout: TUNDRA-only | lazy rain init | fused center+gate SIMD | warp votes | GPU compaction | tuned 4x16\n";',
        1
    )
} else {
    throw 'Could not find the P7 startup banner to upgrade/fix.'
}

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P8 lazy-rain + fused first-stage scout.' -ForegroundColor Green
Write-Host 'Every seed now initializes only temperature+blend before its first exact screen.'
Write-Host 'Center and 15 spatially spread gate/ring points execute together across all 16 seed lanes.'
Write-Host 'Four rain permutation tables are built only for seed groups that survive that temperature screen.'
Write-Host 'Ambiguous first points retain exact d0/temperature state and only evaluate the missing rain half after init.'
Write-Host 'All later P7 warp-vote probes, P4 compaction, tuned 4 seeds/block and tuned batch are preserved.'
Write-Host 'Scout acceptance/jackpot recall is unchanged: every rejection is still an exact in-target non-Tundra proof.'
Write-Host 'Also fixed the P7 banner literal-\\n cosmetic bug.'
