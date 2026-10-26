# Codex mission V12: research and implement a REAL Minecraft 26.3 seedfinder

You are the lead world-generation reverse engineer and performance engineer for this Git repository. DO THE IMPLEMENTATION, not just a plan. Research primary sources and existing efficient seedfinding techniques, then replace the broken candidate strategy with a code-based approach.

## Objective

Find a naturally generated, uninterrupted horizontal row of at least 15 dead bushes, adjacent in X or Z at one identical Y, in Minecraft Java 26.3 DEFAULT NORMAL Overworld. No single-biome preset, edited blocks or mods. Only claim a discovery when proven by exact vanilla-generated blocks.

Published calibration: seed -6825794747614541757, 14 bushes starting X=16530130 Z=-25619539 proceeding +X; seed 923652124993224547, 12 bushes starting X=5653714 Z=10797037 proceeding +X. Independent validation matters more than trusting inherited code. Observed false positives: seed 124767051 (V9 predicted 17, actual 1), 178096580 (predicted 15 strict, manually found no line), and seeds 114006365, 134574834, 176410742 (V10 exact results 2,1,2 on predicted lines). V4 GPU reached ~11.25M RNG location checks/sec with AMD RX 7800 XT yet missed the real placement problem.

## First, safely migrate project to the D: drive

Your starting directory is likely C:\Users\ASUS\Deadbush15\deadbush15. Find actual repository root with git rev-parse --show-toplevel and inspect status. Move the primary working checkout to D:\Yes\Deadbush15 (not directly into an occupied D:\Yes root). Preserve Git history and branch deadbush15, tracked source, necessary vendor dependencies, useful untracked CSV/checkpoints, and current settings. Check disk availability and do not overwrite existing files; if destination exists, inspect it before deciding a safe alternative. Do NOT delete C: originals: leave them as recoverable backup. Avoid needless transfer of huge disposable server-generated worlds, but do not delete their originals or any user data. Confirm migration with git status and representative file comparisons. Continue all work on D: and update hardcoded paths in scripts/README. If D: unavailable, explain and continue on C:, without wasting the session.

## Learn genuine modern seedfinding first; leave a concise technical research record

Study authoritative sources: Mojang 26.3 vanilla mapped classes/source/runtime behavior, current and pinned xpple/cubiomes source, trusted open-source Minecraft seedfinding implementations, and credible technical explanations. Research:
- Modern Xoroshiro128++ seeding and decoration RNG, complete dead-bush placed-feature call chain, global decorator step/index for 26.3, and possible biome-dependent sequence consumption. Do not assume feature index 75 for every situation without verifying.
- Normal-world Badlands distribution, climate/density/noise, accurate surface heightmaps, red sand/terracotta/sand support rules, neighboring decorators and offset Y rules, full chunk/biome boundaries.
- Seed-constraint reduction, bidirectional/partial seed search if mathematically valid, GPU or CPU SIMD bitset filtering, search-space locality and smarter Badlands area sampling. Modern RNG is NOT automatically an old 48-bit Java LCG structure-seed problem.
- Whether exact worldgen can run in-process through native code or a persistent Java CLI using genuine Minecraft classes, without starting a dedicated server for every candidate.
Write RESEARCH_V12.md with precise sources, class names/functions and confirmed vs hypothesized parts. Do not stop at research.

## Audit and rebuild

Read the existing GPU V4 and V5, biome V7, approximate terrain V9, server-per-seed V10, full saved-chunk V11, actual results and scripts. Previous ranking failed: V9 strict 14 predicted but 2 bushes generated; 61 V11 seeds were queued after 9 early worlds mostly yielded maximum 2-3 actual bushes. Height guesses, XZ-only RNG coincidences and dy=0 lines are not enough. Starting a fresh vanilla server per seed is NOT an acceptable hot-path design. Do not repeat the V10/V11 loop as the alleged solution.

Implement V12:
1. Establish exact 26.3 feature RNG draw order and all decoration, terrain and block survival conditions from code, especially initial feature position/heightmap, multiple placements and triangle offsets.
2. Design and implement a high-throughput layered native/GPU or in-process Java worldgen finder. Cheap mathematically necessary GPU/OpenCL or SIMD constraints first, normal-world Badlands and useful terrain checks next, and precise in-process block/placement validation for survivors. Prefer proven necessary prefilters to heuristics that silently discard real records. Consider seed+coordinate strategies rather than uniform random chunks.
3. Search for >=15 true same-Y adjacent dead bushes, not simply predicted XZ run >=15. Output seed, exact X/Y/Z, direction, run, evidence and reproducible commands ONLY when validation backs it. Provide restart/resume, useful output CSV and meaningful throughput measurements.
4. Calibration: independently recover the real 14-block published row end-to-end. A real 15-long subset of a 16/17 pattern must count. Run several previously failed candidate seeds to expose old false positives. Negative coordinates and chunk-border rows must work.
5. If full exact block generation cannot be implemented feasibly in this session, implement the strongest correct, reusable stages toward it and label all approximations honestly. Never invent a confirmed seed. V11 full-block scanning may be a final independent oracle, but not the per-seed search engine.

## Execution constraints

Prioritize coding over endless planning or repeated test cycles. Run ONE small correctness comparison against independent vanilla 26.3 and ONE meaningful representative benchmark after substantive implementation. Do not launch multi-million-seed tests, dozens of ephemeral servers, or long benchmark matrices without concrete need. Use AMD RX 7800 XT gfx1101 16GB and Windows PowerShell/MSYS2 UCRT64 toolchain. Preserve previous findings/checkpoints and backup. Make focused commits on deadbush15 branch. Write README_V12.md with build, run and resume commands rooted at D:\Yes\Deadbush15 plus limitations.

Definition of done: useful working code-based seedfinder improvements, reproduced calibration, proof of correctness or clearly stated gaps, throughput measurement and a command I can run. A theoretical report, more manual teleport suggestions, or a batch of vanilla server startups are NOT success.

Start executing immediately. Decide engineering details yourself and implement the best defensible version.