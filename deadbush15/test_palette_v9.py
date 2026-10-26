#!/usr/bin/env python3
"""Fast regression checks for Java 26.3 and legacy chunk block palettes.

Run from deadbush15: py -3 test_palette_v9.py
No Minecraft server, downloads, or generated world needed.
"""
from verify_world_v4 import World, NBTError
from verify_normal_v9 import examine

class MockWorld(World):
    def __init__(self, chunk):
        super().__init__(".", "minecraft:dead_bush")
        self.fixture = chunk

    def chunk(self, cx, cz):
        return self.fixture


def packed_linear(positions, bits=4):
    per_long = 64 // bits
    longs = [0] * (4096 // per_long)
    for lx, ly, lz, index in positions:
        offset = (ly * 16 + lz) * 16 + lx
        longs[offset // per_long] |= index << ((offset % per_long) * bits)
    return longs


def check(label, palette):
    section = {
        "Y": 4,
        "block_states": {
            "palette": palette,
            "data": packed_linear([(3, 8, 5, 1), (4, 8, 5, 1)])
        },
        "biomes": {"palette": ["minecraft:badlands"]}
    }
    w = MockWorld({"Status": "minecraft:full", "sections": [section]})
    for x in (-13, -12):
        actual = w.dead_bush_y(x, 5)
        assert actual == {72}, (label, x, actual)
        assert w.biome_at(x, 72, 5) == "minecraft:badlands", label
    assert w.dead_bush_y(-11, 5) == set(), label
    row = {"start_x": -13, "start_z": 5, "dx": 1, "dz": 0, "run": 2}
    assert examine(w, row) == ("BLOCKS_MATCH_BADLANDS", 2, 2, "72"), label
    print("PASS:", label)


def main():
    check("26.3 string palette", ["minecraft:stone", "minecraft:dead_bush"])
    check("26.3 compound id palette", [{"id": "minecraft:stone"}, {"id": "minecraft:dead_bush", "properties": {}}])
    check("26.3 compact default-state compound", [{"": "minecraft:stone"}, {"": "minecraft:dead_bush"}])
    check("legacy Name palette", [{"Name": "minecraft:stone"}, {"Name": "minecraft:dead_bush", "Properties": {}}])
    check("legacy name palette", [{"name": "minecraft:stone"}, {"name": "minecraft:dead_bush"}])

    for bad in (17, None, {}, {"unknown": "minecraft:dead_bush"}):
        try:
            World.block_palette_id(bad)
        except NBTError:
            pass
        else:
            raise AssertionError(f"Invalid palette unexpectedly accepted: {bad!r}")
    print("PASS: malformed entries rejected")
    print("PASS: 26.3/legacy palette compatibility (synthetic sections)")


if __name__ == "__main__":
    main()
