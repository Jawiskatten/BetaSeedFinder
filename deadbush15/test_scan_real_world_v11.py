#!/usr/bin/env python3
"""Focused offline V11 scanner regression; no server downloads or manual worlds."""
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

from scan_real_world_v11 import chunk_bushes, find_rows, scan_world


def section_with_bushes(local_xs, y=70, z=5, biome="minecraft:badlands"):
    bits=4
    longs=[0]*256
    ly=y%16
    for x in local_xs:
        i=(ly*16+z)*16+x
        longs[i//16] |= 1 << ((i%16)*bits)
    return {"Y": y//16, "block_states": {"palette": [
        "minecraft:red_sand", "minecraft:dead_bush"], "data": longs},
        "biomes": {"palette":[biome]}}


def make_chunk(xs, biome="minecraft:badlands"):
    return {"Status":"minecraft:full",
            "sections":[section_with_bushes(xs,biome=biome)]}


def run():
    # Published 14-bush geometry across a chunk boundary, with 26.3 string IDs.
    c0=make_chunk(range(7,16))
    c1=make_chunk(range(0,5))
    a=list(chunk_bushes(c0,0,0))
    b=list(chunk_bushes(c1,1,0))
    assert len(a)==9 and len(b)==5
    rows=find_rows(a+b,min_run=14,top=1)
    assert rows and rows[0]["run"]==14 and rows[0]["start_x"]==7
    assert rows[0]["start_y"]==70 and rows[0]["start_z"]==5
    assert rows[0]["dx"]==1 and rows[0]["dz"]==0
    assert not find_rows(a+b,min_run=15), "Do not invent a record"

    # A single additional bush beyond the chunk boundary is exactly a 15-row.
    c1=make_chunk(range(0,6))
    real=list(chunk_bushes(c0,0,0))+list(chunk_bushes(c1,1,0))
    assert find_rows(real,min_run=15,top=1)[0]["run"]==15
    assert not list(chunk_bushes(make_chunk(range(0,6),"minecraft:desert"),1,0))
    assert len(list(chunk_bushes(make_chunk(range(0,6),"minecraft:desert"),1,0,False)))==6
    print("PASS: 14-bush reference geometry, 15-bush extension, cross-chunk rows, biome filter")

    # Verify saved-world scan does not inspect unrelated realms or require
    # predicted coordinates. The file is a fake marker: chunk parser is mocked.
    with TemporaryDirectory() as folder:
        root=Path(folder)
        region=root/"dimensions/minecraft/overworld/region"
        region.mkdir(parents=True)
        (region/"r.0.0.mca").write_bytes(b"")
        with patch("scan_real_world_v11.iter_region_chunks",
                   return_value=iter([(0,0,c0),(1,0,c1)])):
            result=scan_world(root,14,3)
        assert result["chunks"]==2 and result["bushes"]==15
        assert result["rows"][0]["run"]==15 and result["rows"][0]["start_x"]==7
        print("PASS: full-world scanner detects real rows outside RNG shortlist")


if __name__=="__main__":
    run()
