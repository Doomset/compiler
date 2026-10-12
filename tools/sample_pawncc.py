"""Sampling profiler for the 32-bit pawncc.exe on Windows (no VS profiler needed).

    python sample_pawncc.py <pawncc.exe> <module-dir-with-pawnc.dll+pdb> [-- pawncc args...]

Every ~1 ms the main thread is suspended, its WOW64 context read (EIP, ESP) and a conservative stack scan finds
return addresses inside pawnc.dll (a word whose preceding bytes are a CALL). Symbols come from dbghelp/PDB.
Prints self time (leaf EIP) and inclusive time (function anywhere on the stack) per function.
"""
from __future__ import annotations

import collections
import ctypes
import subprocess
import sys
import time
from ctypes import wintypes as wt

sys.stdout.reconfigure(encoding="utf-8")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
winmm = ctypes.WinDLL("winmm")

PROCESS_ALL_ACCESS = 0x1F0FFF
THREAD_ALL = 0x1F03FF
TH32CS_SNAPTHREAD = 0x4
LIST_MODULES_32BIT = 0x1
CTX_FLAGS = 0x10003  # CONTEXT_i386 | CONTROL | INTEGER
CTX_SIZE = 716
OFF_EIP, OFF_ESP = 184, 196

k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.Wow64GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2), ("Index", wt.ULONG),
                ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", wt.ULONG), ("Scope", wt.ULONG), ("Tag", wt.ULONG),
                ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_wchar * 512)]


def thread_ids(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    ids = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            ids.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return ids


def modules(hproc):
    arr = (wt.HMODULE * 256)()
    need = wt.DWORD()
    if not psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(need), LIST_MODULES_32BIT):
        return {}
    out = {}
    for i in range(need.value // ctypes.sizeof(wt.HMODULE)):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleFileNameExW(hproc, arr[i], name, 260)
        out[name.value.lower()] = arr[i] if isinstance(arr[i], int) else ctypes.cast(arr[i], ctypes.c_void_p).value
    return out


def main():
    exe, moddir = sys.argv[1], sys.argv[2]
    extra = sys.argv[4:] if len(sys.argv) > 3 and sys.argv[3] == "--" else []
    winmm.timeBeginPeriod(1)
    proc = subprocess.Popen([exe] + extra, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    pid = proc.pid
    hproc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    dll_path = (moddir.rstrip("\\") + "\\pawnc.dll").lower()
    base = None
    t_start = time.time()
    tid = None
    while base is None and proc.poll() is None:
        mods = modules(hproc)
        for path, b in mods.items():
            if path.endswith("pawnc.dll"):
                base = b
                dll_path = path
        time.sleep(0.01)
    if base is None:
        print("pawnc.dll never loaded")
        return 1
    dll_size = 0x200000
    dbg.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES ... keep simple
    dbg.SymInitializeW(hproc, moddir, False)
    dbg.SymLoadModuleExW.restype = ctypes.c_uint64
    got = dbg.SymLoadModuleExW(hproc, None, dll_path, None, ctypes.c_uint64(base), dll_size, None, 0)
    print(f"pawnc.dll base=0x{base:08x} symbols_loaded={'yes' if got else 'NO ' + str(ctypes.get_last_error())}")
    tids = thread_ids(pid)
    tid = tids[0]
    hthr = k32.OpenThread(THREAD_ALL, False, tid)
    ctx = ctypes.create_string_buffer(CTX_SIZE)
    stack = ctypes.create_string_buffer(16384)
    selfc, incl, total = collections.Counter(), collections.Counter(), 0
    outside = 0
    cache = {}

    def sym(addr):
        if addr in cache:
            return cache[addr]
        si = SYMBOL_INFOW()
        si.SizeOfStruct = 88
        si.MaxNameLen = 512
        disp = ctypes.c_uint64()
        ok = dbg.SymFromAddrW(hproc, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(si))
        cache[addr] = si.Name if ok else f"?{addr - base:#x}"
        return cache[addr]

    t0 = time.time()
    while proc.poll() is None:
        if k32.SuspendThread(hthr) == 0xFFFFFFFF:
            break
        ctypes.memset(ctx, 0, CTX_SIZE)
        ctypes.c_uint32.from_buffer(ctx, 0).value = CTX_FLAGS
        if k32.Wow64GetThreadContext(hthr, ctx):
            eip = ctypes.c_uint32.from_buffer(ctx, OFF_EIP).value
            esp = ctypes.c_uint32.from_buffer(ctx, OFF_ESP).value
            n = ctypes.c_size_t()
            k32.ReadProcessMemory(hproc, ctypes.c_void_p(esp), stack, 16384, ctypes.byref(n))
            k32.ResumeThread(hthr)
            total += 1
            if base <= eip < base + dll_size:
                selfc[sym(eip)] += 1
                seen = {sym(eip)}
                words = ctypes.cast(stack, ctypes.POINTER(ctypes.c_uint32))
                for i in range(n.value // 4):
                    w = words[i]
                    if base + 8 <= w < base + dll_size:
                        # a return address is preceded by a CALL (E8 rel32, FF /2 forms)
                        b = (ctypes.c_ubyte * 8)()
                        k32.ReadProcessMemory(hproc, ctypes.c_void_p(w - 8), b, 8, None)
                        if b[3] == 0xE8 or (b[6] == 0xFF and b[7] in (0xD0, 0xD1, 0xD2, 0xD3, 0xD6, 0xD7)) or b[2] == 0xFF or b[1] == 0xFF:
                            seen.add(sym(w - 1))
                for s in seen:
                    incl[s] += 1
            else:
                outside += 1
        else:
            k32.ResumeThread(hthr)
        time.sleep(0.001)
    wall = time.time() - t0
    out = proc.stdout.read().decode("cp1251", "replace")
    print(f"samples={total} wall={wall:.1f}s outside-pawnc.dll={outside} ({outside * 100 / max(total, 1):.1f}%)")
    print(out.strip().splitlines()[-1] if out.strip() else "")
    print("\n== SELF (leaf) ==")
    for name, c in selfc.most_common(25):
        print(f"{c * 100 / total:5.1f}%  {c:6d}  {name}")
    print("\n== INCLUSIVE ==")
    for name, c in incl.most_common(30):
        print(f"{c * 100 / total:5.1f}%  {c:6d}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
