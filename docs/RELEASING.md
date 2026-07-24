# Releasing

The public source repository does not commit generated executables.

The GitHub workflow builds an internal artifact named:

```text
BetaSeedFinder-NVIDIA-Worker
```

That artifact is not the end-user download.

The final release is assembled on the maintainer's Windows AMD/HIP development machine:

```powershell
.\scripts\build-universal-release.ps1 -Publish
```

The script:

1. Builds the AMD worker locally.
2. Downloads the latest successful NVIDIA worker from GitHub Actions.
3. Bundles required non-system worker DLL dependencies.
4. Builds the Java application and bundled runtime.
5. Verifies both backend executables exist in the archive.
6. Creates and uploads:

```text
BetaSeedFinder-v0.5.0-alpha.3-windows-x64-universal.zip
```

Normal users download the universal ZIP from **Releases**, extract it, and run `BetaSeedFinder.exe`.
