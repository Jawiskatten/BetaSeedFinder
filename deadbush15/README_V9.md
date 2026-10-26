# Deadbush15 V9 — terrain-aware *candidate ranking* (Minecraft Java 26.3 normal Overworld)

The V4 GPU scans hypothetical decoration RNG alignments, and V7 removes most candidates not in Badlands. The V5 and V8 rankers evaluate only the zero-height-offset (flat ground) hypothesis. V9 adds Cubiomes **approximate Overworld surface height** to improve candidate ranking **without opening Minecraft or generating server worlds**.

## What V9 actually computes

For each candidate row, V9 exactly replays the feature-index-75 decoration RNG used by the earlier code, preserving for every attempt:

- The original heightmap sample X/Z (the `Count(20)` attempt position).
- The final block X/Z after the four offsets.
- The Y offset of that placement.

It queries the `mapApproxHeight` API of the **same pinned xpple/cubiomes Minecraft-26.3 library used by V7**, with quart coordinates and per-seed cache. It calculates approximate planting height at the base plus the Y offset, checks approximate terrain at the final block X/Z, and scores the longest row compatible with a single Y-level. Scoring uses both a 2-block and a permissive 6-block tolerance; these tolerances are heuristic, **not Minecraft rules**. Candidates are ranked by lenient score, strict score and zero-offset run. Output includes seed, coordinates and teleport command.

**V9 is not an exact Minecraft terrain generator.** Cubiomes' approximate terrain height does not reconstruct block-level red sand / terracotta / air, surface top, structures, caves, neighboring decorators, or actual survival of the dead-bush feature. V9 cannot *confirm* a 15-bush record and cannot reliably reject every false positive. In particular, neither of the two observed false positives is guaranteed to be eliminated. It is an experiment to improve ranking before implementing exact blocks.

## Build in PowerShell

    cd D:\Yes\Deadbush15\deadbush15
    git pull origin deadbush15
    powershell -ExecutionPolicy Bypass -File .\build_v9.ps1

This reuses `vendor/cubiomes` and its already-built `libcubiomes_static.a` from V7. It compiles in C++17 (avoids the Cubiomes `lerp` name conflict) and runs an RNG calibration that verifies the 14- and 12-bush reference placements. The output prints V9 terrain scores for those two references and the two known 8-RNG false positives. Height scores themselves **cannot** be asserted against the true worlds without actual-height data.

## Score the existing 3,433 biome-compatible candidates

    powershell -ExecutionPolicy Bypass -File .\rank_v9.ps1

This reads `v7_badlands.csv` and writes `v9_terrain_ranked.csv`. It does **not** re-run the 100M GPU scan or V7 filter.

## Score V8's 16 flat candidates (optional)

    powershell -ExecutionPolicy Bypass -File .\rank_v9.ps1 -InputCsv v8_flat_s300000000_n2000000_l4_r7.badlands.csv -OutputCsv v9_flat_ranked.csv

## Interpreting results

- `approx_strict_run` — longest run using height tolerances of 2 blocks.
- `approx_loose_run` — longest run using tolerances of 6 blocks.
- `dy0_run` — longest run with zero offset, matching V5's flat score.
- `approx_y` — **estimated** candidate Y in the score, not a confirmed real Minecraft coordinate.
- `verification=APPROX_TERRAIN_ONLY` — this finder **did not** generate or examine real blocks.

For actual verification, a saved unmodified Minecraft world must have the candidate chunks generated, and a 26.3-compatible Anvil reader must confirm all blocks share the same Y and eligible biome. If all V9 scores remain low, that is evidence to improve the search before brute forcing billions more seeds. An exact terrain/block stage is still needed to meet the record-finding goal.

## Verify actual blocks from a NORMAL 26.3 singleplayer world (no server)

After creating a **normal/default** Minecraft Java 26.3 world using a candidate seed, visit the candidate with the `tp_command`, allow terrain to generate, then **Save and Quit**. V9 can inspect those saved chunk blocks directly:

    py -3 verify_normal_v9.py --world "$env:APPDATA\.minecraft\saves\MyWorld" --csv v9_terrain_ranked.csv --output v9_saved_world_verified.csv

The verifier requires the world-generation settings to prove a **normal** noise Overworld (not single-biome), a matching seed, and fully saved candidate chunks. It reports the longest **actual same-Y dead-bush run** and the same-Y run with Badlands-family biomes. `BLOCKS_MATCH_BADLANDS` with `actual_badlands_same_y >= 15` is strong saved-block evidence, but the world must be unmodified and naturally generated for a record claim. `MISSING_CHUNKS` means visit the coordinates and save the world; it is not a false-positive result. This script imports the existing V6 save/NBT reader and V4 Anvil reader.