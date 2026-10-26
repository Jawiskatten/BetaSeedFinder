$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$javaHome = 'C:\Program Files\Microsoft\jdk-25.0.4.101-hotspot'
$serverJar = Join-Path $PSScriptRoot '.v10_runtime\server_26.3.jar'
$runtimeRoot = Join-Path $PSScriptRoot '.v14_runtime'
$outer = Join-Path $runtimeRoot 'outer'
$innerJar = Join-Path $outer 'META-INF\versions\26.3\server-26.3.jar'
if (!(Test-Path (Join-Path $javaHome 'bin\javac.exe'))) { throw "Java 25 JDK not found: $javaHome" }
if (!(Test-Path $serverJar)) { throw "Official 26.3 server bundle not found: $serverJar" }
if (!(Test-Path $innerJar)) {
    New-Item -ItemType Directory -Force -Path $outer | Out-Null
    Push-Location $outer
    try { & (Join-Path $javaHome 'bin\jar.exe') -xf $serverJar }
    finally { Pop-Location }
    if ($LASTEXITCODE -ne 0 -or !(Test-Path $innerJar)) { throw 'Failed to extract vanilla 26.3 runtime classes' }
}
$classes = Join-Path $runtimeRoot 'classes'
New-Item -ItemType Directory -Force -Path $classes | Out-Null
$deps = Get-ChildItem (Join-Path $outer 'META-INF\libraries') -Filter '*.jar' -Recurse | ForEach-Object FullName
$classpath = (@($innerJar) + $deps) -join ';'
& (Join-Path $javaHome 'bin\javac.exe') -encoding UTF-8 -cp $classpath -d $classes `
    (Join-Path $PSScriptRoot 'v14\VanillaTerrainEngine.java')
if ($LASTEXITCODE -ne 0) { throw 'Vanilla terrain engine compilation failed' }
[IO.File]::WriteAllText((Join-Path $runtimeRoot 'classpath.txt'), $classpath, [Text.UTF8Encoding]::new($false))
Write-Host 'Built persistent vanilla 26.3 terrain engine (noise base columns only; no server started).'

# Build the pinned Cubiomes static library first if it is not ready.
$lib = Join-Path $PSScriptRoot 'vendor\cubiomes\build-d15\libcubiomes_static.a'
if (!(Test-Path $lib)) {
    & (Join-Path $PSScriptRoot 'build_v7.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Pinned Cubiomes build failed' }
}
$gpp = 'C:\msys64\ucrt64\bin\g++.exe'
if (!(Test-Path $gpp)) { throw "MSYS2 UCRT64 g++ not found: $gpp" }
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
& $gpp -O3 -march=native -std=c++17 -fwrapv -static -static-libgcc -static-libstdc++ -I (Join-Path $PSScriptRoot 'vendor') `
    (Join-Path $PSScriptRoot 'deadbush15_v14.cpp') $lib -lm `
    -o (Join-Path $PSScriptRoot 'deadbush15_v14.exe')
if ($LASTEXITCODE -ne 0) { throw 'V14 compilation failed' }
Write-Host 'Built deadbush15_v14.exe'
