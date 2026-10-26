# V12 worldgen research record

## Sources and version pins

- The input is Mojang's official 26.3 server bundle already present at `deadbush15/.v10_runtime/server_26.3.jar` (SHA-256 `D052F14D7A173734FBA553711E5B570162E2F2A313267EE31A21B975A679BE64`). Since 26.1, Minecraft ships readable class names in the game jar. I inspected its embedded `META-INF/versions/26.3/server-26.3.jar`, its vanilla `data/minecraft/worldgen` resources, and bytecode with Java 25 `javap`. Release notes: [Minecraft Java 26.3](https://www.minecraft.net/en-us/article/minecraft-java-edition-26-3), [Mojang feedback notes](https://feedback.minecraft.net/hc/en-us/articles/48913133328013-Minecraft-Java-Edition-26-3).
- The bundle is `.v10_runtime/server_26.3.jar` (SHA-256 `D052F14D7A173734FBA553711E5B570162E2F2A313267EE31A21B975A679BE64`). I compared pinned xpple/cubiomes commit [`b99f6e472018cab903a096697117aab9d9170a19`](https://github.com/xpple/cubiomes/tree/b99f6e472018cab903a096697117aab9d9170a19) with current upstream HEAD [`aee74239609f3b371747e4573808c04a72518979`](https://github.com/xpple/cubiomes/tree/aee74239609f3b371747e4573808c04a72518979), dated 2026-10-26. `generator.c` and `biomes.c` are identical; current `rng.h` adds boolean helpers and a short-skip optimization, without changing the random methods used here. The pinned version remains appropriate and is already vendored at `vendor/cubiomes`. Relevant code: `biomes.h` (`MC_26_3`, Badlands IDs), `generator.c` (`setupGenerator`, `applySeed`, `getBiomeAt`, `mapApproxHeight`), and `rng.h` (modern Xoroshiro and Java-compatible bounded-int paths). Cubiomes is useful for high-speed biome/climate and approximate terrain queries; it does not provide final Minecraft block states or exact surface heightmaps.
- The broader seedfinding ecosystem includes [xpple/SeedMapper](https://github.com/xpple/SeedMapper), which embeds cubiomes rather than starting a Minecraft server per seed. Cubiomes and SeedMapper address biome/structure exploration; they do not supply the dead-bush feature's final block survival result.

## Confirmed 26.3 dead-bush call chain

The official Badlands placed-feature JSON is `data/minecraft/worldgen/placed_feature/patch_dead_bush_badlands.json`. It names `minecraft:dead_bush` and orders modifiers as:

1. `CountPlacement(20)`
2. `InSquarePlacement`
3. `HeightmapPlacement(WORLD_SURFACE_WG)`
4. `BiomeFilter`
5. `CountPlacement(4)`
6. `OffsetPlacement` using trapezoid X `[-7,+7]`, Y `[-3,+3]`, Z `[-7,+7]`, plateau 0
7. `BlockPredicateFilter` requiring air

The configured feature is `data/minecraft/worldgen/feature/dead_bush.json`, type `simple_block`, with `minecraft:dead_bush` as the state. `TrapezoidInt.sample` in the symmetric plateau-zero case computes `nextInt(max+1) - nextInt(max+1)`, so X/Z are each `nextInt(8)-nextInt(8)` and Y is `nextInt(4)-nextInt(4)`.

The relevant Java classes/methods, confirmed in the 26.3 jar, are:

- `ChunkGenerator.applyBiomeDecoration`: constructs `WorldgenRandom` with `XoroshiroRandomSource`, sets the decoration seed, obtains the per-step `FeatureSorter.StepFeatureData.indexMapping`, calls `setFeatureSeed`, then `FeaturePlacer.placeWithBiomeCheck`.
- `WorldgenRandom.setDecorationSeed(long,int,int)` and `setFeatureSeed(long,int,int)`: chunk block origin and global feature index feed the decoration RNG; feature seed adds `index + 10000 * step`.
- `FeatureSorter.buildFeaturesPerStep` / `StepFeatureData.indexMapping`: feature index is assigned from the complete biome feature registry/order for a decoration step. It is not simply the local index in an individual biome's JSON.
- `FeaturePlacer.placeWithBiomeCheck` and its modifier pipeline: each placement modifier transforms the complete current position list before the next modifier runs. Thus all 20 square positions are sampled first, then the biome filter selects positions, then the four offset placements are generated for each accepted base. The biome filter consumes no random values, but its accept/reject pattern changes later RNG consumption.
- `InSquarePlacement.modify`, `HeightmapPlacement.modify`, `CountPlacement.count`, `OffsetPlacement.modify`, `BiomeFilter` and `TrapezoidInt.sample` provide the exact modifier behavior above. `OffsetPlacement.modify` samples X, then Y, then Z.
- `DryVegetationBlock.mayPlaceOn` and the `minecraft:supports_dry_vegetation` tag are the survival side to model. `BlockPredicateFilter` separately requires the destination to be air. A real placement still depends on the vanilla world-surface heightmap, final biome, current destination block, support block, and interactions with neighboring features.

The 26.3 Badlands-family biome JSONs all include `patch_dead_bush_badlands` in `vegetal_decoration`; its local list index is 3 in `badlands` and `eroded_badlands`, and 4 in `wooded_badlands`. The feature RNG index is supplied by `FeatureSorter.indexMapping`, so the historic hard-coded 75 is **not independently proven as the global index for every placement context**. V12 exposes an index range for this reason.

## V12 algorithm and guarantees

`deadbush15_v12.cpp` replaces the old per-attempt RNG replay with two necessary-condition layers:

1. Cubiomes 26.3 is queried at quart Y levels from -64 to 320. Every target XZ must share at least one Y where all positions are in a Badlands-family biome. A real same-Y Badlands row must pass this test. This is a necessary biome check only; it does not say terrain or bushes exist.
2. For each chunk that could reach the proposed row, the code exactly replays the vanilla RNG seeding and the first 20 `in_square` positions. It branches on both outcomes of each biome filter and unions possible offset XZ positions. This over-approximates the set of placements, so a row rejected at the supplied feature index cannot come from that modeled placed feature. A pass only says every XZ can be produced under some branch; positions can require incompatible branches, different Y offsets, unsupported blocks, or non-air destination blocks.

The branch state count is bounded by the number of distinct RNG offsets (at most 21 for 20 accepted/rejected bases), not by `2^20`: state merging compares both Xoroshiro words exactly. Negative coordinates use floor division, and chunk origins near row ends are enumerated from the full `[-7,+7]` placement reach.

## Independent RNG comparison

I ran a small harness against Mojang's 26.3 `WorldgenRandom`, `XoroshiroRandomSource`, `setDecorationSeed`, `setFeatureSeed`, and `nextInt` methods, then compared it with the C++ implementation. For seed `923652124993224547`, chunk `(353357,674814)`, index `75`, the Java decoration seed was `2065035730784772755`; feature seed was `2065035730784862830`. The first three square bases and four X/Y/Z offsets per base matched byte-for-byte between Java and C++. This validates the RNG seeding and bounded-draw model, not the feature index or generated world blocks.

The upper-bound self-test also confirms the two published rows are possible at index 75. Existing saved-world results are retained in `v10_verified.csv`: the reported V9 rows for `114006365`, `134574834`, and `176410742` had actual same-Y Badlands runs of 2, 1, and 2. Of the five recorded failed candidates, the V12 XZ upper bound excludes `178096580`; four remain possible, including those three actual false positives. This is a lossless coarse filter, not a substitute for a block oracle.

## Terrain, seed strategy, and unresolved work

- The target should be searched as seed + candidate row coordinates, then tested against exact per-chunk placement streams and Badlands biome constraints. Random full-64-bit world-seed scanning alone wastes effort because a strong XZ run is rare. Reverse-Xoroshiro reduction is not assumed to be a Java 48-bit LCG problem.
- The current implementation does not solve for new seed/coordinate pairs and does not use the GPU. It annotates existing candidate CSV rows. The RX 7800 XT can be useful for SIMD/OpenCL upper-bound screening, but no GPU stage was justified until exact modifier math and correctness were known.
- There is no exact Cubiomes surface-height/block API in the pinned tree. `mapApproxHeight` is a heuristic ranker and was already disproven as a correctness check by saved-world false positives. Vanilla `WORLD_SURFACE_WG`, block-state survival, structure/neighbor effects, and final placed-feature index still need an in-process vanilla integration or another exact implementation.
- A persistent Java worldgen worker is feasible in principle because the official 26.3 classes are directly callable, but it requires registry/data-pack bootstrap, a `WorldGenRegion`/chunk cache, full terrain/noise setup, and safe regeneration across seeds. That integration was not completed here. No per-seed dedicated-server loop is part of V12.

Therefore V12 is useful as a conservative, restartable candidate-stage improvement, but it does not complete discovery of a new 15+ row or independently regenerate the published row's blocks. No new seed is claimed.
