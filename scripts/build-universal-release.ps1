param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$RepositoryName = 'BetaSeedFinder',
    [string]$OutputDirectory = '',
    [switch]$Publish
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'release'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

function Invoke-Gh(
    [Parameter(Mandatory = $true)][string[]]$Arguments,
    [Parameter(Mandatory = $true)][string]$FailureMessage
) {
    & gh @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $FailureMessage
    }
}

function Test-GhCommand([string[]]$Arguments) {
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & gh @Arguments 1>$null 2>$null
        return ($LASTEXITCODE -eq 0)
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
}

function Get-HipSearchDirectories {
    $directories = New-Object Collections.Generic.List[string]

    foreach ($value in @($env:HIP_PATH, $env:HIP_SDK_DIR)) {
        if (-not [string]::IsNullOrWhiteSpace($value)) {
            foreach ($relative in @('bin', 'lib\llvm\bin')) {
                $candidate = Join-Path $value $relative
                if (Test-Path $candidate -PathType Container) {
                    $directories.Add((Resolve-Path $candidate).Path)
                }
            }
        }
    }

    $base = 'C:\Program Files\AMD\ROCm'
    if (Test-Path $base -PathType Container) {
        foreach ($versionDirectory in (
            Get-ChildItem $base -Directory | Sort-Object Name -Descending
        )) {
            foreach ($relative in @('bin', 'lib\llvm\bin')) {
                $candidate = Join-Path $versionDirectory.FullName $relative
                if (Test-Path $candidate -PathType Container) {
                    $directories.Add($candidate)
                }
            }
        }
    }

    return @($directories | Select-Object -Unique)
}

function Test-SystemDependency([string]$Name) {
    $lower = $Name.ToLowerInvariant()

    if ($lower -like 'api-ms-win-*.dll' -or $lower -like 'ext-ms-win-*.dll') {
        return $true
    }

    return $lower -in @(
        'kernel32.dll','kernelbase.dll','ntdll.dll','user32.dll',
        'advapi32.dll','shell32.dll','ole32.dll','oleaut32.dll',
        'gdi32.dll','gdi32full.dll','comdlg32.dll','combase.dll',
        'ws2_32.dll','bcrypt.dll','bcryptprimitives.dll','crypt32.dll',
        'rpcrt4.dll','version.dll','cfgmgr32.dll','setupapi.dll',
        'winmm.dll','psapi.dll','imm32.dll','shlwapi.dll','sechost.dll',
        'wintrust.dll','wtsapi32.dll','iphlpapi.dll','dnsapi.dll',
        'powrprof.dll','userenv.dll','dwmapi.dll','msvcrt.dll','ucrtbase.dll'
    )
}

function Get-PeDependencies(
    [Parameter(Mandatory = $true)][string]$Binary,
    [Parameter(Mandatory = $true)][string]$Dumpbin
) {
    $output = & $Dumpbin /nologo /dependents $Binary 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin could not inspect $Binary"
    }

    return @(
        $output |
            ForEach-Object { $_.Trim() } |
            Where-Object {
                $_ -match '^[A-Za-z0-9][A-Za-z0-9_.-]*\.dll$'
            } |
            Sort-Object -Unique
    )
}

function Find-DependencyFile(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string[]]$SearchDirectories
) {
    foreach ($directory in $SearchDirectories) {
        if ([string]::IsNullOrWhiteSpace($directory)) { continue }
        $candidate = Join-Path $directory $Name
        if (Test-Path $candidate -PathType Leaf) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
}

function New-PortableWorkerBundle(
    [Parameter(Mandatory = $true)][string]$Worker,
    [Parameter(Mandatory = $true)][string]$Destination,
    [string[]]$PreferredSearchDirectories = @()
) {
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    $targetWorker = Join-Path $Destination 'BetaSeedFinderWorker.exe'
    Copy-Item -LiteralPath $Worker -Destination $targetWorker -Force

    $dumpbinCommand = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbinCommand) {
        throw 'dumpbin.exe is unavailable after the AMD build initialized Visual Studio.'
    }

    $searchDirectories = New-Object Collections.Generic.List[string]
    $searchDirectories.Add((Split-Path $Worker -Parent))

    foreach ($directory in $PreferredSearchDirectories) {
        if (Test-Path $directory -PathType Container) {
            $searchDirectories.Add((Resolve-Path $directory).Path)
        }
    }

    foreach ($directory in ($env:Path -split ';')) {
        if (-not [string]::IsNullOrWhiteSpace($directory) -and
                (Test-Path $directory -PathType Container)) {
            $searchDirectories.Add($directory)
        }
    }

    $system32 = Join-Path $env:WINDIR 'System32'
    if (Test-Path $system32 -PathType Container) {
        $searchDirectories.Add($system32)
    }

    $search = @($searchDirectories | Select-Object -Unique)
    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($targetWorker)
    $inspected = @{}
    $copied = New-Object Collections.Generic.List[string]

    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        $currentKey = $current.ToLowerInvariant()
        if ($inspected.ContainsKey($currentKey)) { continue }
        $inspected[$currentKey] = $true

        foreach ($dependency in (
            Get-PeDependencies -Binary $current -Dumpbin $dumpbinCommand.Source
        )) {
            if (Test-SystemDependency $dependency) { continue }

            $target = Join-Path $Destination $dependency
            if (Test-Path $target -PathType Leaf) {
                if (-not $inspected.ContainsKey($target.ToLowerInvariant())) {
                    $queue.Enqueue($target)
                }
                continue
            }

            $source = Find-DependencyFile `
                -Name $dependency `
                -SearchDirectories $search

            if (-not $source) {
                Write-Warning "Could not bundle dependency $dependency for $Worker"
                continue
            }

            Copy-Item -LiteralPath $source -Destination $target -Force
            $copied.Add($dependency)
            $queue.Enqueue($target)
        }
    }

    Write-Host "Portable worker: $targetWorker"
    if ($copied.Count -gt 0) {
        Write-Host ('Bundled DLLs: ' + (($copied | Sort-Object -Unique) -join ', '))
    } else {
        Write-Host 'Bundled DLLs: none required'
    }

    return $targetWorker
}

function Expand-NestedArchives([string]$Root) {
    $expanded = @{}

    for ($round = 0; $round -lt 4; $round++) {
        $archives = @(
            Get-ChildItem $Root -Recurse -File -Filter '*.zip' |
                Where-Object { -not $expanded.ContainsKey($_.FullName) }
        )

        if ($archives.Count -eq 0) { break }

        foreach ($archive in $archives) {
            $expanded[$archive.FullName] = $true
            $destination = Join-Path $archive.DirectoryName (
                'expanded-' + $archive.BaseName + '-' + $round
            )
            New-Item -ItemType Directory -Force -Path $destination | Out-Null
            Expand-Archive -LiteralPath $archive.FullName `
                -DestinationPath $destination `
                -Force
        }
    }
}

function Find-NvidiaWorker([string]$Root) {
    $candidates = @(
        Get-ChildItem $Root -Recurse -File -Filter 'BetaSeedFinderWorker.exe'
    )
    if ($candidates.Count -eq 0) { return $null }

    $backendCandidate = $candidates | Where-Object {
        $_.FullName -match '[\\/]backend[\\/]nvidia[\\/]'
    } | Select-Object -First 1

    if ($backendCandidate) { return $backendCandidate.FullName }
    return $candidates[0].FullName
}

function Download-LatestNvidiaWorker(
    [Parameter(Mandatory = $true)][string]$Repository,
    [Parameter(Mandatory = $true)][string]$Destination
) {
    $json = & gh run list `
        --repo $Repository `
        --workflow windows-nvidia.yml `
        --branch main `
        --status success `
        --limit 20 `
        --json databaseId,createdAt,headBranch,headSha

    if ($LASTEXITCODE -ne 0) {
        throw 'Could not list successful NVIDIA workflow runs.'
    }

    $runs = @($json | ConvertFrom-Json)
    if ($runs.Count -eq 0) {
        throw 'No successful NVIDIA workflow run was found.'
    }

    $artifactNames = @(
        'BetaSeedFinder-NVIDIA-Worker',
        'BetaSeedFinder-Windows-NVIDIA'
    )

    foreach ($run in $runs) {
        foreach ($artifactName in $artifactNames) {
            $attempt = Join-Path $Destination (
                'run-' + $run.databaseId + '-' + $artifactName
            )
            Remove-Item $attempt -Recurse -Force -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force -Path $attempt | Out-Null

            $oldPreference = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'Continue'
                & gh run download $run.databaseId `
                    --repo $Repository `
                    --name $artifactName `
                    --dir $attempt 2>$null
                $downloaded = ($LASTEXITCODE -eq 0)
            }
            finally {
                $ErrorActionPreference = $oldPreference
            }

            if (-not $downloaded) {
                Remove-Item $attempt -Recurse -Force -ErrorAction SilentlyContinue
                continue
            }

            Expand-NestedArchives $attempt
            $worker = Find-NvidiaWorker $attempt
            if ($worker) {
                Write-Host "NVIDIA artifact: $artifactName"
                Write-Host "NVIDIA workflow run: $($run.databaseId)"
                Write-Host "NVIDIA commit: $($run.headSha)"
                return $worker
            }
        }
    }

    throw 'Successful NVIDIA runs were found, but no NVIDIA worker could be extracted.'
}

if (-not (Get-Command gh.exe -ErrorAction SilentlyContinue)) {
    throw 'GitHub CLI was not found.'
}
if (-not (Test-GhCommand @('auth', 'status'))) {
    throw 'GitHub CLI is not authenticated. Run gh auth login first.'
}

$owner = (& gh api user --jq .login).Trim()
if ([string]::IsNullOrWhiteSpace($owner)) {
    throw 'Could not determine the authenticated GitHub account.'
}
$repository = "$owner/$RepositoryName"

Write-Host '============================================================'
Write-Host 'BetaSeedFinder universal Windows release'
Write-Host '============================================================'
Write-Host "Repository: $repository"
Write-Host

& (Join-Path $root 'scripts\Verify-ProjectSource.ps1') -ProjectRoot $root

Write-Host
Write-Host '1/4 Building AMD worker locally...'
$amdWorker = & (Join-Path $root 'scripts\build-native.ps1') `
    -ProjectRoot $root `
    -Backend AMD

if (-not (Test-Path $amdWorker -PathType Leaf)) {
    throw 'The AMD worker build did not return a valid executable.'
}

$temp = Join-Path $env:TEMP (
    'BetaSeedFinder-universal-' + [Guid]::NewGuid().ToString('N')
)
$downloadRoot = Join-Path $temp 'nvidia-download'
$bundleRoot = Join-Path $temp 'workers'
New-Item -ItemType Directory -Force -Path $downloadRoot, $bundleRoot | Out-Null

try {
    Write-Host
    Write-Host '2/4 Downloading the latest successful NVIDIA worker...'
    $nvidiaWorker = Download-LatestNvidiaWorker `
        -Repository $repository `
        -Destination $downloadRoot

    Write-Host
    Write-Host '3/4 Creating portable AMD and NVIDIA worker bundles...'
    $amdPortable = New-PortableWorkerBundle `
        -Worker $amdWorker `
        -Destination (Join-Path $bundleRoot 'amd') `
        -PreferredSearchDirectories (Get-HipSearchDirectories)

    $nvidiaPortable = New-PortableWorkerBundle `
        -Worker $nvidiaWorker `
        -Destination (Join-Path $bundleRoot 'nvidia')

    Write-Host
    Write-Host '4/4 Packaging one universal BetaSeedFinder.exe download...'
    $zip = & (Join-Path $root 'scripts\package-windows.ps1') `
        -ProjectRoot $root `
        -AmdWorker $amdPortable `
        -NvidiaWorker $nvidiaPortable `
        -OutputDirectory $OutputDirectory `
        -RequireUniversal

    if (-not (Test-Path $zip -PathType Leaf)) {
        throw 'The universal package was not created.'
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entryNames = @($archive.Entries | ForEach-Object {
            $_.FullName.Replace('/', '\')
        })

        foreach ($required in @(
            'BetaSeedFinder.exe',
            'backend\amd\BetaSeedFinderWorker.exe',
            'backend\nvidia\BetaSeedFinderWorker.exe',
            'BACKENDS.txt'
        )) {
            if ($entryNames -notcontains $required) {
                throw "Universal package is missing: $required"
            }
        }

        if (-not ($entryNames | Where-Object {
            $_ -like 'runtime\*'
        })) {
            throw 'Universal package is missing the bundled Java runtime.'
        }
    }
    finally {
        $archive.Dispose()
    }

    $version = (Get-Content (Join-Path $root 'VERSION.txt') -Raw).Trim()
    $sumPath = Join-Path $OutputDirectory 'SHA256SUMS.txt'

    if ($Publish) {
        Write-Host
        Write-Host "Publishing universal package as GitHub prerelease $version..."

        if (Test-GhCommand @('release', 'view', $version, '--repo', $repository)) {
            Invoke-Gh `
                -Arguments @(
                    'release', 'upload', $version,
                    $zip, $sumPath,
                    '--repo', $repository,
                    '--clobber'
                ) `
                -FailureMessage 'Could not update the GitHub release.'
        }
        else {
            $notes = @"
Universal Windows package.

- One user-facing launcher: BetaSeedFinder.exe
- AMD HIP worker included
- NVIDIA CUDA worker included
- Java runtime included
- Automatic AMD/NVIDIA backend selection

This is alpha software. Keep backups of important search results.
"@

            Invoke-Gh `
                -Arguments @(
                    'release', 'create', $version,
                    $zip, $sumPath,
                    '--repo', $repository,
                    '--target', 'main',
                    '--prerelease',
                    '--title', "BetaSeedFinder $version",
                    '--notes', $notes
                ) `
                -FailureMessage 'Could not create the GitHub release.'
        }

        Write-Host
        Write-Host "Published release: https://github.com/$repository/releases/tag/$version" `
            -ForegroundColor Green

        & gh release view $version --repo $repository --web
    }

    Write-Host
    Write-Host 'UNIVERSAL WINDOWS RELEASE COMPLETE' -ForegroundColor Green
    Write-Host "Package: $zip"
    Write-Host 'Users download this ZIP from Releases, extract it, and run BetaSeedFinder.exe.'
}
finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}
