# Architecture

BetaSeedFinder has two cooperating processes:

1. The Java Swing desktop application owns settings, run history, exact Java terrain verification, result storage, and the UI.
2. `BetaSeedFinderWorker.exe` performs the high-throughput early GPU pipeline. AMD and NVIDIA releases contain vendor-specific builds of the same shared source.

The launcher selects a worker from `backend/amd` or `backend/nvidia`. In packaged builds the Java runtime is bundled by `jpackage`; users launch only `BetaSeedFinder.exe`.

The worker implements P20, Stage1/Stage0, P19, and exact coarse stages. The remaining P-number headers under `native/src` are reachable production optimization modules; unused experiment files are excluded.
