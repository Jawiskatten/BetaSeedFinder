#!/usr/bin/env python3
"""Vanilla 26.3 candidate world generator/Anvil verifier (stdlib only).

Deliberately limited to a short ranked list: spawning worlds is expensive.
Requires Java 25+, the official Mojang 26.3 server jar, and --accept-eula.
Server binds to 127.0.0.1 and is controlled through its own stdin (no RCON).
The process creates a new, isolated directory for each distinct candidate seed.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.request import urlopen
from urllib.parse import urlparse

from verify_world_v4 import World, get_world_seed, verify

VERSION = "26.3"
MANIFEST = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"


def download_server(path: Path):
    """Download only Mojang's official server, checking the manifest SHA-1."""
    with urlopen(MANIFEST, timeout=20) as response:
        manifest = json.load(response)
    versions = [v for v in manifest['versions'] if v['id'] == VERSION and v['type'] == 'release']
    if len(versions) != 1:
        raise RuntimeError('Mojang version manifest does not list 26.3 release')
    meta_url = versions[0]['url']
    if urlparse(meta_url).hostname not in {'piston-meta.mojang.com','launchermeta.mojang.com'}:
        raise RuntimeError('Unexpected Minecraft version metadata host')
    with urlopen(meta_url, timeout=20) as response:
        details = json.load(response)
    server = details['downloads']['server']
    url = server['url']
    if urlparse(url).hostname not in {'piston-data.mojang.com','launcher.mojang.com'}:
        raise RuntimeError('Unexpected server download host')
    expected = server['sha1'].lower()
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix('.download')
    sha1 = hashlib.sha1()
    try:
        with urlopen(url, timeout=60) as src, temp.open('wb') as dst:
            while True:
                part = src.read(1024 * 1024)
                if not part: break
                sha1.update(part)
                dst.write(part)
        if sha1.hexdigest() != expected:
            raise RuntimeError('Mojang server jar checksum did not match')
        temp.replace(path)
    finally:
        temp.unlink(missing_ok=True)
    print(f'Downloaded verified vanilla {VERSION} server jar: {path}')


def java_check(binary: str):
    completed = subprocess.run([binary, '-version'], capture_output=True, text=True, timeout=12)
    report = completed.stderr + completed.stdout
    match = re.search(r'\b(?:version |openjdk )"?(\d+)', report)
    if not match or int(match.group(1)) < 25 or completed.returncode:
        raise RuntimeError('Minecraft 26.3 requires Java 25+. Found: ' + report.strip().splitlines()[0][:180] if report.strip() else 'Java not found')
    print('Java:', report.strip().splitlines()[0])


def load_shortlist(path: Path, maximum: int, min_run: int):
    selected = []
    unique_seeds = set()
    with path.open(newline='', encoding='utf-8-sig') as f:
        for row in csv.DictReader(f):
            if int(row['run']) < min_run:
                continue
            seed = int(row['seed_signed'])
            if not -(1 << 63) <= seed < (1 << 63):
                raise ValueError('Seed out of 64-bit signed range')
            if seed in unique_seeds:
                continue
            x,z = int(row['start_x']),int(row['start_z'])
            dx,dz = int(row['dx']),int(row['dz'])
            run=int(row['run'])
            if abs(x)>29_999_900 or abs(z)>29_999_900 or dx+dz!=1 or (dx,dz) not in {(0,1),(1,0)} or not 1<=run<=64:
                raise ValueError('Invalid candidate position/direction/length')
            selected.append(dict(row,seed_signed=str(seed)))
            unique_seeds.add(seed)
            if len(selected)>=maximum: break
    return selected


def send(proc, command):
    if proc.poll() is not None:
        raise RuntimeError(f'Vanilla server exited with code {proc.returncode}')
    proc.stdin.write(command + '\n')
    proc.stdin.flush()


def read_last_log(path, chars=1800):
    if not path.exists(): return ''
    with path.open('rb') as f:
        f.seek(max(0, path.stat().st_size - chars))
        return f.read().decode('utf-8', 'replace')


def until_ready(proc, log, deadline):
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError('Vanilla server died during startup:\n' + read_last_log(log))
        # Server prints the ready message before it can process console commands.
        if re.search(r'Done \([0-9.]+s\)!', read_last_log(log, 8192)):
            return
        time.sleep(0.4)
    raise TimeoutError('Vanilla server startup timed out:\n' + read_last_log(log))


def all_row_chunks_full(world, x,z,dx,dz,n):
    seen = {( (x+dx*i)//16, (z+dz*i)//16) for i in range(n)}
    world.chunk.cache_clear()
    for cx,cz in seen:
        ch = world.chunk(cx,cz)
        if ch is None:
            return False
        # Full chunks are required; absent/partial structures are not evidence.
        status = str(ch.get('Status', ch.get('status', '')))
        if status not in {'minecraft:full','full'}:
            return False
    return True


def generate_one(server_jar, java, scratch, candidate, startup_timeout, chunk_timeout):
    seed=int(candidate['seed_signed']);x=int(candidate['start_x']);z=int(candidate['start_z'])
    dx=int(candidate['dx']);dz=int(candidate['dz']);run=int(candidate['run'])
    scratch.mkdir(parents=True)
    # Explicit accept-eula flag is checked in main before reaching here.
    (scratch/'eula.txt').write_text('eula=true\n', encoding='ascii')
    # Dedicated isolated world. No publicly reachable listener or remote console.
    props = (f'level-name=world\nlevel-seed={seed}\nlevel-type=minecraft:normal\n'
             'server-ip=127.0.0.1\nserver-port=25568\n'
             'enable-rcon=false\nonline-mode=true\nmax-players=1\n'
             'sync-chunk-writes=true\nspawn-protection=0\n'
             'view-distance=2\nsimulation-distance=2\nmax-tick-time=-1\n')
    (scratch/'server.properties').write_text(props, encoding='ascii')
    log=scratch/'server.log'
    world=World(scratch/'world', 'minecraft:dead_bush')
    with log.open('wb') as log_f:
        proc=subprocess.Popen([java,'-Xms256M','-Xmx2G','-jar',str(server_jar),'nogui'],
                              cwd=scratch, stdin=subprocess.PIPE, stdout=log_f, stderr=subprocess.STDOUT,
                              text=True, encoding='utf-8', errors='replace')
        try:
            until_ready(proc, log, time.monotonic()+startup_timeout)
            # Bound all relevant chunks, plus one chunk on each side.
            x2=x+dx*(run-1);z2=z+dz*(run-1)
            a,b,c,d=min(x,x2)-16,min(z,z2)-16,max(x,x2)+16,max(z,z2)+16
            send(proc,f'forceload add {a} {b} {c} {d}')
            deadline=time.monotonic()+chunk_timeout
            next_save=0.0
            while time.monotonic()<deadline:
                if proc.poll() is not None:
                    raise RuntimeError('Server exited during generation:\n'+read_last_log(log))
                now=time.monotonic()
                if now>=next_save:
                    send(proc,'save-all flush')
                    next_save=now+4
                if all_row_chunks_full(world,x,z,dx,dz,run):
                    status,levels,longest=verify(world,x,z,dx,dz,run,require_badlands=True)
                    # If blocks are absent in already-saved full chunks, verification is decisive.
                    return status, levels, longest
                time.sleep(0.5)
            return 'CHUNK_TIMEOUT',[],0
        finally:
            if proc.poll() is None:
                try:send(proc,'stop')
                except (OSError,RuntimeError):pass
                try:proc.wait(timeout=25)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    try:proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=5)
            if proc.stdin:proc.stdin.close()


def main(argv=None):
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--download-server',action='store_true', help='Fetch official 26.3 server with SHA-1 verification')
    ap.add_argument('--server-jar',type=Path,default=Path('server_26.3.jar'))
    ap.add_argument('--csv',type=Path,default=Path('v5_shortlist.csv'))
    ap.add_argument('--java',default='java',help='Path to Java 25+ executable')
    ap.add_argument('--max-worlds',type=int,default=3,help='Unique worlds to generate, maximum 25')
    ap.add_argument('--min-run',type=int,default=15)
    ap.add_argument('--work-dir',type=Path,default=Path('v5_verification_work'))
    ap.add_argument('--output',type=Path,default=Path('v5_world_verified.csv'))
    ap.add_argument('--startup-timeout',type=int,default=120)
    ap.add_argument('--chunk-timeout',type=int,default=120)
    ap.add_argument('--accept-eula',action='store_true',help='You have read and agree to Mojang server EULA')
    ap.add_argument('--keep-worlds',action='store_true',help='Retain generated world saves for inspection')
    ap.add_argument('--dry-run',action='store_true',help='Show shortlist without starting server')
    args=ap.parse_args(argv)
    if args.download_server:
        download_server(args.server_jar.resolve())
        return 0
    if not 1<=args.max_worlds<=25: ap.error('max-worlds must be 1..25')
    candidates=load_shortlist(args.csv,args.max_worlds,args.min_run)
    print(f'Candidate seeds selected: {len(candidates)} (min reported XZ run {args.min_run})')
    for row in candidates:
        print(f"  seed={row['seed_signed']} XZ={row['run']} at {row['start_x']},{row['start_z']} same-row-flat={row.get('same_row_flat_longest','?')}")
    if args.dry_run:return 0
    if not candidates: return 0
    if not args.accept_eula:
        ap.error('Read https://aka.ms/MinecraftEULA then pass --accept-eula only if you agree')
    if not args.server_jar.is_file():ap.error('Missing server jar. Run --download-server first.')
    try:java_check(args.java)
    except (OSError,RuntimeError) as exc:ap.error(str(exc))
    args.work_dir.mkdir(parents=True,exist_ok=True)
    # Refuse ambiguous overwrite; CSV is append-only only for an exact matching header.
    columns=list(candidates[0])+['world_verification','verified_y','longest_actual_same_y']
    if args.output.exists() and args.output.stat().st_size:
        with args.output.open(newline='',encoding='utf-8') as f:
            if next(csv.reader(f)) != columns:ap.error('Result CSV has incompatible header')
    with args.output.open('a',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,fieldnames=columns)
        if f.tell()==0:writer.writeheader()
        for row in candidates:
            print(f"Generating seed {row['seed_signed']} in isolated vanilla 26.3 server...")
            try:
                if args.keep_worlds:
                    folder=args.work_dir/f"seed_{row['seed_signed']}"
                    if folder.exists():raise RuntimeError('Existing world folder; refusing to overwrite: '+str(folder))
                    status, ys, best=generate_one(args.server_jar.resolve(),args.java,folder,row,args.startup_timeout,args.chunk_timeout)
                else:
                    with tempfile.TemporaryDirectory(prefix='seed_',dir=args.work_dir) as tmp:
                        status,ys,best=generate_one(args.server_jar.resolve(),args.java,Path(tmp),row,args.startup_timeout,args.chunk_timeout)
            except Exception as exc:
                status,ys,best='ERROR',[],0
                print('ERROR:',str(exc)[:500])
            row=dict(row,world_verification=status,verified_y=';'.join(map(str,ys)),longest_actual_same_y=str(best))
            writer.writerow(row);f.flush()
            print(f"  {status} claimed={row['run']} same-level actual={best}; results in {args.output}")
    print('Complete. Natural-generation evidence requires clean vanilla server, matching seed, and BLOCKS_MATCH_BADLANDS.')
    return 0

if __name__=='__main__':
    sys.exit(main())