# Building

## Java application

Install JDK 17 or newer, then run:

```powershell
.uild.ps1
```

The JAR is written to `build/java/BetaSeedFinder.jar`.

## NVIDIA worker

Install CUDA Toolkit and Visual Studio 2022 C++ Build Tools:

```powershell
.uild.ps1 -Target NVIDIA
```

## AMD worker

Install AMD HIP SDK:

```powershell
.uild.ps1 -Target AMD
```

## Self-contained Windows package

After one or both workers exist:

```powershell
.\scripts\package-windows.ps1
```

The result is a ZIP containing one top-level launcher, a bundled Java runtime, and internal worker folders. No Java installation is required on the destination PC.
