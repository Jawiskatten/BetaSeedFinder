param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path
$source = Join-Path $root 'native\src\single_biome_radius.cpp'
if (-not (Test-Path $source -PathType Leaf)) {
    throw "Single-biome source not found: $source"
}

$outputDir = Join-Path $root 'build\native\amd'
$output = Join-Path $outputDir 'SingleBiomeRadiusFinder.exe'
$log = Join-Path $outputDir 'single-biome-radius-build.log'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

function Import-VisualStudioEnvironment {
    $existingCompiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($existingCompiler) {
        Write-Host "MSVC:    $($existingCompiler.Source)"
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere -PathType Leaf)) {
        throw 'Visual Studio 2022 C++ Build Tools were not found.'
    }

    $installation = (& $vswhere `
        -latest `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if (-not $installation) {
        throw 'Visual Studio 2022 C++ Build Tools were not found.'
    }

    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $devCmd -PathType Leaf)) {
        throw "VsDevCmd.bat was not found: $devCmd"
    }

    $tempCommand = Join-Path ([IO.Path]::GetTempPath()) (
        'SingleBiomeRadius-vsenv-' + [Guid]::NewGuid().ToString('N') + '.cmd'
    )
    @(
        '@echo off'
        ('call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul' -f $devCmd)
        'if errorlevel 1 exit /b 1'
        'set'
    ) | Set-Content -LiteralPath $tempCommand -Encoding ASCII

    try {
        $environment = & $tempCommand
        $exitCode = $LASTEXITCODE
    }
    finally {
        Remove-Item -LiteralPath $tempCommand -Force -ErrorAction SilentlyContinue
    }
    if ($exitCode -ne 0) {
        throw "Visual Studio developer environment initialization failed with exit code $exitCode."
    }

    foreach ($line in $environment) {
        if ($line -isnot [string]) { continue }
        $index = $line.IndexOf('=')
        if ($index -gt 0) {
            [Environment]::SetEnvironmentVariable(
                $line.Substring(0, $index),
                $line.Substring($index + 1),
                'Process'
            )
        }
    }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'cl.exe is still unavailable after initializing Visual Studio.'
    }
}

Import-VisualStudioEnvironment

$hipcc = $null
$configuredHipRoots = @($env:HIP_PATH, $env:HIP_SDK_DIR) |
    Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
    Select-Object -Unique

foreach ($configuredRoot in $configuredHipRoots) {
    foreach ($name in @('hipcc.exe','hipcc.bat','hipcc.bin.exe','hipcc')) {
        $candidate = Join-Path $configuredRoot ("bin\\" + $name)
        if (Test-Path $candidate -PathType Leaf) { $hipcc = $candidate; break }
    }
    if ($hipcc) { break }
}

if (-not $hipcc) {
    $base = 'C:\Program Files\AMD\ROCm'
    if (Test-Path $base) {
        foreach ($versionDir in (Get-ChildItem $base -Directory | Sort-Object Name -Descending)) {
            foreach ($name in @('hipcc.exe','hipcc.bat','hipcc.bin.exe','hipcc')) {
                $candidate = Join-Path $versionDir.FullName ("bin\\" + $name)
                if (Test-Path $candidate -PathType Leaf) { $hipcc = $candidate; break }
            }
            if ($hipcc) { break }
        }
    }
}
if (-not $hipcc) {
    throw 'AMD HIP SDK hipcc was not found. Set HIP_SDK_DIR or install the AMD HIP SDK.'
}

Write-Host "HIP:     $hipcc"
Write-Host "Source:  $source"
Write-Host "Output:  $output"

$arguments = @(
    $source,
    '-O3','-std=c++17',
    '--offload-arch=gfx1030','--offload-arch=gfx1031','--offload-arch=gfx1032',
    '--offload-arch=gfx1100','--offload-arch=gfx1101','--offload-arch=gfx1102',
    '--offload-arch=gfx1200','--offload-arch=gfx1201',
    '-ffp-contract=off','-fno-fast-math','-fno-associative-math',
    '-Wno-unused-result',
    '-o',$output
)

Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
$previousPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    & $hipcc @arguments 2>&1 | Tee-Object -FilePath $log
    $exitCode = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $previousPreference
}

if ($exitCode -ne 0) {
    throw "SingleBiomeRadiusFinder build failed with exit code $exitCode. See $log"
}
if (-not (Test-Path $output -PathType Leaf)) {
    throw "Compiler exited successfully but did not create $output"
}

$hash = (Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host
Write-Host "Created: $output" -ForegroundColor Green
Write-Host "SHA-256: $hash"
return $output
