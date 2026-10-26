# Deadbush15 V4 — threshold-gated GPU scout

**Experimental Java 26.3 dead-bush *decoration RNG* search; not a complete terrain/biome generator. No verified 15-bush world.** Uses the same Xoroshiro placement replay and 3x3 chunk neighborhoods as V3; existing V1/V2/V3 files are unchanged.

## What's new

- **Lossless threshold filter:** GPU returns exact longest cardinal (X/Z) row scores when they reach `--prefilter`, otherwise zero. It does **not** skip any score `>= --prefilter` within the chosen `--mode`. Uses a sparse bitset run detector before exact scoring, so most locations avoid full row scoring. **All placements still get generated**; GPU speed gains are NOT guaranteed.
- **1-byte scores per GPU item**, instead of V3's 4-byte integer. Reduces transfers to host; not necessarily the primary bottleneck.
- **XZ mode** suggested to avoid discarding candidates from nonzero decoration Y offsets. XZ is an optimistic *upper bound* on any level-aligned row, **not** a terrain model. `flat` still available, reproduces V3 logic.
- **Candidate-only CPU rechecks** validate exact GPU scores, and now record `flat_run` and `xz_run` (best anywhere in the same 3×3 neighborhood, *not necessarily the same positions*).
- **V4-only checkpoints and separate output CSVs.** Search ranges are a sequence of full 64-bit world seed values, with N deterministic chunk centers per seed.
- **Biome-aware verification from saved worlds:** `verify_world_v4.py` checks that each bush exists at a shared Y-level in an already generated save and its sampled biome ends in `badlands` (includes eroded/wooded variants). Requires creating the candidate world and visiting the chunks. This is *not* automatic worldgen or proof the save was never edited.

## Windows install/build

Requires Git, MSYS2 UCRT64 GCC, and OpenCL support from AMD Adrenalin driver.

```powershell
cd C:\Users\ASUS\Deadbush15\deadbush15
git pull origin deadbush15
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v4.cpp -o deadbush15_v4.exe
.\deadbush15_v4.exe --cpu-self-test
.\deadbush15_v4.exe --gpu-self-test
```

`--gpu-self-test` checks 2 published seeds plus CPU/GPU parity at 6 thresholds, 2 modes and multiple locations. **Do not run a long search if any test fails.**

## Small speed comparison, same parameters for V3 and V4

No stale benchmark candidates: use new files and range outside any existing runs.

```powershell
# V3 reference throughput (note: reports lengths >= 12 but scores all locations)
.\deadbush15_v3.exe --gpu-search --start 110000000 --count 100000 --batch 8192 --locations 4 --spread-span 1000000 --mode xz --report 12 --target 15 --output v3_xz_benchmark.csv

# V4 threshold-gated throughput, same seed range and locations
.\deadbush15_v4.exe --gpu-search --start 110000000 --count 100000 --batch 8192 --locations 4 --spread-span 1000000 --mode xz --prefilter 12 --report 12 --target 15 --output v4_xz_benchmark.csv
```

Compare **location checks/s** on the same machine, not just world seeds/s. Do not claim an improvement before measuring; V4's threshold prefilter could be slower on some hardware.

## Main search (new range)

This starts at `110100000` to avoid the 100M V3 run plus benchmark interval above:

```powershell
.\deadbush15_v4.exe --gpu-search --start 110100000 --count 100000000 --batch 8192 --locations 4 --spread-span 1000000 --mode xz --prefilter 12 --report 12 --target 15 --output gpu_v4_candidates.csv --checkpoint gpu_v4_progress.log --resume
```

Run the identical command again to resume. Never reuse a checkpoint/output from a different run or version. The per-GPU-item `run` is an **optimistic XZ score**, not a verified row; `flat_run` is the *largest* flat score found anywhere in the scanned neighborhood.

### Validate a seed in a Minecraft world save

Generate and visit the candidate coordinates in an unmodified 26.3 world, save & quit, then:

```powershell
py -3 verify_world_v4.py --world "$env:APPDATA\.minecraft\saves\YourWorld" --csv gpu_v4_candidates.csv --min-run 12 --output verified_v4.csv
```

`BLOCKS_MATCH_BADLANDS` means the claimed row blocks exist at the same Y, and the biome samples are badlands; it does **not** prove natural origin or world generation correctness. `MISSING_CHUNKS` means not generated. Other statuses include `WRONG_BIOME`, `UNKNOWN_BIOME`, and `NOT_VERIFIED`.

## Scope and limits

V4 does not implement reverse-Xoroshiro seed targeting, biome prediction from world seeds, automatic Minecraft chunk generation, or a proof that a 15-bush world exists. No diagonal patterns. Optimized for avoiding work in final scoring, **not skipping RNG generation**. A 12–15 XZ candidate may be invalid on real terrain. The GPU OpenCL build and performance must be verified on your RX 7800 XT.
