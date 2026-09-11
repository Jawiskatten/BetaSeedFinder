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

if ($text.Contains('TUNDRA_P5_OUTER_FIRST')) {
    Write-Host 'Tundra P5 outer-first scout is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P4_GPU_COMPACTION')) {
    throw 'P5 requires the P4 GPU-compaction scout first.'
}
if (-not $text.Contains('TUNDRA_P3_GROUPED_SEARCH')) {
    throw 'P5 requires the tuned P3 grouped Tundra scout.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P5 requires the exact 864x864 square target patch.'
}

$backupPath = $sourcePath + '.p4-before-p5.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P4 does not alter searchKernel itself; it inserts its compaction block immediately
# after it. Replace only that kernel so tuned SPB/batch settings and P4 handoff survive.
$kernelPattern = '(?s)__global__ void searchKernel\(.*?\r?\n\}\r?\n\r?\n// TUNDRA_P4_GPU_COMPACTION'
$matches = [regex]::Matches($text, $kernelPattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one P4 searchKernel block, found $($matches.Count)."
}

$newKernel = @'
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
    // TUNDRA_P5_OUTER_FIRST
    // Four (or autotuned number of) independent seeds share one 64-thread block.
    // The mature path intentionally checks likely edge failures before spending
    // work on deeper interior rings. Every tested point remains inside the exact
    // target square, so this can add false positives but can never reject a real
    // all-Tundra 864x864 jackpot.
    const int globalLane = static_cast<int>(threadIdx.x);
    const int seedGroup = globalLane / SEARCH_LANES_PER_SEED;
    const int lane = globalLane - seedGroup * SEARCH_LANES_PER_SEED;
    const int seedIndex = static_cast<int>(blockIdx.x) * SEARCH_SEEDS_PER_BLOCK + seedGroup;
    const bool validSeed = seedIndex < count;

    __shared__ SearchClimateState states[SEARCH_SEEDS_PER_BLOCK];
    SearchClimateState& s = states[seedGroup];

    std::uint64_t attempt = 0;
    std::int64_t seed = 0;
    if (validSeed) {
        attempt = startAttempt + static_cast<std::uint64_t>(seedIndex);
        seed = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(sequence, attempt));
        initSearchClimate(s, seed, lane);
    }
    __syncthreads();

    if (lane == 0) {
        s.centerIsTundra = 0;
        s.mismatch = 0;
        if (validSeed) {
            s.centerIsTundra = searchIsTundraAt(s, centerX, centerZ) ? 1 : 0;
            probeRadius[seedIndex] = 0;
            baseBiome[seedIndex] = s.centerIsTundra
                    ? static_cast<unsigned char>(TUNDRA) : 255;
        }
    }
    __syncthreads();

    const int target = ringRadii[ringCount - 1];
    const int denseMin = target > 96 ? target - 96 : 1;

    // The dynamic record gate is kept first. Passing this exact in-target
    // perimeter proves the seed is still capable of improving bestExact and
    // gives P4 a meaningful score even if a later outer-edge test rejects it.
    if (minRequiredRadius > 0) {
        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0) {
            for (int sample = lane; sample < 64; sample += SEARCH_LANES_PER_SEED) {
                int dx = 0;
                int dz = 0;
                squareProbePoint864(minRequiredRadius, sample, dx, dz);
                if (!searchIsTundraAt(s, centerX + dx, centerZ + dz)) {
                    atomicExch(&s.mismatch, 1);
                }
            }
        }
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0 && lane == 0) {
            if (s.mismatch != 0) {
                s.centerIsTundra = 0;
            } else if (minRequiredRadius < target) {
                probeRadius[seedIndex] = static_cast<unsigned short>(minRequiredRadius);
            }
        }
        __syncthreads();
    }

    // Fresh-run bootstrap only: preserve P3's ascending sparse scoring while no
    // exact record exists yet. This normally lasts a single batch and quickly
    // establishes a strong minRequiredRadius for the fast mature path below.
    if (minRequiredRadius <= 0) {
        for (int ring = 0; ring < ringCount; ++ring) {
            const int r = ringRadii[ring];

            if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
            __syncthreads();

            if (validSeed && s.centerIsTundra != 0) {
                for (int sample = lane; sample < SEARCH_THREADS; sample += SEARCH_LANES_PER_SEED) {
                    const int pointIndex = ring * SEARCH_THREADS + sample;
                    if (!searchIsTundraAt(
                            s,
                            centerX + probeDx[pointIndex],
                            centerZ + probeDz[pointIndex])) {
                        atomicExch(&s.mismatch, 1);
                    }
                }
            }
            __syncthreads();

            if (validSeed && s.centerIsTundra != 0 && lane == 0) {
                if (s.mismatch != 0) {
                    s.centerIsTundra = 0;
                } else if (r < target) {
                    probeRadius[seedIndex] = static_cast<unsigned short>(r);
                }
            }
            __syncthreads();
        }
    }

    // ------------------------ P5 mature outer-first path --------------------
    // Stage A: only 64 evenly distributed samples per useful outer-band layer.
    // This is intentionally done across ALL outer layers before densifying any
    // one layer. Broad/edge intrusions die after 1/4 of the old dense work.
    // Layers at/below the current record gate are skipped: they cannot help
    // reject a jackpot and exact verification handles any interior hole.
    for (int r = target; r >= denseMin; r -= 8) {
        if (r <= minRequiredRadius) continue;

        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0) {
            // 16 samples per side. denseSquareProbePoint864 uses 64 slots/side;
            // t=0,4,8,...60 gives an evenly spread 64-point perimeter scout.
            for (int coarse = lane; coarse < 64; coarse += SEARCH_LANES_PER_SEED) {
                const int side = coarse / 16;
                const int t = coarse - side * 16;
                const int denseSample = side * 64 + t * 4;
                int dx = 0;
                int dz = 0;
                denseSquareProbePoint864(r, denseSample, dx, dz);
                if (!searchIsTundraAt(s, centerX + dx, centerZ + dz)) {
                    atomicExch(&s.mismatch, 1);
                }
            }
        }
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0 && lane == 0 && s.mismatch != 0) {
            s.centerIsTundra = 0;
        }
        __syncthreads();
    }

    // Stage B: only the seeds that passed every cheap outer sweep receive the
    // remaining 192 points/layer. Skip t%4==0 because Stage A already checked
    // those exact samples, eliminating 25% duplicate dense evaluations.
    for (int r = target; r >= denseMin; r -= 8) {
        if (r <= minRequiredRadius) continue;

        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0) {
            for (int sample = lane; sample < 256; sample += SEARCH_LANES_PER_SEED) {
                if ((sample & 63) % 4 == 0) continue; // already checked in Stage A
                int dx = 0;
                int dz = 0;
                denseSquareProbePoint864(r, sample, dx, dz);
                if (!searchIsTundraAt(s, centerX + dx, centerZ + dz)) {
                    atomicExch(&s.mismatch, 1);
                }
            }
        }
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0 && lane == 0 && s.mismatch != 0) {
            s.centerIsTundra = 0;
        }
        __syncthreads();
    }

    // Only exceptionally strong outer-band survivors spend work on sparse
    // interior rings above the dynamic gate. Check from outside inward, where
    // a boundary crossing is statistically more likely to be encountered first.
    for (int ring = ringCount - 1; ring >= 0; --ring) {
        const int r = ringRadii[ring];
        if (r <= minRequiredRadius || r >= denseMin) continue;

        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0) {
            for (int sample = lane; sample < SEARCH_THREADS; sample += SEARCH_LANES_PER_SEED) {
                const int pointIndex = ring * SEARCH_THREADS + sample;
                if (!searchIsTundraAt(
                        s,
                        centerX + probeDx[pointIndex],
                        centerZ + probeDz[pointIndex])) {
                    atomicExch(&s.mismatch, 1);
                }
            }
        }
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0 && lane == 0 && s.mismatch != 0) {
            s.centerIsTundra = 0;
        }
        __syncthreads();
    }

    // Surviving every configured in-target P5 sample means "full scout pass".
    // P4 compaction then guarantees mandatory exact block-by-block verification.
    if (validSeed && s.centerIsTundra != 0 && lane == 0) {
        probeRadius[seedIndex] = static_cast<unsigned short>(target);
    }
}

// TUNDRA_P4_GPU_COMPACTION
'@

$text = [regex]::Replace($text, $kernelPattern, $newKernel, 1)

# Fix the P4 banner's missing newline while upgrading its label.
$bannerPattern = 'std::cout\s*<<\s*"P4 scout:[^"\r\n]*";'
if ([regex]::Matches($text, $bannerPattern).Count -eq 1) {
    $text = [regex]::Replace(
        $text,
        $bannerPattern,
        'std::cout << "P5 scout: TUNDRA-only | outer-first staged rejection | GPU compaction | tuned grouped search\\n";',
        1
    )
} else {
    # Fallback for a manually corrected banner.
    $text = $text.Replace(
        'P4 scout: TUNDRA-only | GPU survivor compaction | tuned grouped search | dense outer-band',
        'P5 scout: TUNDRA-only | outer-first staged rejection | GPU compaction | tuned grouped search'
    )
}

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P5 outer-first staged scout.' -ForegroundColor Green
Write-Host 'Mature search order: record gate -> cheap outer band -> dense remainder -> sparse interior.'
Write-Host 'Dense outer samples are split 64 + 192, with no duplicate evaluations.'
Write-Host 'Outer layers at/below the current exact-record gate are skipped.'
Write-Host 'P4 GPU survivor compaction and your tuned seeds/block + batch settings are preserved.'
Write-Host 'True 864x864 all-Tundra jackpot recall is unchanged; every full scout survivor remains exact-verified.'
