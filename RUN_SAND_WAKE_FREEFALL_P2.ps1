param(
    [UInt64]$Count = 1000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(100000,100000000)][UInt64]$ScoutChunk = 1000000,
    [ValidateRange(256,32768)][int]$Batch = 32768,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,60)][int]$MinPotentialDrop = 5,
    [ValidateRange(1,60)][int]$MinDrop = 5,
    [switch]$Resume
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Reuse the proven P1 build/orchestration, but repin it to the corrected cave
# simulator and swap in the V2 full-stack exact oracle.
$BaseRef = 'f5b3ac69117a1031ce7da6f143ace39e325e14e4'
$FixedRef = '1ca046d361e5fc51e424a0ca08a3cc98229a4fd3'
$basePath = Join-Path $PSScriptRoot '.RUN_SAND_WAKE_FREEFALL_P2_BASE.ps1'

Invoke-WebRequest -UseBasicParsing `
    "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$BaseRef/RUN_SAND_WAKE_FREEFALL_P1.ps1" `
    -OutFile $basePath

$text = [IO.File]::ReadAllText($basePath)
$text = $text.Replace("`$BranchRef = '50e5139caa0a8892a830cc685a3f12779bac01aa'", "`$BranchRef = '$FixedRef'")
$text = $text.Replace('Beta173SandWakeFreefallOracle.java','Beta173SandWakeFreefallOracleV2.java')
$text = $text.Replace('net.minecraft.src.Beta173SandWakeFreefallOracle','net.minecraft.src.Beta173SandWakeFreefallOracleV2')
$text = $text.Replace('SAND-WAKE FREEFALL P1','SAND-WAKE FREEFALL P2')
$text = $text.Replace('sand_wake_freefall_p1','sand_wake_freefall_p2')
[IO.File]::WriteAllText($basePath,$text,[Text.UTF8Encoding]::new($false))

$invoke = @{
    Count = $Count
    StartIndex = $StartIndex
    SeedMode = $SeedMode
    ScoutChunk = $ScoutChunk
    Batch = $Batch
    TerrainThreads = $TerrainThreads
    MinPotentialDrop = $MinPotentialDrop
    MinDrop = $MinDrop
    Resume = [bool]$Resume
}
# PowerShell boxes Nullable[T] differently depending on whether a value was
# supplied. Under StrictMode, an omitted nullable can be plain $null and has no
# .HasValue property, so test against $null instead of dereferencing it.
if ($null -ne $RandomKey) { $invoke.RandomKey = [UInt64]$RandomKey }

& $basePath @invoke
exit $LASTEXITCODE
