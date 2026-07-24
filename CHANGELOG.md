### Windows MSVC environment hotfix

- Replaced the fragile nested `cmd.exe` quoting used to initialize Visual Studio.
- Initialize `VsDevCmd.bat` through a temporary command file instead.
- Print the resolved `cl.exe` path before invoking NVCC.

### GitHub Actions packaging-order hotfix

- Run strict repository verification before CUDA setup creates `cuda_download`.
- Use a stable `BetaSeedFinder-Windows-NVIDIA` artifact name.
- Document where the generated `BetaSeedFinder.exe` is downloaded.

### Windows Java build hotfix

- Fixed `javac` argument-file paths on Windows by using absolute forward-slash paths.
- Added `--release 17` so builds made with newer JDKs remain Java 17 compatible.

### CI hotfix

- Fixed the public-tree CUDA compatibility verification.
- Install CUDA runtime development headers in the Windows NVIDIA workflow.
- Add an explicit CUDA include path and clear missing-header diagnostic.
- Corrected PowerShell build examples in the README.

# Changelog

## v0.5.0-alpha.2

- Replaced the root-level batch-file maze with one source build entry point: `build.ps1`.
- Reduced the public Java tree to the 40 files required by the desktop application.
- Moved the reachable native production source into `native/src`.
- Added a self-contained `jpackage` release pipeline with one user-facing `BetaSeedFinder.exe` and bundled Java.
- Renamed internal native executables to `BetaSeedFinderWorker.exe`.
- Added clean CI and Windows NVIDIA packaging workflows.
- Removed old P-number validation utilities, unused native experiments, duplicate launchers, backend placeholders, screenshots, optional assets, large reference datasets, and publishing scripts from the public tree.

## v0.5.0-alpha.1

- Initial public alpha source release with dual-GPU source and NVIDIA CUDA validation.
