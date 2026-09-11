param(
    [Parameter(Mandatory=$true)][Int64]$Seed,
    [int]$Target = 432,
    [int]$CenterX = 0,
    [int]$CenterZ = 0,
    [int]$Margin = 0,
    [int]$Scale = 3,
    [switch]$Rebuild,
    [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build\tools\SingleBiomeMap.exe'
if ($Rebuild -or -not (Test-Path $exe -PathType Leaf)) {
    & (Join-Path $root 'scripts\build-single-biome-map.ps1') -ProjectRoot $root
}

$pngOutput = Join-Path $root ("single_biome_map_${Seed}_square_r${Target}.png")
$tempBmp = Join-Path $env:TEMP ("single_biome_map_{0}.bmp" -f [guid]::NewGuid().ToString('N'))

$toolLines = & $exe `
    --seed $Seed `
    --center-x $CenterX `
    --center-z $CenterZ `
    --target $Target `
    --margin $Margin `
    --scale $Scale `
    --output $tempBmp 2>&1
$exitCode = $LASTEXITCODE
$toolLines | ForEach-Object { Write-Host $_ }
if ($exitCode -ne 0) { exit $exitCode }

$outputText = ($toolLines | ForEach-Object { "$_" }) -join "`n"

# Parse both the map renderer's labels and the finder's labels.
$coverageMatch = [regex]::Match($outputText, '(?:tundraCoverage|realCoverage)=([0-9.]+)%')
$coverage = if ($coverageMatch.Success) { [double]::Parse($coverageMatch.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) } else { 0.0 }

$biomeMatch = [regex]::Match($outputText, 'biome=([A-Z_]+)')
$biome = if ($biomeMatch.Success) { $biomeMatch.Groups[1].Value } elseif ($outputText -match 'tundraCoverage=') { 'TUNDRA' } else { 'UNKNOWN' }

$sizeMatch = [regex]::Match($outputText, 'targetSize=([0-9]+)x([0-9]+)')
$sideX = if ($sizeMatch.Success) { [int]$sizeMatch.Groups[1].Value } else { 2 * $Target }
$sideZ = if ($sizeMatch.Success) { [int]$sizeMatch.Groups[2].Value } else { 2 * $Target }
$totalCells = [int64]$sideX * [int64]$sideZ

$sameMatch = [regex]::Match($outputText, '(?:tundraBlocks|sameBiomeBlocks)=([0-9]+)/([0-9]+)')
if ($sameMatch.Success) {
    $sameBiomeBlocks = [int64]$sameMatch.Groups[1].Value
    $totalCells = [int64]$sameMatch.Groups[2].Value
} else {
    $sameBiomeBlocks = [int64][math]::Round(($coverage / 100.0) * $totalCells)
}

$wrongMatch = [regex]::Match($outputText, 'wrongBlocks=([0-9]+)')
$wrongBlocks = if ($wrongMatch.Success) { [int64]$wrongMatch.Groups[1].Value } else { $totalCells - $sameBiomeBlocks }
$wrongPct = [math]::Max(0.0, 100.0 - $coverage)
$oneWrongPer = if ($wrongBlocks -gt 0) { [math]::Round($totalCells / [double]$wrongBlocks, 1) } else { [double]::PositiveInfinity }

$wrongOffsets = @()
$wrongOffsetsMatch = [regex]::Match($outputText, 'firstWrongOffsets=(.+?)(?:\r?\n|$)')
if ($wrongOffsetsMatch.Success) {
    foreach ($m in [regex]::Matches($wrongOffsetsMatch.Groups[1].Value, '\((-?[0-9]+),(-?[0-9]+)\)')) {
        $wrongOffsets += "($($m.Groups[1].Value), $($m.Groups[2].Value))"
    }
}

# Exact palette used by native/src/single_biome_map.cpp.
$palette = [ordered]@{
    'TUNDRA'          = [System.Drawing.Color]::FromArgb(185,213,220)
    'TAIGA'           = [System.Drawing.Color]::FromArgb(76,116,111)
    'FOREST'          = [System.Drawing.Color]::FromArgb(65,125,55)
    'RAINFOREST'      = [System.Drawing.Color]::FromArgb(38,112,54)
    'SEASONAL FOREST' = [System.Drawing.Color]::FromArgb(94,143,64)
    'SWAMPLAND'       = [System.Drawing.Color]::FromArgb(78,96,66)
    'SAVANNA'         = [System.Drawing.Color]::FromArgb(190,173,89)
    'SHRUBLAND'       = [System.Drawing.Color]::FromArgb(137,147,91)
    'PLAINS'          = [System.Drawing.Color]::FromArgb(132,172,86)
    'DESERT'          = [System.Drawing.Color]::FromArgb(224,201,128)
}

$image = $null
$canvas = $null
$g = $null
$fonts = @()
$disposables = @()
try {
    $image = [System.Drawing.Bitmap]::FromFile($tempBmp)
    $panelWidth = [math]::Max(500, [int]($image.Width * 0.31))
    $canvas = [System.Drawing.Bitmap]::new($image.Width + $panelWidth, $image.Height)
    $g = [System.Drawing.Graphics]::FromImage($canvas)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

    $bg = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(22,26,32)); $disposables += $bg
    $panel = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(29,34,41)); $disposables += $panel
    $white = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(245,247,250)); $disposables += $white
    $muted = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(172,182,194)); $disposables += $muted
    $accent = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(185,213,220)); $disposables += $accent
    $divider = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(57,65,76), 2); $disposables += $divider
    $swatchPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(18,21,26), 1); $disposables += $swatchPen

    $titleFont = [System.Drawing.Font]::new('Segoe UI', 20, [System.Drawing.FontStyle]::Bold); $fonts += $titleFont
    $bigFont = [System.Drawing.Font]::new('Segoe UI', 25, [System.Drawing.FontStyle]::Bold); $fonts += $bigFont
    $headerFont = [System.Drawing.Font]::new('Segoe UI', 12, [System.Drawing.FontStyle]::Bold); $fonts += $headerFont
    $bodyFont = [System.Drawing.Font]::new('Segoe UI', 10); $fonts += $bodyFont
    $smallFont = [System.Drawing.Font]::new('Segoe UI', 9); $fonts += $smallFont

    $g.FillRectangle($bg, 0, 0, $canvas.Width, $canvas.Height)
    $g.DrawImage($image, 0, 0, $image.Width, $image.Height)
    $g.FillRectangle($panel, $image.Width, 0, $panelWidth, $image.Height)
    $g.DrawLine($divider, $image.Width, 0, $image.Width, $image.Height)

    $x = $image.Width + 28
    $right = $canvas.Width - 28
    $y = 24

    $g.DrawString('Beta 1.7.3 Biome Map', $titleFont, $white, $x, $y); $y += 43
    $g.DrawString("Seed $Seed", $smallFont, $muted, $x, $y); $y += 32

    $coverageText = ('{0:F6}%' -f $coverage)
    $g.DrawString($coverageText, $bigFont, $accent, $x, $y); $y += 42
    $g.DrawString("$biome coverage", $bodyFont, $muted, $x, $y); $y += 34

    $g.DrawString('STATS', $headerFont, $white, $x, $y); $y += 24
    $g.DrawLine($divider, $x, $y, $right, $y); $y += 12

    $stats = @(
        @('Target square', "$sideX x $sideZ blocks"),
        @('Matching blocks', ('{0:N0} / {1:N0}' -f $sameBiomeBlocks, $totalCells)),
        @('Wrong blocks', ('{0:N0}' -f $wrongBlocks)),
        @('Wrong area', ('{0:F6}%' -f $wrongPct)),
        @('Wrong density', $(if ($wrongBlocks -gt 0) { "1 per $oneWrongPer blocks" } else { 'NONE' })),
        @('Center', "($CenterX, $CenterZ)"),
        @('Render scale', "${Scale}x")
    )
    foreach ($row in $stats) {
        $g.DrawString($row[0], $bodyFont, $muted, $x, $y)
        $g.DrawString($row[1], $bodyFont, $white, $x + 155, $y)
        $y += 23
    }

    if ($wrongOffsets.Count -gt 0) {
        $y += 10
        $g.DrawString('FIRST WRONG OFFSETS', $headerFont, $white, $x, $y); $y += 24
        $g.DrawLine($divider, $x, $y, $right, $y); $y += 10
        $shown = [math]::Min(8, $wrongOffsets.Count)
        for ($i = 0; $i -lt $shown; $i += 2) {
            $leftText = $wrongOffsets[$i]
            $rightText = if ($i + 1 -lt $shown) { $wrongOffsets[$i + 1] } else { '' }
            $g.DrawString($leftText, $smallFont, $muted, $x, $y)
            if ($rightText) { $g.DrawString($rightText, $smallFont, $muted, $x + 145, $y) }
            $y += 20
        }
    }

    $y += 12
    $g.DrawString('BIOME COLORS', $headerFont, $white, $x, $y); $y += 24
    $g.DrawLine($divider, $x, $y, $right, $y); $y += 12

    foreach ($entry in $palette.GetEnumerator()) {
        $brush = [System.Drawing.SolidBrush]::new($entry.Value)
        try {
            $g.FillRectangle($brush, $x, $y + 1, 24, 20)
            $g.DrawRectangle($swatchPen, $x, $y + 1, 24, 20)
        } finally { $brush.Dispose() }
        $g.DrawString($entry.Key, $bodyFont, $white, $x + 36, $y)
        $y += 25
    }

    $y += 8
    $g.DrawString('White cross = (0,0). Dark lines = biome boundaries.', $smallFont, $muted, $x, $y)

    if (Test-Path $pngOutput -PathType Leaf) { Remove-Item $pngOutput -Force }
    $canvas.Save($pngOutput, [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    if ($g) { $g.Dispose() }
    if ($canvas) { $canvas.Dispose() }
    if ($image) { $image.Dispose() }
    foreach ($f in $fonts) { if ($f) { $f.Dispose() } }
    foreach ($d in $disposables) { if ($d) { $d.Dispose() } }
    Remove-Item $tempBmp -Force -ErrorAction SilentlyContinue
}

if (-not $NoOpen) { Start-Process $pngOutput | Out-Null }
Write-Host "PNG: $pngOutput" -ForegroundColor Green
