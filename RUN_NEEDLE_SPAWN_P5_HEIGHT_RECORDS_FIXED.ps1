param(
    [UInt64]$Count = 10000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(256,32768)][int]$Batch = 32768,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(63,127)][int]$MinTopY = 70,
    [ValidateRange(25,10000)][int]$Top = 5000
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = $PSScriptRoot
$baseCommit = '329c0c12fb33bd9e2839d4e4d6cfb9264ff614dd'
$raw = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$baseCommit/RUN_NEEDLE_SPAWN_P5_HEIGHT_RECORDS.ps1"
$original = Join-Path $root 'RUN_NEEDLE_SPAWN_P5_HEIGHT_RECORDS.ps1'
if (-not (Test-Path $original -PathType Leaf)) {
    Invoke-WebRequest -UseBasicParsing $raw -OutFile $original
}

$text = [IO.File]::ReadAllText($original)
$old = '$nativeSourceDir=Join-Path $p1build ''source\native'''
$new = '$nativeSourceDir=Get-BetaGpuNativeSourceDir $root'
if (-not $text.Contains($old)) {
    if (-not $text.Contains($new)) {
        throw 'Could not locate the P5 nativeSourceDir line to patch.'
    }
} else {
    $text = $text.Replace($old, $new)
}

$patched = Join-Path $root 'RUN_NEEDLE_SPAWN_P5_HEIGHT_RECORDS_LOCALFIX.ps1'
[IO.File]::WriteAllText($patched, $text, [Text.UTF8Encoding]::new($false))

$invoke = @{
    Count = $Count
    StartIndex = $StartIndex
    SeedMode = $SeedMode
    Batch = $Batch
    TerrainThreads = $TerrainThreads
    MinTopY = $MinTopY
    Top = $Top
}
if ($null -ne $RandomKey) { $invoke.RandomKey = [UInt64]$RandomKey }

Write-Host 'P5 include-path fix active: using native\src for gpu_runtime_compat.hpp and the other shared GPU headers.'
& $patched @invoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
