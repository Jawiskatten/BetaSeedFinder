#!/usr/bin/env python3
"""V10: automatically generate vanilla Java 26.3 candidate worlds and inspect real dead bushes.

No player, teleport, Minecraft client, or third-party Python module required.
Runs Mojang's 26.3 dedicated server separately for each distinct seed, forces
candidate chunks, saves, then uses our verified 26.3 NBT reader. This trades
speed for accuracy: each new world requires vanilla generation. Nothing here
is a high-throughput exact seed search, and no positive claim is made without
checking actual generated blocks.
"""
import argparse
import csv
import hashlib
import json
import os
import queue
import re
import socket
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zipfile
from collections import defaultdict
from pathlib import Path

from verify_normal_v9 import examine
from verify_world_v4 import World
from scan_real_world_v11 import scan_world

VERSION = "26.3"
MANIFEST_URL = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"
FINISHED = {"BLOCKS_MATCH_BADLANDS", "WRONG_BIOME", "NOT_VERIFIED"}
OUTPUT_FIELDS = ("world_verification", "actual_same_y", "actual_badlands_same_y",
                 "confirmed_y", "actual_best_x", "actual_best_z", "actual_best_y",
                 "v10_seconds", "v10_error", "actual_world_best_run",
                 "actual_world_best_x", "actual_world_best_y", "actual_world_best_z",
                 "actual_world_best_dx", "actual_world_best_dz")


def read_json(url):
    with urllib.request.urlopen(url, timeout=35) as response:
        return json.load(response)


def official_server_jar(target):
    """Download exactly 26.3 from Mojang, verify its published SHA-1."""
    manifest = read_json(MANIFEST_URL)
    version = next((item for item in manifest["versions"] if item["id"] == VERSION), None)
    if version is None:
        raise RuntimeError("26.3 not found in Mojang version manifest")
    detail = read_json(version["url"])
    info = detail["downloads"]["server"]
    if not info["url"].startswith("https://") or not info["url"].split("/")[2].endswith(".mojang.com"):
        raise RuntimeError("Server download URL is not a Mojang HTTPS address")
    target.parent.mkdir(parents=True, exist_ok=True)
    expected = info["sha1"].lower()
    if target.exists() and sha1(target) == expected:
        return target
    download = target.with_suffix(".part")
    try:
        digest = hashlib.sha1()
        with urllib.request.urlopen(info["url"], timeout=90) as response, download.open("wb") as f:
            while True:
                block = response.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
                f.write(block)
        if digest.hexdigest() != expected:
            raise RuntimeError("Official server jar checksum mismatch; refusing to execute")
        download.replace(target)
    finally:
        download.unlink(missing_ok=True)
    return target


def sha1(file):
    digest = hashlib.sha1()
    with file.open("rb") as f:
        for buf in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(buf)
    return digest.hexdigest()


def validate_jar(path):
    if not path.is_file():
        raise RuntimeError(f"Missing server jar: {path}; use --download-server")
    with zipfile.ZipFile(path) as jar:
        try:
            version = json.loads(jar.read("version.json"))["id"]
        except (KeyError, ValueError) as e:
            raise RuntimeError("Cannot identify jar version; supply Mojang's official 26.3 server jar") from e
        if version != VERSION:
            raise RuntimeError(f"Server jar is {version}, expected exactly {VERSION}")


def get_java(configured):
    candidates = [configured] if configured else []
    candidates.append("java")
    if os.name == "nt":
        home = Path(os.environ.get("APPDATA", "")) / ".minecraft" / "runtime"
        if home.is_dir():
            candidates.extend(str(p) for p in home.glob("*/windows-x64/*/bin/java.exe"))
    seen = set()
    for java in candidates:
        if not java or java in seen:
            continue
        seen.add(java)
        try:
            p = subprocess.run([java, "-version"], capture_output=True, text=True, timeout=10)
        except (FileNotFoundError, OSError, subprocess.TimeoutExpired):
            continue
        match = re.search(r'version\s+"?(\d+)', p.stdout + p.stderr)
        if match and int(match.group(1)) >= 25:
            return java
    raise RuntimeError("Minecraft 26.3 requires Java 25+. Set --java PATH_TO_JAVA25.EXE "
                       "or install Java 25; the launcher-bundled Java is also searched.")


def candidates(input_path, min_run, min_strict, max_seeds, completed, excluded=frozenset()):
    with input_path.open(newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    if not rows or not {"seed_signed","run","start_x","start_z","dx","dz"}.issubset(rows[0]):
        raise RuntimeError("CSV must have seed_signed,run,start_x,start_z,dx,dz columns")
    groups = defaultdict(list)
    for row in rows:
        if int(row["seed_signed"]) in excluded:
            continue
        length = int(row["run"])
        strict = int(row.get("approx_strict_run") or 0)
        if length < min_run or strict < min_strict:
            continue
        key = (row["seed_signed"],row["start_x"],row["start_z"],
               row["dx"],row["dz"],row["run"])
        if key in completed:
            continue
        if int(row["dx"]) + int(row["dz"]) != 1 or min(int(row["dx"]),int(row["dz"])) < 0:
            raise ValueError(f"Invalid direction: {row}")
        if not 1 <= length <= 64:
            raise ValueError("Invalid row length")
        x,z = int(row["start_x"]),int(row["start_z"])
        if not (-29999984 < x < 29999984 and -29999984 < z < 29999984):
            raise ValueError("Candidate is outside world border")
        groups[int(row["seed_signed"])].append(row)
    order = sorted(groups, key=lambda seed: (
        -max(int(r.get("approx_strict_run") or 0) for r in groups[seed]),
        -max(int(r["run"]) for r in groups[seed]), seed))
    return [(seed,groups[seed]) for seed in order[:max_seeds]], rows[0].keys()


def record_key(row):
    return tuple(row[k] for k in ("seed_signed","start_x","start_z","dx","dz","run"))


def completed_keys(output):
    if not output.is_file():
        return set()
    with output.open(newline="", encoding="utf-8-sig") as f:
        return {record_key(row) for row in csv.DictReader(f)
                if row.get("world_verification") in FINISHED}


def write_row(output, original_columns, row):
    columns = list(original_columns) + [k for k in OUTPUT_FIELDS if k not in original_columns]
    exists = output.is_file() and output.stat().st_size > 0
    with output.open("a",newline="",encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        if not exists:
            writer.writeheader()
        writer.writerow(row)
        f.flush()


def target_chunks(rows):
    """Get all row chunks, and one bordering chunk in each direction for decoration."""
    chunks = set()
    for r in rows:
        x,z,dx,dz,n = (int(r[k]) for k in ("start_x","start_z","dx","dz","run"))
        x2,z2 = x+dx*(n-1),z+dz*(n-1)
        for cx in range(min(x,x2)//16-1,max(x,x2)//16+2):
            for cz in range(min(z,z2)//16-1,max(z,z2)//16+2):
                chunks.add((cx,cz))
    if len(chunks) > 64:
        raise ValueError("Candidate chunk region unexpectedly large")
    return sorted(chunks)


def server_properties(seed, port=0):
    return "\n".join([
        f"level-seed={seed}",
        "level-name=world",
        "level-type=minecraft:normal",
        "server-ip=127.0.0.1",
        f"server-port={port}",
        "management-server-enabled=false",
        "online-mode=false",
        "white-list=true",
        "max-players=1",
        "view-distance=2",
        "simulation-distance=2",
        "max-tick-time=-1",
        "pause-when-empty-seconds=-1",
        "enable-rcon=false",
        "enable-query=false",
        "enable-command-block=false",
        "allow-nether=false",
        "generate-structures=true",
        "difficulty=peaceful",
        "spawn-protection=0",
        "sync-chunk-writes=true",
        "motd=V10 ephemeral local seed verifier",
        ""]) 


def send(proc, command):
    if proc.poll() is not None:
        raise RuntimeError(f"Vanilla server exited early with code {proc.returncode}")
    proc.stdin.write(command + "\n")
    proc.stdin.flush()


def run_world(server_jar, java, seed, rows, workdir, boot_timeout, gen_timeout, heap_gb):
    """Server console commands generated and executed without any connected players."""
    workdir.mkdir(parents=True, exist_ok=False)
    (workdir / "eula.txt").write_text("eula=true\n",encoding="utf-8")
    # Bind to loopback on a currently-unused port; never expose a test server
    # to the local network or collide with someone's regular Minecraft server.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    (workdir / "server.properties").write_text(server_properties(seed,port),encoding="utf-8")
    logs = []
    messages = queue.Queue()
    command = [java, "-Xms512M", f"-Xmx{heap_gb}G", "-jar", str(server_jar.resolve()), "nogui"]
    started = time.monotonic()
    proc = subprocess.Popen(command,cwd=workdir,stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,text=True,errors="replace",bufsize=1)
    def drain():
        for line in proc.stdout:
            line = line.rstrip()
            logs.append(line)
            if len(logs) > 500:
                del logs[:100]
            messages.put(line)
    thread = threading.Thread(target=drain,daemon=True)
    thread.start()
    chunks = target_chunks(rows)
    success = False
    try:
        # Some 26.3 log messages changed; also probe the console with a say marker.
        ready = False
        last_ping = 0
        until = time.monotonic() + boot_timeout
        while time.monotonic() < until:
            if proc.poll() is not None:
                raise RuntimeError(f"Server exited on boot (exit={proc.returncode}): " + " | ".join(logs[-8:]))
            try:
                msg = messages.get(timeout=1)
                if "Done (" in msg or "[Server] V10_BOOT_OK" in msg:
                    ready = True
                    break
            except queue.Empty:
                pass
            now = time.monotonic()
            if now - last_ping >= 8:
                send(proc, "say V10_BOOT_OK")
                last_ping = now
        if not ready:
            raise RuntimeError("Server failed boot readiness check: " + " | ".join(logs[-8:]))
        # Mark all needed chunks as force-loaded, then check they have actually loaded.
        for cx,cz in chunks:
            send(proc, f"execute in minecraft:overworld run forceload add {cx*16} {cz*16}")
        confirmed = set()
        until = time.monotonic() + gen_timeout
        last_check = 0
        while len(confirmed) < len(chunks) and time.monotonic() < until:
            if proc.poll() is not None:
                raise RuntimeError("Server exited during generation: " + " | ".join(logs[-8:]))
            now = time.monotonic()
            if now-last_check >= 3:
                for i,(cx,cz) in enumerate(chunks):
                    if i not in confirmed:
                        send(proc,f"execute in minecraft:overworld if loaded {cx*16+8} 80 {cz*16+8} run say V10_CHUNK_OK_{i}")
                last_check = now
            try:
                line = messages.get(timeout=1)
            except queue.Empty:
                continue
            match = re.search(r"\[Server\] V10_CHUNK_OK_(\d+)(?:\s|$)", line)
            if match:
                confirmed.add(int(match.group(1)))
        if len(confirmed) != len(chunks):
            raise RuntimeError(f"Only {len(confirmed)}/{len(chunks)} forced chunks became loaded within {gen_timeout}s. "
                               + " | ".join(logs[-8:]))
        send(proc,"save-all flush")
        send(proc,"stop")
        try:
            proc.wait(timeout=75)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError("Timed out during vanilla save/stop") from exc
        if proc.returncode != 0:
            raise RuntimeError(f"Server exited with code {proc.returncode}: " + " | ".join(logs[-8:]))
        success = True
    finally:
        if proc.poll() is None:
            try:
                send(proc,"stop")
                proc.wait(timeout=12)
            except (OSError,RuntimeError,subprocess.TimeoutExpired):
                proc.kill()
                proc.wait(timeout=10)
    return success, time.monotonic()-started


def locate_best_actual_row(world, row):
    """Locate real same-height Badlands sub-row, including its true X/Z/Y start."""
    x,z,dx,dz,n = (int(row[k]) for k in ("start_x","start_z","dx","dz","run"))
    positions = [(x+i*dx,z+i*dz) for i in range(n)]
    found = [world.dead_bush_y(px,pz) for px,pz in positions]
    if any(item is None for item in found):
        return 0,"","",""
    from verify_normal_v9 import BADLANDS
    best=(0,"","","")
    levels = set().union(*found)
    for y in levels:
        consecutive=0
        for i,((px,pz),states) in enumerate(zip(positions,found)):
            good = y in states and world.biome_at(px,y,pz) in BADLANDS
            consecutive = consecutive+1 if good else 0
            if consecutive>best[0]:
                start_x,start_z=positions[i-consecutive+1]
                best=(consecutive,start_x,start_z,y)
    return best


def verify_world(folder, seed, rows):
    # 26.3 may store overworld under new dimension path. Read saved chunk blocks,
    # not cubiomes or server console predictions.
    modern = folder / "world/dimensions/minecraft/overworld"
    world = World(modern if (modern / "region").is_dir() else folder / "world",
                  "minecraft:dead_bush")
    # Scan *all* generated chunks for actual same-Y rows. This may find
    # a record outside the RNG candidate line, including across chunk edges.
    full_scan = scan_world(folder / "world", min_run=2, top=1, require_badlands=True)
    best_world = full_scan["rows"][0] if full_scan["rows"] else {}
    results = []
    for row in rows:
        status,any_count,bad_count,levels = examine(world,row)
        best,bx,bz,by = locate_best_actual_row(world,row)
        if best != bad_count:
            raise RuntimeError(f"Internal verifier disagreement: row={row} best={best}, badlands={bad_count}")
        results.append({**row,"world_verification":status,"actual_same_y":any_count,
                        "actual_badlands_same_y":bad_count,"confirmed_y":levels,
                        "actual_best_x":bx,"actual_best_z":bz,"actual_best_y":by,
                        "actual_world_best_run":best_world.get("run",0),
                        "actual_world_best_x":best_world.get("start_x",""),
                        "actual_world_best_y":best_world.get("start_y",""),
                        "actual_world_best_z":best_world.get("start_z",""),
                        "actual_world_best_dx":best_world.get("dx",""),
                        "actual_world_best_dz":best_world.get("dz","")})
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv",type=Path,default=Path("v9_ranked.csv"))
    parser.add_argument("--output",type=Path,default=Path("v10_verified.csv"))
    parser.add_argument("--min-run",type=int,default=15)
    parser.add_argument("--min-strict",type=int,default=12,
                        help="Heuristic V9 strict-score minimum, NOT a proof")
    parser.add_argument("--max-seeds",type=int,default=3,
                        help="Limit world creations; pass a larger value if desired")
    parser.add_argument("--target",type=int,default=15)
    parser.add_argument("--exclude-seeds",default="",help="Comma-separated seeds already checked manually")
    parser.add_argument("--java",default=None,help="Path to Java 25+ executable, auto-detected otherwise")
    parser.add_argument("--server-jar",type=Path,default=Path(".v10_runtime/server_26.3.jar"))
    parser.add_argument("--download-server",action="store_true",
                        help="Download exactly 26.3 from Mojang and SHA-1 verify it")
    parser.add_argument("--accept-eula",action="store_true",
                        help="You have read and agree to the Minecraft EULA; required for running the vanilla server")
    parser.add_argument("--dry-run",action="store_true",help="Preview seed plan, no downloads/server/EULA")
    parser.add_argument("--boot-timeout",type=int,default=180)
    parser.add_argument("--gen-timeout",type=int,default=180)
    parser.add_argument("--heap-gb",type=int,default=2)
    parser.add_argument("--keep-worlds",action="store_true",
                        help="Retain every generated world in .v10_work (uses lots of disk)")
    parser.add_argument("--workdir",type=Path,default=Path(".v10_work"))
    args = parser.parse_args(argv)
    if args.max_seeds < 1 or args.heap_gb < 1 or args.heap_gb > 16 or not 1 <= args.target <= 64:
        parser.error("Invalid --max-seeds, --heap-gb or --target")
    if args.boot_timeout < 20 or args.gen_timeout < 20:
        parser.error("Timeouts must be at least 20 seconds")
    try:
        excluded = {int(v.strip()) for v in args.exclude_seeds.split(',') if v.strip()}
    except ValueError:
        parser.error("--exclude-seeds must be comma-separated integers")
    selected,columns = candidates(args.csv,args.min_run,args.min_strict,args.max_seeds,
                                  completed_keys(args.output), excluded)
    print(f"Selected {len(selected)} distinct unverified world seeds from {args.csv}.",flush=True)
    for seed,rows in selected:
        print(f"  seed={seed} rows={len(rows)} strict={max(int(r.get('approx_strict_run') or 0) for r in rows)} "
              f"best_XZ={max(int(r['run']) for r in rows)} force_chunks={len(target_chunks(rows))}",flush=True)
    if args.dry_run or not selected:
        return 0
    if not args.accept_eula:
        parser.error("Vanilla server EULA consent required. Read https://aka.ms/MinecraftEULA then pass --accept-eula if you agree.")
    try:
        server = official_server_jar(args.server_jar) if args.download_server else args.server_jar
        validate_jar(server)
        java = get_java(args.java)
        print(f"Using Java: {java} | Minecraft server: {server.resolve()}",flush=True)
        args.workdir.mkdir(parents=True,exist_ok=True)
        for index,(seed,rows) in enumerate(selected,1):
            print(f"[{index}/{len(selected)}] Generating NORMAL vanilla world seed={seed}",flush=True)
            # Entirely new unique scratch directory; only our own created directories can be removed.
            scratch = Path(tempfile.mkdtemp(prefix=f"seed_{seed}_",dir=args.workdir)).resolve()
            # Server wants a fresh directory; preallocated scratch is its container.
            try:
                ok,elapsed = run_world(server,java,seed,rows,scratch/"server",
                                       args.boot_timeout,args.gen_timeout,args.heap_gb)
                results = verify_world(scratch/"server",seed,rows)
                found = False
                for result in results:
                    result["v10_seconds"] = round(elapsed,1)
                    result["v10_error"] = ""
                    write_row(args.output,columns,result)
                    status = result["world_verification"]
                    value = result["actual_badlands_same_y"]
                    print(f"  {status}: predicted={result['run']} actual_badlands={value} "
                          f"x={result['start_x']} z={result['start_z']} "
                          f"actual_start=({result['actual_best_x']},{result['actual_best_y']},{result['actual_best_z']}) "
                          f"best_anywhere_in_saved_world={result['actual_world_best_run']}",flush=True)
                    # A genuine 15-bush sub-row inside a predicted 17-bush
                    # span still beats the record even if the full 17 is false.
                    found |= (max(value, int(result["actual_world_best_run"]))>=args.target)
                if found:
                    print(f"ACTUAL SAVED-BLOCK HIT >= {args.target}: keeping world at {scratch}",flush=True)
                    print("Inspect world integrity and natural origin before any record claim.",flush=True)
                    return 0
            except Exception as exc:
                print(f"  SERVER/VERIFIER ERROR: {exc}",file=sys.stderr,flush=True)
                for row in rows:
                    write_row(args.output,columns,{**row,"world_verification":"ERROR",
                              "actual_same_y":0,"actual_badlands_same_y":0,"confirmed_y":"",
                              "v10_seconds":"","v10_error":str(exc)[:500]})
                # Continue to the next seed; failures are never counted as negatives.
            finally:
                if not args.keep_worlds and not any(
                    # Preserve worlds explicitly if they produce a record.
                    max(int(line.get("actual_badlands_same_y") or 0),
                        int(line.get("actual_world_best_run") or 0))>=args.target
                    for line in (locals().get("results") or [])):
                    shutil.rmtree(scratch,ignore_errors=True)
                # Make sure stale data from one seed cannot influence the next.
                if "results" in locals():
                    del results
        print(f"Batch complete. Saved-block results: {args.output}",flush=True)
        return 0
    except (OSError,RuntimeError,ValueError,zipfile.BadZipFile,KeyError) as e:
        print("ERROR:",e,file=sys.stderr)
        return 2


if __name__=="__main__":
    sys.exit(main())
