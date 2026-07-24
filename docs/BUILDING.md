# Building

## Java application

Install JDK 17 or newer:

```powershell
.\build.ps1
```

The JAR is written to `build/java/BetaSeedFinder.jar`.

## NVIDIA worker

Install CUDA Toolkit and Visual Studio 2022 C++ Build Tools:

```powershell
.\build.ps1 -Target NVIDIA
```

## AMD worker

Install AMD HIP SDK and Visual Studio 2022 C++ Build Tools:

```powershell
.\build.ps1 -Target AMD
```

`HIP_PATH` and `HIP_SDK_DIR` are both recognized.

## Package one or both local workers

```powershell
.\scripts\package-windows.ps1
```

For a guaranteed two-backend package:

```powershell
.\scripts\package-windows.ps1 -RequireUniversal
```

The generated ZIP contains one launcher, a bundled Java runtime, and internal backend folders.
