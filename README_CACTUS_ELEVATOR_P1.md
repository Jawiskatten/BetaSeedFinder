# Beta 1.7.3 Cactus Spawn Elevator P1

This search targets a population-created collision bridge that can move the player a large vertical distance before the first playable frame.

## Mechanism

1. Beta's initial spawn selector accepts `(0,0)` when the first uncovered block is sand. A clean target has sand at Y63 and air at Y64, so the saved spawn is still `(0,64,0)` even if a floating terrain mass begins a few blocks higher.
2. The client performs its `Building terrain` chunk loads before it creates the player. Those loads populate nearby chunks.
3. In a desert, `ChunkProviderGenerate.populate` performs 10 cactus generator calls. Population chunk `(-1,-1)` can spill a cactus onto `(0,64,0)` because its feature coordinate range reaches the origin.
4. `WorldGenCactus` can create a 1-, 2-, or 3-block cactus. A two-block cactus at Y64..65 can bridge the player's initial feet Y65 into terrain beginning at Y67. A three-block cactus can bridge one block farther.
5. `Entity.preparePlayerToSpawn()` does not search for a nearby safe location. It repeatedly increments the player's Y by exactly one while the player collides. Once the cactus bridges the initial gap into an overhead terrain mass, that loop can continue through the entire mass until the player is clear.

Unlike the lake/freefall experiment, the displacement is therefore not capped by the feature's own height. The cactus only needs to bridge a 1-2 block gap; the natural overhead terrain can supply the rest of the collision column.

## Search pipeline

`RUN_CACTUS_ELEVATOR_P1.ps1` performs three stages:

- **GPU geometry scout:** exact Beta terrain/noise at the origin. Requires desert sand at Y63, an initially collision-free player at feet Y65, and geometry where a hypothetical 2/3-high cactus would produce at least `MinLift` blocks of upward collision push.
- **Exact four-chunk population prefilter:** runs real Beta generation for the minimum chunk neighborhood that populates `(-1,-1)`. Requires a real vanilla cactus stack at `(0,64,0)` and measures the actual isolated-population lift.
- **Authoritative client-startup oracle:** reproduces the exact 17x17 `Building terrain` load order before player creation and measures the final collision-push lift.

The fast and authoritative best-result files are persistent across scout chunks and resume, so the displayed best is run-wide rather than resetting every batch.

## First benchmark

Start small enough to validate candidate rate and runtime:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\RUN_CACTUS_ELEVATOR_P1.ps1 -Count 10000000 -ScoutChunk 1000000 -MinLift 5
```

If the pipeline produces sensible geometry candidates and/or exact cactus bridges, scale the count and increase `MinLift` for record hunting.

## Status

The source mechanics are supported by the exact Beta 1.7.3 client code, but P1 is still an experimental seed search until a candidate is verified in the real client. Do not treat GPU geometry candidates as hits; the authoritative client-startup oracle is the final result.
