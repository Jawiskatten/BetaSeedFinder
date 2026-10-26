param(
    [UInt64]$Start = 0,
    [UInt64]$Count = 10000,
    [int]$Locations = 4,
    [int]$SpreadSpan = 1000000,
    [int]$Report = 24,
    [int]$Prefilter = 24,
    [string]$Prefix = 'v14_search',
    [switch]$Resume
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$scanner = Join-Path $PSScriptRoot 'deadbush15_v14.exe'
$filter = Join-Path $PSScriptRoot 'deadbush15_v12.exe'
$engineClasses = Join-Path $PSScriptRoot '.v14_runtime\classes'
$cpFile = Join-Path $PSScriptRoot '.v14_runtime\classpath.txt'
if (!(Test-Path $scanner) -or !(Test-Path $filter) -or !(Test-Path $cpFile)) {
    throw 'Build first with build_v14.ps1 and build_v12.ps1.'
}
$gpuCsv = "$Prefix.gpu.csv"
$gpuProgress = "$Prefix.gpu.progress"
$survivorCsv = "$Prefix.badlands_rng.csv"
$terrainCsv = "$Prefix.vanilla_base_columns.csv"
$javaHome = 'C:\Program Files\Microsoft\jdk-25.0.4.101-hotspot'
$classpath = [IO.File]::ReadAllText($cpFile)
if (!$Resume -and ((Test-Path $gpuCsv) -or (Test-Path $gpuProgress) -or (Test-Path $survivorCsv))) {
    throw 'Output already exists. Choose another -Prefix or pass -Resume.'
}

$gpuArgs = @('--gpu-search','--start',"$Start",'--count',"$Count",'--locations',"$Locations",
    '--spread-span',"$SpreadSpan",'--report',"$Report",'--prefilter',"$Prefilter",
    '--target','15','--index','75','--mode','xz','--output',$gpuCsv,'--checkpoint',$gpuProgress)
if ($Resume) { $gpuArgs += '--resume' }
& $scanner @gpuArgs
if ($LASTEXITCODE -ne 0) { throw "GPU search failed with exit code $LASTEXITCODE" }

$filterArgs = @('--input',$gpuCsv,'--output',$survivorCsv,'--index','75','--rng-only')
if ($Resume) { $filterArgs += '--resume' }
& $filter @filterArgs
if ($LASTEXITCODE -ne 0) { throw "Badlands/RNG filter failed with exit code $LASTEXITCODE" }

$javaOut = "$terrainCsv.$([guid]::NewGuid().ToString('N')).stdout.log"
$javaErr = "$terrainCsv.$([guid]::NewGuid().ToString('N')).stderr.log"
$engine = Start-Process -FilePath (Join-Path $javaHome 'bin\java.exe') -NoNewWindow -Wait -PassThru `
    -ArgumentList @('-cp',"$engineClasses;$classpath",'VanillaTerrainEngine','--annotate',$survivorCsv,$terrainCsv) `
    -RedirectStandardOutput $javaOut -RedirectStandardError $javaErr
if ($engine.ExitCode -ne 0) { Get-Content $javaOut,$javaErr -ErrorAction SilentlyContinue; throw "Vanilla terrain-column pass failed with exit code $($engine.ExitCode)" }
Get-Content $javaOut | ForEach-Object { Write-Host $_ }
Write-Host "Saved proposals: $gpuCsv"
Write-Host "Badlands/RNG survivors: $survivorCsv"
Write-Host "Exact vanilla noise base-column annotations: $terrainCsv (not post-structure bush verification)"
