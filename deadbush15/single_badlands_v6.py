#!/usr/bin/env python3
"""Deadbush15 V6: GPU scout + single-biome Badlands shortlist and Minecraft 26.3 saved-world verification.

Never equate an RNG alignment with generated dead bushes. Windows stdlib Python 3.10+.
"""
import argparse
import csv
import gzip
import os
import subprocess
import sys
from pathlib import Path

from verify_world_v4 import World, read_nbt

VERSION = "Minecraft Java 26.3"
CALIBRATION_SEED = -6825794747614541757
CALIBRATION_X = 16530130
CALIBRATION_Z = -25619539


def nbt_file(path):
    raw = path.read_bytes()
    return read_nbt(gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw)


def find_worldgen(world):
    """26.3 stores world generation settings outside level.dat; legacy saves differ."""
    for file in (world / "data/minecraft/world_gen_settings.dat", world / "level.dat"):
        if not file.is_file():
            continue
        obj = nbt_file(file)
        # 26.1+ SavedData files use lowercase `data` (unlike legacy
        # `level.dat` with uppercase `Data`). Unwrap only known metadata keys.
        choices = [obj]
        for o in choices:
            if len(choices) > 20:
                break
            if not isinstance(o, dict):
                continue
            for key in ("data", "Data", "WorldGenSettings", "world_gen_settings"):
                child = o.get(key)
                if isinstance(child, dict):
                    choices.append(child)
        for o in choices:
            if not isinstance(o, dict):
                continue
            dimensions = o.get("dimensions", o.get("Dimensions"))
            if isinstance(dimensions, dict) and "minecraft:overworld" in dimensions:
                return o, file
    return None, None


def preset_check(world):
    settings, source = find_worldgen(world)
    if settings is None:
        return None, None, "No readable overworld world-generation settings. For 26.3 expected data/minecraft/world_gen_settings.dat."
    dimension = settings["dimensions"].get("minecraft:overworld") if "dimensions" in settings else settings["Dimensions"].get("minecraft:overworld")
    if not isinstance(dimension, dict):
        return None, None, "Missing overworld generator information"
    generator = dimension.get("generator", dimension.get("Generator", {}))
    if not isinstance(generator, dict):
        return None, None, "Missing generator dictionary"
    source_data = generator.get("biome_source", generator.get("biomeSource", {}))
    if not isinstance(source_data, dict):
        return None, None, "No biome source in world-generation settings"
    actual = (source_data.get("type"), source_data.get("biome"), generator.get("type"))
    valid = actual == ("minecraft:fixed", "minecraft:badlands", "minecraft:noise")
    seed = settings.get("seed", settings.get("Seed"))
    if not isinstance(seed, int):
        seed = None
    return valid, seed, f"{source}: biome_source={source_data}, generator_type={generator.get('type')}, seed={seed}"


def candidate_row(world, row):
    x,z,dx,dz,length = (int(row[k]) for k in ("start_x","start_z","dx","dz","run"))
    if not 1 <= length <= 64 or (dx,dz) not in ((1,0),(0,1)):
        raise ValueError("Bad row coordinate/axis/run length")
    coords = [(x+i*dx, z+i*dz) for i in range(length)]
    w = World(world / "dimensions/minecraft/overworld", "minecraft:dead_bush")
    # New 26.1+ path first. Old saves are read only if new layout does not exist.
    if not (w.folder / "region").is_dir():
        w = World(world, "minecraft:dead_bush")
    for cx,cz in {(x//16,z//16) for x,z in coords}:
        chunk = w.chunk(cx,cz)
        if chunk is None:
            return {"status":"MISSING_CHUNKS","same_y":0,"any_y":0,"badlands_any_y":0,"notes":"Teleport to XZ, let chunks generate, save and quit"}
        st = str(chunk.get("Status",chunk.get("status", "")))
        if st not in ("full","minecraft:full"):
            return {"status":"INCOMPLETE_CHUNKS","same_y":0,"any_y":0,"badlands_any_y":0,"notes":f"Chunk {cx},{cz} status={st}"}
    ys=[w.dead_bush_y(x,z) for x,z in coords]
    if any(item is None for item in ys):
        return {"status":"MISSING_CHUNKS","same_y":0,"any_y":0,"badlands_any_y":0,"notes":"Missing block data"}
    any_best, biome_best = 0,0
    any_run, biome_run = 0,0
    for (x,z),heights in zip(coords,ys):
        any_run = any_run+1 if heights else 0
        biome_ok=bool(heights) and any((w.biome_at(x,y,z) or "") == "minecraft:badlands" for y in heights)
        biome_run = biome_run+1 if biome_ok else 0
        any_best=max(any_best,any_run);biome_best=max(biome_best,biome_run)
    same_best=0
    for y in set().union(*ys):
        running=0
        for heights in ys:
            running=running+1 if y in heights else 0
            same_best=max(same_best,running)
    status = "ROW_PRESENT_BADLANDS" if biome_best == length else ("ROW_PRESENT_WRONG_BIOME" if any_best == length else "NOT_VERIFIED")
    return {"status":status,"same_y":same_best,"any_y":any_best,"badlands_any_y":biome_best,"notes":"Same-Y and 2D horizontal runs measured from saved blocks"}


def run_scan(a):
    binary = Path(a.exe)
    if not binary.is_file():
        raise FileNotFoundError(f"{binary} not found; compile V4 or use --exe")
    cmd = [str(binary.resolve()), "--gpu-search", "--start", str(a.start), "--count", str(a.count),
           "--batch", str(a.batch), "--locations", str(a.locations), "--spread-span",str(a.spread),
           "--mode", a.mode, "--prefilter", str(a.report), "--report", str(a.report),
           "--target",str(a.target), "--output",str(a.output), "--checkpoint",str(a.checkpoint),"--resume"]
    print("PRESET: Single Biome > Badlands, normal terrain. NOT Superflat.")
    print("This GPU stage only scores decoration RNG, not actual badlands terrain.")
    print("Executing:",subprocess.list2cmdline(cmd),flush=True)
    return subprocess.call(cmd)


def parse_csv(path):
    with Path(path).open(newline="",encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def tp(x,z):
    return f"/tp @s {int(x)} 160 {int(z)}"


def run_prepare(a):
    if a.raw_csv:
        exe=Path(a.ranker)
        if not exe.is_file():
            raise FileNotFoundError(f"{exe} missing; compile V5 ranker first")
        temp=Path(a.shortlist)
        cmd=[str(exe.resolve()),"--rank","--csv",str(a.raw_csv),"--min-run",str(a.min_run),
             "--top",str(a.top),"--output",str(temp)]
        subprocess.run(cmd,check=True)
    rows=parse_csv(a.shortlist)
    if not rows:raise RuntimeError("No shortlist rows")
    commands=["# Java 26.3: Create New World > World Type: Single Biome > Customize: Badlands.",
              "# Normal terrain; NOT Superflat. Enable cheats, use spectator mode.",
              "# Each seed requires a NEW world with the same preset.",
              "# RNG alignments below are NOT verified rows.",
              "# Calibration: check published reference in this exact preset before trusting RNG equivalence:",
              f"# WORLD SEED {CALIBRATION_SEED}", f"{tp(CALIBRATION_X,CALIBRATION_Z)}", ""]
    for i,row in enumerate(rows[:a.top],1):
        commands.extend([f"# Candidate {i}: WORLD SEED {row['seed_signed']}  RNG_XZ={row['run']}  same_row_flat={row.get('same_row_flat_longest','unknown')}",
                         tp(row["start_x"],row["start_z"]), ""])
    Path(a.output).write_text("\n".join(commands)+"\n",encoding="utf-8")
    print(f"Wrote {len(rows[:a.top])} candidates plus calibration to {a.output}")
    print("First calibration seed:",CALIBRATION_SEED," | ",tp(CALIBRATION_X,CALIBRATION_Z))
    for row in rows[:min(3,a.top)]:
        print("Candidate",row['seed_signed'],tp(row["start_x"],row["start_z"]),"same-row-flat",row.get("same_row_flat_longest","?"))
    return 0


def run_verify(a):
    path=Path(a.world)
    if not path.is_dir():raise FileNotFoundError(f"World save not found: {path}")
    preset,seed,detail=preset_check(path)
    print("Detected preset:",("SINGLE BADLANDS" if preset else "NOT SINGLE BADLANDS" if preset is False else "UNKNOWN"),detail)
    if not preset or seed is None:
        raise RuntimeError("Cannot prove this is a Single-Biome Badlands world with the expected seed. Refusing to score.")
    rows=[r for r in parse_csv(a.shortlist) if int(r['seed_signed'])==seed]
    if not rows:raise RuntimeError(f"No rows in shortlist for this world's seed {seed}")
    results=[]
    for row in rows:
        score=candidate_row(path,row)
        record=dict(row,**score)
        results.append(record)
        print(f"{score['status']} seed={seed} x={row['start_x']} z={row['start_z']} claimed={row['run']} "+
              f"real_any_Y={score['any_y']} real_same_Y={score['same_y']} badlands_any_Y={score['badlands_any_y']}")
    with Path(a.output).open("w",newline="",encoding="utf-8") as f:
        writer=csv.DictWriter(f,fieldnames=list(results[0]));writer.writeheader();writer.writerows(results)
    print("Results:",a.output,"- actual saved blocks, not RNG guesses")
    return 0


def build_parser():
    p=argparse.ArgumentParser(description=__doc__)
    sub=p.add_subparsers(dest="command",required=True)
    scan=sub.add_parser("scan",help="Invoke existing V4 OpenCL finder under a single-biome Badlands workflow")
    for name,kw in {"exe":{"default":"deadbush15_v4.exe"},"start":{"type":int,"default":210100000},
       "count":{"type":int,"default":1000000},"batch":{"type":int,"default":8192},
       "locations":{"type":int,"default":4},"spread":{"type":int,"default":1000000},
       "mode":{"choices":["flat","xz"],"default":"flat"},"report":{"type":int,"default":10},
       "target":{"type":int,"default":15},"output":{"default":"single_badlands_rng.csv"},
       "checkpoint":{"default":"single_badlands.progress"}}.items():
        scan.add_argument("--"+name.replace("_","-"),**kw)
    prep=sub.add_parser("prepare",help="Rank existing GPU results and generate seed + teleport commands")
    prep.add_argument("--raw-csv",help="Optional V4 GPU CSV to feed to V5 ranker")
    prep.add_argument("--ranker",default="deadbush15_v5.exe")
    prep.add_argument("--shortlist",default="single_badlands_shortlist.csv")
    prep.add_argument("--min-run",type=int,default=12)
    prep.add_argument("--top",type=int,default=10)
    prep.add_argument("--output",default="single_badlands_tp.txt")
    check=sub.add_parser("verify",help="Read ACTUAL generated blocks from an existing Java 26.3 single-biome Badlands save")
    check.add_argument("--world",required=True,help="Full save directory, usually %APPDATA%/.minecraft/saves/<WorldName>")
    check.add_argument("--shortlist",default="single_badlands_shortlist.csv")
    check.add_argument("--output",default="single_badlands_verified.csv")
    return p


def main():
    args=build_parser().parse_args()
    try:
        return {"scan":run_scan,"prepare":run_prepare,"verify":run_verify}[args.command](args)
    except (OSError,ValueError,RuntimeError,subprocess.CalledProcessError) as e:
        print("ERROR:",e,file=sys.stderr);return 2

if __name__ == "__main__":
    sys.exit(main())