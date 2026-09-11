param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\tu4_water_finder.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "TU4 water source not found: $sourcePath"
}

function Read-SourceText {
    return [System.IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")
}

$text = Read-SourceText

if ($text.Contains('TU4_WATER_P15_PURE_SCREEN_TOP5')) {
    Write-Host 'TU4 Water P15 top-5 routing is already applied.' -ForegroundColor Green
    exit 0
}

if (-not $text.Contains('TU4_WATER_P13_SCREEN_RECALL_AUDIT')) {
    throw 'P15b requires the local source to have P13b/P12c applied first.'
}

if (-not $text.Contains('TU4_WATER_P14_PURE_SCREEN_TOP8')) {
    Write-Host 'P14 marker is missing; applying P14 automatically...' -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot 'patch-tu4-water-p14-pure-screen-top8.ps1') -ProjectRoot $ProjectRoot
    if ($LASTEXITCODE -ne 0) {
        throw "P14 patcher exited with code $LASTEXITCODE."
    }

    $text = Read-SourceText
    if (-not $text.Contains('TU4_WATER_P14_PURE_SCREEN_TOP8')) {
        throw 'P14 patcher returned but the P14 marker is still missing.'
    }
}
else {
    Write-Host 'P14 is already present.' -ForegroundColor DarkGray
}

Write-Host 'Applying P15 top-5 routing...' -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'patch-tu4-water-p15-pure-screen-top5.ps1') -ProjectRoot $ProjectRoot
if ($LASTEXITCODE -ne 0) {
    throw "P15 patcher exited with code $LASTEXITCODE."
}

$text = Read-SourceText
if (-not $text.Contains('TU4_WATER_P15_PURE_SCREEN_TOP5')) {
    throw 'P15 patcher returned but the P15 marker is still missing.'
}

Write-Host 'P15 chain complete: P13b/P12c -> P14 top-8 -> P15 top-5.' -ForegroundColor Green
Write-Host 'Rebuild and verify the 96617-land reference before benchmarking.'
