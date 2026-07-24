$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$backend = Join-Path $root 'backend\nvidia'
$exe = Join-Path $backend 'gpu_p20_benchmark.exe'
$data = Join-Path $root 'gpu_p20_benchmark\data'
$exactnessLog = Join-Path $backend 'nvidia_exactness_results.txt'
$benchmarkLog = Join-Path $backend 'nvidia_mega_benchmark.txt'
$systemLog = Join-Path $backend 'nvidia_system_info.txt'
$marker = Join-Path $backend 'NVIDIA_BACKEND_VALIDATED.txt'
$returnDir = Join-Path $root 'return'
$returnZip = Join-Path $returnDir 'BetaSeedFinder-NVIDIA-TEST-RETURN.zip'
$stage = Join-Path $env:TEMP 'BetaSeedFinder-NVIDIA-Test-Return'

New-Item -ItemType Directory -Path $backend -Force | Out-Null
New-Item -ItemType Directory -Path $returnDir -Force | Out-Null
if (Test-Path $marker) { Remove-Item $marker -Force }
if (-not (Test-Path $exe)) { throw "NVIDIA worker is missing: $exe" }

$systemLines = @(
    'BetaSeedFinder NVIDIA tester system information',
    ('Date: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')),
    ('Windows: ' + [Environment]::OSVersion.VersionString)
)
$nvidiaSmi = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
if ($nvidiaSmi) {
    $systemLines += (& $nvidiaSmi.Source --query-gpu=name,driver_version,compute_cap,memory.total --format=csv,noheader 2>&1 | Out-String).TrimEnd()
} else {
    $systemLines += 'nvidia-smi.exe was not found in PATH.'
}
Set-Content $systemLog $systemLines -Encoding UTF8

$tests = @(
    @{ Name = 'P20 exactness'; Args = @('validate', (Join-Path $data 'gpu_p20_reference_1000.bin')) },
    @{ Name = 'Optimized Stage0 exactness'; Args = @('stage0optvalidate', (Join-Path $data 'gpu_stage0_reference_1000.bin')) },
    @{ Name = 'P19 exactness'; Args = @('p19validate', (Join-Path $data 'gpu_p19_reference_128.bin')) },
    @{ Name = 'Coarse exactness'; Args = @('coarsevalidate', (Join-Path $data 'gpu_coarse_reference_64.bin')) }
)

Set-Content $exactnessLog @('BetaSeedFinder NVIDIA exactness validation','==========================================') -Encoding UTF8
$step = 1
foreach ($test in $tests) {
    Write-Host "[$step/6] $($test.Name)..." -ForegroundColor Cyan
    & $exe @($test.Args) 2>&1 | Tee-Object -FilePath $exactnessLog -Append
    if ($LASTEXITCODE -ne 0) { throw "$($test.Name) failed with exit code $LASTEXITCODE." }
    $step++
}

Write-Host '[5/6] MEGA production benchmark...' -ForegroundColor Cyan
& $exe pipelineprofile 32768 123456789 3 mega full 2>&1 | Tee-Object -FilePath $benchmarkLog
if ($LASTEXITCODE -ne 0) { throw "MEGA benchmark failed with exit code $LASTEXITCODE." }

$workerHash = (Get-FileHash $exe -Algorithm SHA256).Hash
@(
    'BetaSeedFinder NVIDIA backend validation passed.',
    ('Validated: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')),
    ('Worker SHA-256: ' + $workerHash),
    'Exactness: P20, optimized Stage0, P19, coarse',
    'Production benchmark: MEGA full passed'
) | Set-Content $marker -Encoding UTF8

Write-Host '[6/6] Creating return ZIP...' -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage 'backend\nvidia') -Force | Out-Null
foreach ($name in @('gpu_p20_benchmark.exe','NVIDIA_BACKEND_VALIDATED.txt','nvidia_exactness_results.txt','nvidia_mega_benchmark.txt','nvidia_system_info.txt')) {
    $source = Join-Path $backend $name
    if (Test-Path $source) { Copy-Item $source (Join-Path $stage 'backend\nvidia') -Force }
}
Set-Content (Join-Path $stage 'RETURN_PACKAGE_README.txt') @('Send this ZIP to the project owner.',("Worker SHA-256: $workerHash")) -Encoding UTF8
if (Test-Path $returnZip) { Remove-Item $returnZip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $returnZip -CompressionLevel Optimal
$returnHash = (Get-FileHash $returnZip -Algorithm SHA256).Hash
Set-Content (Join-Path $returnDir 'SHA256SUMS.txt') "$returnHash  $([IO.Path]::GetFileName($returnZip))" -Encoding ASCII
Remove-Item $stage -Recurse -Force

Write-Host 'ALL NVIDIA TESTS PASSED.' -ForegroundColor Green
Write-Host "Return ZIP: $returnZip" -ForegroundColor Green
Write-Host "Worker SHA-256: $workerHash" -ForegroundColor Green
