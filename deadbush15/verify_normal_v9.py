#!/usr/bin/env python3
"""Verify real dead-bush blocks in an existing Minecraft 26.3 NORMAL-world save.

No server launches and no new worlds are generated here. Candidate chunks must
already exist on disk after visiting them in a fresh vanilla world.
"""
import argparse
import csv
import sys
from pathlib import Path
from single_badlands_v6 import find_worldgen
from verify_world_v4 import World

BADLANDS = {"minecraft:badlands", "minecraft:wooded_badlands", "minecraft:eroded_badlands"}


def world_seed_and_preset(folder):
    settings, source_file = find_worldgen(folder)
    if settings is None:
        raise RuntimeError('Could not find world generation settings (26.3 world?)')
    dims = settings.get('dimensions', settings.get('Dimensions', {}))
    over = dims.get('minecraft:overworld', {})
    gen = over.get('generator', {}) if isinstance(over, dict) else {}
    biome = gen.get('biome_source', {}) if isinstance(gen, dict) else {}
    kind = biome.get('type') if isinstance(biome, dict) else None
    preset = biome.get('preset') if isinstance(biome, dict) else None
    if kind != 'minecraft:multi_noise' or preset != 'minecraft:overworld' or gen.get('type') != 'minecraft:noise':
        raise RuntimeError(f'World is not confirmed normal Overworld: source={source_file} biome_source={biome}')
    seed = settings.get('seed', settings.get('Seed'))
    if not isinstance(seed, int):
        raise RuntimeError('Cannot prove the actual world seed from worldgen settings')
    return seed


def examine(w, record):
    x, z, dx, dz, n = (int(record[k]) for k in ('start_x','start_z','dx','dz','run'))
    if not (1 <= n <= 64 and (dx,dz) in {(1,0),(0,1)}):
        raise ValueError('Invalid reported row geometry')
    coords = [(x+i*dx,z+i*dz) for i in range(n)]
    for cx,cz in {(px//16,pz//16) for px,pz in coords}:
        ch = w.chunk(cx,cz)
        if ch is None:
            return ('MISSING_CHUNKS',0,0,'')
        if ch.get('Status',ch.get('status')) not in ('full','minecraft:full'):
            return ('INCOMPLETE_CHUNKS',0,0,'')
    ys = [w.dead_bush_y(px,pz) for px,pz in coords]
    if any(y is None for y in ys):
        return ('MISSING_CHUNKS',0,0,'')
    best_any=best_bad=0
    full_levels=[]
    for y in set().union(*ys):
        run_any=run_bad=0
        for (px,pz),present in zip(coords,ys):
            has=y in present
            is_b=has and w.biome_at(px,y,pz) in BADLANDS
            run_any=run_any+1 if has else 0
            run_bad=run_bad+1 if is_b else 0
            best_any=max(best_any,run_any)
            best_bad=max(best_bad,run_bad)
        if run_bad == n:
            full_levels.append(y)
    status = ('BLOCKS_MATCH_BADLANDS' if best_bad == n
              else 'WRONG_BIOME' if best_any == n else 'NOT_VERIFIED')
    return status,best_any,best_bad,';'.join(map(str,sorted(full_levels)))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--world',type=Path,required=True,help='Normal-world save folder (containing level.dat)')
    p.add_argument('--csv',type=Path,default=Path('v9_terrain_ranked.csv'))
    p.add_argument('--output',type=Path,default=Path('v9_saved_world_verified.csv'))
    args=p.parse_args()
    if not args.world.is_dir():p.error('World save directory not found')
    seed=world_seed_and_preset(args.world)
    new=World(args.world/'dimensions/minecraft/overworld','minecraft:dead_bush')
    w=new if (new.folder/'region').is_dir() else World(args.world,'minecraft:dead_bush')
    with args.csv.open(newline='',encoding='utf-8-sig') as f:
        rows=[r for r in csv.DictReader(f) if int(r['seed_signed'])==seed]
    if not rows:raise RuntimeError(f'No candidate from {args.csv} matches save seed {seed}')
    print('Vanilla normal-world seed:',seed)
    results=[]
    for row in rows:
        status,any_run,bad_run,levels=examine(w,row)
        results.append({**row,'world_verification':status,'actual_same_y':any_run,
                        'actual_badlands_same_y':bad_run,'confirmed_y':levels})
        print(f'{status} x={row["start_x"]} z={row["start_z"]} row={row["run"]} actual_badlands={bad_run} actual_any={any_run} y={levels}')
    with args.output.open('w',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=list(results[0]));writer.writeheader();writer.writerows(results)
    print('Wrote:',args.output)
    print('Evidence is from saved blocks only; for natural-world provenance use an untouched vanilla world.')
    return 0

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,ValueError,RuntimeError) as e:
        print('ERROR:',e,file=sys.stderr);sys.exit(2)