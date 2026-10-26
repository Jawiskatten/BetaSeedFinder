# Deadbush15 V10 — unattended *actual vanilla block* verification

V8/V9 guessed terrain and led to many false positives. V10 **does not estimate terrain** for verification. It launches the **official Mojang Java 26.3 dedicated server**, separately for each promising input seed, force-loads the row and nearby chunks, saves and stops, and reads the **actual world-generated blocks** using the existing 26.3-compatible Anvil/NBT verifier.

**What this changes:** no hand-created worlds, no teleporting, no screenshots and no human checking. Results are incremental in `v10_verified.csv`. Temporary normal worlds are deleted after negatives and kept if an actual Badlands dead-bush row reaches the desired length.

**What it does NOT do:** V10 is not an exact GPU seed finder. You still need the earlier GPU search to propose rows, and Minecraft dedicated-server startup/terrain generation is relatively expensive per seed. It does not magically make any of the V9 false positives real. An actual found row still deserves independent checking.

## One-command PowerShell workflow

Run from `D:\Yes\Deadbush15\deadbush15`:

    git pull origin deadbush15
    py -3 test_auto_verify_v10.py
    py -3 auto_verify_v10.py --dry-run --csv v9_ranked.csv
    py -3 auto_verify_v10.py --csv v9_ranked.csv --download-server --accept-eula --min-strict 12 --max-seeds 3 --exclude-seeds 124767051,178096580

The last command downloads **exactly Java 26.3 server.jar from Mojang**, verifies its Mojang SHA-1, and automatically searches your `.minecraft/runtime` for the bundled **Java 25+** runtime, otherwise uses `java` in PATH. **Only pass `--accept-eula` after reading and agreeing to https://aka.ms/MinecraftEULA**. Mojang's server software EULA applies.

If Java isn't found, locate the launcher-provided `java.exe` or install Java 25 and use `--java "C:\path\to\java.exe"`. If you already have the **official 26.3** server jar, omit `--download-server` and use `--server-jar "C:\path\to\server.jar"`.

Defaults: at most 3 distinct seed worlds, V9 strict score >=12, claimed RNG XZ run >=15, 2 GB JVM heap, 3-minute startup timeout and 3-minute chunk-generation timeout per seed. Increase limits deliberately, e.g. `--max-seeds 30 --min-strict 0`. No manual teleport needed. A 3-minute timeout isn't a guarantee a seed was checked; timeouts get **ERROR**, never a false negative.

Each temporary dedicated server listens only on localhost with a random OS port, disables authentication for its isolated loopback-only test environment, whitelists connections, and cleans up world data after no hit. **Don't expose its port to the internet.** Temporary files in `.v10_work`, Mojang jar in `.v10_runtime`. `--keep-worlds` preserves every scratch world for debugging and uses substantial storage.

## Correctness and limitations

- Candidates are sorted by `approx_strict_run` descending for efficiency; this is still just a *priority*, not validation.
- A world with fully generated requested chunks yields `NOT_VERIFIED`, `WRONG_BIOME`, or `BLOCKS_MATCH_BADLANDS`; the saved block reader reports actual same-Y consecutive bushes on the submitted row.
- `MISSING_CHUNKS`, `INCOMPLETE_CHUNKS` and `ERROR` mean the candidate was **not conclusively checked**. They can be retried; V10 will skip completed rows, not errors.
- The verification step doesn't prove that a saved block is natural if you use an edited/custom seed world. V10 itself starts a fresh Mojang server with normal terrain and no datapacks or customizations, so its own results are strong evidence, subject to checking the actual server version and logs.
- V10 is intended for a small shortlist. Running an exact vanilla world for millions of GPU seed candidates would be much too slow; a proper high-throughput solution still requires integrated terrain/biome logic or a different search strategy.

## Testing

    py -3 test_palette_v9.py
    py -3 test_auto_verify_v10.py

These are **offline synthetic tests**, not a completed real Mojang server execution. The first true 26.3 dedicated-server run must be validated on a host with Java 25 and the official jar. Results are never presented as confirmed before real world data is read.
