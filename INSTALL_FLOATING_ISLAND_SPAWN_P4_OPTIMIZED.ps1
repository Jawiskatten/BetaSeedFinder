$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Get-Location).Path
if (-not (Test-Path (Join-Path $root 'scripts\cursed-spawn-origin-p1-common.ps1') -PathType Leaf)) {
    throw 'Run this from the BetaSeedFinder project root (the folder containing scripts\cursed-spawn-origin-p1-common.ps1).'
}

$base = 'https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/floating-island-spawn-p4-optimized'
$stamp = 'p4opt-20260912-v1'
$files = @(
    @{ Remote='native/floating_island_spawn/FloatingIslandSpawnScoutP4.cpp'; Local='native\floating_island_spawn\FloatingIslandSpawnScoutP4.cpp' },
    @{ Remote='native/floating_island_spawn/FloatingIslandSpawnVerifyP4.cpp'; Local='native\floating_island_spawn\FloatingIslandSpawnVerifyP4.cpp' },
    @{ Remote='scripts/run-floating-island-spawn-p4-optimized-amd.ps1'; Local='scripts\run-floating-island-spawn-p4-optimized-amd.ps1' },
    @{ Remote='RUN_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED_AMD.bat'; Local='RUN_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED_AMD.bat' },
    @{ Remote='RUN_FLOATING_ISLAND_SPAWN_P4_MAX_SPEED_AMD.bat'; Local='RUN_FLOATING_ISLAND_SPAWN_P4_MAX_SPEED_AMD.bat' },
    @{ Remote='RESUME_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED_LAST_RUN.bat'; Local='RESUME_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED_LAST_RUN.bat' },
    @{ Remote='RESUME_FLOATING_ISLAND_SPAWN_P4_MAX_SPEED_LAST_RUN.bat'; Local='RESUME_FLOATING_ISLAND_SPAWN_P4_MAX_SPEED_LAST_RUN.bat' },
    @{ Remote='VERIFY_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED_INSTALL.bat'; Local='VERIFY_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED_INSTALL.bat' },
    @{ Remote='README_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED.md'; Local='README_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED.md' }
)

foreach ($f in $files) {
    $dest = Join-Path $root $f.Local
    $dir = Split-Path -Parent $dest
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $url = "$base/$($f.Remote)?v=$stamp"
    Write-Host "Downloading $($f.Remote)..."
    Invoke-WebRequest -UseBasicParsing $url -OutFile $dest
}

Write-Host ''
Write-Host 'P4 optimized files installed.'
Write-Host 'Running compile + correctness self-tests now...'
& (Join-Path $root 'VERIFY_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED_INSTALL.bat')
exit $LASTEXITCODE
