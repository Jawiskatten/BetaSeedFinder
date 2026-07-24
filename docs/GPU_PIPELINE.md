# GPU pipeline

The HIP and CUDA workers share the same production source and binary protocol.

The current production path includes:

- exact cached terrain and climate state reuse;
- eight search centers per world;
- compact upper/lower evaluation;
- P19 gate processing;
- exact coarse generation and connected-component scoring;
- final CPU reconstruction and island measurement.

Profiles differ in early candidate gates. General is broad, Mega favors recall, Record Hunt is tuned against known large-island evidence, and World Record is more aggressive and experimental.

## NVIDIA validation evidence

Tesla T4 / CUDA 12.8 cloud validation completed with:

- zero P20 raw/count/decision mismatches;
- optimized Stage0 exactness pass;
- P19 feature and decision exactness pass;
- coarse score and decision exactness pass;
- `pipelineprofile 32768 123456789 3 mega full` exit code 0;
- measured internal throughput of approximately 13,457.8 seeds/s.

This proves the CUDA computation on Linux. It does not by itself prove the Windows executable, DLL/driver loading, batch launchers, or Java worker communication.
