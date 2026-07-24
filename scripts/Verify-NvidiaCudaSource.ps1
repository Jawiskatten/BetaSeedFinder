param([string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path)
$ErrorActionPreference = 'Stop'
$native = Join-Path $ProjectRoot 'gpu_p20_benchmark\native'

$compat = Get-Content (Join-Path $native 'gpu_runtime_compat.hpp') -Raw
if ($compat -notmatch '#define\s+wall_clock64\s+clock64') { throw 'CUDA wall_clock64 compatibility mapping is missing.' }

$math = Get-Content (Join-Path $native 'p20_exact_math.hpp') -Raw
if ($math -notmatch 'defined\(__HIPCC__\).*defined\(__CUDACC__\)') { throw 'p20 exact math is not CUDA device-callable.' }

$perm = Get-Content (Join-Path $native 'p34_compact_perm.hpp') -Raw
if ($perm -notmatch 'defined\(__HIPCC__\).*defined\(__CUDACC__\)') { throw 'compact permutation helpers are not CUDA device-callable.' }

$stage0 = Get-Content (Join-Path $native 'stage0_exact_gpu.hpp') -Raw
if ($stage0 -notmatch 'noise5Local' -or $stage0 -notmatch '!defined\(BSF_NVIDIA_CUDA\)') { throw 'Turing shared-memory fix is missing.' }

$runner = Get-Content (Join-Path $ProjectRoot 'scripts\Run-NvidiaTester.ps1') -Raw
if ($runner -notmatch 'pipelineprofile\s+32768\s+123456789\s+3\s+mega\s+full') { throw 'NVIDIA tester does not use the MEGA benchmark.' }

Write-Host 'NVIDIA CUDA source verification passed.'
