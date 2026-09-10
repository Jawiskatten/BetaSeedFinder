param([string]$ProjectRoot = "")
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$path = Join-Path $ProjectRoot 'native\src\single_biome_map.cpp'
if (-not (Test-Path $path)) { throw "Map source not found: $path" }
$text = [IO.File]::ReadAllText($path)
if ($text.Contains('EXACT_864_MAP_V1')) { Write-Host 'Map is already exact 864x864.'; exit 0 }

if (-not $text.Contains('#include <algorithm>')) {
    $text = $text.Replace('#include <cmath>', "#include <algorithm>`n#include <cmath>")
}

$oldSize = @'
    const int rr = o.target + o.margin;
    const int width = rr * 2 + 1, height = width;
'@
$newSize = @'
    // EXACT_864_MAP_V1
    // target=432 => exact 864x864 target, offsets [-432,+431].
    const int rr = o.target + o.margin;
    const int width = rr * 2, height = width;
'@
if (-not $text.Contains($oldSize)) { throw 'Could not find map size block.' }
$text = $text.Replace($oldSize, $newSize)

$text = $text.Replace('        const int dz = rr - py;', '        const int dz = rr - 1 - py;')
$text = $text.Replace('            if (std::abs(dx) <= o.target && std::abs(dz) <= o.target) {', '            if (dx >= -o.target && dx < o.target && dz >= -o.target && dz < o.target) {')
$text = $text.Replace('    const int cx = rr, cy = rr;', '    const int cx = rr, cy = rr - 1;')

$oldLabel = @'
              << " square=[" << -o.target << ",+" << o.target << "] on X/Z"
              << " size=" << (o.target * 2 + 1) << 'x' << (o.target * 2 + 1) << '\n'
'@
$newLabel = @'
              << " square=[" << -o.target << ",+" << (o.target - 1) << "] on X/Z"
              << " size=" << (o.target * 2) << 'x' << (o.target * 2) << '\n'
'@
if (-not $text.Contains($oldLabel)) { throw 'Could not find map output label block.' }
$text = $text.Replace($oldLabel, $newLabel)

if (-not $text.Contains('EXACT_864_MAP_V1')) { throw 'Could not patch map source.' }
[IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
Write-Host 'Patched map to exact 864x864 target.'
Write-Host 'target=432 => offsets -432..+431 on both X and Z.'
Write-Host 'Total target positions: 746496.'
