# Deadbush15 V5: prioritize candidates, then test them in vanilla Minecraft 26.3

**Status:** Working C++ 64-bit decoration-offset shortlist tool; locally tested on both published rows. Vanilla world generation/verification is implemented as a Python workflow but has **not been run end-to-end against a live 26.3 server** in this development environment (Java 21 only). No 15-bush world has been verified. V4 remains the GPU scanner.

## Why this is different

V4's `--mode xz` returned many long rows without checking whether the placements have the right vertical offsets. Its `flat_run` refers to **the best flat row anywhere in the search area**, which may have nothing to do with the reported XZ row. V5 replays the Minecraft 26.3 Xoroshiro placements **at each block of the reported XZ row**, recording:

- `same_row_flat_longest`: largest contiguous `dy=0` run **at the exact reported coordinates** under a constant-height model.
- `same_row_flat_count`: number of coordinates covered by at least one placement attempt with `dy=0`.
- `offset_cost`: sum of the minimum absolute `dy` at each position; a ranking heuristic, **not a geometric proof**.
- `full_area_flat_run`: old V4 reference number, retained only for comparison.

The shortlist ranks by `same_row_flat_longest`, then `same_row_flat_count`, then offset cost, then XZ length; this is a heuristic, not a completeness-preserving rejection. A genuine uneven-terrain result can score poorly here. Its purpose is to allocate a **small number of expensive worldgen verifications** usefully.

The Python verifier launches **official, unmodified Mojang 26.3 server.jar** separately for each chosen seed in a private local-only server directory, issues `/forceload` at the candidate coordinates, uses `save-all flush`, confirms that the requested chunks reach `full` status, and reads real overworld Anvil block and biome data. It reports `BLOCKS_MATCH_BADLANDS` only when every claimed position is a `minecraft:dead_bush` at the **same Y** and every position samples a badlands biome. The program does not modify blocks. This provides much stronger evidence than RNG alignment alone.

**Limitations:** This is *not* a seed inversion algorithm and not a fast computational biome/noise predictor. World generation for each seed is expensive (and most seeds likely have no matching biome). The V5 C++ ranking does not generate heightmaps. The automatic generator requires Java **25+**, 26.3 server jar, Internet to fetch it once, and explicit acceptance of the Mojang EULA. A clean isolated server is intended to establish provenance; recordings/screenshots can be used as additional evidence. Failure to find a 15-row in verified candidates does not prove no such seed exists.

## Start from your existing PowerShell folder

```powershell
cd D:\Yes\Deadbush15\deadbush15
git pull origin deadbush15
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v5.cpp -o deadbush15_v5.exe
.\deadbush15_v5.exe --self-test
```

### 1. Rank your **existing** 204,329 V4 results (no more GPU search)

```powershell
.\deadbush15_v5.exe --rank --csv gpu_v4_candidates.csv --min-run 15 --top 20 --output v5_shortlist.csv
Import-Csv .\v5_shortlist.csv | Format-Table -AutoSize
```

20 selected *distinct world seeds*, sorted by a terrain-related but approximate compatibility heuristic. Also try `--min-run 14` if you wish to include tie/near-record candidates.

### 2. Preview world-generation jobs (no installation)

```powershell
py -3 auto_verify_v5.py --csv v5_shortlist.csv --max-worlds 3 --dry-run
```

### 3. Prepare to generate and inspect **real** Minecraft worlds

Minecraft Java 26.3 requires Java 25+: <https://www.minecraft.net/en-us/article/minecraft-java-edition-26-3>

```powershell
winget install Microsoft.OpenJDK.25
```

**Open a new PowerShell** so the Java PATH is refreshed. Verify `java -version` begins with 25 or newer. If multiple Javas are installed, supply an absolute Java 25+ executable path with `--java "...\bin\java.exe"`.

Download the **official Mojang server** via their version manifest, with an SHA-1 comparison:

```powershell
py -3 auto_verify_v5.py --download-server
```

Read <https://aka.ms/MinecraftEULA>. **Only if you agree**, start a small automated test:

```powershell
py -3 auto_verify_v5.py --csv v5_shortlist.csv --server-jar server_26.3.jar --max-worlds 3 --min-run 15 --accept-eula --output v5_world_verified.csv
```

This starts and stops a **loopback-only local vanilla server**, generates candidate chunks using `/forceload`, and checks the saved blocks/biomes. `--max-worlds` defaults to 3 and cannot exceed 25 per invocation. Server directories are deleted only if created by the tool, after the attempt. To inspect them manually instead, add `--keep-worlds`; directories will remain inside `v5_verification_work` and won't be overwritten. The tool never deletes an existing user save. It never enables RCON.

Status meaning:

- `BLOCKS_MATCH_BADLANDS`: claimed row of blocks exists at the same Y, with badlands biome samples, in this program's fresh vanilla server world.
- `WRONG_BIOME`: blocks align but biome samples aren't all badlands.
- `NOT_VERIFIED`: actual saved world doesn't contain the full row; `longest_actual_same_y` gives best same-Y segment in that row.
- `MISSING_CHUNKS` / `CHUNK_TIMEOUT`: could not obtain all required full chunks. Not negative evidence.
- `ERROR`: server exited/crashed or some other verification failure. The console logs the reason.

If the Python `py -3` launcher is missing, try `python` in its place. The script uses Python standard library only.

## Continue scouting (GPU)

Keep the V4 GPU binary and kernel. It has proven useful for generating the loose shortlist; do **not** claim an XZ-22 as a real world. To avoid the expense of rechecking 12+ candidates, run a *new* non-overlapping seed range with `--prefilter 15 --report 15 --target 15`, e.g.:

```powershell
.\deadbush15_v4.exe --gpu-search --start 210100000 --count 10000000 --batch 8192 --locations 4 --spread-span 1000000 --mode xz --prefilter 15 --report 15 --target 15 --output v5_new_gpu_candidates.csv --checkpoint v5_new_gpu.progress --resume
```

You can rank new results with `--rank --csv v5_new_gpu_candidates.csv`. The prefilter is **lossless only for the XZ criterion**; sorting/limiting results is intentionally **not** completeness-preserving for real terrain. Benchmark your GPU before very large sweeps; V4's 4.52M location checks/s isn't directly comparable to V3 flat mode.

## Files

- `deadbush15_v5.cpp`: standalone C++ ranker with old `deadbush15.cpp` included for RNG parity.
- `auto_verify_v5.py`: official server download + automatic candidate generation and verification.
- `verify_world_v4.py`: the Anvil/NBT block and biome reader, reused unchanged from V4.
- `deadbush15_v4.cpp` and `deadbush15_gpu_v4.cl`: continue to handle GPU scout.