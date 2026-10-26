#!/usr/bin/env python3
"""Small offline regression for V10 planning and safe defaults, no Java needed."""
import csv
import tempfile
from pathlib import Path
from auto_verify_v10 import (candidates, completed_keys, locate_best_actual_row,
                             main, server_properties, target_chunks, write_row)

def run():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        infile, outfile = d/"ranked.csv",d/"verified.csv"
        rows = [
            {"seed_signed":"124767051","run":"17","start_x":"15922811",
             "start_z":"-4601178","dx":"0","dz":"1","approx_strict_run":"8"},
            {"seed_signed":"178096580","run":"15","start_x":"-5000432",
             "start_z":"-645111","dx":"1","dz":"0","approx_strict_run":"15"},
            {"seed_signed":"114006365","run":"15","start_x":"6061722",
             "start_z":"-8497785","dx":"1","dz":"0","approx_strict_run":"14"}
        ]
        with infile.open("w",newline="") as f:
            w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)
        selected,columns=candidates(infile,15,12,3,set())
        assert [seed for seed,_ in selected]==[178096580,114006365]
        assert len(target_chunks(selected[0][1]))>=9
        assert "level-seed=178096580" in server_properties(178096580)
        assert "level-type=minecraft:normal" in server_properties(178096580)
        assert "pause-when-empty-seconds=-1" in server_properties(178096580)
        assert main(["--csv",str(infile),"--output",str(outfile),"--dry-run"])==0
        assert not outfile.exists(), "Dry run must not write output"
        write_row(outfile,columns,{**rows[1],"world_verification":"NOT_VERIFIED",
                  "actual_same_y":1,"actual_badlands_same_y":1})
        assert len(completed_keys(outfile))==1
        remaining,_=candidates(infile,15,12,3,completed_keys(outfile))
        assert [seed for seed,_ in remaining]==[114006365]

    # A predicted 17-row with only its middle 15 bushes is STILL a true 15-row.
    class FakeActualWorld:
        def dead_bush_y(self,x,z):
            return {76} if 1001 <= x <= 1015 and z == -12 else set()
        def biome_at(self,x,y,z):
            return "minecraft:badlands"
    size,x,z,y=locate_best_actual_row(FakeActualWorld(),
                 {"start_x":"1000","start_z":"-12","dx":"1","dz":"0","run":"17"})
    assert (size,x,z,y)==(15,1001,-12,76), (size,x,z,y)
    print("PASS: real 15-subrow detected inside 17 predicted locations")
    print("PASS: V10 seed prioritization, chunk planning, resume, dry-run, isolated server configuration")

if __name__=="__main__":
    run()
