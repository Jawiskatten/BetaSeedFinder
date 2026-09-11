param(
    [int]$Batch = 131072,
    [int]$ScreenPool = 96,
    [UInt64]$StartAttempt = 0,
    [UInt64]$MaxAttempts = 0,
    [UInt64]$Sequence = 0,
    [switch]$UseSequence,
    [switch]$Rebuild,
    [switch]$ContinueAfterHit,
    [int]$AuditScreenBatches = 0,
    [Nullable[long]]$VerifySeed = $null
)

$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run-tu4-water-finder.ps1'
if (-not (Test-Path $runner -PathType Leaf)) {
    throw "Base TU4 water runner not found: $runner"
}
if ($ScreenPool -lt 5) {
    throw '-ScreenPool must be at least 5 for the P17 fixed-top5 funnel.'
}

$params = @{
    Batch = $Batch
    TopExact = $ScreenPool
    StartAttempt = $StartAttempt
    MaxAttempts = $MaxAttempts
}
if ($UseSequence) {
    $params.UseSequence = $true
    $params.Sequence = $Sequence
}
if ($Rebuild) { $params.Rebuild = $true }
if ($ContinueAfterHit) { $params.ContinueAfterHit = $true }
if ($AuditScreenBatches -gt 0) { $params.AuditScreenBatches = $AuditScreenBatches }
if ($null -ne $VerifySeed) { $params.VerifySeed = $VerifySeed }

Write-Host "P17 targeted funnel: Batch=$Batch ScreenPool=$ScreenPool FullExact=5 AuditBatches=$AuditScreenBatches" -ForegroundColor Cyan
& $runner @params
exit $LASTEXITCODE
