param(
    [ValidateSet('Java','AMD','NVIDIA','All')]
    [string]$Target = 'Java'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $PSScriptRoot).Path

& (Join-Path $root 'scripts\verify.ps1') -ProjectRoot $root
& (Join-Path $root 'scripts\build-java.ps1') -ProjectRoot $root

if ($Target -in @('AMD','All')) {
    & (Join-Path $root 'scripts\build-native.ps1') -ProjectRoot $root -Backend AMD
}
if ($Target -in @('NVIDIA','All')) {
    & (Join-Path $root 'scripts\build-native.ps1') -ProjectRoot $root -Backend NVIDIA
}

Write-Host
Write-Host "Build complete: $Target" -ForegroundColor Green
