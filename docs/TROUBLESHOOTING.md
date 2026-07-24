# Troubleshooting

## Java is missing

Install JDK 17 or newer and reopen the terminal:

```text
java -version
javac -version
```

## No worker is found

Expected paths:

- `backend\amd\gpu_p20_benchmark.exe`
- `backend\nvidia\gpu_p20_benchmark.exe`

Use `SET_GPU_BACKEND_AUTO.bat`, `SET_GPU_BACKEND_AMD.bat`, or `SET_GPU_BACKEND_NVIDIA.bat` to change selection.

## `hipcc.exe` is missing

Install AMD HIP SDK or set `HIP_SDK_DIR` to its installation folder.

## `nvcc.exe` is missing

Install CUDA Toolkit and Visual Studio C++ Build Tools, or use the GitHub Actions artifact instead of building locally.

## A native exactness test fails

Do not use that worker for record searching. Keep the complete validation log, GPU model, driver version, worker SHA-256, and command output.

## A historical 3D preview fails

Try regenerating the preview and switch to 2D. Older records may lack enough reconstruction metadata.
