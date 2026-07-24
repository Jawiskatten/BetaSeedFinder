# BetaSeedFinder

BetaSeedFinder searches Minecraft Beta 1.7.3 terrain for unusually large floating islands.

Created by **Jawiskatten**.

> **Alpha software:** back up important results, expect rough edges, and read the validation notes before running long searches.

## Current status

The Java desktop application and AMD/HIP backend are established. The shared NVIDIA/CUDA worker has passed P20, optimized Stage0, P19, and coarse exactness tests on a Tesla T4. The complete MEGA production pipeline also completed on that GPU at approximately **13,458 seeds/s**.

The remaining NVIDIA gate is Windows runtime validation of the GitHub-built executable, driver loading, launchers, and Java GUI communication. The source is public-ready now; downloadable Windows binaries should remain marked as prerelease until that smoke test passes.

## Features

- Exact Minecraft Beta 1.7.3 terrain reconstruction
- AMD HIP and NVIDIA CUDA worker source
- Automatic or forced GPU backend selection
- General, Mega, Record Hunt, and experimental World Record profiles
- Search, Islands, Runs, Statistics, and Settings pages
- Interactive 3D island previews
- Persistent run history, checkpoints, and configurable output storage

## Quick start from source

Requirements:

- Windows 10 or Windows 11, 64-bit
- JDK 17 or newer
- A supported AMD or NVIDIA GPU
- A locally built or downloaded native worker

Compile Java:

```text
compile_project.bat
```

AMD:

```text
BUILD_AMD_BACKEND.bat
TEST_AMD_BACKEND.bat
RUN_GPU_GUI_AMD.bat
```

NVIDIA with a local CUDA Toolkit and Visual Studio C++ Build Tools:

```text
BUILD_NVIDIA_BACKEND.bat
TEST_NVIDIA_BACKEND.bat
RUN_GPU_GUI_NVIDIA.bat
```

The **Build NVIDIA Windows worker** workflow can also compile the Windows CUDA worker in GitHub Actions. GitHub-hosted runners do not have an NVIDIA GPU, so the downloaded tester must still be run on an NVIDIA Windows PC.

## Validation status

| Backend | Build status | Runtime validation |
| --- | --- | --- |
| AMD HIP | Multi-architecture Windows build script included | Established on Radeon RX 7800 XT (`gfx1101`) |
| NVIDIA CUDA | Windows CI build workflow included | Linux CUDA exactness and MEGA pipeline passed on Tesla T4; Windows runtime test pending |

The Colab/Tesla T4 validation produced zero P20 decision differences, zero Stage0 decision differences, zero P19 feature/decision differences, and zero coarse score/decision differences. See `docs/NVIDIA_VALIDATION.md`.

Aggressive search profiles use empirical early gates. Exact terrain generation does not mean every profile has mathematically guaranteed recall.

## Repository layout

- `src/` — Java application and validation tools
- `gpu_p20_benchmark/native/` — shared HIP/CUDA worker source
- `gpu_p20_benchmark/data/` — exactness reference files
- `scripts/` — verification, packaging, tester, and publishing scripts
- `.github/workflows/` — source CI, NVIDIA build, and prerelease workflows
- `docs/` — installation, pipeline, releasing, troubleshooting, validation, and asset policy

## Publishing this repository

Run:

```text
VERIFY_GITHUB_SOURCE.bat
PUBLISH_PUBLIC_GITHUB.bat
```

The publishing helper validates the tree, initializes Git when necessary, signs in through GitHub CLI, creates a public `BetaSeedFinder` repository, and pushes the `main` branch. It asks for confirmation before creating or pushing anything.

## Licensing and assets

The code is licensed under MIT. The repository deliberately excludes Minecraft textures, third-party font binaries, and unverified sound files. Optional assets may be added locally only when their licenses permit redistribution.

See `BUILDING.md`, `SUPPORT.md`, and `docs/RELEASING.md` for details.
