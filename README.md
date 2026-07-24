# BetaSeedFinder

GPU-accelerated Minecraft Beta 1.7.3 floating-island seed finder by **Jawiskatten**.

## Download

Normal users should download a Windows package from **Releases**, extract it, and run:

```text
BetaSeedFinder.exe
```

Java is bundled. The release keeps the AMD/NVIDIA workers inside internal folders, so there is only one user-facing launcher.

> Alpha software: keep backups of important results and review the validation notes before very long searches.

## Status

- AMD/HIP production path established on Radeon RX 7800 XT.
- NVIDIA/CUDA exactness and the MEGA production pipeline passed on a Tesla T4.
- The GitHub NVIDIA workflow builds a self-contained Windows app image for the remaining Windows smoke test. Its CUDA job installs NVCC, the CUDA runtime development headers, and Visual Studio integration.

## Build from source

Requirements: Windows 10/11, JDK 17+, and the SDK for the native backend you are building.

```powershell
.\build.ps1
.\build.ps1 -Target NVIDIA
.\build.ps1 -Target AMD
```

Create a clean Windows package after building at least one worker:

```powershell
.\scripts\package-windows.ps1
```

## Repository layout

```text
src/                 Java desktop application
native/src/          Shared HIP/CUDA worker source
scripts/             Build, verification, and packaging commands
docs/                Architecture, building, validation, troubleshooting
.github/workflows/   CI and Windows NVIDIA packaging
```

The repository intentionally excludes output folders, local configuration, compiled binaries, old patch files, duplicate batch launchers, optional third-party assets, large reference datasets, and obsolete research utilities.

## Accuracy

The terrain implementation is exact for the validated stages. Aggressive search profiles also use empirical early gates, so exact terrain generation does not imply mathematically guaranteed recall for every profile.

See `docs/VALIDATION.md` and `docs/ARCHITECTURE.md`.

## License

MIT. Minecraft assets are not distributed with this project.
