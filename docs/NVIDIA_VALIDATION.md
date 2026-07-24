# NVIDIA validation

## Confirmed cloud test

The shared CUDA worker was compiled with CUDA 12.8 and executed on a Tesla T4 (`sm_75`, 15 GB) in Google Colab.

Exactness results:

- P20 decision differences: 0
- optimized Stage0 decision differences: 0
- P19 feature-field differences: 0
- P19 decision differences: 0
- coarse score differences: 0
- coarse decision differences: 0

Production command:

```text
pipelineprofile 32768 123456789 3 mega full
```

Observed production result:

- average batch wall time: 2434.869 ms
- internal throughput: 13,457.8 seeds/s
- P20 pass: 15,274
- Stage1 pass: 9,926
- Stage0.5 pass: 1,425
- P19 pass: 885
- Mega topology rejects: 442
- coarse pass at threshold 85: 3
- process exit code: 0

The hottest phase was the full upper GPU stage at 38.82% of total time.

## Still required

The cloud test does not validate Windows-specific behavior. Before publishing a Windows NVIDIA runtime as stable, test:

- the GitHub-built `.exe` on a real NVIDIA Windows PC
- CUDA driver loading without a locally installed Toolkit
- `RUN_GPU_GUI_NVIDIA.bat`
- automatic backend selection
- Java/native protocol communication
- start, stop, checkpoint, result saving, and clean shutdown

Until then, NVIDIA Windows artifacts must be labeled **prerelease / needs tester validation**.
