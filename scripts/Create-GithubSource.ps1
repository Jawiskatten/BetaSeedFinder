param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$Destination = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path
& (Join-Path $root 'scripts\Verify-GithubSource.ps1') -ProjectRoot $root

if ([string]::IsNullOrWhiteSpace($Destination)) { $Destination = Join-Path $root 'release' }
New-Item -ItemType Directory -Path $Destination -Force | Out-Null

$version = (Get-Content (Join-Path $root 'VERSION.txt') -Raw).Trim()
$folderName = "BetaSeedFinder-$version-source"
$stage = Join-Path $env:TEMP $folderName
$zip = Join-Path $Destination ($folderName + '.zip')
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

function Copy-RelativeFile([string]$Relative) {
    $source = Join-Path $root $Relative
    if (-not (Test-Path $source -PathType Leaf)) { throw "Allowlisted file missing: $Relative" }
    $target = Join-Path $stage $Relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item $source $target -Force
}

$files = @(
    '.gitignore','LICENSE','README.md','BUILDING.md','CHANGELOG.md','CONTRIBUTING.md','SECURITY.md','SUPPORT.md','VERSION.txt',
    'START_BETASEEDFINDER.bat','START_AMD.bat','START_NVIDIA.bat',
    'RUN_GPU_GUI_AUTO.bat','RUN_GPU_GUI_AMD.bat','RUN_GPU_GUI_NVIDIA.bat',
    'SET_GPU_BACKEND_AUTO.bat','SET_GPU_BACKEND_AMD.bat','SET_GPU_BACKEND_NVIDIA.bat',
    'run_gpu_gui.bat','compile_project.bat','BUILD_AMD_BACKEND.bat','TEST_AMD_BACKEND.bat',
    'BUILD_NVIDIA_BACKEND.bat','TEST_NVIDIA_BACKEND.bat','CREATE_NVIDIA_TESTER.bat','NVIDIA_NO_INSTALL_TEST.bat',
    'CREATE_GITHUB_SOURCE_ZIP.bat','VERIFY_GITHUB_SOURCE.bat','PUBLISH_PUBLIC_GITHUB.bat',
    'gpu_p20_benchmark\Build-NvidiaBackend.ps1','gpu_p20_benchmark\build_amd_multiarch.bat',
    'backend\README.md','backend\amd\.gitkeep','backend\nvidia\.gitkeep',
    'assets\README.md','assets\fonts\README.txt','assets\sounds\README.txt',
    '.github\workflows\source-ci.yml','.github\workflows\build-nvidia-windows.yml','.github\workflows\publish-prerelease.yml',
    '.github\ISSUE_TEMPLATE\bug_report.yml','.github\ISSUE_TEMPLATE\feature_request.yml','.github\ISSUE_TEMPLATE\config.yml',
    '.github\pull_request_template.md','.github\dependabot.yml',
    'scripts\Check-DualGpuSource.ps1','scripts\Create-GithubSource.ps1','scripts\Create-NvidiaTester.ps1',
    'scripts\Run-NvidiaTester.ps1','scripts\Verify-GithubSource.ps1','scripts\Verify-NvidiaCudaSource.ps1',
    'scripts\Publish-PublicGitHub.ps1','scripts\SourceFiles.txt',
    'docs\INSTALLATION.md','docs\GPU_PIPELINE.md','docs\TROUBLESHOOTING.md',
    'docs\ASSET_POLICY.md','docs\NVIDIA_VALIDATION.md','docs\RELEASING.md'
)
foreach ($relative in $files) { Copy-RelativeFile $relative }

$sourceList = Get-Content (Join-Path $root 'scripts\SourceFiles.txt') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
foreach ($relative in $sourceList) { Copy-RelativeFile ('src\' + $relative.Replace('/','\')) }

foreach ($relativeDir in @('gpu_p20_benchmark\native','testdata')) {
    $source = Join-Path $root $relativeDir
    if (Test-Path $source) { Copy-Item $source (Join-Path $stage $relativeDir) -Recurse -Force }
}

foreach ($name in @('gpu_p20_reference_1000.bin','gpu_stage0_reference_1000.bin','gpu_p19_reference_128.bin','gpu_coarse_reference_64.bin')) {
    Copy-RelativeFile ('gpu_p20_benchmark\data\' + $name)
}

foreach ($shot in @('islands.png','settings.png')) {
    $source = Join-Path $root ('screenshots\' + $shot)
    if (Test-Path $source) { Copy-RelativeFile ('screenshots\' + $shot) }
}

& (Join-Path $stage 'scripts\Verify-GithubSource.ps1') -ProjectRoot $stage -Strict

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content (Join-Path $Destination 'SHA256SUMS.txt') "$hash  $([IO.Path]::GetFileName($zip))" -Encoding ASCII
Remove-Item $stage -Recurse -Force

Write-Host "Created: $zip" -ForegroundColor Green
Write-Host "SHA-256: $hash" -ForegroundColor Green
