param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$Arch = 'gfx1101'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path
$source = Join-Path $root 'native\src\dungeon_cluster_gpu_scout.cpp'
if (-not (Test-Path $source -PathType Leaf)) { throw "Dungeon GPU scout source not found: $source" }

function Import-VisualStudioEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere -PathType Leaf)) { throw 'Visual Studio 2022 C++ Build Tools were not found.' }
    $installation = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $installation) { throw 'Visual Studio 2022 C++ Build Tools were not found.' }
    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    $tempCommand = Join-Path ([IO.Path]::GetTempPath()) ('DungeonClusterGpu-vsenv-' + [Guid]::NewGuid().ToString('N') + '.cmd')
    @('@echo off', ('call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul' -f $devCmd), 'if errorlevel 1 exit /b 1', 'set') | Set-Content -LiteralPath $tempCommand -Encoding ASCII
    try { $environment = & $tempCommand; $exitCode = $LASTEXITCODE } finally { Remove-Item -LiteralPath $tempCommand -Force -ErrorAction SilentlyContinue }
    if ($exitCode -ne 0) { throw "Visual Studio developer environment initialization failed with exit code $exitCode." }
    foreach ($line in $environment) {
        if ($line -isnot [string]) { continue }
        $index = $line.IndexOf('=')
        if ($index -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $index), $line.Substring($index + 1), 'Process') }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe is still unavailable after initializing Visual Studio.' }
}

function Find-Hipcc {
    foreach ($configuredRoot in @($env:HIP_PATH, $env:HIP_SDK_DIR) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique) {
        foreach ($name in @('hipcc.exe','hipcc.bat','hipcc.bin.exe','hipcc')) {
            $candidate = Join-Path $configuredRoot ("bin\" + $name)
            if (Test-Path $candidate -PathType Leaf) { return $candidate }
        }
    }
    $base = 'C:\Program Files\AMD\ROCm'
    if (Test-Path $base) {
        foreach ($versionDir in (Get-ChildItem $base -Directory | Sort-Object Name -Descending)) {
            foreach ($name in @('hipcc.exe','hipcc.bat','hipcc.bin.exe','hipcc')) {
                $candidate = Join-Path $versionDir.FullName ("bin\" + $name)
                if (Test-Path $candidate -PathType Leaf) { return $candidate }
            }
        }
    }
    throw 'AMD HIP SDK hipcc was not found. Set HIP_SDK_DIR or install the AMD HIP SDK.'
}

Import-VisualStudioEnvironment
$hipcc = Find-Hipcc
$outputDir = Join-Path $root 'build\native\amd'
$output = Join-Path $outputDir 'DungeonClusterGpuScout.exe'
$log = Join-Path $outputDir 'dungeon-cluster-gpu-build.log'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

Write-Host "HIP:    $hipcc"
Write-Host "Arch:   $Arch"
Write-Host "Source: $source"
Write-Host "Output: $output"

$argsList = @($source, '-O3','-std=c++17', ("--offload-arch={0}" -f $Arch), ("-I{0}" -f (Join-Path $root 'native\src')), '-ffp-contract=off','-fno-fast-math','-fno-associative-math','-Wno-unused-result','-o',$output)
Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
$oldPreference = $ErrorActionPreference
try { $ErrorActionPreference = 'Continue'; & $hipcc @argsList 2>&1 | Tee-Object -FilePath $log; $exitCode = $LASTEXITCODE } finally { $ErrorActionPreference = $oldPreference }

if ($exitCode -ne 0) { throw "DungeonClusterGpuScout build failed with exit code $exitCode. See $log" }
if (-not (Test-Path $output -PathType Leaf)) { throw "Compiler exited successfully but did not create $output" }

Write-Host 'Running GPU scout host/RNG self-test...'
& $output --self-test
if ($LASTEXITCODE -ne 0) { throw 'DungeonClusterGpuScout self-test failed.' }

$hash = (Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host
Write-Host "Created: $output" -ForegroundColor Green
Write-Host "SHA-256: $hash"
return $output
