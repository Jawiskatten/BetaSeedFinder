$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$base = 'https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/floating-island-spawn-p5-wave'

$files = @(
    'native/floating_island_spawn/FloatingIslandSpawnScoutP5Wave.cpp',
    'native/floating_island_spawn/FloatingIslandSpawnVerifyP4.cpp',
    'scripts/cursed-spawn-origin-p1-common.ps1',
    'scripts/run-floating-island-spawn-p5-wave-amd.ps1',
    'RUN_FLOATING_ISLAND_SPAWN_P5_MAX_SPEED_AMD.bat',
    'RUN_FLOATING_ISLAND_SPAWN_P5_DESKTOP_AMD.bat',
    'RESUME_FLOATING_ISLAND_SPAWN_P5_LAST_RUN.bat',
    'VERIFY_FLOATING_ISLAND_SPAWN_P5_INSTALL.bat'
)

foreach ($rel in $files) {
    $dest = Join-Path $root ($rel -replace '/', '\')
    $dir = Split-Path -Parent $dest
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $url = "${base}/$($rel)?v=p5prod3"
    Write-Host "Downloading $rel"
    Invoke-WebRequest -UseBasicParsing $url -OutFile $dest
}

# Do not hard-code native\src here. Older/full BetaSeedFinder checkouts can keep
# the GPU terrain headers under gpu_p20_benchmark\native. The same helper used by
# the P4/P5 runners already knows both layouts, so resolve the actual local one.
$common = Join-Path $root 'scripts\cursed-spawn-origin-p1-common.ps1'
. $common
$nativeSourceDir = Get-BetaGpuNativeSourceDir $root
Write-Host "Detected native GPU headers: $nativeSourceDir"

$pillarSource = Join-Path $root 'native\highest_pillar_spawn\HighestPillarSpawnGpuFinder.cpp'
if (-not (Test-Path $pillarSource -PathType Leaf)) {
    throw "Required existing P1 source missing: $pillarSource"
}

foreach ($name in @('gpu_runtime_compat.hpp','p20_exact_math.hpp','coarse_exact_core.hpp','coarse_exact_gpu.hpp')) {
    $p = Join-Path $nativeSourceDir $name
    if (-not (Test-Path $p -PathType Leaf)) { throw "Resolved native GPU header missing: $p" }
}

Write-Host ''
Write-Host 'Compiling and self-testing P5 production pipeline...'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\run-floating-island-spawn-p5-wave-amd.ps1') -SelfTest -MaxSpeed
if ($LASTEXITCODE -ne 0) { throw "P5 production self-test failed with exit code $LASTEXITCODE" }

Write-Host ''
Write-Host 'P5 PRODUCTION INSTALL OK.'
Write-Host 'Max speed: .\RUN_FLOATING_ISLAND_SPAWN_P5_MAX_SPEED_AMD.bat'
Write-Host 'Desktop:   .\RUN_FLOATING_ISLAND_SPAWN_P5_DESKTOP_AMD.bat'
Write-Host 'Resume:    .\RESUME_FLOATING_ISLAND_SPAWN_P5_LAST_RUN.bat'
