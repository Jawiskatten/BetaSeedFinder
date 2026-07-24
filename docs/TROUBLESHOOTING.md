# Troubleshooting

## No GPU worker found

Use a release matching your GPU, or place `BetaSeedFinderWorker.exe` in `backend/amd` or `backend/nvidia`.

## The app does not start

Use the packaged `BetaSeedFinder.exe` from Releases. Source builds require JDK 17 or newer.

## NVIDIA build cannot find cl.exe

Install Visual Studio 2022 C++ Build Tools with the x64 C++ toolset. The build script imports the Visual Studio developer environment automatically.

## Results folder is not writable

Choose another output folder during first-run setup. The selected output path is stored in `config/gui.properties`.
