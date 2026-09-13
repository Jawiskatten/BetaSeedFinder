param(
    [Int64[]]$Seeds = @(-3405360075020439777, 6430576860599818994)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $root 'scripts\cursed-spawn-origin-p1-common.ps1')

$sourceDir = Join-Path $root 'native\floating_island_spawn'
New-Item -ItemType Directory -Force -Path $sourceDir | Out-Null
$base = 'https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/20ff1a20993e3a9df3f2b950e9f9b1ba08514e35/native/floating_island_spawn'
$rawSource = Join-Path $sourceDir 'FloatingIslandSpawnP6SpawnDiagnostic.cpp'
$caveSource = Join-Path $sourceDir 'FloatingIslandSpawnP6CaveDiagnostic.cpp'
Write-Host 'Downloading P6 raw diagnostic dependency...'
Invoke-WebRequest -UseBasicParsing "$base/FloatingIslandSpawnP6SpawnDiagnostic.cpp" -OutFile $rawSource
Write-Host 'Downloading P6 cave-aware diagnostic source...'
Invoke-WebRequest -UseBasicParsing "$base/FloatingIslandSpawnP6CaveDiagnostic.cpp" -OutFile $caveSource

$nativeSourceDir = Get-BetaGpuNativeSourceDir $root
$api = Get-CoarseGpuApi $nativeSourceDir
if ($api -ne 'modern') { throw 'P6 cave diagnostic requires the current modern BetaSeedFinder GPU headers.' }
$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$generated = Prepare-SkyblockP14LatticeHeaders $root $nativeSourceDir 'full' 4
$build = Join-Path $root 'build\floating-island-spawn-p6-vanilla-spawn'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build 'FloatingIslandSpawnP6CaveDiagnostic_AMD.exe'

Write-Host "Compiling P6 cave-aware spawn diagnostic for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$generated" "-I$nativeSourceDir" $caveSource -o $exe
if ($LASTEXITCODE -ne 0) {
    Remove-Item $exe -Force -ErrorAction SilentlyContinue
    throw 'P6 cave-aware diagnostic compilation failed.'
}

Write-Host ''
Write-Host '=== P6 CAVE-AWARE SPAWN DIAGNOSTIC ==='
Write-Host 'Applies Beta 1.7.3 MapGenCaves RNG/path geometry to target chunk (0,0), then uses vanilla player AABB semantics.'
Write-Host ''
$seedArgs = @($Seeds | ForEach-Object { $_.ToString([Globalization.CultureInfo]::InvariantCulture) })
& $exe @seedArgs
if ($LASTEXITCODE -ne 0) { throw "P6 cave-aware diagnostic failed with exit code $LASTEXITCODE" }
