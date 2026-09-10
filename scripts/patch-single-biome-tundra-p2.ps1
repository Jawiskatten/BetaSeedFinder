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

if ($text.Contains('TUNDRA_P2_SEARCH')) {
    Write-Host 'Tundra P2 optimization is already applied.'
    exit 0
}

if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'Apply .\scripts\patch-single-biome-square.ps1 first. P2 assumes the exact 864x864 square target.'
}

$searchPattern = '(?s)__global__ void searchKernel\(.*?__global__ void exactKernel\('
$matches = [regex]::Matches($text, $searchPattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one searchKernel/exactKernel block, found $($matches.Count)."
}

$replacement = @'
struct SearchPerlinState {
    // TUNDRA_P2_SEARCH
    // Search/scout-only compact state. Legacy permutation values are always 0..255,
    // so 256 unsigned bytes are sufficient. The duplicated 256 entries in
    // p20::PerlinState are reproduced with &255 at lookup time.
    unsigned char perm[256];
    double a;
    double b;
};

struct SearchClimateState {
    SearchPerlinState temp[4];
    SearchPerlinState rain[4];
    SearchPerlinState blend[2];
    int centerIsTundra;
    int mismatch;
};

__device__ __forceinline__ void initSearchPerlin(p20::JavaRandom& random, SearchPerlinState& out) {
    out.a = random.nextDouble() * 256.0;
    out.b = random.nextDouble() * 256.0;
    (void)random.nextDouble(); // legacy c offset: consume it even though simplex2 does not use it

    for (int i = 0; i < 256; ++i) out.perm[i] = static_cast<unsigned char>(i);
    for (int i = 0; i < 256; ++i) {
        const int j = random.nextInt(256 - i) + i;
        const unsigned char tmp = out.perm[i];
        out.perm[i] = out.perm[j];
        out.perm[j] = tmp;
    }
}

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

__device__ __forceinline__ double searchSimplex2(
        const SearchPerlinState& s,
        double xCoord,
        double zCoord
) {
    constexpr double F = 0.3660254037844386;
    constexpr double G = 0.21132486540518713;

    const double x = xCoord + s.a;
    const double z = zCoord + s.b;
    const double skew = (x + z) * F;
    const int i = p20::simplexFastFloor(x + skew);
    const int j = p20::simplexFastFloor(z + skew);
    const double unskew = static_cast<double>(i + j) * G;
    const double x0 = x - (static_cast<double>(i) - unskew);
    const double z0 = z - (static_cast<double>(j) - unskew);
    const int i1 = x0 > z0 ? 1 : 0;
    const int j1 = x0 > z0 ? 0 : 1;
    const double x1 = x0 - static_cast<double>(i1) + G;
    const double z1 = z0 - static_cast<double>(j1) + G;
    const double x2 = x0 - 1.0 + 2.0 * G;
    const double z2 = z0 - 1.0 + 2.0 * G;
    const int ii = i & 255;
    const int jj = j & 255;

    const int pjj0 = static_cast<int>(s.perm[jj]);
    const int pjj1 = static_cast<int>(s.perm[(jj + j1) & 255]);
    const int pjj2 = static_cast<int>(s.perm[(jj + 1) & 255]);
    const int g0 = static_cast<int>(s.perm[(ii + pjj0) & 255]) % 12;
    const int g1 = static_cast<int>(s.perm[(ii + i1 + pjj1) & 255]) % 12;
    const int g2 = static_cast<int>(s.perm[(ii + 1 + pjj2) & 255]) % 12;

    double t0 = 0.5 - x0 * x0 - z0 * z0;
    double n0;
    if (t0 < 0.0) n0 = 0.0;
    else { t0 *= t0; n0 = t0 * t0 * p20::simplexGrad2(g0, x0, z0); }

    double t1 = 0.5 - x1 * x1 - z1 * z1;
    double n1;
    if (t1 < 0.0) n1 = 0.0;
    else { t1 *= t1; n1 = t1 * t1 * p20::simplexGrad2(g1, x1, z1); }

    double t2 = 0.5 - x2 * x2 - z2 * z2;
    double n2;
    if (t2 < 0.0) n2 = 0.0;
    else { t2 *= t2; n2 = t2 * t2 * p20::simplexGrad2(g2, x2, z2); }

    return 70.0 * (n0 + n1 + n2);
}

__device__ __forceinline__ double searchOctaveNoise4(
        const SearchPerlinState states[4],
        double x,
        double z,
        double baseScale,
        double octaveScale
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 4; ++octave) {
        const double scale = (baseScale / 1.5) * d7;
        total += searchSimplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= octaveScale;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double searchOctaveNoise2(
        const SearchPerlinState states[2],
        double x,
        double z
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 2; ++octave) {
        const double scale = (0.25 / 1.5) * d7;
        total += searchSimplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= 0.5882352941176471;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ bool searchIsTundraAt(
        const SearchClimateState& s,
        int blockX,
        int blockZ
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

    if (f < 0.1f) return true;
    if (f >= 0.5f) return false;

    const double rainRaw = searchOctaveNoise4(
            s.rain, x, z, 0.05000000074505806, 0.3333333333333333);
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    rain = clamp01(rain);

    int ri = static_cast<int>(rain * 63.0);
    if (ri < 0) ri = 0;
    if (ri > 63) ri = 63;
    float f1 = static_cast<float>(ri) / 63.0f;
    f1 *= f;
    return f1 < 0.2f;
}

__device__ __forceinline__ void squareProbePoint864(
        int r,
        int lane,
        int& dx,
        int& dz
) {
    const int side = lane / 16;
    const int t = lane % 16;
    const int along = (t * (2 * r - 1)) / 15;
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
    const int seedIndex = static_cast<int>(blockIdx.x);
    const int lane = static_cast<int>(threadIdx.x);
    if (seedIndex >= count || lane >= SEARCH_THREADS) return;

    __shared__ SearchClimateState s;
    const std::uint64_t attempt = startAttempt + static_cast<std::uint64_t>(seedIndex);
    const std::int64_t seed = static_cast<std::int64_t>(
            p20::splitMixDeterministicSeed(sequence, attempt));
    initSearchClimate(s, seed);

    if (lane == 0) {
        s.centerIsTundra = searchIsTundraAt(s, centerX, centerZ) ? 1 : 0;
        probeRadius[seedIndex] = 0;
        baseBiome[seedIndex] = s.centerIsTundra ? static_cast<unsigned char>(TUNDRA) : 255;
    }
    __syncthreads();

    if (s.centerIsTundra == 0) return;

    if (minRequiredRadius > 0) {
        if (lane == 0) s.mismatch = 0;
        __syncthreads();

        int dx = 0;
        int dz = 0;
        squareProbePoint864(minRequiredRadius, lane, dx, dz);
        if (!searchIsTundraAt(s, centerX + dx, centerZ + dz)) {
            atomicExch(&s.mismatch, 1);
        }
        __syncthreads();

        if (s.mismatch != 0) return;
        if (lane == 0) {
            probeRadius[seedIndex] = static_cast<unsigned short>(minRequiredRadius);
        }
        __syncthreads();
    }

    for (int ring = 0; ring < ringCount; ++ring) {
        const int r = ringRadii[ring];
        if (r <= minRequiredRadius) continue;

        if (lane == 0) s.mismatch = 0;
        __syncthreads();

        const int pointIndex = ring * SEARCH_THREADS + lane;
        if (!searchIsTundraAt(
                s,
                centerX + probeDx[pointIndex],
                centerZ + probeDz[pointIndex])) {
            atomicExch(&s.mismatch, 1);
        }
        __syncthreads();

        if (s.mismatch != 0) return;
        if (lane == 0) {
            probeRadius[seedIndex] = static_cast<unsigned short>(r);
        }
        __syncthreads();
    }
}

__global__ void exactKernel(
'@

$text = [regex]::Replace($text, $searchPattern, $replacement, 1)

$oldLaunch = @'
                dProbeDx, dProbeDz, dRingRadii, static_cast<int>(ringRadii.size()),
                dProbeRadius, dBiome);
'@

$newLaunch = @'
                dProbeDx, dProbeDz, dRingRadii, static_cast<int>(ringRadii.size()),
                bestExact >= 0 ? std::min(o.target, bestExact + 1) : 0,
                dProbeRadius, dBiome);
'@

if (-not $text.Contains($oldLaunch)) {
    throw 'Could not find searchKernel launch arguments to add the dynamic record gate.'
}
$text = $text.Replace($oldLaunch, $newLaunch)

$oldBanner = '    std::cout << "A full target hit is always exact-verified before being reported. Ctrl+C stops the run.\n\n";'
$newBanner = @'
    std::cout << "P2 scout: TUNDRA-only | compact 8-bit permutation state | dynamic record gate\n";
    std::cout << "A full target hit is always exact-verified before being reported. Ctrl+C stops the run.\n\n";
'@
if ($text.Contains($oldBanner)) {
    $text = $text.Replace($oldBanner, $newBanner.TrimEnd())
}

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P2 search optimization.' -ForegroundColor Green
Write-Host 'Search is now Tundra-only; verify mode remains generic/exact.'
Write-Host 'Scout state uses compact 8-bit permutations; exact verifier and realCoverage remain unchanged.'
Write-Host 'After a record exists, scout work below currentBest+1 is skipped.'
