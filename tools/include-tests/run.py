"""Include search: two compilers find the same files and give the same messages and AMX.

The missing-path cache of srccache_load (libpawnc.c) answers "missing" and "a file" from directory
listings instead of stat(). This test builds a tree with the cases that cache must judge right or leave
to stat(), compiles it with a reference compiler and a candidate, and compares the results:
- ASCII and Cyrillic (CP1251) names in another letter case, Ё/ё, '/' and '\\' (a Cyrillic name is
  written with its extension: without one pawncc does not find it, in any version);
- #tryinclude of a missing file, into a missing directory, of a name that is a directory;
- a Cyrillic name in a directory that also holds a name with other letters (left to stat());
- a file found through an include path (-i) after misses in the current directory.

    python tools/include-tests/run.py <reference build dir> <candidate build dir> <include dir>

Both compilers must find the same default includes (<exe>\\..\\include): put the candidate next to the
reference, e.g. MODS ZONA\\compiler and MODS ZONA\\compiler-cand.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ref = Path(sys.argv[1]).resolve() / "pawncc.exe"
cand = Path(sys.argv[2]).resolve() / "pawncc.exe"
include = sys.argv[3]
work = Path(tempfile.mkdtemp(prefix="include-tests-"))
src = work / "src"

FILES = {
    # path relative to src (str, written as CP1251) -> content
    "inc/lower_ascii.inc": "#define INC_LOWER_ASCII 1\n",
    "модуль/ядро.inc": "#define INC_CORE 2\n",
    "модуль/ёлка.inc": "#define INC_YO 3\n",
    "модуль/Смешанный.inc": "#define INC_MIXED 4\n",
    "чужие/данные.inc": "#define INC_FOREIGN_DIR 5\n",
    "чужие/ünïcödé.inc": "#define INC_UNICODE 6\n",
    "чужие/αβγ.inc": "#define INC_GREEK 7\n",
    "каталог.inc": "#define INC_DIR_NAMESAKE 8\n",
    "каталог/внутри.inc": "#define INC_INSIDE 9\n",
    "ipath/найти.inc": "#define INC_IPATH 10\n",
    "ipath/ascii_ip.inc": "#define INC_IPATH_ASCII 11\n",
}
MAIN = r'''
#include "INC/LOWER_ASCII"
#include "модуль\ЯДРО.INC"
#include "МОДУЛЬ/ЁЛКА.inc"
#include "модуль/смешанный.Inc"
#include "чужие/ДАННЫЕ.inc"
#tryinclude "чужие/нет_такого.inc"
#tryinclude "нет_каталога/ядро.inc"
#tryinclude "модуль/ядро2.inc"
#tryinclude "каталог"
#include "каталог/ВНУТРИ.inc"
#include <НАЙТИ.inc>
#include <ASCII_IP>
#tryinclude <отсутствует.inc>
#tryinclude "Модуль.inc"

main()
{
    new total = INC_LOWER_ASCII + INC_CORE + INC_YO + INC_MIXED + INC_FOREIGN_DIR + INC_INSIDE + INC_IPATH
        + INC_IPATH_ASCII;
#if defined INC_DIR_NAMESAKE
    total += 100;
#endif
    return total;
}
'''
# with a missing include: the message must name the same file
MISSING = '''
#include "модуль/НЕТ.inc"
main() {}
'''


def write(rel, text):
    p = src / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    try:
        p.write_bytes(text.encode("cp1251"))
    except UnicodeEncodeError:  # the foreign names: their content is ASCII
        p.write_bytes(text.encode("ascii"))


for rel, text in FILES.items():
    write(rel, text)
(src / "main.pwn").write_bytes(MAIN.encode("cp1251"))
(src / "missing.pwn").write_bytes(MISSING.encode("cp1251"))


def compile_with(compiler, name, tag):
    out = work / f"{name}_{tag}"
    r = subprocess.run([str(compiler), "-i" + include, "-i" + str(src / "ipath"), "-d3", "-o" + str(out),
                        name + ".pwn"], cwd=src, capture_output=True,
                       env=dict(os.environ, PAWNCC_FIXED_NOW="1760300000"))
    text = (r.stdout + r.stderr).decode("cp1251", "replace").replace(str(work), "<work>")
    lines = [l for l in text.splitlines()
             if l.strip() and not l.startswith("Pawn compiler") and not l.startswith("[perf]")]
    amx = Path(str(out) + ".amx")
    return r.returncode, lines, amx.read_bytes() if amx.is_file() else None


bad = 0
for name in ("main", "missing"):
    a = compile_with(ref, name, "ref")
    b = compile_with(cand, name, "cand")
    same = a == b
    print(("ok   " if same else "DIFF ") + f"{name}: rc {a[0]}/{b[0]}, AMX {'same' if a[2] == b[2] else 'differs'}")
    if not same:
        bad += 1
        print("  ref: ", a[1])
        print("  cand:", b[1])
    elif name == "main" and a[0] != 0:
        bad += 1
        print("  main.pwn must compile:", a[1])
    elif name == "missing" and (a[0] == 0 or not any("нет" in l.lower() for l in a[1])):
        bad += 1
        print("  missing.pwn must fail naming the file:", a[1])
shutil.rmtree(work, ignore_errors=True)
print("FAIL" if bad else "PASS", f"{bad} case(s) differ")
sys.exit(1 if bad else 0)
