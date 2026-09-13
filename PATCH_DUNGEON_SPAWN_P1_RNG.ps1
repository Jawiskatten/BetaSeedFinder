$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$path = Join-Path $PSScriptRoot 'RUN_DUNGEON_SPAWN_P1.ps1'
if (-not (Test-Path $path -PathType Leaf)) { throw "Missing runner: $path" }

$text = [IO.File]::ReadAllText($path)
$old = '$bytes=New-Object byte[] 8; [Security.Cryptography.RandomNumberGenerator]::Fill($bytes); $RandomKey=[BitConverter]::ToUInt64($bytes,0)'
$new = @'
$bytes = New-Object byte[] 8
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
        $RandomKey = [BitConverter]::ToUInt64($bytes,0)
'@.Trim()

if ($text.Contains($new)) {
    Write-Host 'Dungeon P1 RNG compatibility patch already applied.'
    exit 0
}
if (-not $text.Contains($old)) { throw 'Expected RandomNumberGenerator::Fill line not found; runner may already differ.' }
$text = $text.Replace($old,$new)
[IO.File]::WriteAllText($path,$text,[Text.UTF8Encoding]::new($false))
Write-Host 'Patched RUN_DUNGEON_SPAWN_P1.ps1 for Windows PowerShell 5.1 / .NET Framework.'
