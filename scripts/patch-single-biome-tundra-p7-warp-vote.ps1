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

if ($text.Contains('TUNDRA_P7_WARP_VOTE')) {
    Write-Host 'Tundra P7 warp-vote scout is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P6_CHUNKED_EXIT')) {
    throw 'P7 requires the P6 chunked early-exit scout first.'
}
if (-not $text.Contains('TUNDRA_P4_GPU_COMPACTION')) {
    throw 'P7 requires P4 GPU compaction.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P7 requires exact 864x864 square semantics.'
}

# This kernel is intentionally specialized for the measured winner on the
# RX 7800 XT: 4 seeds/block => 16 contiguous lanes/seed. That width fits evenly
# inside both AMD wave32 and wave64 execution and lets a ballot isolate each seed.
$spbPattern = 'static constexpr int SEARCH_SEEDS_PER_BLOCK = 4; // TUNDRA_P3_GROUPED_SEARCH'
if (-not $text.Contains($spbPattern)) {
    throw 'P7 is specialized for the tuned winner SEARCH_SEEDS_PER_BLOCK=4. Re-run/apply the GPU tuner winner first.'
}

$backupPath = $sourcePath + '.p6-before-p7.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# Replace searchKernel only. Exact verification, P4 compaction, tuned batch,
# coverage accounting and all other local patches remain untouched.
$kernelPattern = '(?s)__global__ void searchKernel\(.*?\r?\n\}\r?\n\r?\n// TUNDRA_P4_GPU_COMPACTION'
$matches = [regex]::Matches($text, $kernelPattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one P6 searchKernel block, found $($matches.Count)."
}

$newKernel = @'
// TUNDRA_P7_WARP_VOTE
// P7 is specialized for the tuned 4-seeds/block configuration. Each seed owns
// 16 contiguous lanes. 16 divides both AMD wave32 and wave64, so a warp ballot
// can isolate failures for one seed without a shared atomic or block barrier.
static_assert(SEARCH_SEEDS_PER_BLOCK == 4, "P7 requires the tuned 4-seeds/block configuration");
static_assert(SEARCH_LANES_PER_SEED == 16, "P7 requires 16 contiguous lanes per seed");

__device__ __forceinline__ bool p7SeedAnyFailed(
        bool failed,
        int globalLane,
        int laneInSeed
) {
    // HIP ballots are 64-bit on both wave32 and wave64. Higher bits are simply
    // unused on wave32. Because every 16-lane seed group is wholly contained in
    // one wave, masking its 16 bits gives an independent per-seed vote.
    const unsigned long long votes = __ballot(failed ? 1 : 0);
    const int laneInWave = globalLane % warpSize;
    const int seedBaseInWave = laneInWave - laneInSeed;
    const unsigned long long seedMask = 0xFFFFULL << seedBaseInWave;
    return (votes & seedMask) != 0ULL;
}

__device__ __forceinline__ bool p7WaveHasLiveSeed(bool alive) {
    // All lanes in a 16-lane seed group keep the same local alive value.
    // A zero ballot means every seed represented by this hardware wave is dead;
    // with no block-wide barriers after initialization, that wave may return.
    return __ballot(alive ? 1 : 0) != 0ULL;
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

    if (validSeed) {
        const std::uint64_t attempt = startAttempt + static_cast<std::uint64_t>(seedIndex);
        const std::int64_t seed = static_cast<std::int64_t>(
                p20::splitMixDeterministicSeed(sequence, attempt));
        initSearchClimate(s, seed, lane);
    }

    // Climate initialization is the only point where lanes write different
    // pieces of shared SearchClimateState. One block barrier makes all states
    // visible; every later climate lookup is read-only.
    __syncthreads();

    // Only the leader evaluates the center, then a per-seed ballot broadcasts
    // failure to its other 15 lanes. This removes P6's second block barrier and
    // shared center/mismatch bookkeeping from the hot path.
    bool centerFailed = false;
    if (validSeed && lane == 0) {
        const bool centerTundra = searchIsTundraAt(s, centerX, centerZ);
        centerFailed = !centerTundra;
        probeRadius[seedIndex] = 0;
        baseBiome[seedIndex] = centerTundra ? static_cast<unsigned char>(TUNDRA) : 255;
    }
    const bool groupCenterFailed = p7SeedAnyFailed(centerFailed, globalLane, lane);
    bool alive = validSeed && !groupCenterFailed;

    // On RDNA wave32 this retires two dead seed groups at once. On wave64 it
    // retires the whole 4-seed block. There are deliberately no __syncthreads()
    // below this point, so independent wave retirement is safe.
    if (!p7WaveHasLiveSeed(alive)) return;

    const int target = ringRadii[ringCount - 1];
    const int denseMin = target > 96 ? target - 96 : 1;

    // ------------------------- dynamic record gate -------------------------
    // Same 64 exact in-target samples as P6, same 16-sample early-exit chunks.
    if (minRequiredRadius > 0) {
        for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
            bool failed = false;
            if (alive) {
                const int logical = base + lane;
                if (logical < 64) {
                    // Bijective side-spread permutation used by P6.
                    const int sample = (logical & 15) * 4 + (logical >> 4);
                    int dx = 0;
                    int dz = 0;
                    squareProbePoint864(minRequiredRadius, sample, dx, dz);
                    failed = !searchIsTundraAt(s, centerX + dx, centerZ + dz);
                }
            }
            if (p7SeedAnyFailed(failed, globalLane, lane)) alive = false;
        }

        if (alive && lane == 0 && minRequiredRadius < target) {
            probeRadius[seedIndex] = static_cast<unsigned short>(minRequiredRadius);
        }
        if (!p7WaveHasLiveSeed(alive)) return;
    }

    // ---------------------------- bootstrap -------------------------------
    // First-batch scoring only. Point set and ordering are identical to P6,
    // but chunk completion uses warp votes instead of shared atomics/barriers.
    if (minRequiredRadius <= 0) {
        for (int ring = 0; ring < ringCount; ++ring) {
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
                if (p7SeedAnyFailed(failed, globalLane, lane)) alive = false;
            }

            if (alive && lane == 0 && r < target) {
                probeRadius[seedIndex] = static_cast<unsigned short>(r);
            }
            if (!p7WaveHasLiveSeed(alive)) return;
        }
    }

    // ---------------------- outer-band coarse pass ------------------------
    // Same 64 points/layer as P6. The first 16 are distributed across all
    // four sides, so broad edge intrusions still tend to die after one ballot.
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
            if (p7SeedAnyFailed(failed, globalLane, lane)) alive = false;
        }

        if (!p7WaveHasLiveSeed(alive)) return;
    }

    // ---------------------- dense outer remainder -------------------------
    // Exactly P6's remaining 192 samples/layer. No Stage-A duplicates.
    for (int r = target; r >= denseMin; r -= 8) {
        if (r <= minRequiredRadius) continue;

        for (int base = 0; base < 192; base += SEARCH_LANES_PER_SEED) {
            bool failed = false;
            if (alive) {
                const int logical = base + lane;
                if (logical < 192) {
                    const int side = logical & 3;
                    const int local = logical >> 2;
                    const int t = local + 1 + local / 3; // excludes multiples of 4
                    const int denseSample = side * 64 + t;
                    int dx = 0;
                    int dz = 0;
                    denseSquareProbePoint864(r, denseSample, dx, dz);
                    failed = !searchIsTundraAt(s, centerX + dx, centerZ + dz);
                }
            }
            if (p7SeedAnyFailed(failed, globalLane, lane)) alive = false;
        }

        if (!p7WaveHasLiveSeed(alive)) return;
    }

    // ------------------------- sparse interior ----------------------------
    // Same P6 rings, outside-in, only above the dynamic exact-record gate.
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
            if (p7SeedAnyFailed(failed, globalLane, lane)) alive = false;
        }

        if (!p7WaveHasLiveSeed(alive)) return;
    }

    // P7 has exactly the same full-scout acceptance set as P6. P4 compaction
    // still sends every target survivor to mandatory exact block verification.
    if (alive && lane == 0) {
        probeRadius[seedIndex] = static_cast<unsigned short>(target);
    }
}

// TUNDRA_P4_GPU_COMPACTION
'@

$text = [regex]::Replace($text, $kernelPattern, $newKernel, 1)

$bannerPattern = 'std::cout\s*<<\s*"P6 scout:[^"\r\n]*\\n";'
if ([regex]::Matches($text, $bannerPattern).Count -eq 1) {
    $text = [regex]::Replace(
        $text,
        $bannerPattern,
        'std::cout << "P7 scout: TUNDRA-only | warp-vote early exit | 16 lanes/seed | GPU compaction | tuned grouped search\\n";',
        1
    )
} else {
    $text = $text.Replace(
        'P6 scout: TUNDRA-only | chunked early exit | spatially spread probes | GPU compaction | tuned grouped search',
        'P7 scout: TUNDRA-only | warp-vote early exit | 16 lanes/seed | GPU compaction | tuned grouped search'
    )
}

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P7 warp-vote scout.' -ForegroundColor Green
Write-Host 'Kept the tuner-proven 4 seeds/block (16 lanes/seed); no underfilled microchunks.'
Write-Host 'Removed hot-loop shared mismatch atomics and block-wide barriers; one HIP ballot now resolves each 16-probe chunk.'
Write-Host 'A hardware wave returns as soon as every seed in that wave is dead.'
Write-Host 'P7 evaluates the same scout point set and uses the same 16-sample early-exit boundaries as P6.'
Write-Host 'P4 GPU compaction, exact verification, tuned batch and jackpot recall are preserved.'
