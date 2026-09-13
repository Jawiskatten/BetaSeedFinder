param(
    [Int64]$Seed = -3405360075020439777,
    [string]$WorldPath = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$tools = Join-Path $root 'tools'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
$script = Join-Path $tools 'diagnose_beta173_actual_save.py'
$url = 'https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/2cc72e323dcd6ac6dd36fc5a7df1f27da914ef30/tools/diagnose_beta173_actual_save.py'

Write-Host 'Downloading actual-save diagnostic...'
Invoke-WebRequest -UseBasicParsing $url -OutFile $script

$argsList = @($script, '--seed', $Seed.ToString([Globalization.CultureInfo]::InvariantCulture))
if (-not [string]::IsNullOrWhiteSpace($WorldPath)) {
    $argsList += @('--world', $WorldPath)
}

$py = Get-Command py -ErrorAction SilentlyContinue
if ($py) {
    & $py.Source -3 @argsList
    if ($LASTEXITCODE -ne 0) { throw "Actual-save diagnostic failed with exit code $LASTEXITCODE" }
    exit 0
}

$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & $python.Source @argsList
    if ($LASTEXITCODE -ne 0) { throw "Actual-save diagnostic failed with exit code $LASTEXITCODE" }
    exit 0
}

throw 'Python was not found. Install Python or run the tools/diagnose_beta173_actual_save.py script with an available Python 3 interpreter.'
