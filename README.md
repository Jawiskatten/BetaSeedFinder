# BetaSeedFinder

GPU-accelerated Minecraft Beta 1.7.3 floating-island seed finder by **Jawiskatten**.

## Download

Normal users should open **Releases**, download:

```text
BetaSeedFinder-v0.5.0-alpha.3-windows-x64-universal.zip
```

Extract it and run:

```text
BetaSeedFinder.exe
```

That single launcher automatically selects the included AMD or NVIDIA worker. Java is bundled. Users do not download anything from the Actions page and do not install the CUDA Toolkit.

> Alpha software: keep backups of important results before very long searches.

## Package layout

```text
BetaSeedFinder.exe
app/
runtime/
backend/
  amd/
    BetaSeedFinderWorker.exe
  nvidia/
    BetaSeedFinderWorker.exe
README.txt
BACKENDS.txt
LICENSE.txt
```

The backend folders are internal. Users launch only `BetaSeedFinder.exe`.

## Status

- AMD/HIP production path established on Radeon RX 7800 XT.
- NVIDIA/CUDA exactness and the MEGA production pipeline passed on a Tesla T4.
- GitHub Actions builds the internal NVIDIA worker.
- The maintainer release script combines that worker with the locally built AMD worker into one universal Windows package.

## Build from source

Requirements: Windows 10/11, JDK 17+, and the SDK for the native backend being built.

```powershell
.\build.ps1
.\build.ps1 -Target NVIDIA
.\build.ps1 -Target AMD
```

Create a package after building one or both workers:

```powershell
.\scripts\package-windows.ps1
```

Maintainers create the final two-backend release with:

```powershell
.\scripts\build-universal-release.ps1 -Publish
```

See `docs/BUILDING.md`, `docs/RELEASING.md`, and `docs/VALIDATION.md`.

## Repository layout

```text
src/                 Java desktop application
native/src/          Shared HIP/CUDA worker source
scripts/             Build, verification, packaging, and release commands
docs/                Architecture, building, validation, releasing, troubleshooting
.github/workflows/   CI and internal NVIDIA worker build
```

The repository intentionally excludes compiled binaries, output folders, local configuration, old patch files, duplicate launchers, third-party game assets, and obsolete research utilities.

## Accuracy

The terrain implementation is exact for the validated stages. Aggressive profiles also use empirical early gates, so exact terrain generation does not imply mathematically guaranteed recall for every profile.

## License

MIT. Minecraft assets are not distributed with this project.
