# Literal Lava Spawn P2 — AMD GPU overlay

This finder targets one thing: a new Beta 1.7.3 player spawning with their feet
inside a naturally populated lava lake.

## Hard requirements

- the pre-population spawn check accepts sand at exactly world X=0,Z=0;
- the saved world spawn is therefore `(0,64,0)`;
- the new player bounding box begins at world Y=65;
- the lava-lake attempt from population chunk `(-1,-1)` covers `(0,65,0)`;
- the lake's terrain-boundary validation passes;
- both player body blocks are non-solid after the lake edit.

The score then prefers two lava blocks through the player's body and more lava
surrounding the spawn column. Results are written under
`out\lava_spawn_origin_p2`.

Run `BENCHMARK_LITERAL_LAVA_SPAWN_P2_AMD.bat` first. For the real hunt run
`RUN_LITERAL_LAVA_SPAWN_P2_AMD.bat` (100 million seeds).

The important output is `top_overall.csv`. Every row has `qualified=1` and
`lava_body_blocks>=1`.

P2 exactly reproduces the population RNG and lake ellipsoid mask. Its fast GPU
terrain-boundary check does not carve Beta caves, so every hit must still be opened
in vanilla Beta 1.7.3. Cave/lake intersections can reject a small number of apparent
hits in the real game.
