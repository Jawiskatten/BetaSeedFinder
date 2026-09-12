$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-CoarseGpuApi([string]$NativeSourceDir) {
    $text = [System.IO.File]::ReadAllText((Join-Path $NativeSourceDir "coarse_exact_gpu.hpp"))
    if ($text -match 'coarseOffsetX' -and $text -match 'terrainPermutationCache') { return "modern" }
    return "legacy"
}

function Get-BetaGpuNativeSourceDir([string]$ProjectRoot) {
    foreach ($candidate in @((Join-Path $ProjectRoot "native\src"), (Join-Path $ProjectRoot "gpu_p20_benchmark\native"))) {
        if ((Test-Path (Join-Path $candidate "coarse_exact_gpu.hpp") -PathType Leaf) -and
            (Test-Path (Join-Path $candidate "coarse_exact_core.hpp") -PathType Leaf) -and
            (Test-Path (Join-Path $candidate "p20_exact_math.hpp") -PathType Leaf) -and
            (Test-Path (Join-Path $candidate "gpu_runtime_compat.hpp") -PathType Leaf)) { return $candidate }
    }
    throw "Could not find BetaSeedFinder's native GPU terrain headers. Install this overlay into the full project."
}

function Replace-RequiredLiteral([string]$Text, [string]$Old, [string]$New, [string]$Label) {
    if (-not $Text.Contains($Old)) { throw "Could not patch $Label." }
    return $Text.Replace($Old, $New)
}

function Prepare-SkyblockP14LatticeHeaders(
        [string]$ProjectRoot,
        [string]$NativeSourceDir,
        [string]$Mode,
        [int]$Radius
) {
    if ($Mode -notin @("full","broad")) { throw "P14 vertical mode must be full or broad." }
    if ($Radius -notin @(4,6,8,10,12)) { throw "P14 radius must be 4, 6, 8, 10, or 12." }
    $storedYLevels = if ($Mode -eq "full") { 9 } else { 7 }
    $implicitTop = $Mode -eq "full"
    $rejectTop = $Mode -ne "full"
    $exactYMax = if ($Mode -eq "full") { 127 } else { 103 }
    $size = 8 * $Radius + 5
    $fromCoarse = -4 * $Radius
    $generated = Join-Path $ProjectRoot ("build\skyblock-v11-p14-{0}-r{1}\generated-include" -f $Mode,$Radius)
    New-Item -ItemType Directory -Force -Path $generated | Out-Null

    # Full mode stores original Beta nodes 7..15 and synthesizes exact node 16 as -10.
    # Broad mode stores nodes 7..13 and rejects components touching node 13.
    # Both modes retain the original 17-level world coordinates and corrected
    # legacy Y-gradient carry; only out-of-contract vertical work is removed.
    $core = [System.IO.File]::ReadAllText((Join-Path $NativeSourceDir "coarse_exact_core.hpp"))
    $core = [regex]::Replace($core, 'static constexpr int SIZE\s*=\s*\d+\s*;', ("static constexpr int SIZE = {0};" -f $size), 1)
    $core = [regex]::Replace($core, 'static constexpr int FROM_COARSE\s*=\s*-?\d+\s*;', ("static constexpr int FROM_COARSE = {0};" -f $fromCoarse), 1)
    $yPattern = 'static constexpr int Y_LEVELS\s*=\s*\d+\s*;'
    if (-not [regex]::IsMatch($core, $yPattern)) { throw "Could not find Y_LEVELS in coarse_exact_core.hpp." }
    $core = [regex]::Replace($core, $yPattern,
        "static constexpr int FULL_Y_LEVELS = 17;`r`nstatic constexpr int Y_BASE = 7;`r`nstatic constexpr int Y_LEVELS = $storedYLevels;", 1)
    $core = [regex]::Replace($core, 'static constexpr int MIN_INTERESTING_Y\s*=\s*\d+\s*;', 'static constexpr int MIN_INTERESTING_Y = 1;', 1)
    $core = Replace-RequiredLiteral $core `
        'double v = static_cast<double>(y) * yScale + p.b;' `
        'double v = static_cast<double>(y + Y_BASE) * yScale + p.b;' `
        'host cropped Perlin Y coordinates'
    $core = $core.Replace(
        'd6 = d6 * static_cast<double>(Y_LEVELS) / 16.0;',
        'd6 = d6 * static_cast<double>(FULL_Y_LEVELS) / 16.0;')
    $core = $core.Replace(
        'const double d7 = static_cast<double>(Y_LEVELS) / 2.0 + d6 * 4.0;',
        'const double d7 = static_cast<double>(FULL_Y_LEVELS) / 2.0 + d6 * 4.0;')
    $core = $core.Replace(
        'double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;',
        'double d9 = (static_cast<double>(y + Y_BASE) - d7) * 12.0 / d5;')
    $core = $core.Replace(
        'if (y > Y_LEVELS - 4) {',
        'if (y + Y_BASE > FULL_Y_LEVELS - 4) {')
    $core = $core.Replace(
        'static_cast<float>(y - (Y_LEVELS - 4))',
        'static_cast<float>((y + Y_BASE) - (FULL_Y_LEVELS - 4))')
    # Beta's original Perlin implementation has a subtle legacy carry:
    # when consecutive Y samples land in the same permutation cell, it reuses
    # gradient values computed at the first sample in that run. Starting the
    # cropped loop at world node 7 would reset that state and generate different
    # terrain. Warm the carry from the beginning of node 7's same-cell run.
    $coreCarryOld = @'
    int previousYi = -2147483647;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int y = 0; y < Y_LEVELS; ++y) {
'@
    $coreCarryNew = @'
    int previousYi = -2147483647;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    {
        int carryWorldY = Y_BASE;
        double targetV = static_cast<double>(Y_BASE) * yScale + p.b;
        const int targetFloor = p20::javaFloor(targetV);
        const int targetYi = targetFloor & 255;
        while (carryWorldY > 0) {
            const double previousV = static_cast<double>(carryWorldY - 1) * yScale + p.b;
            if ((p20::javaFloor(previousV) & 255) != targetYi) break;
            --carryWorldY;
        }
        double v = static_cast<double>(carryWorldY) * yScale + p.b;
        const int floor = p20::javaFloor(v);
        previousYi = floor & 255;
        v -= static_cast<double>(floor);
        const double yf = v;
        const double yfm1 = v - 1.0;
        const int p0 = xp0 + previousYi;
        const int p00 = p.perm[p0] + zi;
        const int p01 = p.perm[p0 + 1] + zi;
        const int p1 = xp1 + previousYi;
        const int p10 = p.perm[p1] + zi;
        const int p1Next = p.perm[p1 + 1] + zi;
        q00 = p20::lerp(xf, p20::grad3(p.perm[p00], x, yf, z), p20::grad3(p.perm[p10], x1, yf, z));
        q01 = p20::lerp(xf, p20::grad3(p.perm[p01], x, yfm1, z), p20::grad3(p.perm[p1Next], x1, yfm1, z));
        q10 = p20::lerp(xf, p20::grad3(p.perm[p00 + 1], x, yf, z1), p20::grad3(p.perm[p10 + 1], x1, yf, z1));
        q11 = p20::lerp(xf, p20::grad3(p.perm[p01 + 1], x, yfm1, z1), p20::grad3(p.perm[p1Next + 1], x1, yfm1, z1));
    }
    for (int y = 0; y < Y_LEVELS; ++y) {
'@
    $core = Replace-RequiredLiteral $core $coreCarryOld $coreCarryNew 'host legacy Y carry warm-up'
    $core = Replace-RequiredLiteral $core 'if (y == 0 || yi != previousYi) {' 'if (yi != previousYi) {' 'host legacy Y carry condition'

    if ($core -notmatch ("static constexpr int SIZE = {0};" -f $size) -or
        $core -notmatch ("static constexpr int FROM_COARSE = {0};" -f $fromCoarse) -or
        $core -notmatch 'static constexpr int Y_BASE = 7;' -or
        $core -notmatch ("static constexpr int Y_LEVELS = {0};" -f $storedYLevels) -or
        $core -notmatch 'static_cast<double>\(y \+ Y_BASE\) \* yScale' -or
        $core -notmatch 'const int p10 = p\.perm\[p1\] \+ zi;' -or
        $core -notmatch 'const int p1Next = p\.perm\[p1 \+ 1\] \+ zi;') {
        throw ("Could not specialize P14 terrain lattice: mode={0}, radius={1}, size={2}, storedY={3}." -f $Mode,$Radius,$size,$storedYLevels)
    }
    [System.IO.File]::WriteAllText((Join-Path $generated "coarse_exact_core.hpp"), $core, [System.Text.UTF8Encoding]::new($false))

    $gpuHeader = [System.IO.File]::ReadAllText((Join-Path $NativeSourceDir "coarse_exact_gpu.hpp"))
    # P14's 64-thread winner was safe only because SIZE=37. P14 wide
    # windows can exceed one wave, so cooperatively initialize every X/Z axis
    # cache entry instead of assuming blockDim.x >= SIZE.
    $axisPattern = 'if\s*\(lane\s*<\s*coarsecore::SIZE\)\s*\{[\s\S]*?cache\.zFade\[lane\]\s*=\s*p20::fade\(z\);\s*\}'
    $axisMatches = [regex]::Matches($gpuHeader, $axisPattern)
    if ($axisMatches.Count -ne 2) {
        throw "Could not patch both P14 cooperative X/Z axis-cache initializers."
    }
    $gpuHeader = [regex]::Replace($gpuHeader, $axisPattern, {
        param($m)
        $block = [regex]::Replace($m.Value, '\blane\b', 'axis')
        return [regex]::Replace($block,
            '^if\s*\(axis\s*<\s*coarsecore::SIZE\)\s*\{',
            'for (int axis = lane; axis < coarsecore::SIZE; axis += static_cast<int>(blockDim.x)) {')
    })
    $gpuHeader = Replace-RequiredLiteral $gpuHeader `
        'double y = static_cast<double>(lane) * scaleY + p.b;' `
        'double y = static_cast<double>(lane + coarsecore::Y_BASE) * scaleY + p.b;' `
        'GPU cropped Perlin Y coordinates'
    $gpuHeader = $gpuHeader.Replace(
        'd6 = d6 * static_cast<double>(coarsecore::Y_LEVELS) / 16.0;',
        'd6 = d6 * static_cast<double>(coarsecore::FULL_Y_LEVELS) / 16.0;')
    $gpuHeader = $gpuHeader.Replace(
        'const double d7 = static_cast<double>(coarsecore::Y_LEVELS) / 2.0 + d6 * 4.0;',
        'const double d7 = static_cast<double>(coarsecore::FULL_Y_LEVELS) / 2.0 + d6 * 4.0;')
    $gpuHeader = $gpuHeader.Replace(
        'double d9 = (static_cast<double>(y) - d7) * 12.0 / d5;',
        'double d9 = (static_cast<double>(y + coarsecore::Y_BASE) - d7) * 12.0 / d5;')
    $gpuHeader = $gpuHeader.Replace(
        'if (y > coarsecore::Y_LEVELS - 4) {',
        'if (y + coarsecore::Y_BASE > coarsecore::FULL_Y_LEVELS - 4) {')
    $gpuHeader = $gpuHeader.Replace(
        'static_cast<float>(y - (coarsecore::Y_LEVELS - 4))',
        'static_cast<float>((y + coarsecore::Y_BASE) - (coarsecore::FULL_Y_LEVELS - 4))')

    # The base terrain kernel normally skips the two four-octave surface-noise
    # generators. This finder needs their exact X=0,Z=0 values so its spawn-sand
    # gate matches Beta's unpopulated chunk rather than guessing from biome alone.
    $surfaceSignaturePattern = 'unsigned char\* signs,\s*\r?\n\s*int coarseOffsetX,'
    if ([regex]::Matches($gpuHeader, $surfaceSignaturePattern).Count -lt 1) {
        throw "Could not add exact origin surface-noise outputs to GPU signature."
    }
    $gpuHeader = [regex]::Replace($gpuHeader, $surfaceSignaturePattern,
        "unsigned char* signs,`r`n        double* originSandNoise,`r`n        double* originStoneNoise,`r`n        double* originTemperature,`r`n        double* originRainfall,`r`n        int coarseOffsetX,", 1)

    $gpuHeader = Replace-RequiredLiteral $gpuHeader @'
    double* climateOut[3] = {temp, rain, climateBlend};

    for (int kind = 0; kind < 3; ++kind) {
'@ @'
    double* climateOut[3] = {temp, rain, climateBlend};
    __shared__ double exactOriginClimateRaw[3];
    if (lane < 3) exactOriginClimateRaw[lane] = 0.0;
    __syncthreads();

    for (int kind = 0; kind < 3; ++kind) {
'@ 'exact origin climate scratch'
    $gpuHeader = Replace-RequiredLiteral $gpuHeader @'
            const double weight = 0.55 / d6;
            for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
'@ @'
            const double weight = 0.55 / d6;
            if (lane == 0) {
                exactOriginClimateRaw[kind] += p20::simplex2(perlin, 0.0, 0.0) * weight;
            }
            for (int column = lane; column < coarsecore::COLUMNS; column += blockDim.x) {
'@ 'exact origin climate octave accumulation'
    $gpuHeader = Replace-RequiredLiteral $gpuHeader @'
        temp[c] = d3;
        rain[c] = d4;
    }
    __syncthreads();

    if (terrainPermutationCache == nullptr && lane == 0) rng.setSeed(seed);
'@ @'
        temp[c] = d3;
        rain[c] = d4;
    }
    if (lane == 0) {
        const double originBlend = exactOriginClimateRaw[2] * 1.1 + 0.5;
        const double keepTemp = 0.99;
        double exactTemp = (exactOriginClimateRaw[0] * 0.15 + 0.7) * keepTemp + originBlend * 0.01;
        double exactRain = (exactOriginClimateRaw[1] * 0.15 + 0.5) * 0.998 + originBlend * 0.002;
        exactTemp = 1.0 - (1.0 - exactTemp) * (1.0 - exactTemp);
        if (exactTemp < 0.0) exactTemp = 0.0; else if (exactTemp > 1.0) exactTemp = 1.0;
        if (exactRain < 0.0) exactRain = 0.0; else if (exactRain > 1.0) exactRain = 1.0;
        originTemperature[seedIndex] = exactTemp;
        originRainfall[seedIndex] = exactRain;
    }
    __syncthreads();

    if (terrainPermutationCache == nullptr && lane == 0) rng.setSeed(seed);
'@ 'exact origin climate finalization'

    $skipSurfaceOld = @'
    if (terrainPermutationCache == nullptr && lane == 0) {
        for (int i = 0; i < 8; ++i) p20::consumePerlin(rng, perlin);
    }
    __syncthreads();
'@
    $skipSurfaceNew = @'
    if (terrainPermutationCache == nullptr) {
        double sandAtOrigin = 0.0;
        double stoneAtOrigin = 0.0;
        double surfaceAmplitude = 1.0;
        for (int i = 0; i < 8; ++i) {
            if (i == 4) surfaceAmplitude = 1.0;
            if (lane == 0) p20::initPerlin(rng, perlin);
            __syncthreads();
            if (lane == 0) {
                // Vanilla calls these as 16x16x1 arrays, so element zero takes
                // the general 3-D Perlin path at coordinate (0,0,0), not the
                // special yLen==1 helper used by gravel noise.
                const double contribution = p20::perlin3(perlin, 0.0, 0.0, 0.0) / surfaceAmplitude;
                if (i < 4) sandAtOrigin += contribution;
                else stoneAtOrigin += contribution;
            }
            __syncthreads();
            surfaceAmplitude *= 0.5;
        }
        if (lane == 0) {
            originSandNoise[seedIndex] = sandAtOrigin;
            originStoneNoise[seedIndex] = stoneAtOrigin;
        }
    }
    __syncthreads();
'@
    $gpuHeader = Replace-RequiredLiteral $gpuHeader $skipSurfaceOld $skipSurfaceNew 'exact origin surface-noise evaluation'

    # Preserve the same legacy Y-gradient carry on GPU. The first retained
    # node may reuse gradients originating at an earlier omitted node.
    $gpuHeader = Replace-RequiredLiteral $gpuHeader @'
    double yFade[coarsecore::Y_LEVELS];
};
'@ @'
    double yFade[coarsecore::Y_LEVELS];
    int carryYIndex;
    double carryYFrac;
    double carryYFracMinus1;
};
'@ 'GPU Y-carry cache fields'

    $gpuPrepareOld = @'
    if (lane < coarsecore::Y_LEVELS) {
        double y = static_cast<double>(lane + coarsecore::Y_BASE) * scaleY + p.b;
        const int fy = p20::javaFloor(y);
        y -= static_cast<double>(fy);
        cache.yIndex[lane] = fy & 255;
        cache.yFrac[lane] = y;
        cache.yFracMinus1[lane] = y - 1.0;
        cache.yFade[lane] = p20::fade(y);
    }

    __syncthreads();
'@
    $gpuPrepareNew = @'
    if (lane < coarsecore::Y_LEVELS) {
        double y = static_cast<double>(lane + coarsecore::Y_BASE) * scaleY + p.b;
        const int fy = p20::javaFloor(y);
        y -= static_cast<double>(fy);
        cache.yIndex[lane] = fy & 255;
        cache.yFrac[lane] = y;
        cache.yFracMinus1[lane] = y - 1.0;
        cache.yFade[lane] = p20::fade(y);
    }
    if (lane == 0) {
        int carryWorldY = coarsecore::Y_BASE;
        double targetY = static_cast<double>(coarsecore::Y_BASE) * scaleY + p.b;
        const int targetYi = p20::javaFloor(targetY) & 255;
        while (carryWorldY > 0) {
            const double previousY = static_cast<double>(carryWorldY - 1) * scaleY + p.b;
            if ((p20::javaFloor(previousY) & 255) != targetYi) break;
            --carryWorldY;
        }
        double y = static_cast<double>(carryWorldY) * scaleY + p.b;
        const int fy = p20::javaFloor(y);
        y -= static_cast<double>(fy);
        cache.carryYIndex = fy & 255;
        cache.carryYFrac = y;
        cache.carryYFracMinus1 = y - 1.0;
    }

    __syncthreads();
'@
    $gpuHeader = Replace-RequiredLiteral $gpuHeader $gpuPrepareOld $gpuPrepareNew 'GPU legacy Y carry preparation'

    $gpuCarryOld = @'
    int previousYi = -2147483647;
    double q00 = 0.0, q01 = 0.0, q10 = 0.0, q11 = 0.0;
    for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
'@
    $gpuCarryNew = @'
    int previousYi = cache.carryYIndex;
    const double carryYf = cache.carryYFrac;
    const double carryYfm1 = cache.carryYFracMinus1;
    const int carryP0 = xp0 + previousYi;
    const int carryP00 = p.perm[carryP0] + zi;
    const int carryP01 = p.perm[carryP0 + 1] + zi;
    const int carryP1 = xp1 + previousYi;
    const int carryP10 = p.perm[carryP1] + zi;
    const int carryP1Next = p.perm[carryP1 + 1] + zi;
    double q00 = p20::lerp(xf, grad3BranchlessExact(p.perm[carryP00], x, carryYf, z), grad3BranchlessExact(p.perm[carryP10], x1, carryYf, z));
    double q01 = p20::lerp(xf, grad3BranchlessExact(p.perm[carryP01], x, carryYfm1, z), grad3BranchlessExact(p.perm[carryP1Next], x1, carryYfm1, z));
    double q10 = p20::lerp(xf, grad3BranchlessExact(p.perm[carryP00 + 1], x, carryYf, z1), grad3BranchlessExact(p.perm[carryP10 + 1], x1, carryYf, z1));
    double q11 = p20::lerp(xf, grad3BranchlessExact(p.perm[carryP01 + 1], x, carryYfm1, z1), grad3BranchlessExact(p.perm[carryP1Next + 1], x1, carryYfm1, z1));
    for (int y = 0; y < coarsecore::Y_LEVELS; ++y) {
'@
    $gpuHeader = Replace-RequiredLiteral $gpuHeader $gpuCarryOld $gpuCarryNew 'GPU legacy Y carry warm-up'
    $gpuHeader = Replace-RequiredLiteral $gpuHeader 'if (y == 0 || yi != previousYi) {' 'if (yi != previousYi) {' 'GPU legacy Y carry condition'

    # Preserve fused final-density output for the generated stored Y slice.
    $needle = 'signs[base + y] = d8 > 0.0 ? 1 : 0;'
    $replacement = "noise1[base + y] = d8;`r`n            signs[base + y] = d8 > 0.0 ? 1 : 0;"
    if (-not $gpuHeader.Contains($needle)) { throw "Could not fuse final density storage into coarse_exact_gpu.hpp." }
    $gpuHeader = $gpuHeader.Replace($needle, $replacement)
    if ([regex]::Matches($gpuHeader, 'for \(int axis = lane; axis < coarsecore::SIZE; axis \+= static_cast<int>\(blockDim.x\)\)').Count -ne 2 -or
        $gpuHeader -notmatch 'lane \+ coarsecore::Y_BASE' -or
        $gpuHeader -notmatch 'coarsecore::FULL_Y_LEVELS' -or
        $gpuHeader -notmatch 'noise1\[base \+ y\] = d8;' -or
        $gpuHeader -notmatch 'originSandNoise\[seedIndex\]' -or
        $gpuHeader -notmatch 'originStoneNoise\[seedIndex\]' -or
        $gpuHeader -notmatch 'originTemperature\[seedIndex\]' -or
        $gpuHeader -notmatch 'originRainfall\[seedIndex\]' -or
        $gpuHeader -notmatch 'carryYIndex' -or
        $gpuHeader -notmatch 'const int carryP10 = p\.perm\[carryP1\] \+ zi;' -or
        $gpuHeader -notmatch 'const int carryP1Next = p\.perm\[carryP1 \+ 1\] \+ zi;') {
        throw "Generated P14 GPU header failed validation."
    }
    [System.IO.File]::WriteAllText((Join-Path $generated "coarse_exact_gpu.hpp"), $gpuHeader, [System.Text.UTF8Encoding]::new($false))
    $implicitLiteral = if ($implicitTop) { "true" } else { "false" }
    $rejectLiteral = if ($rejectTop) { "true" } else { "false" }
    $configHeader = @"
#pragma once
namespace p14config {
static constexpr const char* MODE_NAME = "$Mode";
static constexpr int CHUNK_RADIUS = $Radius;
static constexpr int Y_BASE = 7;
static constexpr int STORED_Y_LEVELS = $storedYLevels;
static constexpr bool IMPLICIT_TOP = $implicitLiteral;
static constexpr bool REJECT_TOP_BOUNDARY = $rejectLiteral;
static constexpr int EXACT_BLOCK_Y_MIN = 64;
static constexpr int EXACT_BLOCK_Y_MAX = $exactYMax;
}
"@
    [System.IO.File]::WriteAllText((Join-Path $generated "skyblock_p14_config.hpp"), $configHeader, [System.Text.UTF8Encoding]::new($false))
    return $generated
}

function Get-Hipcc {
    $command = Get-Command hipcc -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    if ($env:HIP_PATH) {
        $candidate = Join-Path $env:HIP_PATH "bin\hipcc.exe"
        if (Test-Path $candidate -PathType Leaf) { return $candidate }
    }
    foreach ($root in @("C:\Program Files\AMD\ROCm", "C:\Program Files\AMD\ROCm\7.1", "C:\Program Files\AMD\ROCm\7.0")) {
        if (Test-Path $root) {
            $candidate = Get-ChildItem $root -Filter hipcc.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($candidate) { return $candidate.FullName }
        }
    }
    throw "hipcc was not found."
}

function Get-HipGpuArchitectures([string]$Hipcc) {
    if ($env:SKYBLOCK_HIP_ARCH) { return @($env:SKYBLOCK_HIP_ARCH -split '[,;\s]+' | Where-Object { $_ } | Select-Object -Unique) }
    $hipBin = Split-Path -Parent $Hipcc
    foreach ($tool in @((Join-Path $hipBin "amdgpu-arch.exe"), (Join-Path $hipBin "rocminfo.exe"))) {
        if (Test-Path $tool -PathType Leaf) {
            $text = (& $tool 2>&1 | Out-String)
            $found = @([regex]::Matches($text, '\bgfx[0-9a-f]+\b', 'IgnoreCase') | ForEach-Object { $_.Value.ToLowerInvariant() } | Where-Object { $_ -ne 'gfx000' } | Select-Object -Unique)
            if ($found.Count -gt 0) { return $found }
        }
    }
    Write-Warning "Could not detect AMD architecture; defaulting to gfx1101."
    return @("gfx1101")
}

function Get-NativeSignature([string]$Source, [string]$NativeSourceDir, [string]$BackendKey, [string]$VariantKey) {
    $parts = New-Object System.Collections.Generic.List[string]
    $parts.Add("P14-wide-window-radii-v2")
    $parts.Add($VariantKey)
    $parts.Add($BackendKey)
    foreach ($file in @($Source, (Join-Path $ProjectRoot "scripts\cursed-spawn-origin-p1-common.ps1"), (Join-Path $NativeSourceDir "gpu_runtime_compat.hpp"), (Join-Path $NativeSourceDir "p20_exact_math.hpp"), (Join-Path $NativeSourceDir "coarse_exact_core.hpp"), (Join-Path $NativeSourceDir "coarse_exact_gpu.hpp"), (Join-Path $NativeSourceDir "terrain_perlin_cache.hpp"), (Join-Path $NativeSourceDir "climate_perlin_cache.hpp"))) {
        if (-not (Test-Path $file -PathType Leaf)) { throw "Required source missing: $file" }
        $item = Get-Item $file
        $parts.Add("$($item.FullName)|$($item.Length)|$($item.LastWriteTimeUtc.Ticks)")
    }
    return ($parts -join "`n")
}
