# SingleBiomeRadiusFinder

GPU search for Minecraft Beta 1.7.3 seeds whose biome around a chosen center remains unchanged for as large a radius as possible. The default target is a Euclidean 432-block disk centered at `(0,0)`.

## What it searches

The finder reproduces Beta 1.7.3's climate pipeline directly:

- temperature: `seed * 9871`, 4 simplex octaves
- rainfall: `seed * 39811`, 4 simplex octaves
- climate blend: `seed * 543321`, 2 simplex octaves
- legacy 64x64 quantized biome lookup behavior

No terrain chunks are generated during the search.

The GPU scout tests 64 points on concentric rings every 32 blocks. Candidates are then exact-checked from the center outward at every integer block position inside the disk. Every seed that reaches the target ring in the scout is mandatorily exact-checked, so a genuine full target-radius hit cannot be discarded by the scout.

`[PROBE RECORD]` is only a cheap scout record. `[RECORD]` is an exact block-level result for a candidate that was sent to the verifier. `[JACKPOT]` means every integer block coordinate in the requested disk has the same biome as the center.

Exact record candidates are written to `single_biome_radius_hits.csv`.

## Build and run on AMD Windows

From the repository root:

```powershell
.\scripts\run-single-biome-radius.ps1 -Rebuild
```

That builds `build\native\amd\SingleBiomeRadiusFinder.exe` and starts the default 432-radius search.

Useful examples:

```powershell
# Search forever for a 432-radius hit around 0,0
.\scripts\run-single-biome-radius.ps1

# Search a larger target
.\scripts\run-single-biome-radius.ps1 -Target 512

# More exact candidates per batch (slower, better running-record coverage)
.\scripts\run-single-biome-radius.ps1 -TopExact 4

# Reproduce/resume a run
.\scripts\run-single-biome-radius.ps1 -UseSequence -Sequence 123456789 -StartAttempt 100000000
```

The executable also has an exact verifier for a known world/center:

```powershell
.\build\native\amd\SingleBiomeRadiusFinder.exe --verify-seed -123456789 --center-x 37 --center-z -82 --target 432
```

## Beta 1.7.3 spawn caveat

The original Beta 1.7.3 initial-spawn search begins at `(0,0)` but, when that column is not a valid sand spawn, its random walk uses the World's ordinary `Random` instance rather than a RNG initialized from the world seed. Therefore the final spawn X/Z is not, in general, a pure function of the world seed.

For seed-only searching the finder consequently defaults to `(0,0)`. After creating a promising world, read its actual spawn X/Z and use `--verify-seed ... --center-x ... --center-z ...` to answer the literal "432 blocks from this world's spawn" question exactly.

## Direct executable options

```text
--target N
--center-x N
--center-z N
--batch N
--top-exact N
--sequence N
--start-attempt N
--max-attempts N
--status-seconds X
--log PATH
--continue-after-hit
--verify-seed SEED
```
