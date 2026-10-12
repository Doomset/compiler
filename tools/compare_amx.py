"""Compare two AMX files and report every difference with context.

    python compare_amx.py a.amx b.amx

Known benign differences between two compiler runs:
  * the compile time/date string that Pawn embeds (HH:MM:SS), when the runs are some seconds apart;
  * nothing else: code, data, publics, natives, libraries and debug info must match byte for byte.
Exit code 0 when the files are equal apart from HH:MM:SS strings, 1 otherwise.
"""
import re
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
print(f"sizes: {len(a):,} / {len(b):,}")
if len(a) != len(b):
    print("DIFFERENT SIZE")
    sys.exit(1)
diffs = [i for i in range(len(a)) if a[i] != b[i]]
print(f"differing bytes: {len(diffs)}")
if not diffs:
    sys.exit(0)
ranges, start, prev = [], diffs[0], diffs[0]
for d in diffs[1:]:
    if d - prev > 16:
        ranges.append((start, prev))
        start = d
    prev = d
ranges.append((start, prev))
time_re = re.compile(rb"\d\d:\d\d:\d\d")
unexplained = 0
for s, e in ranges:
    ctx_a = a[max(0, s - 12):e + 12]
    ctx_b = b[max(0, s - 12):e + 12]
    is_time = bool(time_re.search(ctx_a)) and bool(time_re.search(ctx_b)) and e - s < 8
    unexplained += 0 if is_time else 1
    print(f"  0x{s:08x}-0x{e:08x} {'time string' if is_time else 'UNEXPLAINED'}")
    if not is_time:
        print("    a:", ctx_a.replace(b"\x00", b"|")[:100])
        print("    b:", ctx_b.replace(b"\x00", b"|")[:100])
print("RESULT:", "equal apart from time strings" if unexplained == 0 else f"{unexplained} unexplained difference range(s)")
sys.exit(0 if unexplained == 0 else 1)
