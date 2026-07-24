# Native backends

Compiled workers are generated locally or by GitHub Actions and are not committed.

Expected runtime locations:

- `backend/amd/gpu_p20_benchmark.exe`
- `backend/nvidia/gpu_p20_benchmark.exe`

The Java backend resolver checks those paths and can be forced with the AMD/NVIDIA launchers.
