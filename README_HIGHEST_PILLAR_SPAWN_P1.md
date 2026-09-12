# Highest Pillar Spawn P1

Beta 1.7.3 AMD GPU scout for seeds where the saved spawn is exactly X=0,Z=0 and the player is pushed upward onto an isolated 1x1 support block as high as possible.

## Hard gate

- X=0,Z=0 must be a valid Beta sand spawn before population.
- Player collision is simulated upward from feet Y=65 using the raw Beta terrain field.
- The final support block must have air in all 8 neighboring blocks at the same Y, making the top locally 1x1.

## Ranking

1. actual player feet Y
2. consecutive 1x1 pillar depth below the support block
3. vertical drop to the highest adjacent 3x3 terrain

Additional CSV metrics include sampled drop at radius 2 and radius 4 and top-level clearance counts.

## Accuracy note

The GPU scout uses exact raw Beta 1.7.3 terrain/surface math used by the existing finder, but it does not apply every later population edit such as caves/lakes. Verify record seeds in a fresh Beta 1.7.3 world.

## Run

```powershell
.\VERIFY_HIGHEST_PILLAR_SPAWN_P1_INSTALL.bat
.\RUN_HIGHEST_PILLAR_SPAWN_P1_AMD.bat
```

The default run checks 100,000,000 seeds and writes leaderboards under `out/highest_pillar_spawn_p1/`.
