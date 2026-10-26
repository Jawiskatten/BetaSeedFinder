#!/usr/bin/env python3
"""V11: discover natural dead-bush lines in ALL saved Minecraft 26.3 chunks.

Unlike V4-V9, this does not guess RNG XZ or terrain height. It scans actual
block states saved by vanilla worldgen, including positions outside the
candidate rows. Scanning a finite saved region is not proof that no longer row
exists elsewhere in an ungenerated world.
"""
import argparse
import csv
import gzip
import re
import struct
import sys
import zlib
from pathlib import Path

from verify_world_v4 import NBTError, World, read_nbt

TARGET = "minecraft:dead_bush"
BADLANDS = {"minecraft:badlands", "minecraft:wooded_badlands",
            "minecraft:eroded_badlands"}
MASK = (1 << 64) - 1
REGION_FILE = re.compile(r"r\.(-?\d+)\.(-?\d+)\.mca$")


def section_biome(section, lx, ly, lz):
    biomes = section.get("biomes", {})
    if not isinstance(biomes, dict):
        return None
    palette = biomes.get("palette", [])
    if not palette:
        return None
    if len(palette) == 1:
        return str(palette[0])
    bits = max(1, (len(palette) - 1).bit_length())
    per_long = 64 // bits
    index = ((ly // 4 * 4 + lz // 4) * 4 + lx // 4)
    data = biomes.get("data", [])
    if index // per_long >= len(data):
        return None
    palette_index = ((int(data[index // per_long]) & MASK)
                     >> ((index % per_long) * bits)) & ((1 << bits)-1)
    return str(palette[palette_index]) if palette_index < len(palette) else None


def chunk_bushes(chunk, cx, cz, require_badlands=True):
    """Yield coordinates (x,y,z) for every real saved dead_bush block."""
    if chunk.get("Status",chunk.get("status")) not in ("full","minecraft:full"):
        return
    for section in chunk.get("sections",chunk.get("Sections", [])):
        bs = section.get("block_states",section.get("BlockStates", {}))
        if not isinstance(bs, dict):
            continue
        palette = bs.get("palette",bs.get("Palette",[]))
        if not palette:
            continue
        ids = [World.block_palette_id(p) for p in palette]
        bush_ids = {i for i,name in enumerate(ids) if name == TARGET}
        if not bush_ids:
            continue
        sy = int(section.get("Y",0))
        if len(palette) == 1:
            # A one-entry section palette has no block_states.data.
            if 0 not in bush_ids:
                continue
            indices = range(4096)
            for i in indices:
                lx,lz,ly = i%16,(i//16)%16,i//256
                if not require_badlands or section_biome(section,lx,ly,lz) in BADLANDS:
                    yield (cx*16+lx,sy*16+ly,cz*16+lz)
            continue
        bits = max(4, (len(palette)-1).bit_length())
        per_long = 64 // bits
        data = bs.get("data",bs.get("Data",[]))
        if len(data) < (4096 + per_long-1)//per_long:
            raise NBTError(f"Truncated block state packed data at {cx},{cz}, section {sy}")
        mask=(1<<bits)-1
        for i in range(4096):
            palette_idx = ((int(data[i//per_long]) & MASK) >> ((i%per_long)*bits)) & mask
            if palette_idx in bush_ids:
                lx,lz,ly = i%16,(i//16)%16,i//256
                if not require_badlands or section_biome(section,lx,ly,lz) in BADLANDS:
                    yield (cx*16+lx,sy*16+ly,cz*16+lz)


def iter_region_chunks(region_path):
    """Read a region file once; handle zlib, gzip, uncompressed and .mcc chunks."""
    match=REGION_FILE.fullmatch(region_path.name)
    if not match:
        return
    rx,rz=map(int,match.groups())
    with region_path.open("rb") as f:
        header=f.read(4096)
        if len(header)!=4096:
            raise NBTError(f"Truncated region header {region_path}")
        for idx in range(1024):
            location=header[idx*4:idx*4+4]
            sector=int.from_bytes(location[:3],"big")
            count=location[3]
            if not sector:
                continue
            cx=rx*32+(idx%32)
            cz=rz*32+(idx//32)
            f.seek(sector*4096)
            length=struct.unpack(">I",f.read(4))[0]
            if length<1 or length>64_000_000:
                raise NBTError(f"Invalid chunk length at ({cx},{cz})")
            kind=f.read(1)
            if len(kind)!=1:
                raise NBTError(f"Truncated chunk codec at ({cx},{cz})")
            kind=kind[0]
            external=bool(kind & 0x80)
            codec=kind & 0x7f
            if external:
                outside=region_path.parent/f"c.{cx}.{cz}.mcc"
                if not outside.is_file():
                    raise NBTError(f"Missing external chunk file {outside}")
                payload=outside.read_bytes()
            else:
                payload=f.read(length-1)
                if len(payload)!=length-1:
                    raise NBTError(f"Truncated chunk payload at ({cx},{cz})")
            if codec==2:
                data=zlib.decompress(payload)
            elif codec==1:
                data=gzip.decompress(payload)
            elif codec==3:
                data=payload
            else:
                raise NBTError(f"Unknown chunk compression {codec} at ({cx},{cz})")
            nbt=read_nbt(data)
            chunk=nbt.get("Level",nbt)
            if "xPos" in chunk and int(chunk["xPos"])!=cx:
                raise NBTError(f"Chunk X coordinate mismatch at ({cx},{cz})")
            if "zPos" in chunk and int(chunk["zPos"])!=cz:
                raise NBTError(f"Chunk Z coordinate mismatch at ({cx},{cz})")
            yield cx,cz,chunk


def find_rows(points, min_run=2, top=30):
    """Find each maximal x/z straight horizontal row at one absolute Y."""
    points=set(points)
    rows=[]
    for x,y,z in points:
        for dx,dz in ((1,0),(0,1)):
            if (x-dx,y,z-dz) in points:
                continue
            n=1
            while (x+dx*n,y,z+dz*n) in points:
                n+=1
            if n>=min_run:
                rows.append(dict(run=n,start_x=x,start_y=y,start_z=z,dx=dx,dz=dz,
                                 tp_command=f"/tp @s {x} {y+2} {z}"))
    rows.sort(key=lambda row:(-row["run"],row["start_x"],row["start_z"],row["start_y"]))
    return rows[:top]


def scan_world(folder, min_run=2,top=30,require_badlands=True):
    modern=folder/"dimensions/minecraft/overworld/region"
    legacy=folder/"region"
    directory=modern if modern.is_dir() else legacy
    if not directory.is_dir():
        raise FileNotFoundError(f"No overworld region directory found under {folder}")
    points=set()
    count=0
    for path in sorted(directory.glob("r.*.*.mca")):
        if not REGION_FILE.fullmatch(path.name):
            continue
        for cx,cz,chunk in iter_region_chunks(path):
            if chunk.get("Status",chunk.get("status")) not in ("full","minecraft:full"):
                continue
            count+=1
            points.update(chunk_bushes(chunk,cx,cz,require_badlands))
    return dict(chunks=count,bushes=len(points),rows=find_rows(points,min_run,top),
                region_directory=str(directory))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--world",type=Path,required=True,help="Minecraft save folder")
    p.add_argument("--min-run",type=int,default=2)
    p.add_argument("--top",type=int,default=30)
    p.add_argument("--output",type=Path,default=Path("v11_real_rows.csv"))
    p.add_argument("--any-biome",action="store_true")
    args=p.parse_args()
    if not 1<=args.min_run<=64 or not 1<=args.top<=10000:
        p.error("Invalid --min-run/--top")
    try:
        result=scan_world(args.world,args.min_run,args.top,not args.any_biome)
        print(f"Scanned {result['chunks']} fully generated chunks; "
              f"found {result['bushes']} actual dead-bush blocks in "
              f"{'all biomes' if args.any_biome else 'Badlands-family biomes'}.")
        for r in result["rows"][:15]:
            print(f"  row={r['run']} xyz=({r['start_x']},{r['start_y']},{r['start_z']}) "
                  f"direction=({r['dx']},{r['dz']})")
        columns=["run","start_x","start_y","start_z","dx","dz","tp_command"]
        with args.output.open("w",newline="",encoding="utf-8") as f:
            w=csv.DictWriter(f,fieldnames=columns)
            w.writeheader()
            w.writerows(result["rows"])
        print("Saved actual generated row results to",args.output)
        print("Warning: ungenerated chunks cannot be excluded as potential rows.")
        return 0
    except (OSError,ValueError,RuntimeError,zlib.error,EOFError,struct.error) as e:
        print("ERROR:",e,file=sys.stderr)
        return 2

if __name__=="__main__":
    sys.exit(main())
