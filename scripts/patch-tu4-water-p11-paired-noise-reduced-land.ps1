param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\tu4_water_finder.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "TU4 water source not found: $sourcePath"
}

$text = [System.IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")

if ($text.Contains('TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND')) {
    Write-Host 'TU4 Water P11 paired-noise/reduced-land optimization is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P10_RESTORE_PARTIAL_SORT')) {
    throw 'P11 requires TU4 Water P10 first.'
}
if (-not $text.Contains('TU4_WATER_P8_BATCHED_EXACT')) {
    throw 'P11 expects the P8 batched exact evaluator underneath P10.'
}

$backupPath = $sourcePath + '.p10-before-water-p11.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P11 attacks GPU arithmetic rather than host-side bookkeeping.
#
# 1) The P5/P10 scout evaluates two X positions with identical Z for each lane.
#    p20::perlin2 was called twice, redundantly recomputing the Z floor/fade and
#    related setup. p11Perlin2PairX computes the common Z path once while keeping
#    each X path and every gradient/lerp operation exactly the same.
#
# 2) Exact terrain repeatedly evaluates the same Perlin octave at y=7 and y=8
#    for one X/Z lattice point. p11Perlin3PairY shares the identical X/Z setup
#    and evaluates the two Y paths separately. The returned values are bitwise
#    equivalent to two p20::perlin3 calls because arithmetic within each output
#    keeps the original operation order.
#
# 3) P8's land kernel atomically added once per coarse cell (~40,000 atomics per
#    finalist) into a single counter. P11 reduces 256 cell results inside each
#    workgroup and performs one atomic add per workgroup (~157 per finalist).
#
# Scout metric, P6 16/4/4 portfolio, P7 800x800 metric, TopExact=24, and exact
# terrain equations are unchanged. This is intended as a pure throughput patch.

$markerPos = $text.IndexOf('// TU4_WATER_P10_RESTORE_PARTIAL_SORT')
if ($markerPos -lt 0) {
    throw 'Could not locate P10 marker.'
}
$marker = @'
// TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND
// Share repeated Perlin setup in scout/exact kernels and reduce land-count atomics.
// Search semantics and exact terrain results remain unchanged.
'@
$text = $text.Insert($markerPos, $marker + "`n")

# -------------------------------------------------------------------------
# Paired Perlin helpers. Keep these local to the TU4 finder so the validated
# shared p20 exact-math header remains untouched.
# -------------------------------------------------------------------------
$scoutKernelPos = $text.IndexOf('__global__ void waterScoutKernel(')
if ($scoutKernelPos -lt 0) {
    throw 'Could not locate waterScoutKernel.'
}

$pairHelpers = @'
// P11: exact-equivalent pair of p20::perlin2 calls that share the Z-axis setup.
__device__ __forceinline__ void p11Perlin2PairX(
        const p20::PerlinState& p,
        double xCoordA,
        double xCoordB,
        double zCoord,
        double& outA,
        double& outB
) {
    double z = zCoord + p.c;
    const int fz = p20::javaFloor(z);
    const int zi = fz & 255;
    z -= static_cast<double>(fz);
    const double z1 = z - 1.0;
    const double zf = p20::fade(z);

    {
        double x = xCoordA + p.a;
        const int fx = p20::javaFloor(x);
        const int xi = fx & 255;
        x -= static_cast<double>(fx);
        const double x1 = x - 1.0;
        const double xf = p20::fade(x);
        const int xp0 = p.perm[xi];
        const int xp1 = p.perm[xi + 1];
        const int p00 = p.perm[xp0] + zi;
        const int p10 = p.perm[xp1] + zi;
        const double q0 = p20::lerp(xf,
                p20::grad2Legacy(p.perm[p00], x, z),
                p20::grad3(p.perm[p10], x1, 0.0, z));
        const double q1 = p20::lerp(xf,
                p20::grad3(p.perm[p00 + 1], x, 0.0, z1),
                p20::grad3(p.perm[p10 + 1], x1, 0.0, z1));
        outA = p20::lerp(zf, q0, q1);
    }

    {
        double x = xCoordB + p.a;
        const int fx = p20::javaFloor(x);
        const int xi = fx & 255;
        x -= static_cast<double>(fx);
        const double x1 = x - 1.0;
        const double xf = p20::fade(x);
        const int xp0 = p.perm[xi];
        const int xp1 = p.perm[xi + 1];
        const int p00 = p.perm[xp0] + zi;
        const int p10 = p.perm[xp1] + zi;
        const double q0 = p20::lerp(xf,
                p20::grad2Legacy(p.perm[p00], x, z),
                p20::grad3(p.perm[p10], x1, 0.0, z));
        const double q1 = p20::lerp(xf,
                p20::grad3(p.perm[p00 + 1], x, 0.0, z1),
                p20::grad3(p.perm[p10 + 1], x1, 0.0, z1));
        outB = p20::lerp(zf, q0, q1);
    }
}

// P11: exact-equivalent pair of p20::perlin3 calls sharing X/Z setup.
__device__ __forceinline__ void p11Perlin3PairY(
        const p20::PerlinState& p,
        double xCoord,
        double yCoordA,
        double yCoordB,
        double zCoord,
        double& outA,
        double& outB
) {
    double x = xCoord + p.a;
    double z = zCoord + p.c;
    const int fx = p20::javaFloor(x);
    const int fz = p20::javaFloor(z);
    const int xi = fx & 255;
    const int zi = fz & 255;
    x -= static_cast<double>(fx);
    z -= static_cast<double>(fz);
    const double x1 = x - 1.0;
    const double z1 = z - 1.0;
    const double xf = p20::fade(x);
    const double zf = p20::fade(z);
    const int xp0 = p.perm[xi];
    const int xp1 = p.perm[xi + 1];

    {
        double y = yCoordA + p.b;
        const int fy = p20::javaFloor(y);
        const int yi = fy & 255;
        y -= static_cast<double>(fy);
        const double y1 = y - 1.0;
        const double yf = p20::fade(y);
        const int p0 = xp0 + yi;
        const int p00 = p.perm[p0] + zi;
        const int p01 = p.perm[p0 + 1] + zi;
        const int p1 = xp1 + yi;
        const int p10 = p.perm[p1] + zi;
        const int p11 = p.perm[p1 + 1] + zi;
        const double q00 = p20::lerp(xf,
                p20::grad3(p.perm[p00], x, y, z),
                p20::grad3(p.perm[p10], x1, y, z));
        const double q01 = p20::lerp(xf,
                p20::grad3(p.perm[p01], x, y1, z),
                p20::grad3(p.perm[p11], x1, y1, z));
        const double q10 = p20::lerp(xf,
                p20::grad3(p.perm[p00 + 1], x, y, z1),
                p20::grad3(p.perm[p10 + 1], x1, y, z1));
        const double q11 = p20::lerp(xf,
                p20::grad3(p.perm[p01 + 1], x, y1, z1),
                p20::grad3(p.perm[p11 + 1], x1, y1, z1));
        const double r0 = p20::lerp(yf, q00, q01);
        const double r1 = p20::lerp(yf, q10, q11);
        outA = p20::lerp(zf, r0, r1);
    }

    {
        double y = yCoordB + p.b;
        const int fy = p20::javaFloor(y);
        const int yi = fy & 255;
        y -= static_cast<double>(fy);
        const double y1 = y - 1.0;
        const double yf = p20::fade(y);
        const int p0 = xp0 + yi;
        const int p00 = p.perm[p0] + zi;
        const int p01 = p.perm[p0 + 1] + zi;
        const int p1 = xp1 + yi;
        const int p10 = p.perm[p1] + zi;
        const int p11 = p.perm[p1 + 1] + zi;
        const double q00 = p20::lerp(xf,
                p20::grad3(p.perm[p00], x, y, z),
                p20::grad3(p.perm[p10], x1, y, z));
        const double q01 = p20::lerp(xf,
                p20::grad3(p.perm[p01], x, y1, z),
                p20::grad3(p.perm[p11], x1, y1, z));
        const double q10 = p20::lerp(xf,
                p20::grad3(p.perm[p00 + 1], x, y, z1),
                p20::grad3(p.perm[p10 + 1], x1, y, z1));
        const double q11 = p20::lerp(xf,
                p20::grad3(p.perm[p01 + 1], x, y1, z1),
                p20::grad3(p.perm[p11 + 1], x1, y1, z1));
        const double r0 = p20::lerp(yf, q00, q01);
        const double r1 = p20::lerp(yf, q10, q11);
        outB = p20::lerp(zf, r0, r1);
    }
}

'@
$text = $text.Insert($scoutKernelPos, $pairHelpers)

# -------------------------------------------------------------------------
# Scout: replace two perlin2 calls per tail octave with one paired path.
# -------------------------------------------------------------------------
$scoutKernelPos = $text.IndexOf('__global__ void waterScoutKernel(')
$scoutEvalStart = $text.IndexOf('    double noise5A = 0.0;', $scoutKernelPos)
if ($scoutEvalStart -lt 0) {
    throw 'Could not locate P5/P10 scout paired-noise evaluation start.'
}
$scoutEvalEnd = $text.IndexOf('    const double d7A = heightCenterFromNoise5(noise5A);', $scoutEvalStart)
if ($scoutEvalEnd -lt 0) {
    throw 'Could not locate P5/P10 scout paired-noise evaluation end.'
}

$newScoutEval = @'
    double noise5A = 0.0;
    double noise5B = 0.0;
    // P11: both samples share Z, so compute the common Perlin Z path once.
    double amplitude = 1.0 / 4096.0;
#pragma unroll
    for (int tail = 0; tail < 4; ++tail) {
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        double sampleA;
        double sampleB;
        p11Perlin2PairX(
                tailPerlin[tail],
                coarseXA * scale,
                coarseXB * scale,
                coarseZ * scale,
                sampleA,
                sampleB);
        noise5A += sampleA * weight;
        noise5B += sampleB * weight;
        amplitude /= 2.0;
    }

'@
$text = $text.Remove($scoutEvalStart, $scoutEvalEnd - $scoutEvalStart).Insert($scoutEvalStart, $newScoutEval)

# -------------------------------------------------------------------------
# Exact density: pair y=7/y=8 evaluations for noise2/noise3/noise1.
# -------------------------------------------------------------------------
$exactKernelPos = $text.IndexOf('__global__ void exactSeaDensityKernel(')
if ($exactKernelPos -lt 0) {
    throw 'Could not locate exactSeaDensityKernel.'
}

$replacements = @(
    @(
@'
        n2y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n2y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
'@.TrimEnd(),
@'
        double pair7, pair8;
        p11Perlin3PairY(p, coarseX * sx, 7.0 * sy, 8.0 * sy, coarseZ * sz, pair7, pair8);
        n2y7 += pair7 * weight;
        n2y8 += pair8 * weight;
'@.TrimEnd()
    ),
    @(
@'
        n3y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n3y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
'@.TrimEnd(),
@'
        double pair7, pair8;
        p11Perlin3PairY(p, coarseX * sx, 7.0 * sy, 8.0 * sy, coarseZ * sz, pair7, pair8);
        n3y7 += pair7 * weight;
        n3y8 += pair8 * weight;
'@.TrimEnd()
    ),
    @(
@'
        n1y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n1y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
'@.TrimEnd(),
@'
        double pair7, pair8;
        p11Perlin3PairY(p, coarseX * sx, 7.0 * sy, 8.0 * sy, coarseZ * sz, pair7, pair8);
        n1y7 += pair7 * weight;
        n1y8 += pair8 * weight;
'@.TrimEnd()
    )
)

foreach ($pair in $replacements) {
    $old = $pair[0]
    $new = $pair[1]
    $count = ([regex]::Matches($text, [regex]::Escape($old))).Count
    if ($count -ne 1) {
        throw "Expected exactly one exact y-pair block, found $count."
    }
    $text = $text.Replace($old, $new)
}

# -------------------------------------------------------------------------
# Exact land count: one atomic per workgroup instead of one per coarse cell.
# -------------------------------------------------------------------------
$countKernelStart = $text.IndexOf('__global__ void countLandKernel(')
if ($countKernelStart -lt 0) {
    throw 'Could not locate countLandKernel.'
}
$countKernelEnd = $text.IndexOf('struct ExactWorkspace {', $countKernelStart)
if ($countKernelEnd -lt 0) {
    throw 'Could not locate end of countLandKernel.'
}

$newCountKernel = @'
__global__ void countLandKernel(
        const double* allSeaDensity,
        int* landCount,
        int candidateCount
) {
    const int candidate = static_cast<int>(blockIdx.x);
    const int tid = static_cast<int>(threadIdx.x);
    const int cell = static_cast<int>(blockIdx.y * blockDim.x + threadIdx.x);
    if (candidate >= candidateCount) return;

    const double* seaDensity = allSeaDensity
            + static_cast<std::size_t>(candidate) * COARSE_POINT_COUNT;
    int localLand = 0;

    if (cell < COARSE_CELL_COUNT) {
        const int cx = cell / COARSE_CELLS;
        const int cz = cell - cx * COARSE_CELLS;
        const int row0 = cx * COARSE_POINTS;
        const int row1 = (cx + 1) * COARSE_POINTS;

        const double q00 = seaDensity[row0 + cz];
        const double q01 = seaDensity[row0 + cz + 1];
        const double q10 = seaDensity[row1 + cz];
        const double q11 = seaDensity[row1 + cz + 1];

        double x0 = q00;
        double x1 = q01;
        const double dx0 = (q10 - q00) * 0.25;
        const double dx1 = (q11 - q01) * 0.25;

        for (int subX = 0; subX < 4; ++subX) {
            double v = x0;
            const double dz = (x1 - x0) * 0.25;
            for (int subZ = 0; subZ < 4; ++subZ) {
                if (v > 0.0) ++localLand;
                v += dz;
            }
            x0 += dx0;
            x1 += dx1;
        }
    }

    __shared__ int blockLand[EXACT_THREADS];
    blockLand[tid] = localLand;
    __syncthreads();

    for (int stride = EXACT_THREADS / 2; stride > 0; stride >>= 1) {
        if (tid < stride) blockLand[tid] += blockLand[tid + stride];
        __syncthreads();
    }

    if (tid == 0) atomicAdd(&landCount[candidate], blockLand[0]);
}

'@
$text = $text.Remove($countKernelStart, $countKernelEnd - $countKernelStart).Insert($countKernelStart, $newCountKernel)

# P8 flattened all candidate cells into one 1-D launch. P11 uses a 2-D grid so
# every workgroup belongs to exactly one candidate and can reduce locally.
$launchStart = $text.IndexOf('    const int totalCells = n * COARSE_CELL_COUNT;')
if ($launchStart -lt 0) {
    throw 'Could not locate P8 flattened land-kernel launch.'
}
$launchError = $text.IndexOf('    HIP_CHECK(hipGetLastError());', $launchStart)
if ($launchError -lt 0) {
    throw 'Could not locate P8 land-kernel launch error check.'
}
$launchEnd = $launchError + '    HIP_CHECK(hipGetLastError());'.Length

$newLandLaunch = @'
    const int cellBlocksPerCandidate =
            (COARSE_CELL_COUNT + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            countLandKernel,
            dim3(static_cast<unsigned int>(n), static_cast<unsigned int>(cellBlocksPerCandidate)),
            dim3(EXACT_THREADS), 0, 0,
            w.dDensity, w.dLand, n);
    HIP_CHECK(hipGetLastError());
'@.TrimEnd()
$text = $text.Remove($launchStart, $launchEnd - $launchStart).Insert($launchStart, $newLandLaunch)

$text = $text.Replace(
    'Scout P10: P8 batched exact + restored fast partial_sort pools; P6 portfolio and exact metric unchanged.',
    'Scout P11: paired Perlin scout/exact + reduced-atomic land count + P8 batching; exact metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P11 paired-noise + reduced-atomic optimization.' -ForegroundColor Green
Write-Host 'Scout: paired X samples share Perlin Z setup; same 64 samples and same score.'
Write-Host 'Exact density: y=7/y=8 Perlin evaluations share X/Z setup; equations unchanged.'
Write-Host 'Land count: ~40000 atomics/finalist -> ~157 atomics/finalist via workgroup reduction.'
Write-Host 'P6 16/4/4 portfolio, P7 800x800 metric, P8 exact batching, and TopExact=24 are unchanged.'
Write-Host 'VERIFY the 96617-land record before benchmarking. P10 baseline is ~2.69M seeds/s.'