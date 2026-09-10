param([string]$ProjectRoot = "")
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$root = (Resolve-Path $ProjectRoot).Path
$source = Join-Path $root 'native\src\single_biome_map.cpp'
$outDir = Join-Path $root 'build\tools'
$exe = Join-Path $outDir 'SingleBiomeMap.exe'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio C++ Build Tools not found.' }
    $install = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if (-not $install) { throw 'Visual Studio C++ Build Tools not found.' }
    $devCmd = Join-Path $install 'Common7\Tools\VsDevCmd.bat'
    $tmp = Join-Path $env:TEMP ('biomemap-vsenv-' + [guid]::NewGuid().ToString('N') + '.cmd')
    @('@echo off', ('call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul' -f $devCmd), 'set') | Set-Content $tmp -Encoding ASCII
    try { $envLines = & $tmp } finally { Remove-Item $tmp -Force -ErrorAction SilentlyContinue }
    foreach ($line in $envLines) {
        if ($line -is [string]) {
            $i = $line.IndexOf('=')
            if ($i -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0,$i), $line.Substring($i+1), 'Process') }
        }
    }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe unavailable.' }

& cl.exe /nologo /std:c++17 /O2 /EHsc ('/I' + (Join-Path $root 'native\src')) $source ('/Fe:' + $exe)
if ($LASTEXITCODE -ne 0) { throw "Map build failed with exit code $LASTEXITCODE" }
Write-Host "Created: $exe" -ForegroundColor Green
