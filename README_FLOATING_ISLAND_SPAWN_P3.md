# Floating Island Spawn P3

Target: a genuine floating raw-terrain component that the Beta 1.7.3 player physically spawns on because it obstructs the normal spawn placement.

Required sequence at X/Z = 0,0:

1. Beta's spawn eligibility selects a sand surface.
2. At least one air block separates that lower spawn-valid surface from an upper solid mass.
3. The upper mass intersects the initial player AABB at feet Y=65 and pushes the player upward.
4. The final support block beneath the player belongs to a solid component that is flood-filled exactly at block resolution.
5. The component must be fully contained within the verification box. Anything touching the verification boundary is rejected because it cannot be proven disconnected from ground.

The known proof seed `6430576860599818994` is used as the deterministic self-test.

## Ranking

Primary: total blocks in the verified floating component.

Tie-breakers: footprint columns, exposed top-surface blocks, then player spawn height.

Leaderboards:

- `top_largest.csv`
- `top_blocks.csv`
- `top_footprint.csv`
- `top_top_surface.csv`
- `top_highest_spawn.csv`

## Run

```bat
RUN_FLOATING_ISLAND_SPAWN_P3_AMD.bat
```

Resume the latest run with:

```bat
RESUME_FLOATING_ISLAND_SPAWN_P3_LAST_RUN.bat
```

Verify/compile only with:

```bat
VERIFY_FLOATING_ISLAND_SPAWN_P3_INSTALL.bat
```

The default run searches 100,000,000 unique-48 candidates at radius 4. The block-component verifier automatically uses the largest safe symmetric radius up to 48 blocks supported by the generated exact terrain lattice.
