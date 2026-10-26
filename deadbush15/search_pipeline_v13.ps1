param(
    [UInt64]$Start = 0,
    [UInt64]$Count = 1000000,
    [int]$Locations = 4,
    [int]$SpreadSpan = 1000000,
    [int]$Report = 14,
    [int]$Prefilter = 14,
    [string]$Prefix = 'v13_search',
    [switch]$Resume
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$scanner = Join-Path $PSScriptRoot 'deadbush15_v13.exe'
$filter = Join-Path $PSScriptRoot 'deadbush15_v12.exe'
if (!(Test-Path $scanner) -or !(Test-Path $filter)) {
    throw 'Build first with build_v13.ps1 and build_v12.ps1.'
}
$gpuCsv = "$Prefix.gpu.csv"
$gpuProgress = "$Prefix.gpu.progress"
$outCsv = "$Prefix.badlands_rng.csv"
$outProgress = "$outCsv.meta"
if (!$Resume -and ((Test-Path $gpuCsv) -or (Test-Path $gpuProgress) -or (Test-Path $outCsv) -or (Test-Path $outProgress))) {
    throw 'Output already exists. Choose another -Prefix or pass -Resume.'
}

$gpuArgs = @('--gpu-search','--start',"$Start",'--count',"$Count",'--locations',"$Locations",
    '--spread-span',"$SpreadSpan",'--report',"$Report",'--prefilter',"$Prefilter",
    '--target','15','--mode','xz','--output',$gpuCsv,'--checkpoint',$gpuProgress)
if ($Resume) { $gpuArgs += '--resume' }
& $scanner @gpuArgs
if ($LASTEXITCODE -ne 0) { throw "GPU scan failed with exit code $LASTEXITCODE" }

$filterArgs = @('--input',$gpuCsv,'--output',$outCsv,'--index','75')
if ($Resume) { $filterArgs += '--resume' }
& $filter @filterArgs
if ($LASTEXITCODE -ne 0) { throw "V12 survivor filter failed with exit code $LASTEXITCODE" }
Write-Host "Saved GPU proposals to $gpuCsv and Badlands/RNG survivors to $outCsv. Survivors are not terrain verified."
