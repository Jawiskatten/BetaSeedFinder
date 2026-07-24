param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [switch]$Strict
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path

$required = @(
    '.gitignore','LICENSE','README.md','BUILDING.md','CHANGELOG.md','CONTRIBUTING.md','SECURITY.md','SUPPORT.md','VERSION.txt',
    'src\GuiMain.java','src\GpuBackendLocator.java',
    'gpu_p20_benchmark\native\gpu_p20_benchmark.cpp',
    'gpu_p20_benchmark\native\gpu_runtime_compat.hpp',
    'gpu_p20_benchmark\data\gpu_p20_reference_1000.bin',
    'gpu_p20_benchmark\data\gpu_stage0_reference_1000.bin',
    'gpu_p20_benchmark\data\gpu_p19_reference_128.bin',
    'gpu_p20_benchmark\data\gpu_coarse_reference_64.bin',
    '.github\workflows\source-ci.yml',
    '.github\workflows\build-nvidia-windows.yml',
    '.github\workflows\publish-prerelease.yml',
    '.github\ISSUE_TEMPLATE\bug_report.yml',
    '.github\ISSUE_TEMPLATE\feature_request.yml',
    '.github\dependabot.yml',
    'PUBLISH_PUBLIC_GITHUB.bat','scripts\Publish-PublicGitHub.ps1',
    'scripts\SourceFiles.txt','scripts\Create-GithubSource.ps1',
    'scripts\Verify-GithubSource.ps1','scripts\Verify-NvidiaCudaSource.ps1'
)
foreach ($relative in $required) {
    if (-not (Test-Path (Join-Path $root $relative))) { throw "Required source file missing: $relative" }
}

if (Test-Path (Join-Path $root 'scripts\P63-SourceFiles.txt')) {
    throw 'Obsolete internal source manifest remains: scripts\P63-SourceFiles.txt'
}

$sourceList = Get-Content (Join-Path $root 'scripts\SourceFiles.txt') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
foreach ($relative in $sourceList) {
    if (-not (Test-Path (Join-Path $root ('src\' + $relative)))) { throw "Manifest source file missing: $relative" }
}

& (Join-Path $root 'scripts\Verify-NvidiaCudaSource.ps1') -ProjectRoot $root
& (Join-Path $root 'scripts\Check-DualGpuSource.ps1')

$version = (Get-Content (Join-Path $root 'VERSION.txt') -Raw).Trim()
if ($version -notmatch '^v\d+\.\d+\.\d+-alpha\.\d+$') { throw "Public alpha VERSION.txt is invalid: $version" }
$gui = Get-Content (Join-Path $root 'src\GuiMain.java') -Raw
if ($gui -notmatch [regex]::Escape('APP_VERSION = "' + $version + '"')) { throw 'GuiMain.APP_VERSION does not match VERSION.txt.' }

$workflow = Get-Content (Join-Path $root '.github\workflows\build-nvidia-windows.yml') -Raw
if ($workflow -notmatch 'actions/checkout@v7') { throw 'NVIDIA workflow does not use the expected checkout major.' }
if ($workflow -notmatch 'Jimver/cuda-toolkit@v0\.2\.35') { throw 'NVIDIA workflow CUDA action is not pinned to v0.2.35.' }

if ($Strict) {
    $forbiddenDirs = @('out','config','bin','release','return','old','OldGenerator','island_tail_analysis_export','BetaSeedFinder-v0.5.0-alpha-source')
    foreach ($name in $forbiddenDirs) {
        if (Test-Path (Join-Path $root $name)) { throw "Forbidden generated/legacy directory is present: $name" }
    }

    $badExtensions = @('.exe','.dll','.class','.obj','.o','.pdb','.lib','.exp','.wav','.ttf','.otf')
    $badFiles = Get-ChildItem $root -Recurse -File | Where-Object {
        $badExtensions -contains $_.Extension.ToLowerInvariant() -or
        ($_.Extension -eq '.png' -and $_.FullName -match '[\\/]assets[\\/]textures[\\/]')
    }
    if ($badFiles) { throw 'Forbidden binary/unlicensed files found: ' + (($badFiles.FullName) -join ', ') }

    $allowedSource = @{}
    foreach ($relative in $sourceList) { $allowedSource[$relative.Replace('/','\').ToLowerInvariant()] = $true }
    $unexpectedSource = Get-ChildItem (Join-Path $root 'src') -Recurse -Filter *.java | Where-Object {
        $relative = $_.FullName.Substring((Join-Path $root 'src').Length + 1).ToLowerInvariant()
        -not $allowedSource.ContainsKey($relative)
    }
    if ($unexpectedSource) { throw 'Unexpected/non-production Java sources found: ' + (($unexpectedSource.FullName) -join ', ') }

    $personalPatterns = @(
        ('D:' + '\\Yes\\BetaSeedFinder'),
        ('C:' + '\\Users\\'),
        ('/home/' + 'oai/'),
        ('/mnt/' + 'data/')
    )
    $textFiles = Get-ChildItem $root -Recurse -File | Where-Object {
        $_.Length -lt 5MB -and $_.Extension -in @('.md','.txt','.java','.cpp','.hpp','.ps1','.bat','.yml','.yaml','.cmake')
    }
    foreach ($file in $textFiles) {
        $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
        foreach ($pattern in $personalPatterns) {
            if ($content -match [regex]::Escape($pattern)) { throw "Personal/build path found in $($file.FullName): $pattern" }
        }
    }
}

Write-Host ('Public source verification passed' + $(if ($Strict) { ' (strict).' } else { '.' }))
