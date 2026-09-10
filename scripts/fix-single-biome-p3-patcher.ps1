param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$p3Path = Join-Path $ProjectRoot 'scripts\patch-single-biome-tundra-p3.ps1'
if (-not (Test-Path $p3Path -PathType Leaf)) {
    throw "P3 patcher not found: $p3Path"
}

$text = [System.IO.File]::ReadAllText($p3Path)

$old = '$kernelPattern = ''(?s)__global__ void searchKernel\(.*?\n\}\n\n__global__ void exactKernel\('''
$new = '$kernelPattern = ''(?s)__global__ void searchKernel\(.*?\r?\n\}\r?\n\r?\n__global__ void exactKernel\('''

if ($text.Contains($old)) {
    $text = $text.Replace($old, $new)
    [System.IO.File]::WriteAllText($p3Path, $text, [System.Text.UTF8Encoding]::new($false))
    Write-Host 'Fixed P3 patcher for Windows CRLF source files.' -ForegroundColor Green
} elseif ($text.Contains($new)) {
    Write-Host 'P3 patcher CRLF fix is already applied.' -ForegroundColor Green
} else {
    throw 'Could not find the expected P3 kernel regex line to fix.'
}

& $p3Path -ProjectRoot $ProjectRoot
