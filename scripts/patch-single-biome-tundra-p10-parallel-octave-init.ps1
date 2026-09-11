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

if ($text.Contains('TUNDRA_P10_PARALLEL_OCTAVE_INIT')) {
    Write-Host 'Tundra P10 parallel-octave initialization is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P9_BOUNDED_RAIN')) {
    throw 'P10 requires the P9 bounded-rain scout first.'
}
if (-not $text.Contains('TUNDRA_P8_LAZY_RAIN')) {
    throw 'P10 requires the P8 lazy-rain base.'
}
if (-not $text.Contains('TUNDRA_P4_GPU_COMPACTION')) {
    throw 'P10 requires P4 GPU compaction.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P10 requires exact 864x864 square semantics.'
}
if (-not $text.Contains('static constexpr int SEARCH_SEEDS_PER_BLOCK = 4; // TUNDRA_P3_GROUPED_SEARCH')) {
    throw 'P10 is specialized for the tuned 4-seeds/block winner.'
}

$backupPath = $sourcePath + '.p9-before-p10.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# ---------------------------------------------------------------------------
# P10 strategy
# ---------------------------------------------------------------------------
# P8/P9 made rain lazy, which exposed Perlin-table construction as a large part
# of the unconditional cost.  The old cold initializer has one lane build all
# four temperature octaves serially and another lane build both blend octaves.
#
# The JavaRandom stream for an octave is sequential, but the permutation table
# itself does NOT feed back into JavaRandom. Therefore an owner lane can reach
# the exact RNG state at octave N by replaying only the preceding RNG calls,
# without performing their identity writes or Fisher-Yates swaps. The owner
# then builds octave N with the unchanged initSearchPerlin().
#
# This gives six concurrent cold-table owners per seed:
#   lanes 0..3 : temperature octaves 0..3
#   lanes 4..5 : blend octaves 0..1
# and, after P8's temperature screen, four concurrent rain owners:
#   lanes 0..3 : rain octaves 0..3
#
# The deepest temperature owner executes 3 cheap RNG-only skips + 1 full table
# build instead of one lane executing 4 full table builds. nextInt(bound) is
# deliberately called normally while skipping so Java's rejection loop remains
# bit-exact for every seed. No fixed-draw-count jump or approximation is used.
# ---------------------------------------------------------------------------

$coldPattern = '(?s)__device__ __forceinline__ void p8InitColdClimate\(.*?\r?\n\}'
$coldMatches = [regex]::Matches($text, $coldPattern)
if ($coldMatches.Count -ne 1) {
    throw "Expected exactly one P8 cold initializer, found $($coldMatches.Count)."
}

$newCold = @'
// TUNDRA_P10_PARALLEL_OCTAVE_INIT
// Advance JavaRandom by exactly one SearchPerlinState constructor without
// touching permutation memory. The three discarded offsets consume the same
// six nextBits() calls as three nextDouble() calls. Each nextInt() is executed
// normally because its rejection path is seed/bound dependent.
__device__ __forceinline__ void p10ConsumeSearchPerlinRng(p20::JavaRandom& random) {
    // a, b, c offsets: three Java Random.nextDouble() calls, two state steps each.
    (void)random.nextBits(26); (void)random.nextBits(27);
    (void)random.nextBits(26); (void)random.nextBits(27);
    (void)random.nextBits(26); (void)random.nextBits(27);

    // Fisher-Yates draws. The permutation contents do not affect these bounds
    // or the RNG, so only reproducing nextInt() is required to reach the exact
    // start state of the following octave.
    for (int i = 0; i < 256; ++i) {
        (void)random.nextInt(256 - i);
    }
}

// Exact parallel cold-state construction. All owner lanes run the same code
// with independent JavaRandom registers and disjoint shared-memory destinations.
// The final block barrier already present in the P8/P9 kernel publishes every
// completed table before any climate evaluation begins.
__device__ __forceinline__ void p8InitColdClimate(
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

    // Replay complete preceding-octave RNG streams without their table work.
    // This is exact even when Random.nextInt() rejects and retries.
    for (int prior = 0; prior < octave; ++prior) {
        p10ConsumeSearchPerlinRng(rng);
    }

    if (tempOwner) {
        initSearchPerlin(rng, s.temp[octave]);
    } else {
        initSearchPerlin(rng, s.blend[octave]);
    }
}
'@
$text = [regex]::Replace($text, $coldPattern, $newCold.TrimEnd(), 1)

$rainPattern = '(?s)__device__ __forceinline__ void p8InitRain\(.*?\r?\n\}'
$rainMatches = [regex]::Matches($text, $rainPattern)
if ($rainMatches.Count -ne 1) {
    throw "Expected exactly one P8 lazy-rain initializer, found $($rainMatches.Count)."
}

$newRain = @'
// P10 parallel lazy-rain construction. P8 still decides which seed groups pay
// for rain at all; for a surviving group, lanes 0..3 construct one exact rain
// octave each. The following existing block barrier publishes all four tables.
__device__ __forceinline__ void p8InitRain(
        SearchClimateState& s,
        std::int64_t seed,
        int laneInSeed
) {
    if (laneInSeed >= 4) return;

    const int octave = laneInSeed;
    p20::JavaRandom rng;
    rng.setSeed(multipliedSeed(seed, 39811ULL));

    for (int prior = 0; prior < octave; ++prior) {
        p10ConsumeSearchPerlinRng(rng);
    }

    initSearchPerlin(rng, s.rain[octave]);
}
'@
$text = [regex]::Replace($text, $rainPattern, $newRain.TrimEnd(), 1)

# Keep all P9 bounded-rain evaluation logic; only advertise the new initializer.
$text = $text.Replace(
    'P9 scout: TUNDRA-only | lazy rain + bounded heavy-first octaves | fused SIMD | warp votes | GPU compaction | tuned 4x16',
    'P10 scout: TUNDRA-only | parallel exact octave init | lazy bounded rain | fused SIMD | warp votes | GPU compaction | tuned 4x16'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P10 parallel exact octave initialization.' -ForegroundColor Green
Write-Host 'Cold init now builds temp octaves 0..3 and blend octaves 0..1 concurrently across six lanes/seed.'
Write-Host 'Lazy rain survivors build rain octaves 0..3 concurrently across four lanes/seed.'
Write-Host 'Preceding octave RNG streams are replayed without permutation memory work; Random.nextInt rejection behavior remains exact.'
Write-Host 'No approximate RNG jump, biome shortcut, probe change or acceptance-set change was introduced.'
Write-Host 'P9 bounded rain, P8 lazy rain, P7 warp votes, P4 compaction, tuned batch and exact verifier are preserved.'
Write-Host 'True 864x864 all-Tundra jackpot recall is unchanged.'
