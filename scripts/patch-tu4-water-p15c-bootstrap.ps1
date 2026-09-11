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

function Invoke-PatchAndRequireMarker {
    param(
        [string]$ScriptName,
        [string[]]$Markers,
        [string]$Label
    )

    $textNow = Read-SourceText
    foreach ($marker in $Markers) {
        if ($textNow.Contains($marker)) {
            Write-Host "$Label already present ($marker)." -ForegroundColor DarkGray
            return
        }
    }

    $scriptPath = Join-Path $PSScriptRoot $ScriptName
    if (-not (Test-Path $scriptPath -PathType Leaf)) {
        throw "Required patch script not found: $scriptPath"
    }

    Write-Host "Applying $Label..." -ForegroundColor Cyan
    & $scriptPath -ProjectRoot $ProjectRoot
    if ($LASTEXITCODE -ne 0) {
        throw "$Label patcher exited with code $LASTEXITCODE."
    }

    $textNow = Read-SourceText
    foreach ($marker in $Markers) {
        if ($textNow.Contains($marker)) { return }
    }
    throw "$Label patcher returned but none of the expected markers are present: $($Markers -join ', ')"
}

$text = Read-SourceText

if ($text.Contains('TU4_WATER_P15_PURE_SCREEN_TOP5')) {
    Write-Host 'TU4 Water P15 top-5 routing is already applied.' -ForegroundColor Green
    exit 0
}

# Bootstrap any local direct-chain state from P10 onward.
# P10 -> P12c -> P13b -> P14 -> P15.
if ($text.Contains('TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND') -or
    $text.Contains('TU4_WATER_P12_EXACT_DENSITY_SCREEN')) {
    throw 'P15c detected the P11/normal-P12 branch. Refusing to mix it with the P12c direct-from-P10 chain.'
}

if (-not $text.Contains('TU4_WATER_P12_DIRECT_SCREEN')) {
    if (-not $text.Contains('TU4_WATER_P10_RESTORE_PARTIAL_SORT')) {
        throw 'P15c can bootstrap from P10/P12c/P13b/P14, but the local source has none of those markers.'
    }
    Invoke-PatchAndRequireMarker `
        -ScriptName 'patch-tu4-water-p12c-direct-from-p10.ps1' `
        -Markers @('TU4_WATER_P12_DIRECT_SCREEN') `
        -Label 'P12c direct screen'
}

Invoke-PatchAndRequireMarker `
    -ScriptName 'patch-tu4-water-p13b-screen-recall-audit.ps1' `
    -Markers @('TU4_WATER_P13B_SCREEN_RECALL_AUDIT','TU4_WATER_P13_SCREEN_RECALL_AUDIT') `
    -Label 'P13b screen audit'

Invoke-PatchAndRequireMarker `
    -ScriptName 'patch-tu4-water-p14-pure-screen-top8.ps1' `
    -Markers @('TU4_WATER_P14_PURE_SCREEN_TOP8') `
    -Label 'P14 pure screen top-8'

Invoke-PatchAndRequireMarker `
    -ScriptName 'patch-tu4-water-p15-pure-screen-top5.ps1' `
    -Markers @('TU4_WATER_P15_PURE_SCREEN_TOP5') `
    -Label 'P15 pure screen top-5'

Write-Host 'P15c bootstrap complete.' -ForegroundColor Green
Write-Host 'Local chain is now P10 -> P12c -> P13b -> P14 -> P15 (with already-present stages skipped).'
Write-Host 'Rebuild and verify the 96617-land reference before benchmarking.'