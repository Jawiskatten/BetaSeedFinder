# Deadbush15 v3 — GPU scout + save-file verification

**Minecraft Java 26.3 experimental code.** There is still **no verified 15-bush seed**. V3 searches decoration RNG patterns, *not* generated terrain. OpenCL GPU performance has not yet been measured on RX 7800 XT.

## Features

- OpenCL 1.2 GPU backend uses the AMD driver's `OpenCL.dll` dynamically; no HIP SDK required. One GPU item checks a 3×3 decoration-chunk neighborhood for each sampled location.
- `--locations N` samples N reproducibly dispersed locations per world seed, configured with `--spread-span`. `4` locations = 36 decoration chunks generated per seed (not a continuous 6×6 region).
- GPU passes only short row-length scores to the host; scores meeting `--report` are rechecked using CPU code before logging. A CPU/GPU mismatch aborts search.
- GPU self-test checks both published 12/14-bush examples plus 96 sample seeds for flat/XZ modes and random locations. **Must pass on your machine before searching.**
- Supports deterministic batches and checkpoints (`--checkpoint`, `--resume`), separate from V2's files.
- `verify_world.py` reads *actual* Java Anvil `.mca` files and confirms `minecraft:dead_bush` blocks exist at the same Y for an entire claimed row. It verifies `level.dat`'s seed, and distinguishes missing chunks. **It does not generate terrain**; you must create/load the candidate world in vanilla first.

## Update and build on Windows

```powershell
cd C:\Users\ASUS\Deadbush15\deadbush15
git pull origin deadbush15
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v3.cpp -o deadbush15_v3.exe
.\deadbush15_v3.exe --gpu-self-test
```

Or run `build_v3_windows.bat`. The GPU kernel file (`deadbush15_gpu.cl`) must be next to the exe when launched. If OpenCL device detection, compilation or parity fails, **don't run a long GPU search**. Share the error.

## Target GPU benchmark (short)

```powershell
.\deadbush15_v3.exe --gpu-search --start 0 --count 100000 --batch 4096 --locations 4 --spread-span 1000000 --mode flat --report 9 --target 15 --output gpu_bench.csv --checkpoint gpu_bench.progress --resume
```

If self-test passes, compare the reported **location checks/s** with the V2 baseline of approximately 3.16 million overlapping 3×3-center windows per second. Overlapping windows and dispersed locations are not statistically independent trials. Only scale up if GPU has a meaningful advantage.

```powershell
.\deadbush15_v3.exe --gpu-search --start 100000 --count 100000000 --batch 8192 --locations 4 --spread-span 1000000 --mode flat --report 10 --target 15 --output gpu_candidates.csv --checkpoint gpu_progress.log --resume
```

Restart the same command to resume. V3 checks only 3×3 per location; keep using V2 when you want contiguous multi-chunk regions.

## Verify candidate blocks in Minecraft

1. Create a vanilla Java 26.3 world using the candidate `seed_signed` value. Generate the chunks near `start_x,start_z` by visiting them in the overworld, save and quit.
2. Python 3 (stdlib only):

```powershell
py -3 verify_world.py --world "$env:APPDATA\.minecraft\saves\World Name" --csv gpu_candidates.csv --min-run 10 --output world_verified.csv
```

`VERIFIED` means all claimed X/Z positions have `minecraft:dead_bush` at a common Y level in saved chunks. `MISSING_CHUNKS` means not all chunks were generated; `NOT_VERIFIED` means the pattern didn't survive real generation. One saved world only verifies candidates matching its exact seed. In-game visual verification is still recommended: an edited save could contain artificially planted bushes.

**Limitations:** No automated biome/terrain generation before scoring, no diagonal GPU search, no native support for nonstandard Anvil chunk compression, OpenCL GPU speed unverified. These are filters, not definitive records.

Files: `deadbush15_v3.cpp`, `deadbush15_gpu.cl`, `verify_world.py`, `build_v3_windows.bat`. Existing V1/V2 unchanged.
