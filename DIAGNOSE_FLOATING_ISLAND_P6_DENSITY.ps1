param(
    [Int64[]]$Seeds = @(-3405360075020439777)
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $root 'scripts\cursed-spawn-origin-p1-common.ps1')

$sourceDir = Join-Path $root 'native\floating_island_spawn'
New-Item -ItemType Directory -Force -Path $sourceDir | Out-Null
$source = Join-Path $sourceDir 'FloatingIslandSpawnP6DensityDiagnostic.cpp'
$base = 'https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/cb91e0b0839e42f65736ca703bd813f43893fe1d'
Write-Host 'Downloading P6 density precision diagnostic...'
Invoke-WebRequest -UseBasicParsing "$base/native/floating_island_spawn/FloatingIslandSpawnP6DensityDiagnostic.cpp" -OutFile $source

$nativeSourceDir = Get-BetaGpuNativeSourceDir $root
$api = Get-CoarseGpuApi $nativeSourceDir
if ($api -ne 'modern') { throw 'P6 density diagnostic requires the current modern BetaSeedFinder GPU headers.' }
$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$generated = Prepare-SkyblockP14LatticeHeaders $root $nativeSourceDir 'full' 4
$build = Join-Path $root 'build\floating-island-spawn-p6-vanilla-spawn'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$exe = Join-Path $build 'FloatingIslandSpawnP6DensityDiagnostic_AMD.exe'

Write-Host "Compiling P6 density precision diagnostic for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$generated" "-I$nativeSourceDir" $source -o $exe
if ($LASTEXITCODE -ne 0) {
    Remove-Item $exe -Force -ErrorAction SilentlyContinue
    throw 'P6 density precision diagnostic compilation failed.'
}

Write-Host ''
Write-Host '=== P6 ORIGIN DENSITY PRECISION DIAGNOSTIC ==='
Write-Host 'Ground truth save has Y80-Y81 air. This prints the project GPU density sign around that exact boundary.'
Write-Host ''
$seedArgs = @($Seeds | ForEach-Object { $_.ToString([Globalization.CultureInfo]::InvariantCulture) })
& $exe @seedArgs
if ($LASTEXITCODE -ne 0) { throw "P6 density diagnostic failed with exit code $LASTEXITCODE" }
