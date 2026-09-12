# FloatingIslandSpawn P4 Optimized

P4 keeps the exact Beta 1.7.3 spawn/collision logic from P3 but stops generating a huge terrain window for seeds that can never work.

## Pipeline

1. **Origin-only GPU scout**
   - generated terrain lattice is specialized to `SIZE=1`, `FROM_COARSE=0`
   - exact origin temperature/rainfall, sand noise, stone/depth noise, and cropped Beta density math
   - exact sand spawn gate
   - exact player collision push from feet Y=65
   - hard air-gap gate: 1 or 2 blocks only
   - passing records are compacted on GPU with an atomic append; the host copies only hits

2. **Exact r4 verification only for scout hits**
   - full terrain is generated only for the ~rare scout candidates
   - candidates are generated as one GPU batch
   - all final-density lattices are copied to host in one bulk transfer
   - 6-connected component tracing rejects a candidate immediately if it reaches the guaranteed-solid cropped ground below Y56
   - contained components are genuine floating terrain under the same raw-terrain model as P3

3. **Adaptive verification window**
   - r4 uses approximately +/-48 blocks
   - only a component that remains disconnected from ground but reaches the horizontal boundary is retried at r8 (~+/-96)
   - only a still-unresolved r8 component is retried at r12 (~+/-144)

This avoids spending r8/r12 work on ordinary ground-connected false positives while no longer blindly throwing away a potentially huge floating island at the r4 boundary.

## Ranking files

`top_largest.csv` ranks strict component block count first, footprint second, top surface third.

`top_island_like.csv` favors wide two-dimensional islands by using footprint x min(spanX, spanZ), strongly demoting 17x1 Terraria-style walls without deleting them from the normal leaderboard.

`verified_all.csv` stores every unique verified floating spawn found by the run.

## Launchers

Desktop-friendly search:

```bat
RUN_FLOATING_ISLAND_SPAWN_P4_OPTIMIZED_AMD.bat
```

Maximum throughput / overnight:

```bat
RUN_FLOATING_ISLAND_SPAWN_P4_MAX_SPEED_AMD.bat
```

Resume the last P4 run with the corresponding resume BAT.

## Correctness guard

Every scout hit is re-gated from the independently generated full terrain lattice before component verification. If origin-only metadata and full-window metadata disagree, P4 stops instead of silently accepting or rejecting seeds.

Known seed `6430576860599818994` must pass both self-tests as the verified 9-block 1x1 floating pillar (`airGap=1`, `playerFeetY=74`, `supportY=73`).

Population features such as caves/lakes/trees are still outside the raw-terrain component proof, just as in P3. Finalists should still be opened in actual Beta 1.7.3.
