param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'
$target = Join-Path $PSScriptRoot 'patch-tu4-water-p13b-screen-recall-audit.ps1'
if (-not (Test-Path $target -PathType Leaf)) {
    throw "P13b audit patcher not found: $target"
}
& $target -ProjectRoot $ProjectRoot
