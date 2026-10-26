#!/usr/bin/env python3
"""Read generated Minecraft Java world chunks: verify claimed row blocks and badlands biome.

Usage: py -3 verify_world.py --world "...\\saves\\My World" --csv gpu_candidates.csv
The world MUST already have the candidate chunks generated in the overworld.
No third-party Python packages required.
"""
import argparse
import csv
import gzip
import math
from pathlib import Path
import struct
import zlib
from functools import lru_cache

U64 = (1 << 64) - 1

class NBTError(ValueError):
    pass

class Reader:
    def __init__(self, raw):
        self.buf = memoryview(raw)
        self.pos = 0

    def get(self, n):
        if n < 0 or self.pos + n > len(self.buf):
            raise NBTError("Truncated NBT data")
        out = self.buf[self.pos:self.pos + n]
        self.pos += n
        return out

    def unpack(self, fmt):
        return struct.unpack(fmt, self.get(struct.calcsize(fmt)))[0]

    def string(self):
        n = self.unpack('>H')
        return bytes(self.get(n)).decode('utf-8', 'replace')

    def payload(self, tag, depth=0):
        if depth > 64:
            raise NBTError('NBT recursion depth exceeded')
        if tag == 1: return self.unpack('>b')
        if tag == 2: return self.unpack('>h')
        if tag == 3: return self.unpack('>i')
        if tag == 4: return self.unpack('>q')
        if tag == 5: return self.unpack('>f')
        if tag == 6: return self.unpack('>d')
        if tag == 7:
            n = self.unpack('>i'); return bytes(self.get(n))
        if tag == 8: return self.string()
        if tag == 9:
            subtype = self.unpack('>B')
            n = self.unpack('>i')
            if n < 0 or n > 10000000: raise NBTError('Invalid NBT list size')
            return [self.payload(subtype, depth + 1) for _ in range(n)]
        if tag == 10:
            value = {}
            while True:
                subtype = self.unpack('>B')
                if subtype == 0: break
                key = self.string()
                value[key] = self.payload(subtype, depth + 1)
            return value
        if tag in (11, 12):
            n = self.unpack('>i')
            if n < 0 or n > 10000000: raise NBTError('Invalid NBT array size')
            fmt = '>i' if tag == 11 else '>q'
            return [self.unpack(fmt) for _ in range(n)]
        raise NBTError(f'Unknown NBT tag {tag}')


def read_nbt(raw):
    r = Reader(raw)
    typ = r.unpack('>B')
    if typ != 10:
        raise NBTError('Expected root compound')
    r.string()  # Root name
    return r.payload(10)


def get_world_seed(world):
    path = world / 'level.dat'
    if not path.is_file():
        raise FileNotFoundError(f'World level.dat not found at {path}')
    level = read_nbt(gzip.decompress(path.read_bytes()))
    data = level.get('Data', {})
    settings = data.get('WorldGenSettings', {})
    seed = settings.get('seed')
    if seed is None:
        # Versions that write a root Data.RandomSeed instead.
        seed = data.get('RandomSeed')
    if seed is None:
        raise NBTError('Could not determine seed from level.dat; refusing ungrounded verification')
    return int(seed)


class World:
    def __init__(self, folder, block):
        self.folder = folder
        self.block = block

    @lru_cache(maxsize=256)
    def chunk(self, cx, cz):
        rx, rz = cx // 32, cz // 32
        path = self.folder / 'region' / f'r.{rx}.{rz}.mca'
        if not path.is_file():
            return None
        with path.open('rb') as f:
            index = (cx % 32) + 32 * (cz % 32)
            f.seek(index * 4)
            location = f.read(4)
            if len(location) != 4: return None
            sector = int.from_bytes(location[:3], 'big')
            if sector == 0: return None
            f.seek(sector * 4096)
            length = int.from_bytes(f.read(4), 'big')
            if length <= 1 or length > 20_000_000:
                raise NBTError(f'Invalid chunk payload size at {cx},{cz}')
            compression = f.read(1)
            payload = f.read(length - 1)
        if compression == b'\x02':
            payload = zlib.decompress(payload)
        elif compression == b'\x01':
            payload = gzip.decompress(payload)
        elif compression == b'\x03':
            pass
        else:
            raise NBTError(f'Unsupported compression {compression.hex()} at chunk {cx},{cz}')
        root = read_nbt(payload)
        return root.get('Level', root)

    def biome_at(self, x, y, z):
        chunk = self.chunk(x // 16, z // 16)
        if chunk is None:
            return None
        section_y = y // 16
        for section in chunk.get("sections", chunk.get("Sections", [])):
            if int(section.get("Y", -9999)) != section_y:
                continue
            raw = section.get("biomes", {})
            if not isinstance(raw, dict):
                return None
            palette = raw.get("palette", [])
            if not palette:
                return None
            if len(palette) == 1:
                return str(palette[0])
            bits = max(1, (len(palette) - 1).bit_length())
            per_long = 64 // bits
            idx = ((((y % 16) // 4) * 4 + ((z % 16) // 4)) * 4 + ((x % 16) // 4))
            data = raw.get("data", [])
            if idx // per_long >= len(data):
                return None
            palette_idx = ((int(data[idx // per_long]) & U64) >> ((idx % per_long) * bits)) & ((1 << bits) - 1)
            return str(palette[palette_idx]) if palette_idx < len(palette) else None
        return None

    def dead_bush_y(self, x, z):
        cx, cz = x // 16, z // 16
        chunk = self.chunk(cx, cz)
        if chunk is None:
            return None
        result = set()
        lx, lz = x % 16, z % 16
        for section in chunk.get('sections', chunk.get('Sections', [])):
            bs = section.get('block_states', section.get('BlockStates', {}))
            palette = bs.get('palette', bs.get('Palette', [])) if isinstance(bs, dict) else []
            if not palette:
                continue
            names = [entry.get('Name', entry.get('name', '')) for entry in palette]
            indices = [i for i, name in enumerate(names) if name == self.block]
            if not indices: continue
            sy = int(section.get('Y', 0))
            data = bs.get('data', bs.get('Data', []))
            bits = max(4, (len(palette) - 1).bit_length())
            per_long = 64 // bits
            for ly in range(16):
                if len(palette) == 1:
                    palette_index = 0
                else:
                    linear = (ly * 16 + lz) * 16 + lx
                    long_index = linear // per_long
                    if long_index >= len(data):
                        continue
                    shift = (linear % per_long) * bits
                    palette_index = ((int(data[long_index]) & U64) >> shift) & ((1 << bits) - 1)
                if palette_index in indices:
                    result.add(sy * 16 + ly)
        return result


def verify(world, x, z, dx, dz, length, require_badlands=True):
    ys = []
    for i in range(length):
        pos = world.dead_bush_y(x + dx * i, z + dz * i)
        if pos is None:
            return 'MISSING_CHUNKS', [], 0
        ys.append(pos)
    common = set.intersection(*ys) if ys else set()
    # At one Y-level, all blocks must be present for the entire claimed row.
    if common:
        if not require_badlands:
            return 'BLOCKS_MATCH', sorted(common), length
        missing = False
        for y in sorted(common):
            biomes = [world.biome_at(x + dx * i, y, z + dz * i) for i in range(length)]
            if any(b is None for b in biomes):
                missing = True
            elif all(b.startswith('minecraft:') and b.endswith('badlands') for b in biomes):
                return 'BLOCKS_MATCH_BADLANDS', [y], length
        if missing:
            return 'UNKNOWN_BIOME', sorted(common), length
        return 'WRONG_BIOME', sorted(common), length
    # Longest contiguous subset at a shared Y, for diagnostic purposes.
    best = 0
    for y in set().union(*ys):
        run = 0
        for pos in ys:
            run = run + 1 if y in pos else 0
            best = max(best, run)
    return 'NOT_VERIFIED', [], best


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--world', type=Path, required=True, help='Vanilla Java save folder, containing level.dat and region/')
    ap.add_argument('--csv', type=Path, required=True, help='v2/v3 candidate CSV')
    ap.add_argument('--block', default='minecraft:dead_bush', help='Exact Minecraft block ID')
    ap.add_argument('--min-run', type=int, default=1)
    ap.add_argument('--allow-any-biome', action='store_true', help='Check blocks without requiring a badlands biome')
    ap.add_argument('--output', type=Path, default=Path('world_verified.csv'))
    args = ap.parse_args()
    seed = get_world_seed(args.world)
    world = World(args.world, args.block)
    print(f'World seed: {seed}. Reading actual overworld chunk blocks. Block: {args.block}')
    rows = []
    with args.csv.open(newline='', encoding='utf-8') as f:
        for row in csv.DictReader(f):
            if int(row['seed_signed']) != seed or int(row['run']) < args.min_run:
                continue
            status, heights, best = verify(world, int(row['start_x']), int(row['start_z']),
                                            int(row['dx']), int(row['dz']), int(row['run']),
                                            require_badlands=not args.allow_any_biome)
            row['world_verification'] = status
            row['verified_y'] = ';'.join(map(str, heights))
            row['longest_actual_same_y'] = best
            rows.append(row)
            print(f"{status}: seed={seed} claimed={row['run']} actual_max={best} x={row['start_x']} z={row['start_z']} y={heights}")
    if not rows:
        print('No candidates in this CSV match the loaded world seed and --min-run. Generate a world using a candidate seed first.')
        return
    fields = list(rows[0])
    with args.output.open('w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader();w.writerows(rows)
    print(f'Wrote {args.output}. BLOCKS_MATCH_BADLANDS confirms saved blocks and sampled biome, not natural-origin provenance. Missing chunks must be generated first.')

if __name__ == '__main__':
    main()
