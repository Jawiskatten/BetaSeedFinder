import gzip, tempfile, struct, zlib, csv
from pathlib import Path
from single_badlands_v6 import preset_check,candidate_row,find_worldgen

def nt(t,n,v):
    name=n.encode()
    def payload(t,v):
        if t == 3: return struct.pack('>i',v)
        if t == 4: return struct.pack('>q',v)
        if t == 8:
            s=v.encode(); return struct.pack('>H',len(s))+s
        if t == 10:
            return b''.join(nt(*entry) for entry in v)+b'\x00'
        if t == 9:
            child,items=v;return bytes([child])+struct.pack('>i',len(items))+b''.join(payload(child,item) for item in items)
        raise Exception(t)
    return bytes([t])+struct.pack('>H',len(name))+name+payload(t,v)

def nested(tup):
    return nt(10,'',tup)

# worldgen settings fixture
with tempfile.TemporaryDirectory() as tmp:
    world=Path(tmp)
    wg=world/'data/minecraft/world_gen_settings.dat';wg.parent.mkdir(parents=True)
    contents=nested([
      (4,'seed',123),
      (10,'dimensions',[(10,'minecraft:overworld',[(10,'generator',[(8,'type','minecraft:noise'),(10,'biome_source',[(8,'type','minecraft:fixed'),(8,'biome','minecraft:badlands')])])])])
    ])
    wg.write_bytes(gzip.compress(contents))
    assert preset_check(world)[:2]==(True,123),preset_check(world)
    # 26.3 region folder; one chunk at (0,0) with a 16-long row of bushes Y=0 in badlands
    region=world/'dimensions/minecraft/overworld/region';region.mkdir(parents=True)
    chunk=nested([
        (8,'Status','minecraft:full'),
        (9,'sections',(10,[[
           (3,'Y',0),
           (10,'block_states',[(9,'palette',(10,[[(8,'Name','minecraft:dead_bush')]]))]),
           (10,'biomes',[(9,'palette',(8,['minecraft:badlands']))])
        ]]))
    ])
    compressed=zlib.compress(chunk)
    loc=b'\x00\x00\x02\x01'
    header=loc + b'\x00'*(4096-4) + b'\x00'*4096
    length=len(compressed)+1
    block=struct.pack('>I',length)+b'\x02'+compressed
    (region/'r.0.0.mca').write_bytes(header+block+b'\x00'*(4096-(len(block)%4096)))
    row={'start_x':'0','start_z':'2','dx':'1','dz':'0','run':'15'}
    score=candidate_row(world,row)
    assert score['status']=='ROW_PRESENT_BADLANDS',score
    assert score['same_y']==15,score
    assert score['badlands_any_y']==15,score
    row['start_x']='200';assert candidate_row(world,row)['status']=='MISSING_CHUNKS'
    (wg).write_bytes(gzip.compress(contents.replace(b'minecraft:badlands',b'minecraft:woodland')))
    assert preset_check(world)[0] is False
print('PASS 26.3 worldgen preset and 15-block Anvil save verification + missing chunk + wrong preset checks')