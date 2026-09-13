#!/usr/bin/env python3
import argparse
import gzip
import io
import os
from pathlib import Path
import struct
import sys
import zlib

BLOCK_NAMES = {
    0: "air", 1: "stone", 2: "grass", 3: "dirt", 4: "cobblestone", 5: "planks",
    6: "sapling", 7: "bedrock", 8: "water_moving", 9: "water_still",
    10: "lava_moving", 11: "lava_still", 12: "sand", 13: "gravel",
    14: "gold_ore", 15: "iron_ore", 16: "coal_ore", 17: "log", 18: "leaves",
    20: "glass", 21: "lapis_ore", 24: "sandstone", 30: "web", 31: "tallgrass",
    32: "dead_bush", 37: "yellow_flower", 38: "red_flower", 39: "brown_mushroom",
    40: "red_mushroom", 48: "mossy_cobble", 50: "torch", 51: "fire",
    54: "chest", 56: "diamond_ore", 59: "crops", 65: "ladder", 66: "rail",
    73: "redstone_ore", 74: "glowing_redstone_ore", 78: "snow_layer", 79: "ice",
    81: "cactus", 82: "clay", 83: "reeds",
}

class NBTReader:
    def __init__(self, data: bytes):
        self.f = io.BytesIO(data)

    def read(self, n):
        b = self.f.read(n)
        if len(b) != n:
            raise EOFError("Unexpected end of NBT")
        return b

    def u8(self): return self.read(1)[0]
    def i8(self): return struct.unpack(">b", self.read(1))[0]
    def i16(self): return struct.unpack(">h", self.read(2))[0]
    def u16(self): return struct.unpack(">H", self.read(2))[0]
    def i32(self): return struct.unpack(">i", self.read(4))[0]
    def i64(self): return struct.unpack(">q", self.read(8))[0]
    def f32(self): return struct.unpack(">f", self.read(4))[0]
    def f64(self): return struct.unpack(">d", self.read(8))[0]

    def string(self):
        n = self.u16()
        return self.read(n).decode("utf-8", errors="replace")

    def payload(self, t):
        if t == 1: return self.i8()
        if t == 2: return self.i16()
        if t == 3: return self.i32()
        if t == 4: return self.i64()
        if t == 5: return self.f32()
        if t == 6: return self.f64()
        if t == 7:
            n = self.i32()
            return self.read(n)
        if t == 8: return self.string()
        if t == 9:
            subtype = self.u8()
            n = self.i32()
            return [self.payload(subtype) for _ in range(n)]
        if t == 10:
            out = {}
            while True:
                subtype = self.u8()
                if subtype == 0:
                    break
                name = self.string()
                out[name] = self.payload(subtype)
            return out
        if t == 11:
            n = self.i32()
            return [self.i32() for _ in range(n)]
        if t == 12:
            n = self.i32()
            return [self.i64() for _ in range(n)]
        raise ValueError(f"Unsupported NBT tag type {t}")

    def root(self):
        t = self.u8()
        if t == 0:
            return "", None
        name = self.string()
        return name, self.payload(t)


def load_level_dat(path: Path):
    with gzip.open(path, "rb") as f:
        data = f.read()
    _, root = NBTReader(data).root()
    if not isinstance(root, dict) or "Data" not in root:
        raise ValueError("level.dat missing Data compound")
    return root["Data"]


def candidate_save_dirs():
    seen = set()
    def emit(p):
        p = Path(p)
        try:
            rp = p.resolve()
        except Exception:
            rp = p
        key = str(rp).lower()
        if key not in seen and p.exists():
            seen.add(key)
            yield p

    appdata = os.environ.get("APPDATA")
    user = Path.home()
    direct_saves = []
    instance_roots = []
    if appdata:
        a = Path(appdata)
        direct_saves.append(a / ".minecraft" / "saves")
        instance_roots += [a / "MultiMC" / "instances", a / "PrismLauncher" / "instances"]
    direct_saves += [user / ".minecraft" / "saves"]
    instance_roots += [
        user / "Documents" / "MultiMC" / "instances",
        user / "Documents" / "PrismLauncher" / "instances",
        user / "Desktop" / "MultiMC" / "instances",
        user / "Desktop" / "PrismLauncher" / "instances",
    ]
    for drive in ("C:/", "D:/", "E:/", "V:/"):
        d = Path(drive)
        instance_roots += [
            d / "MultiMC" / "instances", d / "PrismLauncher" / "instances",
            d / "Games" / "MultiMC" / "instances", d / "Games" / "PrismLauncher" / "instances",
        ]

    for saves in direct_saves:
        if saves.exists():
            for level in saves.glob("*/level.dat"):
                yield from emit(level.parent)

    for root in instance_roots:
        if not root.exists():
            continue
        patterns = ["*/.minecraft/saves/*/level.dat", "*/minecraft/saves/*/level.dat", "*/saves/*/level.dat"]
        for pat in patterns:
            for level in root.glob(pat):
                yield from emit(level.parent)


def find_world(seed: int, explicit: str | None):
    if explicit:
        p = Path(explicit)
        if p.name.lower() == "level.dat":
            p = p.parent
        if not (p / "level.dat").exists():
            raise FileNotFoundError(f"No level.dat in {p}")
        data = load_level_dat(p / "level.dat")
        return p, data

    matches = []
    checked = 0
    for p in candidate_save_dirs():
        checked += 1
        try:
            data = load_level_dat(p / "level.dat")
        except Exception:
            continue
        if int(data.get("RandomSeed", 0)) == seed:
            last = int(data.get("LastPlayed", 0))
            try:
                mt = (p / "level.dat").stat().st_mtime_ns
            except Exception:
                mt = 0
            matches.append((last, mt, p, data))
    if not matches:
        raise FileNotFoundError(
            f"Could not auto-find a save with RandomSeed={seed}. Checked {checked} candidate saves. "
            "Rerun with -WorldPath '...\\saves\\YourWorld'."
        )
    matches.sort(key=lambda x: (x[0], x[1]), reverse=True)
    _, _, p, data = matches[0]
    return p, data


def load_chunk(world: Path, cx: int, cz: int):
    rx, rz = cx >> 5, cz >> 5
    region = world / "region" / f"r.{rx}.{rz}.mcr"
    if not region.exists():
        raise FileNotFoundError(f"Region file missing: {region}")
    lx, lz = cx & 31, cz & 31
    index = lx + lz * 32
    with region.open("rb") as f:
        f.seek(index * 4)
        loc = f.read(4)
        if len(loc) != 4:
            raise ValueError("Short region header")
        val = int.from_bytes(loc, "big")
        sector = val >> 8
        count = val & 0xFF
        if sector == 0 or count == 0:
            raise ValueError(f"Chunk ({cx},{cz}) is not present in {region.name}")
        f.seek(sector * 4096)
        length = struct.unpack(">I", f.read(4))[0]
        ctype = f.read(1)[0]
        payload = f.read(length - 1)
    if ctype == 1:
        raw = gzip.decompress(payload)
    elif ctype == 2:
        raw = zlib.decompress(payload)
    else:
        raise ValueError(f"Unsupported region compression type {ctype}")
    _, root = NBTReader(raw).root()
    level = root.get("Level", root) if isinstance(root, dict) else None
    if not isinstance(level, dict):
        raise ValueError("Chunk NBT missing Level compound")
    return level, region


def block_at(blocks: bytes, x: int, y: int, z: int):
    if y < 0 or y >= 128:
        return 0
    idx = ((x & 15) << 11) | ((z & 15) << 7) | y
    return blocks[idx]


def air_runs(blocks, x, z, lo, hi):
    runs = []
    y = lo
    while y <= hi:
        if block_at(blocks, x, y, z) != 0:
            y += 1
            continue
        a = y
        while y + 1 <= hi and block_at(blocks, x, y + 1, z) == 0:
            y += 1
        runs.append((a, y))
        y += 1
    return runs


def solid_runs(blocks, x, z, lo, hi):
    # For this desert test case, treat the terrain blocks as collidable and liquids/air/plants as non-solid.
    nonfull = {0, 6, 8, 9, 10, 11, 30, 31, 32, 37, 38, 39, 40, 50, 51, 55, 59, 63, 65, 66, 69, 75, 76, 77, 78, 83}
    runs = []
    y = lo
    while y <= hi:
        if block_at(blocks, x, y, z) in nonfull:
            y += 1
            continue
        a = y
        while y + 1 <= hi and block_at(blocks, x, y + 1, z) not in nonfull:
            y += 1
        runs.append((a, y))
        y += 1
    return runs


def fmt_runs(runs):
    if not runs:
        return "none"
    return ",".join(str(a) if a == b else f"{a}-{b}" for a, b in runs)


def main():
    ap = argparse.ArgumentParser(description="Read the actual Beta 1.7.3 saved chunk at world spawn and print its final block column.")
    ap.add_argument("--seed", type=int, default=-3405360075020439777)
    ap.add_argument("--world", dest="world_path", default=None, help="Optional explicit world save folder")
    ap.add_argument("--min-y", type=int, default=72)
    ap.add_argument("--max-y", type=int, default=100)
    args = ap.parse_args()

    world, data = find_world(args.seed, args.world_path)
    sx = int(data.get("SpawnX", 0))
    sy = int(data.get("SpawnY", 64))
    sz = int(data.get("SpawnZ", 0))
    cx, cz = sx >> 4, sz >> 4
    level, region = load_chunk(world, cx, cz)
    blocks = level.get("Blocks")
    if not isinstance(blocks, (bytes, bytearray)) or len(blocks) < 32768:
        raise ValueError(f"Chunk Blocks array missing or unexpected length: {len(blocks) if hasattr(blocks, '__len__') else 'n/a'}")

    print("=== ACTUAL BETA 1.7.3 SAVE COLUMN ===")
    print(f"world={world}")
    print(f"seed={int(data.get('RandomSeed', 0))}")
    print(f"saved_spawn=({sx},{sy},{sz}) chunk=({cx},{cz}) region={region.name}")
    print(f"chunk_TerrainPopulated={level.get('TerrainPopulated', 'unknown')}")
    print(f"column_block=({sx},{sz})")
    print()

    lo = max(0, args.min_y)
    hi = min(127, args.max_y)
    print(f"air_runs_y{lo}_{hi}={fmt_runs(air_runs(blocks, sx, sz, lo, hi))}")
    print(f"fullblock_runs_y{lo}_{hi}={fmt_runs(solid_runs(blocks, sx, sz, lo, hi))}")
    print("blocks:")
    for y in range(hi, lo - 1, -1):
        bid = block_at(blocks, sx, y, sz)
        print(f"  y={y:3d} id={bid:3d} {BLOCK_NAMES.get(bid, 'block_'+str(bid))}")

    # Show the first air cavity reached by a 1.8-block-high player whose bounding-box bottom starts at Y=65.
    # For the terrain blocks relevant here, two consecutive air blocks are enough for the AABB to fit.
    bottom = 65
    while bottom < 127:
        b0 = block_at(blocks, sx, bottom, sz)
        b1 = block_at(blocks, sx, bottom + 1, sz)
        if b0 == 0 and b1 == 0:
            break
        bottom += 1
    print()
    if bottom < 127:
        print(f"first_two_air_slots_from_y65={bottom},{bottom+1}")
        print(f"predicted_preparePlayerToSpawn_stop_bottomY={bottom}")
        above = bottom + 2
        print(f"block_immediately_above_slot_y{above}=id{block_at(blocks, sx, above, sz)} {BLOCK_NAMES.get(block_at(blocks, sx, above, sz), 'unknown')}")
    else:
        print("first_two_air_slots_from_y65=none")

    print("\nThis reads the saved .mcr chunk directly, so it is ground truth for the world shown in-game after generation/population.")

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
