"""Text form of the ".key" file scpass.c writes, and the difference of two of them: why a build
missed the speculative second pass (tools/_build/pass-state/last_miss_*.key of MODS ZONA).

    python tools/pass_key_diff.py old.key new.key
"""
import struct
import sys
from pathlib import Path


class R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def raw(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def i(self):
        return struct.unpack("<i", self.raw(4))[0]

    c = i

    def s(self):
        n = self.i()
        return None if n < 0 else self.raw(n).decode("latin-1")


def table(r, label, out):
    n = r.i()
    out.append(f"{label} count={n}")
    for _ in range(max(n, 0)):
        out.append(f"  {label} {r.s()} {r.c()} {r.i()}")


def dump(path):
    r, out = R(Path(path).read_bytes()), []
    out.append("magic " + r.raw(10).decode("latin-1"))
    out.append(f"cell {r.i()} namemax {r.i()}")
    out.append("scalars " + " ".join(str(r.c()) for _ in range(21)))
    table(r, "tag", out)
    table(r, "lib", out)
    out.append(f"curlib {r.i()}")
    table(r, "automaton", out)
    table(r, "state", out)
    n = r.i()
    out.append(f"statelists {n}")
    for _ in range(n):
        ns, fsa, lid = r.i(), r.i(), r.i()
        out.append(f"  statelist fsa={fsa} id={lid} " + ",".join(str(r.i()) for _ in range(ns)))
    n = r.i()
    out.append(f"heaplist {n}: " + " ".join(f"{r.i()}/{r.i()}" for _ in range(n)))
    n = r.i()
    for _ in range(n):
        name, ident, vclass, tag, usage, flags = r.s(), r.i(), r.i(), r.i(), r.i(), r.i()
        addr = "" if name in ("__line", "__timestamp") or ident == 3 else r.c()
        out.append(f"predef {name} ident={ident} vclass={vclass} tag={tag} usage={usage:#x} flags={flags:#x} addr={addr}")
    n = r.i()
    for k in range(n):
        name, ident, vclass, usage, flags, compound, tag, fnum, alev = r.s(), r.i(), r.i(), r.i(), r.i(), r.i(), r.i(), r.i(), r.i()
        addr = r.c() if ident == 4 else ""
        parent, child = r.i(), r.i()
        line = f"sym#{k + 1} {name} ident={ident} vclass={vclass} usage={usage:#x} flags={flags:#x} comp={compound} tag={tag} fnum={fnum} alev={alev} addr={addr} parent={parent} child={child}"
        if ident == 9:
            na = r.i()
            args = []
            for _ in range(na):
                an, aid, aus, nt = r.s(), r.i(), r.i(), r.i()
                tags = [r.i() for _ in range(nt)]
                nd = r.i()
                dims = [(r.i(), r.i()) for _ in range(4)]
                hd, dt = r.i(), r.i()
                if aid == 4 and hd:
                    size, asz, aaddr = r.i(), r.i(), r.c()
                    data = r.raw(size * 4).hex()
                    dv = f"arr{size}/{asz}@{aaddr}:{data[:32]}"
                elif aid == 1 and (hd & 0xC0):
                    dv = f"sizeof {r.s()} {r.i()}"
                else:
                    dv = str(r.c())
                args.append(f"{an}:{aid}:{aus}:{tags}:{nd}:{dims}:{hd}:{dt}:{dv}")
            line += " args=" + "|".join(args)
        else:
            line += f" tags={r.i()},{r.i()},{r.i()} len={r.c()} level={r.i()}"
        ns = r.i()
        if ns >= 0:
            line += " states=" + ",".join(f"{r.s()}:{r.c()}:{r.i()}" for _ in range(ns))
        out.append(line)
    out.append(f"end {r.i():#x} at {r.p} of {len(r.b)}")
    return out


if __name__ == "__main__":
    a, b = dump(sys.argv[1]), dump(sys.argv[2])
    import difflib
    diff = list(difflib.unified_diff(a, b, lineterm="", n=0))
    print(f"{len(a)} vs {len(b)} lines; {sum(1 for l in diff if l.startswith(('+', '-')) and not l.startswith(('+++', '---')))} changed")
    for l in diff[:40]:
        print(l[:220])
