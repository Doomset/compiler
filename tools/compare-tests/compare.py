"""Compile each test script with two compiler builds; diagnostics, exit code and AMX must match.

    python tools/compare-tests/compare.py <old build dir> <new build dir> <include dir with float.inc> [*.pwn]

The include dir is a Pawn standard library (e.g. omp-stdlib). Without scripts, every *.pwn next to this file runs.
"""
import pathlib
import subprocess
import sys
import tempfile

old, new, inc = (pathlib.Path(a) for a in sys.argv[1:4])
here = pathlib.Path(__file__).resolve().parent
scripts = [pathlib.Path(a) for a in sys.argv[4:]] or sorted(here.glob("*.pwn"))
bad = 0
with tempfile.TemporaryDirectory() as tmp:
    for src in scripts:
        res = []
        for build in (old, new):
            out = pathlib.Path(tmp) / "out"   # same name for both: the name is not in the AMX, but keep it equal
            p = subprocess.run([str(build / "pawncc.exe"), f"-i{inc}", "-d3", f"-o{out}", str(src.resolve())],
                               capture_output=True, text=True, cwd=src.resolve().parent)
            amx = out.with_suffix(".amx")
            res.append((p.returncode, p.stdout, amx.read_bytes() if amx.exists() else None))
            if amx.exists():
                amx.unlink()
        (ra, da, aa), (rb, db, ab) = res
        ok = res[0] == res[1]
        bad += not ok
        print(f"{src.name}: exit {ra}/{rb}, messages {'same' if da == db else 'DIFFERENT'}, "
              f"AMX {'same' if aa == ab else 'DIFFERENT'} ({'none' if aa is None else len(aa)} bytes)")
        if da != db:
            print("--- old\n" + da + "--- new\n" + db)
print("mismatches:", bad)
sys.exit(1 if bad else 0)
