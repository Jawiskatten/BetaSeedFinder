$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$native = Join-Path $root 'gpu_p20_benchmark\native'

$bad = Get-ChildItem $native -Recurse -Include *.cpp,*.hpp | Where-Object {
    $_.Name -ne 'gpu_runtime_compat.hpp' -and
    (Select-String -Quiet -Path $_.FullName -SimpleMatch '#include <hip/hip_runtime.h>')
}
if ($bad) {
    throw 'Direct HIP runtime includes remain: ' + (($bad.FullName) -join ', ')
}

$cpp = Get-Content (Join-Path $native 'gpu_p20_benchmark.cpp') -Raw
if ($cpp -notmatch 'BSF_NVIDIA_CUDA') { throw 'CUDA device-reporting branch missing.' }
if ($cpp -notmatch "'P','2','0','S','T','R','0','1'" -or $cpp -notmatch "'S','T','0','R','3','8','1','6'") {
    throw 'Production protocol magic is missing.'
}

$compat = Get-Content (Join-Path $native 'gpu_runtime_compat.hpp') -Raw
if ($compat -notmatch 'cuda_runtime.h' -or $compat -notmatch 'hip_runtime.h') {
    throw 'Runtime compatibility header does not contain both CUDA and HIP paths.'
}

Write-Host 'Dual-GPU source structure check passed.'
