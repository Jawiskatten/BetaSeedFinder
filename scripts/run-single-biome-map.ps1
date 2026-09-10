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

if ($Rebuild -or -not (Test-Path $exe -PathType Leaf)) {
    & (Join-Path $root 'scripts\build-single-biome-map.ps1') -ProjectRoot $root
}

$pngOutput = Join-Path $root ("single_biome_map_${Seed}_square_r${Target}.png")
$tempBmp = Join-Path $env:TEMP ("single_biome_map_{0}.bmp" -f [guid]::NewGuid().ToString('N'))
$tempPng = Join-Path $env:TEMP ("single_biome_map_{0}.png" -f [guid]::NewGuid().ToString('N'))

try {
    & $exe `
        --seed $Seed `
        --center-x $CenterX `
        --center-z $CenterZ `
        --target $Target `
        --margin $Margin `
        --scale $Scale `
        --output $tempBmp

    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    Add-Type -AssemblyName System.Drawing
    $image = $null
    try {
        $image = [System.Drawing.Image]::FromFile($tempBmp)
        $image.Save($tempPng, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        if ($null -ne $image) {
            $image.Dispose()
        }
    }

    # Replace the old PNG only after the new one was successfully created.
    if (Test-Path $pngOutput -PathType Leaf) {
        Remove-Item $pngOutput -Force
    }
    Move-Item $tempPng $pngOutput -Force

    if (-not $NoOpen) {
        Start-Process $pngOutput | Out-Null
    }

    Write-Host "PNG: $pngOutput" -ForegroundColor Green
}
finally {
    Remove-Item $tempBmp -Force -ErrorAction SilentlyContinue
    Remove-Item $tempPng -Force -ErrorAction SilentlyContinue
}
