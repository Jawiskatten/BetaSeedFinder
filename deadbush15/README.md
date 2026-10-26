# Deadbush15 v2: multi-chunk scanner (2026-10-26)

The recommended **v2** finder is `deadbush15_v2.cpp`. The original `deadbush15.cpp` is retained for reproducing earlier runs.

## Windows PowerShell: update, compile, test

```powershell
cd C:\Users\ASUS\Deadbush15\deadbush15
git pull origin deadbush15
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v2.cpp -o deadbush15_v2.exe
.\deadbush15_v2.exe --self-test
```

Alternatively, run `build_v2_windows.bat` from Explorer / CMD. It adds the UCRT64 GCC location to PATH.

## Broader scan: 4 × 4 center chunks plus radius-1 border

```powershell
.\deadbush15_v2.exe --search --start 0 --count 5000000 --threads 8 --region-width 4 --region-height 4 --radius 1 --spread-span 1000000 --mode flat --report 10 --target 15 --checkpoint v2_progress.log --resume --output v2_candidates.csv
```

Run the exact same command again after interruption to resume from the last **contiguously completed** checkpoint. It only checkpoints every ~5 seconds, so a short portion of already searched seeds may be retried and corresponding candidate rows duplicated after a crash. At natural completion, it checkpoints the whole range. The run parameters, CSV path, and checkpoint path should stay unchanged when resuming. To start a **new** range, change `--start`, `--checkpoint`, and `--output`.

- `--region-width 4 --region-height 4`: covers one 4-by-4 region of center chunks per seed; with `--radius 1`, generates its union with a one-chunk border: **6 × 6 = 36 decoration chunks per seed**. It simulates each chunk's decoration stream only once and counts rows across the union.
- `--spread-span 1000000`: deterministically chooses a center chunk somewhere within ±1,000,000 chunks on both axes, based on the world seed. This samples **one region per world seed**, NOT the entire world. Use `--spread-span 0 --chunk-x 10000 --chunk-z -10000` for fixed chunks.
- `--region-width 1 --region-height 1 --spread-span 0 --chunk-x 0 --chunk-z 0`: behaves like the original fixed-origin scanner and uses its optimized 64-bit grid path.
- `--mode flat`: assumes a flat surface and ignores attempts with vertical offset; `--mode xz` ignores Y and may produce many false positives.
- Default searches X and Z straight rows; `--diagonal` includes diagonals (extra cost).

**Correctness:** v2 passes both published 12/14-bush RNG reference checks, compares single-center runs with v1 at arbitrary coordinates, and checks larger-region bit scanning against a brute-force oracle. This is still NOT Minecraft biome, terrain, or final-block verification. **No 15-row world has been verified or found.** A high-scoring CSV entry is only a decoration pattern candidate.

**Performance:** local 250k-seed test (8 CPU threads, 4 × 4 region, radius 1, spread 1M) processed ~77,600 world seeds/s on the test machine. Do not compare its raw seeds/s directly with the 1 × 1 origin test; 36 vs 9 decoration chunks are simulated per world seed and every chunk is used once. Your CPU performance will differ.

---

# Deadbush15 — Minecraft Java Edition 26.3

**Goal:** Find a naturally generated **15-block straight line of `minecraft:dead_bush`** in badlands.

**Current state:** Working, version-specific C++ **decoration RNG scout**. It reproduces both published 12- and 14-bush coordinates exactly. It has **NOT** found or verified a new 15-bush world. Do not post `candidates.csv` as proven world seeds.

## What it does

- Implements 26.3 Xoroshiro128++ worldgen RNG / 64-bit seed expansion and `WorldgenRandom` seeding.
- Replays the `patch_dead_bush_badlands` decoration sequence: 20 attempts per chunk, 4 triangle-offset placements per attempt.
- Uses global decoration feature index **75** (discovered by matching both independent published reference seeds), generation step 9.
- Counts distinct block locations along the X or Z axis. Default is cardinal lines only. Optionally tries diagonals.
- `--radius 1` combines neighboring chunks' plant placements, crucial for the 14-bush reference.
- `--mode flat` only accepts placement attempts with zero Y offset relative to a hypothetical perfectly flat surface. This is a **necessary-but-not-sufficient** test for a flat, uniform badlands plateau, *not* full Minecraft worldgen.
- `--mode xz` ignores Y completely (even more optimistic; useful for reverse engineering, not validation).
- Searches a user-specified range of **64-bit world seeds** at chunk `(0,0)`; supports multithreading and incremental CSV output for candidates meeting `--report`.

## Build on Windows 10/11

Install MSYS2 (https://www.msys2.org/), open the **MSYS2 UCRT64** terminal, then run:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
cd /c/Users/YOUR_USERNAME/Downloads/deadbush15
g++ -O3 -march=native -std=c++20 -pthread deadbush15.cpp -o deadbush15.exe
```

If g++ is already installed, running `build_windows.bat` may suffice. A native Windows binary is **not** included; the included `deadbush15` executable is for Linux only if present.

## Sanity check: recreate the screenshot results

```sh
./deadbush15 --self-test
```

Expected:

```text
PASS published 14-bush seed: reconstructed 14 at (16530130, -25619539)
PASS published 12-bush seed: reconstructed 12 at (5653714, 10797037)
```

World details:

| Published row | World seed | First bush (X,Z) | Index |
|---|---|---|---|
| 14 | `-6825794747614541757` | `16530130, -25619539` | 75 |
| 12 | `923652124993224547` | `5653714, 10797037` | 75 |

To inspect the 14-bush feature simulation in detail:

```sh
./deadbush15 --inspect --seed -6825794747614541757 --chunk-x 1033133 --chunk-z -1601222 --radius 1 --index 75 --mode flat
```

The inspector prints each row of assumed valid placements as `#` on a local X-Z grid. The Y coordinate is not reconstructed.

## Search for 15

For a small reproducible check:

```sh
./deadbush15 --search --start 0 --count 1000000 --index 75 --threads 8 --radius 1 --mode flat --report 8 --target 15 --output candidates.csv
```

For a longer run on your machine (searches seeds 1,000,000 through 100,999,999):

```sh
./deadbush15 --search --start 1000000 --count 100000000 --index 75 --threads 8 --radius 1 --mode flat --report 10 --target 15 --output candidates.csv
```

*Start values and counts are unsigned 64-bit integers.* Use consecutive disjoint `--start` ranges to distribute work among machines. `--count` is a fixed batch size; there is no automatic durable resume offset. CSV hits flush as they are found. The progress display prints every ~5 seconds on long jobs; Ctrl+C stops the job. If stopped early, restart the same range unless you have separately tracked completed batches. `--report` limits what gets written; the best score still prints at the end.

On 8 logical CPU threads in the test environment, a **5 million seed radius-0** scan achieved ~2.43 million world seeds/s, with top *unverified* row 7. A **1 million seed radius-1** scan achieved ~306,000 seeds/s, with top *unverified* row 8. Your speeds will vary.

`--radius 0` is about 8x faster, but misses lines requiring multiple decoration chunks. For record hunting use `--radius 1`.

## Critical limitations and verification

1. **Terrain and biome not generated.** The program assumes every candidate dead-bush position is in suitable badlands at a compatible height, and that the feature is eligible. Many candidate seeds are false positives.
2. **Fixed chunk search location.** The batch scanner checks the vicinity of chunk `(0,0)`, not every possible coordinate in a world. That is a search technique, not a completeness guarantee.
3. **The two screenshot matches validate the algorithm on two inputs, not every biome edge case.** The order of filtered placements can diverge near biome transitions / variable terrain. Earlier or other features can interfere with blocks.
4. `--mode xz` deliberately ignores height. It cannot establish a record.
5. The outputs do not model how other placed features may overwrite plants.

### How to verify a high-scoring candidate

Open a new **unmodified Minecraft Java 26.3** world using the full signed 64-bit `seed_signed` from the CSV. Check the area around the reported start position. The CSV records are relative to the origin chunk at (0,0), so `start_x`/`start_z` are absolute there. Check biome/height; in vanilla, use `/tp @s <x> 120 <z>` and inspect the final world blocks. If no badlands or no dead-bush row appears, **it is not a valid result**.

If you want to search a different fixed chunk for already-known world seeds, `--inspect --seed ... --chunk-x ... --chunk-z ... --radius 1` reconstructs its assumed placements. To scan world seeds at non-origin chunks requires extending the search runner to pass the chosen chunk center into the evaluator.

## Technical references

- Minecraft 26.3 `ChunkGenerator.applyBiomeDecoration` and `WorldgenRandom.setDecorationSeed` / `.setFeatureSeed` via Mojang-mapped 26.3 source (mc-packet-reference.netlify.app).
- `VegetationPlacements.PATCH_DEAD_BUSH_BADLANDS`: `worldSurfaceSquaredWithCount(20)` + `CountPlacement.of(4)` + `OffsetPlacement.ofTriangle(7,3)` + air filter.
- `VegetationFeatures.DEAD_BUSH`: constant-state `SimpleBlockFeature`.
- `BitRandomSource` inherited from `WorldgenRandom` and Xoroshiro128++ / seed expansion.

This independent C++ reimplementation is for research and intentionally does not include Minecraft proprietary game files.