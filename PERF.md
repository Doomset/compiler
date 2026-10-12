# Compile-time performance work (branch `perf/symbol-table`)

Target: a 34 MB Pawn gamemode (YSI + open.mp includes, ~100,000 global symbols, 75 MB AMX with `-d3`).
Machine: Xeon E5-2690 v3 @ 2.6 GHz, Windows 10, MSVC 2022 (Win32, Release, `sNAMEMAX=63` as in the official CI).

## Result

| Build | Compile time | AMX |
|---|---|---|
| unmodified `master` (29f1a8c) | 58.9 s | reference |
| + round 1 (`a9d0e62`) | ~35 s | identical |
| + round 2 (`8582b56`) | 30.2 s | identical |
| + round 3 (`79d1535`) | **22.1 s** | identical |

"Identical" means byte for byte except the compile time string that Pawn embeds (`tools/compare_amx.py`
reports it as a time string). `ctest` gives the same result as the unmodified build: 90 of 93 tests pass,
the same three fail on both (`__pragma`, `__timestamp`, `gh_353_symbol_suggestions`).

## What was slow (sampling profile, `tools/sample_pawncc.py`)

pawncc is single-threaded (98% of one core for the whole run), so the profile is a plain function histogram.

1. `add_symbol` 23% and `reduce_referrers` 21%: both quadratic.
   * `add_symbol` walked the sorted global symbol list from its head for every insertion. A sorted array of
     every 64th inserted symbol now gives a start point behind which the walk continues. Symbols renamed by
     `rename_symbol` (operators) sit out of order, so the index is used only for names larger than the
     largest renamed name; the resulting list is exactly the one the plain walk builds.
   * `reduce_referrers` rescanned every symbol and referrer list for each dead function and restarted until
     nothing changed. It is now a work list over a reverse index (same fixed point).
2. After that ~40% of the time was outside `pawnc.dll` (heap, CRT, kernel). Attributing those samples to
   their calling pawncc function showed:
   * `pc_writebin`: one `fwrite` per AMX cell (~19M calls), each taking the stream lock -> `_fwrite_nolock`.
   * `plungequalifiedfile`: 70,883 include tries, 89% failing, each a `stat` + `fopen` (and the result of
     `stat` was ignored) -> a failing `stat` ends the try, missing paths are remembered by exact string.
   * `findopcode`: `stricmp` at every binary search step -> fold to lower case once, `strcmp`.

3. Round 3, the macro table (20,000+ macros): `#define`/`#undef` walked the whole sorted list twice
   (`adjustindex`, `insert_stringpair`, `delete_stringpair`) and every identifier walked the bucket of its first
   character. All three now start at the head of the nearest lower bucket, and `find_subst` asks a hash set of
   `(length, prefix)` keys first (keys are never removed, so a hit always runs the original search).
   `lex()` compared every operator and reserved word with the first character of every token; the tokens are
   now grouped once by first character, in table order.

## Remaining profile (not done)

Line reading with `fgets` (the UCRT reads character by character; ~7%), the include search that still
reaches the file system (~3%), `stgwrite`/`assemble` text handling, and reading the same include files again
in the second pass (an in-memory source cache would remove most of it but must emulate text-mode reads).

## Tools

* `tools/build-msvc.cmd [dir] [extra flags]` - build like the official Windows CI.
* `tools/sample_pawncc.py <pawncc.exe> <dir with pawnc.dll+pdb> -- <pawncc args>` - sampling profiler
  (needs a `/Zi` + `/DEBUG` build for symbols).
* `tools/compare_amx.py a.amx b.amx` - byte comparison that explains the embedded time string.
* `-D PERF_TRACE` (C flag) prints insertion and include-search counters to stderr.

## Verifying a change

1. Build the unmodified compiler and the changed one.
2. Compile the same sources with both (same output name length and options) and run `compare_amx.py`.
3. `ctest --test-dir <build> -j8` must give the same pass/fail set on both builds.
