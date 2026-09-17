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

if ($text.Contains('// P20_DRY_PLAINS_TERRAIN_MASK')) {
    Write-Host 'P20 dry Plains terrain mask is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('P18_LARGEST_PLAINS_400_SQUARE')) {
    throw 'P20 expects the generated P18 Plains source first.'
}
if (-not $text.Contains('__global__ void p18PlainsMapKernel(')) {
    throw 'P20 could not find the P18 exact Plains biome map kernel.'
}

$headerPath = Join-Path $ProjectRoot 'native\src\plains_dry_terrain.hpp'
if (-not (Test-Path $headerPath -PathType Leaf)) {
    throw "Missing exact dry-terrain helper: $headerPath"
}

$backupPath = $sourcePath + '.p18-before-p20-dry-mask.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# Include the validated Beta 1.7.3 y=63 terrain-density helper.
$includeNeedle = '#include "p20_exact_math.hpp"'
if (-not $text.Contains($includeNeedle)) {
    throw 'Could not find p20_exact_math.hpp include.'
}
if (-not $text.Contains('#include "plains_dry_terrain.hpp"')) {
    $text = $text.Replace(
        $includeNeedle,
        $includeNeedle + "`n" + '#include "plains_dry_terrain.hpp"'
    )
}

# The P18 exact path first generates an exact 800x800 PLAINS climate bitmap.
# Immediately after that kernel, zero every ocean/sea column before the existing
# CPU 4-neighbour flood fill sees the map.
$launchPattern = '(?s)(hipLaunchKernelGGL\(\s*p18PlainsMapKernel,.*?HIP_CHECK\(hipGetLastError\(\)\);)'
$launchMatches = [regex]::Matches($text, $launchPattern)
if ($launchMatches.Count -ne 1) {
    throw "Expected exactly one P18 exact map launch, found $($launchMatches.Count)."
}

$insertion = @'

    // P20_DRY_PLAINS_TERRAIN_MASK
    // Climate PLAINS alone is not enough: oceans can run through the same biome.
    // Remove all base-generator sea/water columns (density <= 0 at y=63) before
    // connected-component measurement so records are one continuous dry landmass.
    dryplains::applyExactDryMask(seed, centerX, centerZ, target, dMap);
'@
$text = [regex]::Replace(
    $text,
    $launchPattern,
    '${1}' + $insertion.TrimEnd(),
    1
)

# Make record/status semantics explicit.
$text = $text.Replace(' plainsArea=', ' dryPlainsArea=')
$text = $text.Replace(' bestPlainsArea=', ' bestDryPlainsArea=')
$text = $text.Replace(
    'P18_LARGEST_PLAINS_400_SQUARE | 8x8 Plains connected scout | exact 4-neighbour 800x800 area | tuned 4x16',
    'P20_DRY_PLAINS_TERRAIN_MASK | 8x8 Plains biome scout | exact dry PLAINS land | 4-neighbour 800x800 area'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$verify = [System.IO.File]::ReadAllText($sourcePath)
if (-not $verify.Contains('// P20_DRY_PLAINS_TERRAIN_MASK')) {
    throw 'P20 dry-mask marker missing after write.'
}
if (-not $verify.Contains('dryplains::applyExactDryMask')) {
    throw 'P20 exact dry-mask call missing after write.'
}

Write-Host 'Applied P20 connected DRY Plains metric.' -ForegroundColor Green
Write-Host 'Exact records now require PLAINS biome + solid base terrain at y=63.'
Write-Host 'Ocean/sea channels can no longer connect separate Plains land masses.'
Write-Host 'Scout remains biome-only for speed; the exact record metric is dry-land connected area.'
