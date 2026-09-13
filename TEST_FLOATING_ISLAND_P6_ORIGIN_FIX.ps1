$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
. (Join-Path $ProjectRoot 'scripts\cursed-spawn-origin-p1-common.ps1')

$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
$src = Join-Path $ProjectRoot 'native\floating_island_spawn\FloatingIslandSpawnScoutP5Wave.cpp'
if (-not (Test-Path $src -PathType Leaf)) { throw "Missing P5 source: $src" }

$build = Join-Path $ProjectRoot 'build\floating-island-spawn-p6-origin-fix'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$patched = Join-Path $build 'FloatingIslandSpawnScoutP6OriginFixed.cpp'
$wrapper = Join-Path $build 'FloatingIslandSpawnP6OriginRegression.cpp'
$exe = Join-Path $build 'FloatingIslandSpawnP6OriginRegression_AMD.exe'

$text = [System.IO.File]::ReadAllText($src)
$old = 'const double terrainValue = slimSimplex2(p, perm, lane, 2.0 * scale, 2.0 * scale) * weight;'
$new = @'
// Vanilla Beta 1.7.3 func_4061_a uses var16 = 16 / 5 = 3 and samples
// its 16x16 climate arrays at local index var17*3 + 1.  Therefore the
// origin density node uses climate sample (1,1), not the geometric (2,2).
const double terrainValue = slimSimplex2(p, perm, lane, 1.0 * scale, 1.0 * scale) * weight;
'@
$matches = ([regex]::Matches($text, [regex]::Escape($old))).Count
if ($matches -ne 1) { throw "Expected exactly one P5 (2,2) terrain-climate sample, found $matches." }
$text = $text.Replace($old, $new.Trim())
[System.IO.File]::WriteAllText($patched, $text, [System.Text.UTF8Encoding]::new($false))

$patchedName = [System.IO.Path]::GetFileName($patched)
$wrapperText = @"
#define main p5_origin_fixed_embedded_main
#include \"$patchedName\"
#undef main

#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    using namespace floating_island_spawn_p5_wave;
    Config c;
    c.batch = 1;
    unsigned int* dCount = nullptr;
    ScoutHit* dHits = nullptr;
    allocateArray(dCount, 1, \"allocate P6 regression counter\");
    allocateArray(dHits, 1, \"allocate P6 regression hit\");
    std::vector<ScoutHit> host;
    try {
        launchBatch(c, 0, 1, dCount, dHits, host, true, 6430576860599818994LL);
        if (host.size() != 1) throw std::runtime_error(\"known real pillar no longer passes corrected origin scout\");
        const auto good = host.front();
        std::cout << \"KNOWN_GOOD PASS seed=\" << good.seed
                  << \" spawnSurfaceY=\" << good.spawnSurfaceY
                  << \" firstUpperY=\" << good.firstUpperY
                  << \" airGap=\" << good.airGap
                  << \" playerFeetY=\" << good.playerFeetY
                  << \" supportY=\" << good.supportY << '\\n';

        host.clear();
        launchBatch(c, 0, 1, dCount, dHits, host, true, -3405360075020439777LL);
        if (!host.empty()) {
            const auto bad = host.front();
            std::cerr << \"FALSE_POSITIVE_STILL_PASSES seed=\" << bad.seed
                      << \" spawnSurfaceY=\" << bad.spawnSurfaceY
                      << \" firstUpperY=\" << bad.firstUpperY
                      << \" airGap=\" << bad.airGap
                      << \" playerFeetY=\" << bad.playerFeetY
                      << \" supportY=\" << bad.supportY << '\\n';
            throw std::runtime_error(\"known two-block-gap false positive still passes\");
        }
        std::cout << \"KNOWN_FALSE_POSITIVE REJECT seed=-3405360075020439777\\n\";
        std::cout << \"P6 ORIGIN CLIMATE FIX REGRESSION OK\\n\";
    } catch (...) {
        if (dHits) (void)hipFree(dHits);
        if (dCount) (void)hipFree(dCount);
        throw;
    }
    checkHip(hipFree(dHits), \"free P6 regression hits\");
    checkHip(hipFree(dCount), \"free P6 regression counter\");
    return 0;
}
"@
[System.IO.File]::WriteAllText($wrapper, $wrapperText, [System.Text.UTF8Encoding]::new($false))

Write-Host "Compiling corrected P6 origin scout regression for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DP5_LANES=32' '-DP5_PERM_U8=1' "-I$nativeSourceDir" "-I$build" $wrapper -o $exe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'P6 origin regression compilation failed.' }

Write-Host ''
Write-Host '=== P6 VANILLA ORIGIN CLIMATE REGRESSION ==='
& $exe
if ($LASTEXITCODE -ne 0) { throw 'P6 origin regression failed.' }
