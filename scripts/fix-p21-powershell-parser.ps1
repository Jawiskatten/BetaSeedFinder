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

# PowerShell uses the backtick, not backslash, to escape a double quote inside a
# double-quoted string. The original P21 script accidentally emitted C-style \"
# escaping in one regex replacement and therefore failed at parse time before
# any source modification could happen.
$bad = '$text = [regex]::Replace($text, $mismatchPattern, "`n    std::cout << \" center=\"", 1)'
$good = '$text = [regex]::Replace($text, $mismatchPattern, "`n    std::cout << `" center=`"", 1)'

if ($text.Contains($bad)) {
    $text = $text.Replace($bad, $good)
    [System.IO.File]::WriteAllText(
        $p21Path,
        $text,
        [System.Text.UTF8Encoding]::new($false)
    )
    Write-Host 'Fixed P21 PowerShell parser quoting.' -ForegroundColor Green
}
else {
    Write-Host 'P21 parser quoting is already fixed or uses a different form.' -ForegroundColor DarkGray
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
