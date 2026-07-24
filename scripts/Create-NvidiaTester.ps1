param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$worker = Join-Path $ProjectRoot 'backend\nvidia\gpu_p20_benchmark.exe'
$dataDir = Join-Path $ProjectRoot 'gpu_p20_benchmark\data'
$outDir = Join-Path $ProjectRoot 'release'
$outZip = Join-Path $outDir 'BetaSeedFinder-NVIDIA-NoInstall-Tester.zip'
$stage = Join-Path $env:TEMP 'BetaSeedFinder-NVIDIA-NoInstall-Tester'

if (-not (Test-Path -LiteralPath $worker)) { throw "NVIDIA worker not found: $worker" }

$requiredData = @(
    'gpu_p20_reference_1000.bin',
    'gpu_stage0_reference_1000.bin',
    'gpu_p19_reference_128.bin',
    'gpu_coarse_reference_64.bin'
)
foreach ($name in $requiredData) {
    $path = Join-Path $dataDir $name
    if (-not (Test-Path -LiteralPath $path)) { throw "Required reference file missing: $path" }
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage 'backend\nvidia') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage 'gpu_p20_benchmark\data') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage 'scripts') -Force | Out-Null

Copy-Item $worker (Join-Path $stage 'backend\nvidia\gpu_p20_benchmark.exe') -Force
foreach ($name in $requiredData) {
    Copy-Item (Join-Path $dataDir $name) (Join-Path $stage 'gpu_p20_benchmark\data') -Force
}
Copy-Item (Join-Path $ProjectRoot 'scripts\Run-NvidiaTester.ps1') (Join-Path $stage 'scripts') -Force
Copy-Item (Join-Path $ProjectRoot 'NVIDIA_NO_INSTALL_TEST.bat') $stage -Force

$workerHash = (Get-FileHash $worker -Algorithm SHA256).Hash
@(
    'BetaSeedFinder NVIDIA no-install tester',
    '',
    'Requirements:',
    '- Windows 10 or 11, 64-bit',
    '- NVIDIA GPU with a current driver',
    '',
    'No Java, CUDA Toolkit, or Visual Studio installation is required.',
    '',
    'Run NVIDIA_NO_INSTALL_TEST.bat and return the generated ZIP.',
    '',
    "Worker SHA-256: $workerHash"
) | Set-Content (Join-Path $stage 'README.txt') -Encoding UTF8

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
if (Test-Path $outZip) { Remove-Item $outZip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $outZip -CompressionLevel Optimal
$zipHash = (Get-FileHash $outZip -Algorithm SHA256).Hash
Set-Content (Join-Path $outDir 'SHA256SUMS.txt') "$zipHash  $([IO.Path]::GetFileName($outZip))" -Encoding ASCII
Remove-Item $stage -Recurse -Force

Write-Host "Created: $outZip" -ForegroundColor Green
Write-Host "SHA-256: $zipHash" -ForegroundColor Green
