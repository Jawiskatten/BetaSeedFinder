param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

Write-Host 'P21 self-patcher retired: the Plains+Seasonal shape objective is generated directly by P22.' -ForegroundColor DarkGray
Write-Host 'Use scripts\run-plains-component-finder.ps1; no P21 parser repair is required.' -ForegroundColor DarkGray
