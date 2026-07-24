# Validation

The shared NVIDIA/CUDA worker passed four exactness suites on a Tesla T4:

- P20 decision differences: 0
- optimized Stage0 decision differences: 0
- P19 feature and decision differences: 0
- coarse score and decision differences: 0

The complete MEGA production pipeline completed with exit code 0 at approximately 13,458 seeds/s on that T4.

The AMD path is established on Radeon RX 7800 XT. The remaining NVIDIA release gate is a Windows smoke test of the GitHub-built app image, driver loading, worker launch, and Java communication.

Aggressive profiles contain empirical early gates. Validation of exact terrain stages is not a proof of perfect recall for every profile.
