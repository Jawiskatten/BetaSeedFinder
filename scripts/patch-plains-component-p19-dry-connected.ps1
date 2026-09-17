param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\plains_component_finder.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "Plains component source not found: $sourcePath"
}

$text = [System.IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")

if ($text.Contains('P19_DRY_CONNECTED_PLAINS')) {
    Write-Host 'P19 dry-connected Plains objective is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('P18_LARGEST_PLAINS_400_SQUARE')) {
    throw 'P19 expects the generated P18 Plains source first.'
}
if (-not $text.Contains('__global__ void p18PlainsMapKernel(')) {
    throw 'P19 could not find the P18 Plains biome-map kernel.'
}
if (-not $text.Contains('ExactResult runExact(')) {
    throw 'P19 could not find runExact.'
}

$backupPath = $sourcePath + '.p18-before-p19-dry-connected.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# ---------------------------------------------------------------------------
# Add the validated Beta 1.7.3 sea-surface terrain evaluator used by the TU4
# water finder.  A Plains column counts only when:
#   1) the exact Beta climate biome is PLAINS, and
#   2) base terrain density at y=63 is > 0 (solid/dry at sea surface).
# This stops oceans/sea channels from connecting one Plains climate region into
# a visually disconnected "mega Plains".
# ---------------------------------------------------------------------------
$insertMarker = '__global__ void p18PlainsMapKernel('
$insertPos = $text.IndexOf($insertMarker)
if ($insertPos -lt 0) {
    throw 'Could not locate P18 map-kernel insertion point.'
}

$terrainCode = @'
// P19_DRY_CONNECTED_PLAINS
// Exact connected DRY Plains objective.  Terrain math is the validated
// Beta 1.7.3 y=63 density path from TU4WaterFinder, adapted to the 800x800
// square.  density > 0 at y=63 means the base generator places solid terrain;
// density <= 0 means the column is sea/water at that level and cannot connect
// two Plains land masses.
static constexpr int P19_TERRAIN_STATE_COUNT = 66;

__device__ __forceinline__ double p19ClimateNoise4(
        const p20::PerlinState* states,
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
        total += p20::simplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= octaveScale;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double p19ClimateNoise2(
        const p20::PerlinState* states,
        double x,
        double z
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 2; ++octave) {
        const double scale = (0.25 / 1.5) * d7;
        total += p20::simplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= 0.5882352941176471;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double p19Clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

__device__ __forceinline__ double p19DensityAtY(
        double noise1,
        double noise2,
        double noise3,
        double d5,
        double d7,
        int y
) {
    double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;
    if (d9 < 0.0) d9 *= 4.0;

    const double blend = (noise1 / 10.0 + 1.0) / 2.0;
    double d8;
    if (blend < 0.0) {
        d8 = noise2 / 512.0;
    } else if (blend > 1.0) {
        d8 = noise3 / 512.0;
    } else {
        const double d10 = noise2 / 512.0;
        const double d11 = noise3 / 512.0;
        d8 = d10 + (d11 - d10) * blend;
    }
    d8 -= d9;
    return d8;
}

__global__ void p19SeaDensityKernel(
        const p20::PerlinState* terrain,
        const p20::PerlinState* tempStates,
        const p20::PerlinState* rainStates,
        const p20::PerlinState* blendStates,
        int coarseMinX,
        int coarseMinZ,
        int coarsePoints,
        double* seaDensity
) {
    const int idx = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int pointCount = coarsePoints * coarsePoints;
    if (idx >= pointCount) return;

    const int ix = idx / coarsePoints;
    const int iz = idx - ix * coarsePoints;
    const double coarseX = static_cast<double>(coarseMinX + ix);
    const double coarseZ = static_cast<double>(coarseMinZ + iz);
    const double climateX = coarseX * 4.0 + 2.0;
    const double climateZ = coarseZ * 4.0 + 2.0;

    const double tempRaw = p19ClimateNoise4(
            tempStates, climateX, climateZ,
            0.02500000037252903, 0.25);
    const double rainRaw = p19ClimateNoise4(
            rainStates, climateX, climateZ,
            0.05000000074505806, 0.3333333333333333);
    const double blendRaw = p19ClimateNoise2(blendStates, climateX, climateZ);

    const double climateBlend = blendRaw * 1.1 + 0.5;
    double temperature = (tempRaw * 0.15 + 0.7) * 0.99 + climateBlend * 0.01;
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + climateBlend * 0.002;
    temperature = 1.0 - (1.0 - temperature) * (1.0 - temperature);
    temperature = p19Clamp01(temperature);
    rain = p19Clamp01(rain);

    double n2y7 = 0.0, n2y8 = 0.0;
    double n3y7 = 0.0, n3y8 = 0.0;
    double n1y7 = 0.0, n1y8 = 0.0;
    double n4 = 0.0;
    double n5 = 0.0;

    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[octave];
        n2y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n2y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double sx = 684.412 * amplitude;
        const double sy = 684.412 * amplitude;
        const double sz = 684.412 * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[16 + octave];
        n3y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n3y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 8; ++octave) {
        const double sx = (684.412 / 80.0) * amplitude;
        const double sy = (684.412 / 160.0) * amplitude;
        const double sz = (684.412 / 80.0) * amplitude;
        const double weight = 1.0 / amplitude;
        const p20::PerlinState& p = terrain[32 + octave];
        n1y7 += p20::perlin3(p, coarseX * sx, 7.0 * sy, coarseZ * sz) * weight;
        n1y8 += p20::perlin3(p, coarseX * sx, 8.0 * sy, coarseZ * sz) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 10; ++octave) {
        const double scale = 1.121 * amplitude;
        const double weight = 1.0 / amplitude;
        n4 += p20::perlin2(
                terrain[40 + octave], coarseX * scale, coarseZ * scale) * weight;
        amplitude /= 2.0;
    }

    amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
        const double scale = 200.0 * amplitude;
        const double weight = 1.0 / amplitude;
        n5 += p20::perlin2(
                terrain[50 + octave], coarseX * scale, coarseZ * scale) * weight;
        amplitude /= 2.0;
    }

    const double d2 = temperature;
    const double d3 = rain * d2;
    double d4 = 1.0 - d3;
    d4 *= d4;
    d4 *= d4;
    d4 = 1.0 - d4;

    double d5 = (n4 + 256.0) / 512.0;
    d5 *= d4;
    if (d5 > 1.0) d5 = 1.0;

    double d6 = n5 / 8000.0;
    if (d6 < 0.0) d6 = -d6 * 0.3;
    d6 = d6 * 3.0 - 2.0;
    if (d6 < 0.0) {
        d6 /= 2.0;
        if (d6 < -1.0) d6 = -1.0;
        d6 /= 1.4;
        d6 /= 2.0;
        d5 = 0.0;
    } else {
        if (d6 > 1.0) d6 = 1.0;
        d6 /= 8.0;
    }
    if (d5 < 0.0) d5 = 0.0;
    d5 += 0.5;
    d6 *= 17.0 / 16.0;
    const double d7 = 17.0 / 2.0 + d6 * 4.0;

    const double density7 = p19DensityAtY(n1y7, n2y7, n3y7, d5, d7, 7);
    const double density8 = p19DensityAtY(n1y8, n2y8, n3y8, d5, d7, 8);

    const double step = (density8 - density7) * 0.125;
    double v = density7;
    for (int subY = 0; subY < 7; ++subY) v += step;
    seaDensity[idx] = v;
}

__global__ void p19ApplyDryLandMaskKernel(
        const double* seaDensity,
        int coarseCells,
        int coarsePoints,
        int side,
        unsigned char* plainsMap
) {
    const int cell = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (cell >= coarseCells * coarseCells) return;

    const int cx = cell / coarseCells;
    const int cz = cell - cx * coarseCells;
    const int row0 = cx * coarsePoints;
    const int row1 = (cx + 1) * coarsePoints;

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
            if (v <= 0.0) {
                const int localX = cx * 4 + subX;
                const int localZ = cz * 4 + subZ;
                plainsMap[localZ * side + localX] = 0;
            }
            v += dz;
        }
        x0 += dx0;
        x1 += dx1;
    }
}

void p19BuildTerrainStates(
        std::int64_t seed,
        std::vector<p20::PerlinState>& terrain,
        std::vector<p20::PerlinState>& temp,
        std::vector<p20::PerlinState>& rain,
        std::vector<p20::PerlinState>& blend
) {
    terrain.resize(P19_TERRAIN_STATE_COUNT);
    temp.resize(4);
    rain.resize(4);
    blend.resize(2);

    p20::JavaRandom rng;
    p20::PerlinState scratch;

    rng.setSeed(multipliedSeed(seed, 9871ULL));
    for (int i = 0; i < 4; ++i) p20::initPerlin(rng, temp[i]);

    rng.setSeed(multipliedSeed(seed, 39811ULL));
    for (int i = 0; i < 4; ++i) p20::initPerlin(rng, rain[i]);

    rng.setSeed(multipliedSeed(seed, 543321ULL));
    for (int i = 0; i < 2; ++i) p20::initPerlin(rng, blend[i]);

    rng.setSeed(seed);
    for (int i = 0; i < 40; ++i) p20::initPerlin(rng, terrain[i]);
    for (int i = 0; i < 8; ++i) p20::initPerlin(rng, scratch);
    for (int i = 40; i < P19_TERRAIN_STATE_COUNT; ++i) p20::initPerlin(rng, terrain[i]);
}

'@
$text = $text.Insert($insertPos, $terrainCode)

# ---------------------------------------------------------------------------
# Replace the P18 biome-only exact flood fill.  We first build the exact Plains
# climate bitmap, then zero every sea/water column using exact y=63 terrain
# density, and finally flood-fill that DRY Plains bitmap on CPU.
# ---------------------------------------------------------------------------
$runPattern = '(?s)ExactResult runExact\(.*?return result;\n\}'
$runMatches = [regex]::Matches($text, $runPattern)
if ($runMatches.Count -ne 1) {
    throw "Expected exactly one runExact function, found $($runMatches.Count)."
}

$newRun = @'
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
    if ((side & 3) != 0 || ((centerX - target) & 3) != 0 || ((centerZ - target) & 3) != 0) {
        throw std::runtime_error("P19 dry Plains terrain mask requires square origin aligned to 4 blocks");
    }

    unsigned char* dMap = nullptr;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dMap),
                        static_cast<std::size_t>(total) * sizeof(unsigned char)));

    hipLaunchKernelGGL(
            p18PlainsMapKernel,
            dim3(1), dim3(EXACT_THREADS), 0, 0,
            seed, centerX, centerZ, target, dMap);
    HIP_CHECK(hipGetLastError());

    const int coarseCells = side / 4;
    const int coarsePoints = coarseCells + 1;
    const int coarsePointCount = coarsePoints * coarsePoints;
    const int coarseMinX = (centerX - target) / 4;
    const int coarseMinZ = (centerZ - target) / 4;

    std::vector<p20::PerlinState> terrain;
    std::vector<p20::PerlinState> temp;
    std::vector<p20::PerlinState> rain;
    std::vector<p20::PerlinState> blend;
    p19BuildTerrainStates(seed, terrain, temp, rain, blend);

    p20::PerlinState* dTerrain = nullptr;
    p20::PerlinState* dTemp = nullptr;
    p20::PerlinState* dRain = nullptr;
    p20::PerlinState* dBlend = nullptr;
    double* dDensity = nullptr;

    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dTerrain),
                        terrain.size() * sizeof(p20::PerlinState)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dTemp),
                        temp.size() * sizeof(p20::PerlinState)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dRain),
                        rain.size() * sizeof(p20::PerlinState)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dBlend),
                        blend.size() * sizeof(p20::PerlinState)));
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&dDensity),
                        static_cast<std::size_t>(coarsePointCount) * sizeof(double)));

    HIP_CHECK(hipMemcpy(dTerrain, terrain.data(),
                        terrain.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(dTemp, temp.data(),
                        temp.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(dRain, rain.data(),
                        rain.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(dBlend, blend.data(),
                        blend.size() * sizeof(p20::PerlinState), hipMemcpyHostToDevice));

    const int pointBlocks = (coarsePointCount + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            p19SeaDensityKernel,
            dim3(pointBlocks), dim3(EXACT_THREADS), 0, 0,
            dTerrain, dTemp, dRain, dBlend,
            coarseMinX, coarseMinZ, coarsePoints, dDensity);
    HIP_CHECK(hipGetLastError());

    const int cellCount = coarseCells * coarseCells;
    const int cellBlocks = (cellCount + EXACT_THREADS - 1) / EXACT_THREADS;
    hipLaunchKernelGGL(
            p19ApplyDryLandMaskKernel,
            dim3(cellBlocks), dim3(EXACT_THREADS), 0, 0,
            dDensity, coarseCells, coarsePoints, side, dMap);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    std::vector<unsigned char> map(static_cast<std::size_t>(total));
    HIP_CHECK(hipMemcpy(
            map.data(), dMap,
            static_cast<std::size_t>(total) * sizeof(unsigned char),
            hipMemcpyDeviceToHost));

    HIP_CHECK(hipFree(dDensity));
    HIP_CHECK(hipFree(dBlend));
    HIP_CHECK(hipFree(dRain));
    HIP_CHECK(hipFree(dTemp));
    HIP_CHECK(hipFree(dTerrain));
    HIP_CHECK(hipFree(dMap));

    std::vector<int> queue(static_cast<std::size_t>(total));
    ExactResult result{};
    result.safeRadius = 0; // P19 semantic: connected DRY PLAINS area
    result.firstMismatchD2 = -1;
    result.baseBiome = static_cast<int>(PLAINS);
    result.componentWidthX = 0;
    result.componentHeightZ = 0;
    result.minX = result.maxX = centerX;
    result.minZ = result.maxZ = centerZ;
    result.touchesBoundary = 0;
    result.measurementHalfSize = target;
    result.expansionCount = 0;

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
$text = [regex]::Replace($text, $runPattern, $newRun.TrimEnd(), 1)

# Output semantics.  Keep P18's biome-only 8x8 scout for now, but make exact
# records unmistakably mean connected DRY Plains land.
$text = $text.Replace(' plainsArea=', ' dryPlainsArea=')
$text = $text.Replace(' bestPlainsArea=', ' bestDryPlainsArea=')
$text = $text.Replace(
    'P18_LARGEST_PLAINS_400_SQUARE | 8x8 Plains connected scout | exact 4-neighbour 800x800 area | tuned 4x16',
    'P19_DRY_CONNECTED_PLAINS | 8x8 biome scout | exact PLAINS + dry y=63 terrain | 4-neighbour 800x800 area'
)

# P17 display leftovers can still say trueSize/measuredWindow even though P18/P19
# are clipped finite-square objectives.  Normalize that diagnostic if present.
$touchPattern = '(?s)\s*<< " trueSize=" << \(result\.touchesBoundary \? "NO" : "YES"\)\s*<< " measuredWindow=" << \(2 \* result\.measurementHalfSize\)\s*<< ''x'' << \(2 \* result\.measurementHalfSize\)\s*<< " expansions=" << result\.expansionCount;'
if ([regex]::IsMatch($text, $touchPattern)) {
    $text = [regex]::Replace(
        $text,
        $touchPattern,
        "`n              << \" touchesSquareBoundary=\" << (result.touchesBoundary ? \"YES\" : \"NO\");",
        1
    )
}

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$verify = [System.IO.File]::ReadAllText($sourcePath)
if (-not $verify.Contains('P19_DRY_CONNECTED_PLAINS')) {
    throw 'P19 marker missing after write.'
}
if (-not $verify.Contains('p19ApplyDryLandMaskKernel')) {
    throw 'P19 dry-land mask kernel missing after write.'
}

Write-Host 'Applied P19 DRY connected Plains objective.' -ForegroundColor Green
Write-Host 'Exact record metric: largest 4-neighbour-connected PLAINS land component inside the 800x800 square.'
Write-Host 'Ocean/sea columns are excluded with exact Beta 1.7.3 terrain density at y=63.'
Write-Host 'The 8x8 scout is still biome-only for speed; only exact records use the dry-land metric.'
