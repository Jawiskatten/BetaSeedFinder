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

function Invoke-PatchAndRequireAnyMarker {
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

# P15c now avoids the brittle P14 multiline patch entirely. It can start from
# P10, P12c, P13/P13b, or P14 on the direct chain, then structurally replaces
# only the finalist-routing region between the screen sort and audit/compaction.
if ($text.Contains('TU4_WATER_P11_PAIRED_NOISE_REDUCED_LAND') -or
    $text.Contains('TU4_WATER_P12_EXACT_DENSITY_SCREEN')) {
    throw 'P15c detected the P11/normal-P12 branch. Refusing to mix it with the P12c direct-from-P10 chain.'
}

if (-not $text.Contains('TU4_WATER_P12_DIRECT_SCREEN')) {
    if (-not $text.Contains('TU4_WATER_P10_RESTORE_PARTIAL_SORT')) {
        throw 'P15c can bootstrap from P10/P12c/P13/P13b/P14, but no compatible marker was found.'
    }
    Invoke-PatchAndRequireAnyMarker `
        -ScriptName 'patch-tu4-water-p12c-direct-from-p10.ps1' `
        -Markers @('TU4_WATER_P12_DIRECT_SCREEN') `
        -Label 'P12c direct screen'
}

Invoke-PatchAndRequireAnyMarker `
    -ScriptName 'patch-tu4-water-p13b-screen-recall-audit.ps1' `
    -Markers @('TU4_WATER_P13B_SCREEN_RECALL_AUDIT','TU4_WATER_P13_SCREEN_RECALL_AUDIT') `
    -Label 'P13b screen audit'

$text = Read-SourceText
if ($text.Contains('TU4_WATER_P15_PURE_SCREEN_TOP5')) {
    Write-Host 'TU4 Water P15 top-5 routing is already applied.' -ForegroundColor Green
    exit 0
}

$backupPath = $sourcePath + '.before-water-p15c-structural.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# Locate the screen sort structurally, then replace whatever routing implementation
# follows it (P12c 6+2, P14 top-8, or another direct-chain variant) with top-5.
$sortStart = $text.IndexOf('std::sort(screen.begin(), screen.end()')
if ($sortStart -lt 0) {
    throw 'Could not locate screen ranking sort in runExactBatch.'
}
$sortClose = $text.IndexOf('    });', $sortStart)
if ($sortClose -lt 0) {
    throw 'Could not locate end of screen ranking sort.'
}
$routeStart = $sortClose + '    });'.Length
while ($routeStart -lt $text.Length -and ($text[$routeStart] -eq "`r" -or $text[$routeStart] -eq "`n")) {
    $routeStart++
}

$compactPos = $text.IndexOf('    // Compact chosen candidates into front slots and run the unchanged full exact.', $routeStart)
if ($compactPos -lt 0) {
    throw 'Could not locate chosen-candidate compaction point.'
}
$auditPos = $text.IndexOf('    if (gP13AuditScreen && n > 1) {', $routeStart)
$routeEnd = $compactPos
if ($auditPos -ge 0 -and $auditPos -lt $compactPos) {
    $routeEnd = $auditPos
}
if ($routeEnd -le $routeStart) {
    throw 'Invalid structural routing bounds.'
}

$newRouting = @'
    // TU4_WATER_P15_PURE_SCREEN_TOP5
    // P13b ground truth: screen ranks 0..4 held the true best-of-24 in
    // 1959/2000 batches = 97.95%. Exact the strongest ~one fifth of the
    // pre-finalists; at TopExact=24 this is five full exact evaluations.
    // Every reported record still uses the unchanged authoritative exact metric.
    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 4) / 5));
    std::vector<int> chosen;
    chosen.reserve(static_cast<std::size_t>(fullN));
    for (int rank = 0; rank < n && static_cast<int>(chosen.size()) < fullN; ++rank) {
        chosen.push_back(screen[static_cast<std::size_t>(rank)].source);
    }

'@
$text = $text.Remove($routeStart, $routeEnd - $routeStart).Insert($routeStart, $newRouting)

# Preserve any existing P14 marker if present; P15 no longer depends on it.
# Update banners from whichever direct-chain stage the local source currently has.
$bannerTargets = @(
    'Scout P12c: P10/P8 + 16x16 full-density screen; 8/24 full exact; final metric unchanged.',
    'Scout P13: P12c screen + optional all-24 recall audit; normal 8/24 search unchanged.',
    'Scout P13b: P12c 8/24 screen + optional all-24 recall audit; final metric unchanged.',
    'Scout P14: pure 16x16 screen top-8 + optional all-24 recall audit; exact metric unchanged.'
)
foreach ($banner in $bannerTargets) {
    $text = $text.Replace($banner,
        'Scout P15: pure 16x16 screen top-5 + optional all-24 recall audit; exact metric unchanged.')
}

# If audit mode is used under P15, the chosen set is five, not eight.
$text = $text.Replace(' chosen8Recall=', ' chosen5Recall=')

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$verify = Read-SourceText
if (-not $verify.Contains('TU4_WATER_P15_PURE_SCREEN_TOP5')) {
    throw 'P15c wrote the source but the P15 marker is missing.'
}

Write-Host 'Applied TU4 Water P15 top-5 structurally.' -ForegroundColor Green
Write-Host 'Skipped brittle P14 dependency; routing was replaced directly after the 16x16 screen sort.'
Write-Host 'TopExact=24 now sends 5 candidates to full exact; final exact metric is unchanged.'
Write-Host 'Rebuild and verify the 96617-land reference before benchmarking.'
