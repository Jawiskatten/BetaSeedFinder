param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$RepositoryName = 'BetaSeedFinder',
    [string]$Description = 'GPU-accelerated Minecraft Beta 1.7.3 floating-island seed finder'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path

# Verify the source components in the working project, then publish only a
# freshly generated curated tree. Local output/config/binaries are never pushed.
& (Join-Path $root 'scripts\Verify-GithubSource.ps1') -ProjectRoot $root

if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
    throw 'Git was not found. Install Git for Windows, then run this script again.'
}

if (-not (Get-Command gh.exe -ErrorAction SilentlyContinue)) {
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) {
        throw 'GitHub CLI (gh) was not found. Install it from https://cli.github.com/ and run again.'
    }
    $install = Read-Host 'GitHub CLI is missing. Install it now with winget? [y/N]'
    if ($install -notmatch '^(?i)y(es)?$') { throw 'Publishing cancelled.' }
    & winget install --id GitHub.cli --exact --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { throw 'GitHub CLI installation failed.' }
    $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
    if (-not (Get-Command gh.exe -ErrorAction SilentlyContinue)) {
        throw 'GitHub CLI was installed but is not available in this terminal. Reopen the terminal and run again.'
    }
}

& gh auth status 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Opening GitHub browser sign-in...'
    & gh auth login --web --git-protocol https
    if ($LASTEXITCODE -ne 0) { throw 'GitHub authentication failed.' }
}

$owner = (& gh api user --jq .login).Trim()
if ([string]::IsNullOrWhiteSpace($owner)) { throw 'Could not determine the authenticated GitHub account.' }
$fullName = "$owner/$RepositoryName"

$packageDir = Join-Path $env:TEMP ("BetaSeedFinder-public-package-" + $PID)
$extractDir = Join-Path $env:TEMP ("BetaSeedFinder-public-repo-" + $PID)
Remove-Item $packageDir,$extractDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $packageDir,$extractDir -Force | Out-Null

& (Join-Path $root 'scripts\Create-GithubSource.ps1') -ProjectRoot $root -Destination $packageDir
$zip = Get-ChildItem $packageDir -Filter 'BetaSeedFinder-*-source.zip' | Select-Object -First 1
if (-not $zip) { throw 'The curated source ZIP was not created.' }
Expand-Archive -LiteralPath $zip.FullName -DestinationPath $extractDir -Force
$children = @(Get-ChildItem $extractDir -Force)
$publishRoot = if ($children.Count -eq 1 -and $children[0].PSIsContainer) { $children[0].FullName } else { $extractDir }

& (Join-Path $publishRoot 'scripts\Verify-GithubSource.ps1') -ProjectRoot $publishRoot -Strict
Set-Location $publishRoot

Write-Host
Write-Host "Account:     $owner"
Write-Host "Repository:  https://github.com/$fullName"
Write-Host "Visibility:  PUBLIC"
Write-Host "Curated tree: $publishRoot"
Write-Host
$confirm = Read-Host 'Create/push this public repository now? [y/N]'
if ($confirm -notmatch '^(?i)y(es)?$') { throw 'Publishing cancelled.' }

& git init
& git branch -M main
& git config user.name $owner
& git config user.email "$owner@users.noreply.github.com"
& git add --all
& git commit -m "Public alpha source release"
if ($LASTEXITCODE -ne 0) { throw 'Git commit failed.' }

& gh repo view $fullName 2>$null
$exists = $LASTEXITCODE -eq 0
if (-not $exists) {
    & gh repo create $fullName --public --description $Description --source . --remote origin --push
    if ($LASTEXITCODE -ne 0) { throw 'GitHub repository creation or first push failed.' }
} else {
    & git remote add origin "https://github.com/$fullName.git"
    & git push -u origin main
    if ($LASTEXITCODE -ne 0) { throw 'Push to the existing repository failed.' }
}

& gh repo edit $fullName --enable-issues=true --enable-wiki=false --description $Description
Write-Host
Write-Host "Published: https://github.com/$fullName" -ForegroundColor Green
Write-Host 'Open the Actions tab and confirm Source verification passes.'
Write-Host "Temporary curated checkout: $publishRoot"
