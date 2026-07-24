param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$RepositoryName = 'BetaSeedFinder',
    [string]$Description = 'GPU-accelerated Minecraft Beta 1.7.3 floating-island seed finder'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path

function Test-GhAuthentication {
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & gh auth status 1>$null 2>$null
        return ($LASTEXITCODE -eq 0)
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Test-GhRepository {
    param([Parameter(Mandatory = $true)][string]$FullName)

    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & gh repo view $FullName 1>$null 2>$null
        return ($LASTEXITCODE -eq 0)
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $FailureMessage
    }
}

function Invoke-Gh {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    & gh @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $FailureMessage
    }
}

# Verify the working project, then publish only a freshly generated curated tree.
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
    if ($install -notmatch '^(?i)y(es)?$') {
        throw 'Publishing cancelled.'
    }

    & winget install --id GitHub.cli --exact --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) {
        throw 'GitHub CLI installation failed.'
    }

    $env:Path = (
        [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
        [Environment]::GetEnvironmentVariable('Path', 'User')
    )

    if (-not (Get-Command gh.exe -ErrorAction SilentlyContinue)) {
        throw 'GitHub CLI was installed but is not available in this terminal. Reopen the terminal and run again.'
    }
}

if (-not (Test-GhAuthentication)) {
    Write-Host 'Opening GitHub browser sign-in...'
    & gh auth login --hostname github.com --web --git-protocol https
    if ($LASTEXITCODE -ne 0) {
        throw 'GitHub authentication failed.'
    }
}

# Ensure Git HTTPS operations use the GitHub CLI credential helper.
& gh auth setup-git
if ($LASTEXITCODE -ne 0) {
    throw 'GitHub CLI could not configure Git authentication.'
}

$owner = (& gh api user --jq .login).Trim()
if ([string]::IsNullOrWhiteSpace($owner)) {
    throw 'Could not determine the authenticated GitHub account.'
}

$fullName = "$owner/$RepositoryName"
$packageDir = Join-Path $env:TEMP ("BetaSeedFinder-public-package-" + $PID)
$extractDir = Join-Path $env:TEMP ("BetaSeedFinder-public-tree-" + $PID)
$checkoutDir = Join-Path $env:TEMP ("BetaSeedFinder-public-checkout-" + $PID)

Remove-Item $packageDir, $extractDir, $checkoutDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $packageDir, $extractDir -Force | Out-Null

& (Join-Path $root 'scripts\Create-GithubSource.ps1') -ProjectRoot $root -Destination $packageDir

$zip = Get-ChildItem $packageDir -Filter 'BetaSeedFinder-*-source.zip' | Select-Object -First 1
if (-not $zip) {
    throw 'The curated source ZIP was not created.'
}

Expand-Archive -LiteralPath $zip.FullName -DestinationPath $extractDir -Force

$children = @(Get-ChildItem $extractDir -Force)
$publishRoot = if ($children.Count -eq 1 -and $children[0].PSIsContainer) {
    $children[0].FullName
}
else {
    $extractDir
}

& (Join-Path $publishRoot 'scripts\Verify-GithubSource.ps1') -ProjectRoot $publishRoot -Strict

$repoExists = Test-GhRepository -FullName $fullName

Write-Host
Write-Host "Account:       $owner"
Write-Host "Repository:    https://github.com/$fullName"
Write-Host "Visibility:    PUBLIC"
Write-Host "Repository:    $(if ($repoExists) { 'EXISTS - update mode' } else { 'NEW - create mode' })"
Write-Host "Curated tree:  $publishRoot"
Write-Host

$confirmText = if ($repoExists) {
    'Update the existing public repository now? [y/N]'
}
else {
    'Create and push this public repository now? [y/N]'
}

$confirm = Read-Host $confirmText
if ($confirm -notmatch '^(?i)y(es)?$') {
    throw 'Publishing cancelled.'
}

if (-not $repoExists) {
    Set-Location $publishRoot

    Invoke-Git -Arguments @('init') -FailureMessage 'Git initialization failed.'
    Invoke-Git -Arguments @('branch', '-M', 'main') -FailureMessage 'Could not select the main branch.'
    Invoke-Git -Arguments @('config', 'user.name', $owner) -FailureMessage 'Could not configure the Git user name.'
    Invoke-Git -Arguments @('config', 'user.email', "$owner@users.noreply.github.com") -FailureMessage 'Could not configure the Git email.'
    Invoke-Git -Arguments @('config', 'core.autocrlf', 'false') -FailureMessage 'Could not configure Git line endings.'
    Invoke-Git -Arguments @('add', '--all') -FailureMessage 'Git add failed.'
    Invoke-Git -Arguments @('commit', '-m', 'Public alpha source release') -FailureMessage 'Git commit failed.'

    Invoke-Gh `
        -Arguments @(
            'repo', 'create', $fullName,
            '--public',
            '--description', $Description,
            '--source', '.',
            '--remote', 'origin',
            '--push'
        ) `
        -FailureMessage 'GitHub repository creation or first push failed.'
}
else {
    # Clone the existing repository so its Git history is preserved.
    Invoke-Gh `
        -Arguments @('repo', 'clone', $fullName, $checkoutDir) `
        -FailureMessage 'Could not clone the existing GitHub repository.'

    # Mirror the new curated tree into the clone while preserving .git.
    & robocopy.exe $publishRoot $checkoutDir /MIR /XD .git /NFL /NDL /NJH /NJS /NP
    $robocopyExit = $LASTEXITCODE
    if ($robocopyExit -gt 7) {
        throw "Could not synchronize the curated source tree. Robocopy exit code: $robocopyExit"
    }

    Set-Location $checkoutDir

    Invoke-Git -Arguments @('config', 'user.name', $owner) -FailureMessage 'Could not configure the Git user name.'
    Invoke-Git -Arguments @('config', 'user.email', "$owner@users.noreply.github.com") -FailureMessage 'Could not configure the Git email.'
    Invoke-Git -Arguments @('config', 'core.autocrlf', 'false') -FailureMessage 'Could not configure Git line endings.'
    Invoke-Git -Arguments @('add', '--all') -FailureMessage 'Git add failed.'

    $status = (& git status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect the repository status.'
    }

    if ([string]::IsNullOrWhiteSpace(($status -join "`n"))) {
        Write-Host
        Write-Host 'The public repository is already up to date.' -ForegroundColor Green
    }
    else {
        Invoke-Git -Arguments @('commit', '-m', 'Update public source') -FailureMessage 'Git commit failed.'
        Invoke-Git -Arguments @('push', 'origin', 'main') -FailureMessage 'Push to the existing repository failed.'
    }
}

Invoke-Gh `
    -Arguments @(
        'repo', 'edit', $fullName,
        '--enable-issues=true',
        '--enable-wiki=false',
        '--description', $Description
    ) `
    -FailureMessage 'The repository was published, but its GitHub settings could not be updated.'

Write-Host
Write-Host "Published: https://github.com/$fullName" -ForegroundColor Green
Write-Host 'Open the Actions tab and confirm Source verification passes.'
