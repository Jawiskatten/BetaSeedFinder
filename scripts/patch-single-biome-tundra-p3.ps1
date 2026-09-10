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

$backupPath = $sourcePath + '.p2-before-p3.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

if ($text.Contains('TUNDRA_P3_GROUPED_SEARCH')) {
    Write-Host 'Tundra P3 optimization is already applied.'
    exit 0
}

if (-not $text.Contains('TUNDRA_P2_SEARCH')) {
    throw 'Apply .\scripts\patch-single-biome-tundra-p2.ps1 first. P3 upgrades the P2 scout.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P3 requires the exact 864x864 square target patch.'
}

$threadMarker = 'static constexpr int SEARCH_THREADS = 64;'
$threadReplacement = @'
static constexpr int SEARCH_THREADS = 64;
static constexpr int SEARCH_SEEDS_PER_BLOCK = 4; // TUNDRA_P3_GROUPED_SEARCH
static constexpr int SEARCH_LANES_PER_SEED = SEARCH_THREADS / SEARCH_SEEDS_PER_BLOCK;
static_assert(SEARCH_THREADS % SEARCH_SEEDS_PER_BLOCK == 0, "Grouped scout must divide the wavefront evenly");
'@
if (-not $text.Contains($threadMarker)) {
    throw 'Could not find SEARCH_THREADS constant.'
}
$text = $text.Replace($threadMarker, $threadReplacement.TrimEnd())

$oldInit = @'
__device__ __forceinline__ void initSearchClimate(SearchClimateState& s, std::int64_t seed) {
    const int lane = static_cast<int>(threadIdx.x);

    if (lane == 0) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 9871ULL));
        for (int i = 0; i < 4; ++i) initSearchPerlin(rng, s.temp[i]);
    } else if (lane == 1) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 39811ULL));
        for (int i = 0; i < 4; ++i) initSearchPerlin(rng, s.rain[i]);
    } else if (lane == 2) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 543321ULL));
        for (int i = 0; i < 2; ++i) initSearchPerlin(rng, s.blend[i]);
    }
    __syncthreads();
}
'@
$newInit = @'
__device__ __forceinline__ void initSearchClimate(
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
        rng.setSeed(multipliedSeed(seed, 39811ULL));
        for (int i = 0; i < 4; ++i) initSearchPerlin(rng, s.rain[i]);
    } else if (laneInSeed == 2) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 543321ULL));
        for (int i = 0; i < 2; ++i) initSearchPerlin(rng, s.blend[i]);
    }
}
'@
if (-not $text.Contains($oldInit)) {
    throw 'Could not find P2 initSearchClimate() to upgrade.'
}
$text = $text.Replace($oldInit, $newInit)

$oldColdShortcut = @'
    if (f < 0.1f) return true;
    if (f >= 0.5f) return false;
'@
$newColdShortcut = @'
    // Exact shortcut: rain is clamped to <=1, so when f < 0.2 we always have
    // f1 = rain * f < 0.2 and Beta 1.7.3 must classify the point as TUNDRA.
    if (f < 0.2f) return true;
    if (f >= 0.5f) return false;
'@
if (-not $text.Contains($oldColdShortcut)) {
    throw 'Could not find the P2 Tundra cold shortcut.'
}
$text = $text.Replace($oldColdShortcut, $newColdShortcut)

$kernelPattern = '(?s)__global__ void searchKernel\(.*?\n\}\n\n__global__ void exactKernel\('
$matches = [regex]::Matches($text, $kernelPattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one P2 searchKernel block, found $($matches.Count)."
}

$newKernel = @'
__device__ __forceinline__ void denseSquareProbePoint864(
        int r,
        int sample,
        int& dx,
        int& dz
) {
    constexpr int SAMPLES_PER_SIDE = 64;
    const int side = sample / SAMPLES_PER_SIDE;
    const int t = sample % SAMPLES_PER_SIDE;
    const int along = (t * (2 * r - 1)) / (SAMPLES_PER_SIDE - 1);
    if (side == 0) {
        dx = -r + along; dz = -r;
    } else if (side == 1) {
        dx = r - 1; dz = -r + along;
    } else if (side == 2) {
        dx = r - 1 - along; dz = r - 1;
    } else {
        dx = -r; dz = r - 1 - along;
    }
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
            baseBiome[seedIndex] = s.centerIsTundra ? static_cast<unsigned char>(TUNDRA) : 255;
        }
    }
    __syncthreads();

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
            } else if (minRequiredRadius < ringRadii[ringCount - 1]) {
                probeRadius[seedIndex] = static_cast<unsigned short>(minRequiredRadius);
            }
        }
        __syncthreads();
    }

    for (int ring = 0; ring < ringCount; ++ring) {
        const int r = ringRadii[ring];
        if (r <= minRequiredRadius) continue;

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
            } else if (r < ringRadii[ringCount - 1]) {
                probeRadius[seedIndex] = static_cast<unsigned short>(r);
            }
        }
        __syncthreads();
    }

    const int target = ringRadii[ringCount - 1];
    const int denseMin = target > 96 ? target - 96 : 1;
    for (int r = target; r >= denseMin; r -= 8) {
        if (validSeed && s.centerIsTundra != 0 && lane == 0) s.mismatch = 0;
        __syncthreads();

        if (validSeed && s.centerIsTundra != 0) {
            for (int sample = lane; sample < 256; sample += SEARCH_LANES_PER_SEED) {
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

    if (validSeed && s.centerIsTundra != 0 && lane == 0) {
        probeRadius[seedIndex] = static_cast<unsigned short>(target);
    }
}

__global__ void exactKernel(
'@

$text = [regex]::Replace($text, $kernelPattern, $newKernel, 1)

$oldGrid = '                dim3(count), dim3(SEARCH_THREADS), 0, 0,'
$newGrid = '                dim3((count + SEARCH_SEEDS_PER_BLOCK - 1) / SEARCH_SEEDS_PER_BLOCK), dim3(SEARCH_THREADS), 0, 0,'
if (-not $text.Contains($oldGrid)) {
    throw 'Could not find searchKernel launch grid.'
}
$text = $text.Replace($oldGrid, $newGrid)

$text = $text.Replace(
    'P2 scout: TUNDRA-only | compact 8-bit permutation state | dynamic record gate',
    'P3 scout: TUNDRA-only | 4 seeds/wave | dense outer-96 band | exact cold shortcut | dynamic record gate'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$runPath = Join-Path $ProjectRoot 'scripts\run-single-biome-radius.ps1'
if (Test-Path $runPath -PathType Leaf) {
    $runText = [System.IO.File]::ReadAllText($runPath)
    if ($runText.Contains('[int]$Batch = 16384')) {
        $runText = $runText.Replace('[int]$Batch = 16384', '[int]$Batch = 262144')
        [System.IO.File]::WriteAllText($runPath, $runText, [System.Text.UTF8Encoding]::new($false))
    } elseif ($runText.Contains('[int]$Batch = 65536')) {
        $runText = $runText.Replace('[int]$Batch = 65536', '[int]$Batch = 262144')
        [System.IO.File]::WriteAllText($runPath, $runText, [System.Text.UTF8Encoding]::new($false))
    }
}

Write-Host 'Applied TUNDRA P3 grouped GPU optimization.' -ForegroundColor Green
Write-Host 'Bulk scout: 4 independent seeds per 64-thread wavefront.'
Write-Host 'Rare full-sparse survivors: dense 3328-point outer-band filter before mandatory exact verification.'
Write-Host 'Tundra test now skips rainfall whenever quantized temperature is below 0.2.'
Write-Host 'A true 864x864 all-Tundra world still cannot be rejected by the scout.'
Write-Host 'Runner default batch raised to 262144 when the known default was found.'
