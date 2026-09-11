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

if ($text.Contains('TUNDRA_P6_CHUNKED_EXIT')) {
    Write-Host 'Tundra P6 chunked early-exit scout is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P5_OUTER_FIRST')) {
    throw 'P6 requires the P5 outer-first scout first.'
}
if (-not $text.Contains('TUNDRA_P4_GPU_COMPACTION')) {
    throw 'P6 requires P4 GPU compaction.'
}
if (-not $text.Contains('TUNDRA_P3_GROUPED_SEARCH')) {
    throw 'P6 requires the tuned grouped Tundra scout.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P6 requires exact 864x864 square semantics.'
}

$backupPath = $sourcePath + '.p5-before-p6.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# Replace only searchKernel. P4 compaction, tuned SPB, tuned batch, exact verifier,
# coverage and every other local patch remain untouched.
$kernelPattern = '(?s)__global__ void searchKernel\(.*?\r?\n\}\r?\n\r?\n// TUNDRA_P4_GPU_COMPACTION'
$matches = [regex]::Matches($text, $kernelPattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one P5 searchKernel block, found $($matches.Count)."
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
    // TUNDRA_P6_CHUNKED_EXIT
    // P6 accepts/rejects exactly the same sampled points as P5. The only changes
    // are evaluation order and early stopping inside a stage after a mismatch.
    // Sparse 64-point sets are traversed in a side-interleaved order so the first
    // chunk covers the whole perimeter rather than one contiguous side.
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

    // Record gate first, same as P5. Process one lane-width chunk at a time.
    // The 64-point permutation is bijective: logical 0..63 maps to all original
    // sample ids exactly once, but the first chunk is spread across all 4 sides.
    if (minRequiredRadius > 0) {
        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
            if (validSeed && s.centerIsTundra != 0 && s.mismatch == 0) {
                const int logical = base + lane;
                if (logical < 64) {
                    const int sample = (logical & 15) * 4 + (logical >> 4);
                    int dx = 0;
                    int dz = 0;
                    squareProbePoint864(minRequiredRadius, sample, dx, dz);
                    if (!searchIsTundraAt(s, centerX + dx, centerZ + dz)) {
                        atomicExch(&s.mismatch, 1);
                    }
                }
            }
            __syncthreads();
        }

        if (validSeed && s.centerIsTundra != 0 && lane == 0) {
            if (s.mismatch != 0) {
                s.centerIsTundra = 0;
            } else if (minRequiredRadius < target) {
                probeRadius[seedIndex] = static_cast<unsigned short>(minRequiredRadius);
            }
        }
        __syncthreads();
    }

    // Fresh-run bootstrap. Same rings and same 64 samples as P5, but chunked.
    if (minRequiredRadius <= 0) {
        for (int ring = 0; ring < ringCount; ++ring) {
            const int r = ringRadii[ring];

            if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
            __syncthreads();

            for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
                if (validSeed && s.centerIsTundra != 0 && s.mismatch == 0) {
                    const int logical = base + lane;
                    if (logical < 64) {
                        const int sample = (logical & 15) * 4 + (logical >> 4);
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
            }

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

    // ---------------- P6 mature outer-first path ----------------
    // Stage A: same 64 coarse points/layer as P5. Each chunk is spatially
    // interleaved across the four sides; once any point fails, later chunks for
    // that seed do no climate work.
    for (int r = target; r >= denseMin; r -= 8) {
        if (r <= minRequiredRadius) continue;

        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
            if (validSeed && s.centerIsTundra != 0 && s.mismatch == 0) {
                const int logical = base + lane;
                if (logical < 64) {
                    const int coarse = (logical & 15) * 4 + (logical >> 4);
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
        }

        if (validSeed && s.centerIsTundra != 0 && lane == 0 && s.mismatch != 0) {
            s.centerIsTundra = 0;
        }
        __syncthreads();
    }

    // Stage B: exactly the same 192 points/layer P5 used (all 256 dense points
    // except t divisible by 4, which Stage A already covered). Logical ids are
    // interleaved side-by-side and chunked for early stopping.
    for (int r = target; r >= denseMin; r -= 8) {
        if (r <= minRequiredRadius) continue;

        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        for (int base = 0; base < 192; base += SEARCH_LANES_PER_SEED) {
            if (validSeed && s.centerIsTundra != 0 && s.mismatch == 0) {
                const int logical = base + lane;
                if (logical < 192) {
                    const int side = logical & 3;
                    const int local = logical >> 2; // 0..47 per side
                    // 1,2,3,5,6,7,...,61,62,63: all t values except multiples of 4.
                    const int t = local + 1 + local / 3;
                    const int denseSample = side * 64 + t;
                    int dx = 0;
                    int dz = 0;
                    denseSquareProbePoint864(r, denseSample, dx, dz);
                    if (!searchIsTundraAt(s, centerX + dx, centerZ + dz)) {
                        atomicExch(&s.mismatch, 1);
                    }
                }
            }
            __syncthreads();
        }

        if (validSeed && s.centerIsTundra != 0 && lane == 0 && s.mismatch != 0) {
            s.centerIsTundra = 0;
        }
        __syncthreads();
    }

    // Same sparse interior rings as P5, outside-in, now chunked and spread.
    for (int ring = ringCount - 1; ring >= 0; --ring) {
        const int r = ringRadii[ring];
        if (r <= minRequiredRadius || r >= denseMin) continue;

        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        for (int base = 0; base < 64; base += SEARCH_LANES_PER_SEED) {
            if (validSeed && s.centerIsTundra != 0 && s.mismatch == 0) {
                const int logical = base + lane;
                if (logical < 64) {
                    const int sample = (logical & 15) * 4 + (logical >> 4);
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
        }

        if (validSeed && s.centerIsTundra != 0 && lane == 0 && s.mismatch != 0) {
            s.centerIsTundra = 0;
        }
        __syncthreads();
    }

    // Acceptance set is identical to P5: every configured sample passed.
    // P4 then compacts every target survivor for mandatory exact verification.
    if (validSeed && s.centerIsTundra != 0 && lane == 0) {
        probeRadius[seedIndex] = static_cast<unsigned short>(target);
    }
}

// TUNDRA_P4_GPU_COMPACTION
'@

$text = [regex]::Replace($text, $kernelPattern, $newKernel, 1)

$bannerPattern = 'std::cout\s*<<\s*"P5 scout:[^"\r\n]*\\n";'
if ([regex]::Matches($text, $bannerPattern).Count -eq 1) {
    $text = [regex]::Replace(
        $text,
        $bannerPattern,
        'std::cout << "P6 scout: TUNDRA-only | chunked early exit | spatially spread probes | GPU compaction | tuned grouped search\\n";',
        1
    )
} else {
    $text = $text.Replace(
        'P5 scout: TUNDRA-only | outer-first staged rejection | GPU compaction | tuned grouped search',
        'P6 scout: TUNDRA-only | chunked early exit | spatially spread probes | GPU compaction | tuned grouped search'
    )
}

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P6 chunked early-exit scout.' -ForegroundColor Green
Write-Host 'P6 evaluates exactly the same scout point set as P5; only ordering/early-stop behavior changed.'
Write-Host '64-point stages are processed one lane-width chunk at a time and spread across all four sides.'
Write-Host 'The 192-point dense remainder is also side-interleaved and stops after the first failing chunk.'
Write-Host 'P4 GPU compaction, exact verification, tuned seeds/block and tuned batch are preserved.'
Write-Host 'Jackpot recall is unchanged, and P5/P6 have the same full-scout acceptance set.'