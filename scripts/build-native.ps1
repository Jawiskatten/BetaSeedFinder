param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [Parameter(Mandatory = $true)]
    [ValidateSet('AMD','NVIDIA')]
    [string]$Backend
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path
$nativeSource = if (Test-Path (Join-Path $root 'native\src\gpu_p20_benchmark.cpp')) {
    Join-Path $root 'native\src\gpu_p20_benchmark.cpp'
} else {
    Join-Path $root 'gpu_p20_benchmark\native\gpu_p20_benchmark.cpp'
}
if (-not (Test-Path $nativeSource)) { throw "Native worker source not found: $nativeSource" }

$outputDir = Join-Path $root ("build\native\" + $Backend.ToLowerInvariant())
$output = Join-Path $outputDir 'BetaSeedFinderWorker.exe'
$log = Join-Path $outputDir 'build.log'
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

    # Do not embed VsDevCmd.bat inside a nested cmd.exe quoted command.
    # A temporary command file safely handles paths containing spaces.
    $tempCommand = Join-Path ([IO.Path]::GetTempPath()) (
        'BetaSeedFinder-vsenv-' + [Guid]::NewGuid().ToString('N') + '.cmd'
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

    $compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $compiler) {
        throw 'cl.exe is still unavailable after initializing Visual Studio.'
    }

    Write-Host "MSVC:    $($compiler.Source)"
}

if ($Backend -eq 'NVIDIA') {
    Import-VisualStudioEnvironment

    $nvcc = $null
    if ($env:CUDA_PATH) {
        $candidate = Join-Path $env:CUDA_PATH 'bin\nvcc.exe'
        if (Test-Path $candidate) { $nvcc = $candidate }
    }
    if (-not $nvcc) {
        $command = Get-Command nvcc.exe -ErrorAction SilentlyContinue
        if ($command) { $nvcc = $command.Source }
    }
    if (-not $nvcc) { throw 'nvcc.exe was not found. Install NVIDIA CUDA Toolkit.' }

    $cudaRoot = if ($env:CUDA_PATH) {
        $env:CUDA_PATH
    } else {
        (Split-Path (Split-Path $nvcc -Parent) -Parent)
    }
    $cudaInclude = Join-Path $cudaRoot 'include'
    $cudaRuntimeHeader = Join-Path $cudaInclude 'cuda_runtime.h'
    if (-not (Test-Path $cudaRuntimeHeader -PathType Leaf)) {
        throw "CUDA development headers are missing: $cudaRuntimeHeader. Install the CUDA cudart/development package."
    }

    $reported = @(& $nvcc --list-gpu-code 2>$null) | ForEach-Object { $_.Trim() }
    $desired = @('sm_52','sm_60','sm_61','sm_70','sm_75','sm_80','sm_86','sm_87','sm_89','sm_90','sm_100','sm_120')
    $selected = @($desired | Where-Object { $reported -contains $_ })
    if ($selected.Count -eq 0) { throw 'The installed CUDA Toolkit reported no supported targets.' }

    $arguments = @(
        '-x','cu',$nativeSource,
        '-I',$cudaInclude,
        '-O3','-std=c++17','-DBSF_NVIDIA_CUDA=1','--cudart=static',
        '--fmad=false','--ftz=false','--prec-div=true','--prec-sqrt=true','-Xcompiler=/EHsc,/MT','-o',$output
    )
    foreach ($code in $selected) {
        $number = $code.Substring(3)
        $arguments += "-gencode=arch=compute_$number,code=$code"
    }
    $ptx = if ($selected -contains 'sm_75') { '75' } else { $selected[0].Substring(3) }
    $arguments += "-gencode=arch=compute_$ptx,code=compute_$ptx"

    & $nvcc @arguments 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) { throw "NVIDIA build failed. See $log" }
}
else {
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
                    if (Test-Path $candidate) { $hipcc = $candidate; break }
                }
                if ($hipcc) { break }
            }
        }
    }
    if (-not $hipcc) { throw 'AMD HIP SDK hipcc was not found. Set HIP_SDK_DIR or install HIP SDK.' }

    $arguments = @(
        $nativeSource,'-O3','-std=c++17',
        '--offload-arch=gfx1030','--offload-arch=gfx1031','--offload-arch=gfx1032',
        '--offload-arch=gfx1100','--offload-arch=gfx1101','--offload-arch=gfx1102',
        '--offload-arch=gfx1200','--offload-arch=gfx1201',
        '-ffp-contract=off','-fno-fast-math','-fno-associative-math','-o',$output
    )
    & $hipcc @arguments 2>&1 | Tee-Object -FilePath $log
    if ($LASTEXITCODE -ne 0) { throw "AMD build failed. See $log" }
}

if (-not (Test-Path $output)) { throw "Compiler did not create $output" }
$hash = (Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Created: $output" -ForegroundColor Green
Write-Host "SHA-256: $hash"
return $output
