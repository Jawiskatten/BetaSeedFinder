param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$ProjectRoot = (Resolve-Path $ProjectRoot).Path

$sourcePath = Join-Path $ProjectRoot 'native\src\tu4_water_finder.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "TU4 water source not found: $sourcePath"
}

function Read-SourceText {
    return [System.IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")
}

$text = Read-SourceText
if ($text.Contains('TU4_WATER_P12_EXACT_DENSITY_SCREEN')) {
    Write-Host 'TU4 Water P12 is already applied.' -ForegroundColor Green
    exit 0
}

if (-not $text.Contains('TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND')) {
    if (-not $text.Contains('TU4_WATER_P10_RESTORE_PARTIAL_SORT')) {
        throw 'P12b expected at least TU4 Water P10 in the local source.'
    }

    $p11 = Join-Path $PSScriptRoot 'patch-tu4-water-p11-paired-noise-reduced-land.ps1'
    if (-not (Test-Path $p11 -PathType Leaf)) {
        throw "P11 patch script not found: $p11"
    }

    Write-Host 'P11 marker is missing from local source; applying P11 automatically...' -ForegroundColor Yellow
    & $p11 -ProjectRoot $ProjectRoot

    $text = Read-SourceText
    if (-not $text.Contains('TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND')) {
        throw 'P11 script returned without installing the P11 marker; refusing to apply P12.'
    }
}

$p12 = Join-Path $PSScriptRoot 'patch-tu4-water-p12-exact-density-screen.ps1'
if (-not (Test-Path $p12 -PathType Leaf)) {
    throw "P12 patch script not found: $p12"
}

Write-Host 'Applying P12 exact-density screen...' -ForegroundColor Cyan
& $p12 -ProjectRoot $ProjectRoot

$text = Read-SourceText
if (-not $text.Contains('TU4_WATER_P12_EXACT_DENSITY_SCREEN')) {
    throw 'P12 script returned without installing the P12 marker.'
}

Write-Host 'P12 chain complete: P11 prerequisite + P12 screen are installed.' -ForegroundColor Green
Write-Host 'Rebuild and verify the 96617-land record before benchmarking.'
