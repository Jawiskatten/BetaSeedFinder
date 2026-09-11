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

$text = [System.IO.File]::ReadAllText($sourcePath)

if (-not $text.Contains('TU4_WATER_P3_JUMP_SCOUT')) {
    throw 'P3b requires TU4 Water P3 jump scout first.'
}

$broken = '<< " scoutBestD7=" << std::fixed << std::setprecision(3) << globalScoutSum\n                          << " scoutLow=" << globalScoutLow << "/32";'
$fixed = @'
<< " scoutBestD7=" << std::fixed << std::setprecision(3) << globalScoutSum
                          << " scoutLow=" << globalScoutLow << "/32";
'@.TrimEnd()

$count = ([regex]::Matches($text, [regex]::Escape($broken))).Count
if ($count -eq 0) {
    if ($text.Contains($fixed)) {
        Write-Host 'TU4 Water P3 build fix is already applied.' -ForegroundColor Green
        exit 0
    }
    throw 'Could not find the P3 literal-newline status expression to repair.'
}
if ($count -ne 1) {
    throw "Expected exactly one broken P3 status expression, found $count."
}

$text = $text.Replace($broken, $fixed)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P3b build fix.' -ForegroundColor Green
Write-Host 'Replaced the accidental literal \\n in the C++ stream expression with a real newline.'
Write-Host 'Search logic, scout ranking, RNG jump, and exact water measurement are unchanged.'
