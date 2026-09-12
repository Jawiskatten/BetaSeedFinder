$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root 'native\floating_island_spawn\FloatingIslandSpawnGpuFinderP3.cpp'
if (-not (Test-Path $source -PathType Leaf)) { throw "P3 source missing: $source" }

$text = [System.IO.File]::ReadAllText($source)
$oldStart = '    auto start = std::chrono::steady_clock::now();'
$newStart = "    const std::uint64_t sessionStartCompleted = completed;`r`n    auto start = std::chrono::steady_clock::now();"
$oldRate = 'const double rate = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;'
$newRate = 'const double rate = seconds > 0.0 ? static_cast<double>(completed - sessionStartCompleted) / seconds : 0.0;'

if ($text.Contains($oldStart) -and -not $text.Contains('sessionStartCompleted')) {
    $text = $text.Replace($oldStart, $newStart)
}
if ($text.Contains($oldRate)) {
    $text = $text.Replace($oldRate, $newRate)
}
if (-not $text.Contains('completed - sessionStartCompleted')) {
    throw 'Could not patch resumed rate calculation.'
}

[System.IO.File]::WriteAllText($source, $text, [System.Text.UTF8Encoding]::new($false))
Write-Host 'P3 resume rate display fixed. This changes display only, not searched seeds/results.'
Write-Host 'It takes effect the next time the worker is compiled/restarted.'
