# Building BetaSeedFinder

## Java

Install JDK 17 or newer and verify:

```text
java -version
javac -version
```

Compile with:

```text
compile_project.bat
```

Classes are written to `bin/`, which is ignored by Git.

## AMD HIP worker

Install AMD HIP SDK for Windows. The build script checks `HIP_SDK_DIR` and common ROCm 7.x locations.

```text
BUILD_AMD_BACKEND.bat
TEST_AMD_BACKEND.bat
```

The multi-architecture script requests `gfx1030`, `gfx1031`, `gfx1032`, `gfx1100`, `gfx1101`, `gfx1102`, `gfx1200`, and `gfx1201`. A toolkit may reject targets it does not support; only change the target list with matching hardware validation.

## NVIDIA CUDA worker

Local requirements:

- NVIDIA CUDA Toolkit with `nvcc`
- Visual Studio C++ Build Tools

```text
BUILD_NVIDIA_BACKEND.bat
TEST_NVIDIA_BACKEND.bat
```

The build uses precise floating-point flags and a static CUDA runtime. It selects architecture targets reported by the installed toolkit and adds a PTX fallback.

## GitHub Actions NVIDIA build

The workflow `.github/workflows/build-nvidia-windows.yml` installs CUDA 12.8.1 on a pinned `windows-2022` runner, verifies the shared source, builds the worker, and packages a no-install tester. The runner can compile CUDA but cannot execute GPU tests.

## Exactness tests

Both backends use the same four reference suites:

- P20 exactness
- optimized Stage0 exactness
- P19 feature/decision exactness
- coarse score/decision exactness

The short production benchmark is:

```text
gpu_p20_benchmark.exe pipelineprofile 32768 123456789 3 mega full
```

## Clean public source archive

```text
CREATE_GITHUB_SOURCE_ZIP.bat
```

The packager uses an explicit allowlist and verifies that binaries, local configuration, run data, old patch files, personal paths, and unlicensed assets are absent.
