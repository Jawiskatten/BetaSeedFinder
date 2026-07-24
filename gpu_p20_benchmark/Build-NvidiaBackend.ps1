param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

function Find-Nvcc {
    if ($env:CUDA_PATH) {
        $candidate = Join-Path $env:CUDA_PATH 'bin\nvcc.exe'
        if (Test-Path $candidate) { return $candidate }
    }

    $base = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA'
    if (Test-Path $base) {
        $versions = Get-ChildItem $base -Directory | Sort-Object Name -Descending
        foreach ($version in $versions) {
            $candidate = Join-Path $version.FullName 'bin\nvcc.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }

    $command = Get-Command nvcc.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw 'nvcc.exe was not found. Install NVIDIA CUDA Toolkit and Visual Studio C++ Build Tools.'
}

$nvcc = Find-Nvcc
$source = Join-Path $ProjectRoot 'gpu_p20_benchmark\native\gpu_p20_benchmark.cpp'
$outputDir = Join-Path $ProjectRoot 'backend\nvidia'
$output = Join-Path $outputDir 'gpu_p20_benchmark.exe'
$log = Join-Path $outputDir 'nvidia_build.log'

if (-not (Test-Path $source)) { throw "Native source not found: $source" }
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$reportedCodes = @(& $nvcc --list-gpu-code 2>$null) | ForEach-Object { $_.Trim() }
$desired = @('sm_52','sm_60','sm_61','sm_70','sm_75','sm_80','sm_86','sm_87','sm_89','sm_90','sm_100','sm_120')
$selected = @($desired | Where-Object { $reportedCodes -contains $_ })
if ($selected.Count -eq 0) {
    throw "The installed CUDA Toolkit reported no usable GPU targets: $($reportedCodes -join ', ')"
}

$args = @(
    '-x', 'cu',
    $source,
    '-O3',
    '-std=c++17',
    '-DBSF_NVIDIA_CUDA=1',
    '--cudart=static',
    '--fmad=false',
    '--ftz=false',
    '--prec-div=true',
    '--prec-sqrt=true',
    '-Xcompiler=/EHsc',
    '-o', $output
)

foreach ($code in $selected) {
    $number = $code.Substring(3)
    $args += "-gencode=arch=compute_$number,code=$code"
}

$ptxCode = if ($selected -contains 'sm_75') { '75' } else { $selected[0].Substring(3) }
$args += "-gencode=arch=compute_$ptxCode,code=compute_$ptxCode"

@(
    "NVCC: $nvcc",
    "Targets: $($selected -join ', ') + compute_$ptxCode PTX",
    "Command: nvcc $($args -join ' ')"
) | Set-Content $log

Write-Host '============================================================'
Write-Host 'BetaSeedFinder NVIDIA backend build'
Write-Host '============================================================'
Write-Host "NVCC:    $nvcc"
Write-Host "Targets: $($selected -join ', ') + PTX compute_$ptxCode"
Write-Host

& $nvcc @args 2>&1 | Tee-Object -FilePath $log -Append
if ($LASTEXITCODE -ne 0) { throw "NVIDIA build failed with exit code $LASTEXITCODE. See $log" }
if (-not (Test-Path $output)) { throw "NVCC reported success but did not create $output" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
"Worker SHA-256: $hash" | Add-Content $log

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    '' | Add-Content $log
    'PE dependencies:' | Add-Content $log
    & $dumpbin.Source /dependents $output 2>&1 | Add-Content $log
}

Write-Host
Write-Host "Built NVIDIA worker: $output"
Write-Host "SHA-256: $hash"
Write-Host 'Run TEST_NVIDIA_BACKEND.bat on an NVIDIA Windows PC.'
