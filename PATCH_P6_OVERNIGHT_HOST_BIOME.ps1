$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$runner = Join-Path $PSScriptRoot 'RUN_FLOATING_ISLAND_P6_OVERNIGHT.ps1'
if (-not (Test-Path $runner -PathType Leaf)) { throw "Missing runner: $runner" }

$text = [System.IO.File]::ReadAllText($runner)
$marker = '# P6_HOST_BIOME_PATCH_V1'
if ($text.Contains($marker)) {
    Write-Host 'P6 overnight host-biome compile patch is already installed.'
    exit 0
}

$needle = '[System.IO.File]::WriteAllText($verifySource, $verifyText, [System.Text.UTF8Encoding]::new($false))'
if (-not $text.Contains($needle)) {
    throw 'Could not find verifier write point in RUN_FLOATING_ISLAND_P6_OVERNIGHT.ps1.'
}

$replacement = @'
# P6_HOST_BIOME_PATCH_V1
# HighestPillarSpawnGpuFinder exposes betaBiomeIsDesert as __device__ only.
# The P6 chunk verifier performs its origin gate on the CPU, so reproduce the
# exact same Beta biome-table test here as ordinary host C++ before compiling.
$hostBiomeOld = '    const bool desert = betaBiomeIsDesert(originTemp, originRain);'
$hostBiomeNew = @"
    int ti = static_cast<int>(originTemp * 63.0);
    int ri = static_cast<int>(originRain * 63.0);
    if (ti < 0) ti = 0; else if (ti > 63) ti = 63;
    if (ri < 0) ri = 0; else if (ri > 63) ri = 63;
    const float f = static_cast<float>(ti) / 63.0f;
    float wet = static_cast<float>(ri) / 63.0f;
    wet *= f;
    const bool desert = wet < 0.2f && f >= 0.95f;
"@
if (-not $verifyText.Contains($hostBiomeOld)) {
    throw 'Could not find host betaBiomeIsDesert call in downloaded P6 verifier.'
}
$verifyText = $verifyText.Replace($hostBiomeOld, $hostBiomeNew.TrimEnd())
[System.IO.File]::WriteAllText($verifySource, $verifyText, [System.Text.UTF8Encoding]::new($false))
'@

$text = $text.Replace($needle, $replacement.TrimEnd())
[System.IO.File]::WriteAllText($runner, $text, [System.Text.UTF8Encoding]::new($false))
Write-Host 'Installed P6 overnight host-biome compile patch.'
