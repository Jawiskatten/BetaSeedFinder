param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$AmdWorker = '',
    [string]$NvidiaWorker = '',
    [string]$OutputDirectory = '',
    [switch]$RequireUniversal,
    [switch]$Publish
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'release'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

function Resolve-FirstFile([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }

        $expanded = if ([IO.Path]::IsPathRooted($candidate)) {
            $candidate
        } else {
            Join-Path $root $candidate
        }

        if (Test-Path $expanded -PathType Leaf) {
            return (Resolve-Path $expanded).Path
        }
    }
    return $null
}

function Copy-WorkerBundle(
    [Parameter(Mandatory = $true)][string]$SourceWorker,
    [Parameter(Mandatory = $true)][string]$DestinationDirectory
) {
    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null

    $workerTarget = Join-Path $DestinationDirectory 'BetaSeedFinderWorker.exe'
    Copy-Item -LiteralPath $SourceWorker -Destination $workerTarget -Force

    # Sidecar runtime DLLs are kept internal beside their worker. This lets the
    # universal package run without exposing extra launchers or backend files.
    $sourceDirectory = Split-Path $SourceWorker -Parent
    Get-ChildItem $sourceDirectory -File -Filter '*.dll' -ErrorAction SilentlyContinue |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (
                Join-Path $DestinationDirectory $_.Name
            ) -Force
        }

    return $workerTarget
}

if ([string]::IsNullOrWhiteSpace($AmdWorker)) {
    $AmdWorker = Resolve-FirstFile @(
        'build\native\amd\BetaSeedFinderWorker.exe',
        'backend\amd\BetaSeedFinderWorker.exe',
        'backend\amd\gpu_p20_benchmark.exe',
        'gpu_p20_benchmark\build\gpu_p20_benchmark.exe'
    )
} else {
    $AmdWorker = Resolve-FirstFile @($AmdWorker)
}

if ([string]::IsNullOrWhiteSpace($NvidiaWorker)) {
    $NvidiaWorker = Resolve-FirstFile @(
        'build\native\nvidia\BetaSeedFinderWorker.exe',
        'backend\nvidia\BetaSeedFinderWorker.exe',
        'backend\nvidia\gpu_p20_benchmark.exe'
    )
} else {
    $NvidiaWorker = Resolve-FirstFile @($NvidiaWorker)
}

if ($RequireUniversal -and (-not $AmdWorker -or -not $NvidiaWorker)) {
    throw 'A universal package requires both the AMD and NVIDIA workers.'
}
if (-not $AmdWorker -and -not $NvidiaWorker) {
    throw 'No native worker was found. Build or provide at least one AMD/NVIDIA worker.'
}

$jpackage = Get-Command jpackage.exe -ErrorAction SilentlyContinue
if (-not $jpackage) {
    $jpackage = Get-Command jpackage -ErrorAction SilentlyContinue
}
if (-not $jpackage) {
    throw 'jpackage was not found. Install JDK 17 or newer.'
}

$jarPath = & (Join-Path $root 'scripts\build-java.ps1') -ProjectRoot $root
$version = (Get-Content (Join-Path $root 'VERSION.txt') -Raw).Trim()
$appVersion = if ($version -match '^v(\d+\.\d+\.\d+)') {
    $Matches[1]
} else {
    '0.5.0'
}

$edition = if ($AmdWorker -and $NvidiaWorker) {
    'universal'
} elseif ($AmdWorker) {
    'amd'
} else {
    'nvidia'
}

$temp = Join-Path $env:TEMP (
    'BetaSeedFinder-package-' + [Guid]::NewGuid().ToString('N')
)
$input = Join-Path $temp 'input'
$imageOut = Join-Path $temp 'image'

Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $input, $imageOut | Out-Null
Copy-Item -LiteralPath $jarPath -Destination (
    Join-Path $input 'BetaSeedFinder.jar'
) -Force

if ($AmdWorker) {
    $null = Copy-WorkerBundle $AmdWorker (
        Join-Path $input 'backend\amd'
    )
}
if ($NvidiaWorker) {
    $null = Copy-WorkerBundle $NvidiaWorker (
        Join-Path $input 'backend\nvidia'
    )
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

if ($LASTEXITCODE -ne 0) {
    throw "jpackage failed with exit code $LASTEXITCODE"
}

$image = Join-Path $imageOut 'BetaSeedFinder'
$packagedBackend = Join-Path $image 'app\backend'
if (Test-Path $packagedBackend) {
    Move-Item $packagedBackend (Join-Path $image 'backend') -Force
}

$amdPackaged = Join-Path $image 'backend\amd\BetaSeedFinderWorker.exe'
$nvidiaPackaged = Join-Path $image 'backend\nvidia\BetaSeedFinderWorker.exe'

if ($RequireUniversal) {
    if (-not (Test-Path $amdPackaged -PathType Leaf)) {
        throw 'The packaged AMD worker is missing.'
    }
    if (-not (Test-Path $nvidiaPackaged -PathType Leaf)) {
        throw 'The packaged NVIDIA worker is missing.'
    }
}

@(
    'BetaSeedFinder'
    '=============='
    ''
    'Run BetaSeedFinder.exe.'
    'Java is bundled; no Java installation is required.'
    'The launcher automatically selects AMD or NVIDIA.'
    "Included backend edition: $edition"
    ''
    'Internal GPU workers are stored under backend\amd and backend\nvidia.'
    'Settings and search results are stored in the config and out folders.'
    'Project: https://github.com/Jawiskatten/BetaSeedFinder'
) | Set-Content (Join-Path $image 'README.txt') -Encoding UTF8

@(
    "Package edition: $edition"
    "AMD worker: $(if (Test-Path $amdPackaged) { 'included' } else { 'not included' })"
    "NVIDIA worker: $(if (Test-Path $nvidiaPackaged) { 'included' } else { 'not included' })"
    'Selection mode: automatic by installed GPU vendor'
) | Set-Content (Join-Path $image 'BACKENDS.txt') -Encoding UTF8

Copy-Item (Join-Path $root 'LICENSE') (
    Join-Path $image 'LICENSE.txt'
) -Force

if (-not (Test-Path (Join-Path $image 'BetaSeedFinder.exe'))) {
    throw 'Packaged launcher is missing.'
}
if (-not (Test-Path (Join-Path $image 'runtime'))) {
    throw 'Bundled Java runtime is missing.'
}

$fileName = "BetaSeedFinder-$version-windows-x64-$edition.zip"
$zip = Join-Path $OutputDirectory $fileName
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $image '*') `
    -DestinationPath $zip `
    -CompressionLevel Optimal

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$sumPath = Join-Path $OutputDirectory 'SHA256SUMS.txt'
Set-Content $sumPath "$hash  $fileName" -Encoding ASCII

Write-Host "Created: $zip" -ForegroundColor Green
Write-Host "Edition: $edition"
Write-Host "SHA-256: $hash"

if ($Publish) {
    $gh = Get-Command gh.exe -ErrorAction SilentlyContinue
    if (-not $gh) {
        throw 'GitHub CLI was not found.'
    }

    & $gh.Source release view $version 1>$null 2>$null
    if ($LASTEXITCODE -ne 0) {
        & $gh.Source release create $version `
            $zip `
            $sumPath `
            --prerelease `
            --generate-notes `
            --title "BetaSeedFinder $version"
    } else {
        & $gh.Source release upload $version `
            $zip `
            $sumPath `
            --clobber
    }

    if ($LASTEXITCODE -ne 0) {
        throw 'GitHub release upload failed.'
    }
}

Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
return $zip
