"""Regression tests of the speculative second pass (scpass.c, PAWNCC_PASS_SAVE / PAWNCC_PASS_LOAD).

1. Same source: a compile that loads the saved state writes the same AMX and messages as the
   normal compile that saved it (every script of tools/compare-tests).
2. Time: a state saved at T1 and loaded at T2 gives the AMX of a normal compile at T2
   (__timestamp, __date, __time are this compile's, not the saved ones).

    python tools/pass-tests/run.py <build dir> <include dir>
    python tools/pass-tests/run.py build-pgo "C:\\...\\MODS ZONA\\tools\\refs\\omp-stdlib"

A whole gamemode after real edits is checked by the build itself: it accepts a speculative AMX only
when the state key of the fresh first pass equals the saved one (MODS ZONA tools/pawn_build.py).
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
compiler = Path(sys.argv[1]).resolve() / "pawncc.exe"
include = sys.argv[2]
work = Path(tempfile.mkdtemp(prefix="pass-tests-"))


def run(src, out, env_extra, now="1760300000"):
    env = {k: v for k, v in os.environ.items() if not k.startswith("PAWNCC_PASS")}
    env.update(env_extra, PAWNCC_FIXED_NOW=now)
    r = subprocess.run([str(compiler), "-i" + include, "-d3", "-o" + str(work / out), str(src)],
                       cwd=src.parent, capture_output=True, env=env)
    return r.returncode, (r.stdout + r.stderr).decode("cp1251", "replace")


def amx(name):
    p = work / (name + ".amx")
    return p.read_bytes() if p.is_file() else None


bad = 0
for src in sorted((HERE.parent / "compare-tests").glob("*.pwn")):
    state = work / (src.stem + ".state")
    rc1, o1 = run(src, src.stem + "_ref", {"PAWNCC_PASS_SAVE": str(state)})
    if not state.is_file():
        print(f"{src.name}: no state saved (rc {rc1})")
        bad += 1
        continue
    rc2, o2 = run(src, src.stem + "_spec", {"PAWNCC_PASS_LOAD": str(state)})
    msgs = lambda o: [l for l in o.splitlines() if "estimated" not in l]
    ok = rc1 == rc2 and amx(src.stem + "_ref") == amx(src.stem + "_spec") and msgs(o1) == msgs(o2)
    bad += not ok
    print(f"{src.name}: {'OK' if ok else 'DIFFERENT'}")

ts = work / "timestamp.pwn"
ts.write_text("new gStamp = __timestamp;\nmain()\n{\n\tprintf(\"%d %s %s\", __timestamp, __date, __time);\n\treturn gStamp;\n}\n",
              encoding="ascii")
state = work / "ts.state"
run(ts, "t1", {"PAWNCC_PASS_SAVE": str(state)}, now="1700000000")
run(ts, "t2_normal", {}, now="1800000000")
run(ts, "t2_spec", {"PAWNCC_PASS_LOAD": str(state)}, now="1800000000")
ok = amx("t2_normal") is not None and amx("t2_normal") == amx("t2_spec")
bad += not ok
print(f"time constants: {'OK' if ok else 'DIFFERENT (the loaded state kept an old time)'}")
print("FAILED" if bad else "ALL OK")
sys.exit(1 if bad else 0)
