# CursedSpawnOrigin P1 — AMD GPU overlay

This finder searches Beta 1.7.3 seeds where the spawn test succeeds immediately at
**world X=0, Z=0**. It does not search for a good coordinate near origin and it does
not move the target after terrain generation.

## Exact fixed-origin gate

For each seed the GPU reproduces:

- the exact Beta climate/biome lookup at `(0,0)`;
- the four-octave sand replacement noise at `(0,0)`;
- the four-octave surface-depth noise at `(0,0)`;
- chunk `(0,0)`'s first three Java `Random` surface-replacement draws;
- exact vertical density interpolation at the origin column.

A seed is saved only when the unpopulated surface block selected by Beta's spawn
check is sand and its Y is at least 63. Both natural desert sand and beach-noise
sand are accepted. `sand_reason` is `2` for desert top and `1` for beach replacement.

## What it scores

All terrain metrics are centered on `(0,0)`, using the native four-block density
lattice within 16 blocks:

- smallest safe walking area around spawn;
- water surrounding the spawn;
- immediate and nearby drops;
- overhang/floating terrain and roof nodes above spawn;
- combinations of several kinds of danger.

The coarse scene metrics are a fast P1 ranking pass. The sand gate itself is exact
for unpopulated Beta terrain. Population features such as lava lakes and dungeons
are not simulated in P1, so inspect the top seeds in vanilla Beta 1.7.3.

## Run

Extract this ZIP into the full `BetaSeedFinder` project folder, then run:

`BENCHMARK_CURSED_SPAWN_ORIGIN_P1_AMD.bat`

For a normal run:

`RUN_CURSED_SPAWN_ORIGIN_P1_AMD.bat`

Results are written under `out\cursed_spawn_origin_p1`. The main boards are:

- `top_overall.csv`
- `top_tiny_safe_area.csv`
- `top_water_prison.csv`
- `top_cliff.csv`
- `top_overhang.csv`
- `top_highest.csv`
- `top_combo.csv`

Every row in every leaderboard has `qualified=1` and refers to spawn X/Z `(0,0)`.

