param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $root 'build\java'
}

$javac = Get-Command javac.exe -ErrorAction SilentlyContinue
if (-not $javac) { $javac = Get-Command javac -ErrorAction SilentlyContinue }
$jar = Get-Command jar.exe -ErrorAction SilentlyContinue
if (-not $jar) { $jar = Get-Command jar -ErrorAction SilentlyContinue }
if (-not $javac -or -not $jar) {
    throw 'JDK 17 or newer was not found. javac and jar must be available in PATH.'
}

$sourceRoot = Join-Path $root 'src'
$manifestPath = Join-Path $root 'scripts\AppSourceFiles.txt'
$version = (Get-Content (Join-Path $root 'VERSION.txt') -Raw).Trim()
$classes = Join-Path $OutputRoot 'classes'
$jarPath = Join-Path $OutputRoot 'BetaSeedFinder.jar'
$sourceArgs = Join-Path $OutputRoot 'javac-sources.txt'
$jarManifest = Join-Path $OutputRoot 'MANIFEST.MF'

Remove-Item $classes -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes | Out-Null

$sources = Get-Content $manifestPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | ForEach-Object {
    $path = Join-Path $sourceRoot $_.Replace('/', '\')
    if (-not (Test-Path $path -PathType Leaf)) { throw "Java source missing: $_" }
    $path
}

# Java argument files treat backslashes as escape characters. Convert absolute
# Windows paths to forward slashes so C:\Users\... is not parsed as C:Users...
$sourceLines = @($sources | ForEach-Object {
    $absolute = [IO.Path]::GetFullPath($_).Replace('\', '/')
    '"' + $absolute + '"'
})
[IO.File]::WriteAllLines($sourceArgs, $sourceLines, [Text.UTF8Encoding]::new($false))
& $javac.Source -encoding UTF-8 --release 17 -d $classes "@$sourceArgs"
if ($LASTEXITCODE -ne 0) { throw "javac failed with exit code $LASTEXITCODE" }

@(
    'Manifest-Version: 1.0',
    'Main-Class: GuiMain',
    "Implementation-Version: $version",
    ''
) | Set-Content $jarManifest -Encoding ASCII

Remove-Item $jarPath -Force -ErrorAction SilentlyContinue
& $jar.Source cfm $jarPath $jarManifest -C $classes .
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $jarPath)) { throw 'JAR creation failed.' }

Write-Host "Compiled $($sources.Count) production Java files."
Write-Host "Created: $jarPath" -ForegroundColor Green
return $jarPath
