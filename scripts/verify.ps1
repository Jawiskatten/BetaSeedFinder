param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [switch]$Strict
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $ProjectRoot).Path

$required = @(
    '.gitignore','LICENSE','README.md','CHANGELOG.md','CONTRIBUTING.md','SECURITY.md','VERSION.txt','build.ps1',
    'src\GuiMain.java','src\GpuBackendLocator.java','src\AppPaths.java',
    'native\src\gpu_p20_benchmark.cpp','native\src\gpu_runtime_compat.hpp','native\CMakeLists.txt',
    'scripts\AppSourceFiles.txt','scripts\build-java.ps1','scripts\build-native.ps1',
    'scripts\package-windows.ps1','scripts\verify.ps1',
    '.github\workflows\ci.yml','.github\workflows\windows-nvidia.yml'
)
foreach ($relative in $required) {
    if (-not (Test-Path (Join-Path $root $relative))) { throw "Required file missing: $relative" }
}

$forbiddenRoot = @(
    'START_AMD.bat','START_NVIDIA.bat','START_BETASEEDFINDER.bat',
    'RUN_GPU_GUI_AMD.bat','RUN_GPU_GUI_NVIDIA.bat','RUN_GPU_GUI_AUTO.bat','run_gpu_gui.bat',
    'SET_GPU_BACKEND_AMD.bat','SET_GPU_BACKEND_NVIDIA.bat','SET_GPU_BACKEND_AUTO.bat',
    'BUILD_AMD_BACKEND.bat','BUILD_NVIDIA_BACKEND.bat','TEST_AMD_BACKEND.bat','TEST_NVIDIA_BACKEND.bat',
    'CREATE_GITHUB_SOURCE_ZIP.bat','CREATE_NVIDIA_TESTER.bat','NVIDIA_NO_INSTALL_TEST.bat',
    'PUBLISH_PUBLIC_GITHUB.bat','VERIFY_GITHUB_SOURCE.bat','compile_project.bat','BUILDING.md','SUPPORT.md'
)
foreach ($name in $forbiddenRoot) {
    if (Test-Path (Join-Path $root $name)) { throw "Obsolete root-level wrapper remains: $name" }
}
foreach ($directory in @('gpu_p20_benchmark','backend','testdata','assets','screenshots')) {
    if (Test-Path (Join-Path $root $directory)) { throw "Obsolete public directory remains: $directory" }
}

$sourceList = Get-Content (Join-Path $root 'scripts\AppSourceFiles.txt') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
$allowed = @{}
foreach ($relative in $sourceList) {
    $normalized = $relative.Replace('/','\').ToLowerInvariant()
    $allowed[$normalized] = $true
    if (-not (Test-Path (Join-Path $root ('src\' + $relative.Replace('/','\'))))) { throw "Manifest source missing: $relative" }
}
$unexpected = Get-ChildItem (Join-Path $root 'src') -Recurse -Filter *.java | Where-Object {
    $relative = $_.FullName.Substring((Join-Path $root 'src').Length + 1).ToLowerInvariant()
    -not $allowed.ContainsKey($relative)
}
if ($unexpected) { throw 'Unexpected/non-application Java files found: ' + (($unexpected.FullName) -join ', ') }

$version = (Get-Content (Join-Path $root 'VERSION.txt') -Raw).Trim()
if ($version -notmatch '^v\d+\.\d+\.\d+-alpha\.\d+$') { throw "Invalid version: $version" }
$gui = Get-Content (Join-Path $root 'src\GuiMain.java') -Raw
if ($gui -notmatch [regex]::Escape('APP_VERSION = "' + $version + '"')) { throw 'GuiMain version does not match VERSION.txt.' }

$runtimeCompatText = Get-Content (Join-Path $root 'native\src\gpu_runtime_compat.hpp') -Raw
if ($runtimeCompatText -notmatch 'BSF_NVIDIA_CUDA' -and $runtimeCompatText -notmatch '__CUDACC__') {
    throw 'CUDA runtime compatibility selection is missing.'
}

$exactMathText = Get-Content (Join-Path $root 'native\src\p20_exact_math.hpp') -Raw
if ($exactMathText -notmatch '__CUDACC__') {
    throw 'CUDA host/device annotations are missing from p20_exact_math.hpp.'
}
$directHip = Get-ChildItem (Join-Path $root 'native\src') -Recurse -File | Where-Object {
    $_.Name -ne 'gpu_runtime_compat.hpp' -and (Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue) -match '#include\s*[<"]hip/hip_runtime'
}
if ($directHip) { throw 'Direct HIP runtime includes remain: ' + (($directHip.FullName) -join ', ') }
foreach ($name in @('p20_cpu_core.hpp','p20_cpu_validate.cpp','p20_fisher_yates_profile.hpp','p40_cross_center_fusion.hpp','CMakeLists.txt')) {
    if (Test-Path (Join-Path $root ('native\src\' + $name))) { throw "Unused native file remains: $name" }
}

$workflowText = (Get-Content (Join-Path $root '.github\workflows\ci.yml') -Raw) + "`n" +
                (Get-Content (Join-Path $root '.github\workflows\windows-nvidia.yml') -Raw)
if ($workflowText -match 'actions/setup-java@v6') { throw 'Invalid setup-java@v6 reference remains.' }
if ($workflowText -notmatch 'actions/setup-java@v5') { throw 'setup-java@v5 is missing.' }

if ($Strict) {
    $badExtensions = @('.exe','.dll','.class','.obj','.o','.pdb','.lib','.exp','.wav','.ttf','.otf','.zip')
    $bad = Get-ChildItem $root -Recurse -File | Where-Object { $badExtensions -contains $_.Extension.ToLowerInvariant() }
    if ($bad) { throw 'Generated/binary files found: ' + (($bad.FullName) -join ', ') }

    $personalPatterns = @(('D:' + '\\Yes\\BetaSeedFinder'), ('C:' + '\\Users\\'), ('/mnt/' + 'data/'), ('/home/' + 'oai/'))
    $textFiles = Get-ChildItem $root -Recurse -File | Where-Object { $_.Length -lt 5MB -and $_.Extension -in @('.md','.txt','.java','.cpp','.hpp','.ps1','.yml','.yaml') }
    foreach ($file in $textFiles) {
        $content = Get-Content $file.FullName -Raw -ErrorAction SilentlyContinue
        foreach ($pattern in $personalPatterns) {
            if ($content -match [regex]::Escape($pattern)) { throw "Personal path found in $($file.FullName): $pattern" }
        }
    }
}

Write-Host ('Repository verification passed' + $(if ($Strict) { ' (strict).' } else { '.' })) -ForegroundColor Green
