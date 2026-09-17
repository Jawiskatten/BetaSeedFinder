param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$p21Path = Join-Path $ProjectRoot 'scripts\patch-plains-component-p21-plains-seasonal-shape.ps1'
if (-not (Test-Path $p21Path -PathType Leaf)) {
    throw "P21 patch script not found: $p21Path"
}

$text = [System.IO.File]::ReadAllText($p21Path)
$changed = $false

# PowerShell uses the backtick, not backslash, to escape a double quote inside a
# double-quoted string. The original P21 script accidentally emitted C-style \"
# escaping in one regex replacement and therefore failed at parse time before
# any source modification could happen.
$bad = '$text = [regex]::Replace($text, $mismatchPattern, "`n    std::cout << \" center=\"", 1)'
$good = '$text = [regex]::Replace($text, $mismatchPattern, "`n    std::cout << `" center=`"", 1)'

if ($text.Contains($bad)) {
    $text = $text.Replace($bad, $good)
    $changed = $true
    Write-Host 'Fixed P21 PowerShell parser quoting.' -ForegroundColor Green
}
else {
    Write-Host 'P21 parser quoting is already fixed or uses a different form.' -ForegroundColor DarkGray
}

# The first P21 revision searched for the P18 coarse-score block with one exact
# multi-line string. Valid local P18 histories can have harmless whitespace or
# formatting differences, which made the patch abort even though the same three
# C++ statements were present. Rewrite that part of the P21 patch to use a
# whitespace-tolerant structural regex instead.
$scoreStart = $text.IndexOf('$oldScoutScore = @''')
$scoreEndNeedle = '$text = $text.Replace($oldScoutScore.TrimEnd(), $newScoutScore.TrimEnd())'
if ($scoreStart -ge 0) {
    $scoreEnd = $text.IndexOf($scoreEndNeedle, $scoreStart)
    if ($scoreEnd -lt 0) {
        throw 'Found old P21 coarse-score patch start but not its end.'
    }
    $scoreEnd += $scoreEndNeedle.Length

    $replacement = @'
$scoutScorePattern = '(?ms)^\s*int\s+bboxArea\s*=\s*0;\s*\r?\n\s*const\s+int\s+connectedSamples\s*=\s*p18LargestSampleComponent\(plainsMask,\s*bboxArea\);\s*\r?\n\s*const\s+int\s+coarseScore\s*=\s*connectedSamples\s*\*\s*128\s*\+\s*bboxArea;'
$scoutScoreRegex = [regex]::new($scoutScorePattern)
$scoutScoreMatches = $scoutScoreRegex.Matches($text)
if ($scoutScoreMatches.Count -ne 1) {
    $nearby = [regex]::Match($text, '(?s).{0,180}p18LargestSampleComponent\(plainsMask.*?.{0,180}')
    $hint = if ($nearby.Success) { $nearby.Value.Replace("`r", ' ').Replace("`n", ' ') } else { '<not found>' }
    throw "Expected exactly one structural P18 coarse-score block, found $($scoutScoreMatches.Count). Nearby source: $hint"
}
$newScoutScore = "        int shapeWeight = 0;`n        const int connectedSamples = p18LargestSampleComponent(plainsMask, shapeWeight);`n        const int coarseScore = connectedSamples * shapeWeight;"
$text = $scoutScoreRegex.Replace($text, $newScoutScore, 1)
'@

    $text = $text.Substring(0, $scoreStart) + $replacement.TrimEnd() + $text.Substring($scoreEnd)
    $changed = $true
    Write-Host 'Made P21 coarse-score matching whitespace-tolerant.' -ForegroundColor Green
}
else {
    Write-Host 'P21 coarse-score matching is already structural.' -ForegroundColor DarkGray
}

if ($changed) {
    [System.IO.File]::WriteAllText(
        $p21Path,
        $text,
        [System.Text.UTF8Encoding]::new($false)
    )
}

# Parse-check the whole P21 script now, so the runner fails here with a concise
# message instead of entering the patch with a wall of parser diagnostics.
$tokens = $null
$errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
    $p21Path,
    [ref]$tokens,
    [ref]$errors
)

if ($errors.Count -gt 0) {
    $details = ($errors | Select-Object -First 5 | ForEach-Object {
        "line $($_.Extent.StartLineNumber): $($_.Message)"
    }) -join '; '
    throw "P21 still has PowerShell parser error(s): $details"
}

Write-Host 'P21 PowerShell syntax parse-check passed.' -ForegroundColor Green
