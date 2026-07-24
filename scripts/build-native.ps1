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
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio 2022 C++ Build Tools were not found.' }
    $installation = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $installation) { throw 'Visual Studio 2022 C++ Build Tools were not found.' }
    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $devCmd)) { throw "VsDevCmd.bat was not found: $devCmd" }

    $commandLine = '""{0}" -no_logo -arch=x64 -host_arch=x64 && set"' -f $devCmd
    $environment = & cmd.exe /d /s /c $commandLine
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment initialization failed.' }
    foreach ($line in $environment) {
        $index = $line.IndexOf('=')
        if ($index -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $index), $line.Substring($index + 1), 'Process')
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe is still unavailable after initializing Visual Studio.' }
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
        '--fmad=false','--ftz=false','--prec-div=true','--prec-sqrt=true','-Xcompiler=/EHsc','-o',$output
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
    $hipcc = $null
    if ($env:HIP_SDK_DIR) {
        foreach ($name in @('hipcc.exe','hipcc.bat','hipcc')) {
            $candidate = Join-Path $env:HIP_SDK_DIR ("bin\\" + $name)
            if (Test-Path $candidate) { $hipcc = $candidate; break }
        }
    }
    if (-not $hipcc) {
        $base = 'C:\Program Files\AMD\ROCm'
        if (Test-Path $base) {
            foreach ($versionDir in (Get-ChildItem $base -Directory | Sort-Object Name -Descending)) {
                foreach ($name in @('hipcc.exe','hipcc.bat','hipcc')) {
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
