param(
    [Int64[]]$Seeds = @(-3405360075020439777, 6430576860599818994)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $root 'scripts\cursed-spawn-origin-p1-common.ps1')

$sourceDir = Join-Path $root 'native\floating_island_spawn'
New-Item -ItemType Directory -Force -Path $sourceDir | Out-Null
$source = Join-Path $sourceDir 'FloatingIslandSpawnP6SpawnDiagnostic.cpp'
$sourceUrl = 'https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/floating-island-spawn-p6-vanilla-spawn/native/floating_island_spawn/FloatingIslandSpawnP6SpawnDiagnostic.cpp?v=p6diag1'
Write-Host 'Downloading P6 exact spawn diagnostic source...'
Invoke-WebRequest -UseBasicParsing $sourceUrl -OutFile $source

$nativeSourceDir = Get-BetaGpuNativeSourceDir $root
$api = Get-CoarseGpuApi $nativeSourceDir
if ($api -ne 'modern') { throw 'P6 diagnostic requires the current modern BetaSeedFinder GPU headers.' }
$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })

# Use the same exact r4 generated lattice as the production verifier. We only
# inspect the origin column, but this avoids introducing another terrain path.
$generated = Prepare-SkyblockP14LatticeHeaders $root $nativeSourceDir 'full' 4
$build = Join-Path $root 'build\floating-island-spawn-p6-vanilla-spawn'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build 'FloatingIslandSpawnP6SpawnDiagnostic_AMD.exe'

Write-Host "Compiling P6 spawn diagnostic for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$generated" "-I$nativeSourceDir" $source -o $exe
if ($LASTEXITCODE -ne 0) {
    Remove-Item $exe -Force -ErrorAction SilentlyContinue
    throw 'P6 spawn diagnostic compilation failed.'
}

Write-Host ''
Write-Host '=== P6 RAW-TERRAIN SPAWN DIAGNOSTIC ==='
Write-Host 'Compares the old project collision approximation with Beta 1.7.3 EntityPlayer/Entity AABB semantics.'
Write-Host 'Caves are deliberately NOT applied yet; a remaining game mismatch isolates the next missing stage.'
Write-Host ''

$seedArgs = @($Seeds | ForEach-Object { $_.ToString([Globalization.CultureInfo]::InvariantCulture) })
& $exe @seedArgs
if ($LASTEXITCODE -ne 0) { throw "P6 spawn diagnostic failed with exit code $LASTEXITCODE" }
