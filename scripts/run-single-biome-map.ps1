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
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build\tools\SingleBiomeMap.exe'
if ($Rebuild -or -not (Test-Path $exe)) { & (Join-Path $root 'scripts\build-single-biome-map.ps1') -ProjectRoot $root }
$output = Join-Path $root ("single_biome_map_${Seed}_square_r${Target}.bmp")
& $exe --seed $Seed --center-x $CenterX --center-z $CenterZ --target $Target --margin $Margin --scale $Scale --output $output
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $NoOpen) { Start-Process $output | Out-Null }
Write-Host "Opened: $output"
