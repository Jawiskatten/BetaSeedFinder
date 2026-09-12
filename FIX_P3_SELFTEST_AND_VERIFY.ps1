$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root 'native\floating_island_spawn\FloatingIslandSpawnGpuFinderP3.cpp'
if (-not (Test-Path $source -PathType Leaf)) { throw "Missing P3 source: $source" }
$text = [System.IO.File]::ReadAllText($source)
$declOld = 'static int runSelfTest(Config c) {'
$declNew = 'static int runSelfTestP3(Config c) {'
$callOld = 'if (c.selfTest) return runSelfTest(c);'
$callNew = 'if (c.selfTest) return runSelfTestP3(c);'
if ($text.Contains($declOld)) { $text = $text.Replace($declOld, $declNew) }
if ($text.Contains($callOld)) { $text = $text.Replace($callOld, $callNew) }
if (-not $text.Contains($declNew)) { throw 'Could not verify P3 self-test declaration patch.' }
if (-not $text.Contains($callNew)) { throw 'Could not verify P3 self-test call patch.' }
[System.IO.File]::WriteAllText($source, $text, [System.Text.UTF8Encoding]::new($false))
Write-Host 'Patched P3 self-test naming locally.'
$buildDir = Join-Path $root 'build\floating-island-spawn-p3-full-r4\native'
if (Test-Path $buildDir) { Remove-Item $buildDir -Recurse -Force }
& (Join-Path $root 'VERIFY_FLOATING_ISLAND_SPAWN_P3_INSTALL.bat')
exit $LASTEXITCODE
