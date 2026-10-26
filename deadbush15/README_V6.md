# Deadbush15 V6 — Single Biome: Badlands (Minecraft Java 26.3)

This version **uses normal noise terrain**, not Superflat. It uses the existing V4 GPU code for fast decoration-RNG scanning, the existing V5 CPU code to rank exact-row offset alignment, and a **new, non-server save verifier** that reads the actual blocks and biome source in a Minecraft 26.3 singleplayer world.

**Truth in labeling:** The GPU never generates terrain. This pipeline eliminates the *wrong-biome problem only when you actually create a Single Biome > Badlands world*. It does **not** eliminate uneven terrain, missing suitable surfaces, feature interactions, or false RNG candidates. No 15-bush world has been confirmed.

## Start here: calibrate against a known published row

In Minecraft Java **26.3**, create a NEW world with **World Type: Single Biome → Customize: Badlands**, keep **normal terrain** (not Superflat), enable cheats, set **seed `-6825794747614541757`**. Enter spectator mode and teleport:

```mcfunction
/gamemode spectator
/tp @s 16530130 160 -25619539
```

The RNG code reconstructs 14 X-position blocks starting there in a default-world badlands biome. **Check that those bushes also exist in the new Single Biome preset before assuming the decoration feature index remains equivalent across presets.** If they don't, don't launch a long GPU search: the placement-order assumptions need re-calibrating.

## Update + compile once (Windows PowerShell)

```powershell
cd D:\Yes\Deadbush15\deadbush15
git pull origin deadbush15
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v4.cpp -o deadbush15_v4.exe
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v5.cpp -o deadbush15_v5.exe
.\deadbush15_v5.exe --self-test
.\deadbush15_v4.exe --gpu-self-test
```

The V6 driver itself is Python standard-library-only.

## GPU scout (one short run first)

```powershell
py -3 single_badlands_v6.py scan --start 210100000 --count 1000000 --mode flat --report 9 --output single_badlands_rng.csv --checkpoint single_badlands.progress
```

`--mode flat` is a stricter, zero-offset decoration RNG heuristic; it **can miss valid rows across uneven ground**. To explore all horizontal alignments, use `--mode xz` with a different checkpoint/output name, but expect many more false positives. The GPU mode works the same as V4 (it is not recalibrated for a different placement-order index).

## Rank candidates and print seed/teleport commands

```powershell
py -3 single_badlands_v6.py prepare --raw-csv single_badlands_rng.csv --min-run 9 --top 10 --shortlist single_badlands_shortlist.csv --output single_badlands_tp.txt
Get-Content .\single_badlands_tp.txt
```

This uses `deadbush15_v5.exe` to annotate the **same reported row**, avoiding V4's misleading global `flat_run` metric. Every seed needs its own **new Single-Biome Badlands** world with cheats enabled. If your shortlist came from `--mode xz`, ranking won't guarantee viability; don't interpret `same_row_flat` as actual heights.

## Verify from a singleplayer save, no server

1. Create one of the candidate worlds using the Single Biome > Badlands preset and its corresponding seed.
2. Spectator teleport using the command in `single_badlands_tp.txt`.
3. Let the area load, then **Save and Quit** Minecraft.
4. Run:

```powershell
py -3 single_badlands_v6.py verify --world "$env:APPDATA\.minecraft\saves\YOUR_WORLD_NAME" --shortlist single_badlands_shortlist.csv --output single_badlands_verified.csv
```

The verifier reads the **actual** 26.3 save layout (`dimensions/minecraft/overworld/region/*.mca` and `data/minecraft/world_gen_settings.dat`). It insists the world-generation settings contain `minecraft:fixed` with `minecraft:badlands`, and that the world seed matches the candidate. It reports (a) longest actual horizontal run at any vertical heights, (b) longest actual run with **identical Y**, and (c) longest actual run in the Badlands biome. `MISSING_CHUNKS` does **not** mean a negative seed — regenerate/load the area and retry.

**Important:** Whether a sloping sequence of bushes qualifies for the Discord record depends on the category's exact rules. Both metrics are reported separately. Saved-world block presence is evidence of generation in that save, not proof that the world is untouched. For a record, corroborate with a fresh world and screenshots.

## Other notes

- Java 26.3 moved worldgen data and overworld region files in 26.1. This V6 verifier corrects V4/V5's outdated paths; older automated-server verifier results should not be treated as authoritative.
- V4 and V5 executables and their GPU kernel must stay alongside `single_badlands_v6.py`.
- Previous searches over default worlds cannot be called confirmed Single-Biome results. The calibration check is particularly important.
- The benchmark should focus on real verified candidates, not inflated `xz` alignments.