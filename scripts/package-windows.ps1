param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$AmdWorker = '',
    [string]$NvidiaWorker = '',
    [string]$OutputDirectory = '',
    [switch]$Publish
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $root 'release' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

function Resolve-FirstFile([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $expanded = if ([IO.Path]::IsPathRooted($candidate)) { $candidate } else { Join-Path $root $candidate }
            if (Test-Path $expanded -PathType Leaf) { return (Resolve-Path $expanded).Path }
        }
    }
    return $null
}

if ([string]::IsNullOrWhiteSpace($AmdWorker)) {
    $AmdWorker = Resolve-FirstFile @(
        'build\native\amd\BetaSeedFinderWorker.exe',
        'backend\amd\BetaSeedFinderWorker.exe',
        'backend\amd\gpu_p20_benchmark.exe',
        'gpu_p20_benchmark\build\gpu_p20_benchmark.exe'
    )
} else { $AmdWorker = Resolve-FirstFile @($AmdWorker) }

if ([string]::IsNullOrWhiteSpace($NvidiaWorker)) {
    $NvidiaWorker = Resolve-FirstFile @(
        'build\native\nvidia\BetaSeedFinderWorker.exe',
        'backend\nvidia\BetaSeedFinderWorker.exe',
        'backend\nvidia\gpu_p20_benchmark.exe'
    )
} else { $NvidiaWorker = Resolve-FirstFile @($NvidiaWorker) }

if (-not $AmdWorker -and -not $NvidiaWorker) {
    throw 'No native worker was found. Build or provide at least one AMD/NVIDIA worker.'
}

$jpackage = Get-Command jpackage.exe -ErrorAction SilentlyContinue
if (-not $jpackage) { $jpackage = Get-Command jpackage -ErrorAction SilentlyContinue }
if (-not $jpackage) { throw 'jpackage was not found. Install JDK 17 or newer.' }

$jarPath = & (Join-Path $root 'scripts\build-java.ps1') -ProjectRoot $root
$version = (Get-Content (Join-Path $root 'VERSION.txt') -Raw).Trim()
$appVersion = if ($version -match '^v(\d+\.\d+\.\d+)') { $Matches[1] } else { '0.5.0' }
$edition = if ($AmdWorker -and $NvidiaWorker) { 'universal' } elseif ($AmdWorker) { 'amd' } else { 'nvidia' }

$temp = Join-Path $env:TEMP ("BetaSeedFinder-package-" + $PID)
$input = Join-Path $temp 'input'
$imageOut = Join-Path $temp 'image'
Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $input, $imageOut | Out-Null
Copy-Item $jarPath (Join-Path $input 'BetaSeedFinder.jar')

if ($AmdWorker) {
    $target = Join-Path $input 'backend\amd\BetaSeedFinderWorker.exe'
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item $AmdWorker $target
}
if ($NvidiaWorker) {
    $target = Join-Path $input 'backend\nvidia\BetaSeedFinderWorker.exe'
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item $NvidiaWorker $target
}

& $jpackage.Source `
    --type app-image `
    --name BetaSeedFinder `
    --dest $imageOut `
    --input $input `
    --main-jar BetaSeedFinder.jar `
    --main-class GuiMain `
    --app-version $appVersion `
    --vendor Jawiskatten `
    --description 'Minecraft Beta 1.7.3 floating-island seed finder' `
    --java-options '-Dbetaseedfinder.appRoot=$APPDIR'
if ($LASTEXITCODE -ne 0) { throw "jpackage failed with exit code $LASTEXITCODE" }

$image = Join-Path $imageOut 'BetaSeedFinder'
$packagedBackend = Join-Path $image 'app\backend'
if (Test-Path $packagedBackend) {
    Move-Item $packagedBackend (Join-Path $image 'backend') -Force
}

@(
    'BetaSeedFinder',
    '==============',
    '',
    'Run BetaSeedFinder.exe.',
    'Java is bundled; no Java installation is required.',
    "Included GPU backend edition: $edition",
    '',
    'Your settings and search results are stored in the config and out folders.',
    'Project: https://github.com/Jawiskatten/BetaSeedFinder'
) | Set-Content (Join-Path $image 'README.txt') -Encoding UTF8
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $image 'LICENSE.txt') -Force

if (-not (Test-Path (Join-Path $image 'BetaSeedFinder.exe'))) { throw 'Packaged launcher is missing.' }
if (-not (Test-Path (Join-Path $image 'runtime'))) { throw 'Bundled Java runtime is missing.' }

$fileName = "BetaSeedFinder-$version-windows-x64-$edition.zip"
$zip = Join-Path $OutputDirectory $fileName
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $image '*') -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content (Join-Path $OutputDirectory 'SHA256SUMS.txt') "$hash  $fileName" -Encoding ASCII

Write-Host "Created: $zip" -ForegroundColor Green
Write-Host "SHA-256: $hash"

if ($Publish) {
    $gh = Get-Command gh.exe -ErrorAction SilentlyContinue
    if (-not $gh) { throw 'GitHub CLI was not found.' }
    & $gh.Source release view $version 1>$null 2>$null
    if ($LASTEXITCODE -ne 0) {
        & $gh.Source release create $version $zip (Join-Path $OutputDirectory 'SHA256SUMS.txt') --prerelease --generate-notes --title "BetaSeedFinder $version"
    } else {
        & $gh.Source release upload $version $zip (Join-Path $OutputDirectory 'SHA256SUMS.txt') --clobber
    }
    if ($LASTEXITCODE -ne 0) { throw 'GitHub release upload failed.' }
}

Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
return $zip
