# Deadbush15 V7: NORMAL-WORLD Badlands biome gate

V7 is a native biome-prediction stage after the V4 GPU scanner. It uses xpple/cubiomes (https://github.com/xpple/cubiomes), pinned to commit b99f6e472018cab903a096697117aab9d9170a19. The upstream library has an MIT license (see vendor/cubiomes/LICENSE after cloning). Its MC_26_3 normal-world climate generator checks the reported XZ lines against Badlands biomes at multiple plausible surface heights.

**This is NOT terrain/block verification, nor Single Biome generation.** Output is marked BIOME_COMPATIBLE, not "verified".

## Build on Windows (PowerShell)

    cd D:\Yes\Deadbush15\deadbush15
    git pull origin deadbush15
    powershell -ExecutionPolicy Bypass -File .\build_v7.ps1

Build installs CMake/Ninja if missing, clones the pinned Cubiomes fork to vendor/cubiomes, compiles both the library and the new exe, and refuses to continue if the published 12/14 seed reference locations do not pass its Badlands calibration.

## Filter the 100M-seed GPU results you already have

    .\badlands_filter_v7.exe --input gpu_v4_candidates.csv --output v7_badlands.csv --rejects v7_rejected.csv
    .\deadbush15_v5.exe --rank --csv v7_badlands.csv --min-run 15 --top 30 --output v7_shortlist.csv
    Import-Csv .\v7_shortlist.csv | Select-Object -First 10 | Format-Table -AutoSize

Or run this whole sequence:

    powershell -ExecutionPolicy Bypass -File .\filter_existing_v7.ps1

Rejected samples are intentionally preserved: the filter checks the actual candidate row at heights 64,72,80,96,112,128,144,160,192. It requires Badlands family for every reported position at at least one sampled Y. Heights outside this range or near biome boundaries can be missed. First test 3 row anchors to avoid full-line climate work when obviously unsuitable.

## Probes

    .\badlands_filter_v7.exe --probe -6825794747614541757 16530130 -25619539
    .\badlands_filter_v7.exe --probe 121919808 -10046036 8654613

The first is the published 14-bush normal-world reference. The second was among our known false positives.

## Only then search a new GPU range

    .\deadbush15_v4.exe --gpu-search --start 300000000 --count 1000000 --batch 8192 --locations 4 --spread-span 1000000 --mode xz --prefilter 15 --report 15 --target 15 --output v7_gpu.csv --checkpoint v7_gpu.progress --resume
    .\badlands_filter_v7.exe --input v7_gpu.csv --output v7_gpu_badlands.csv --rejects v7_gpu_rejected.csv

Note: Actual dead bushes also require terrain height, correct surface block, feature ordering and other-world-feature interactions. The classifier alone cannot establish a naturally generated row of 15. GPU speed is unchanged from V4; no new kernel acceleration is claimed.
