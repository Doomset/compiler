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
| + rounds 4-5 (`2cff1a0`, `48e9904`) and two `forward`s in the gamemode | 13.2 s | identical |
| + round 6 | 12.0 s | identical |
| + LTCG + PGO build (round 7, same sources) | 10.0 s | identical |
| + round 8 (initializer fast path), LTCG + PGO | 8.9 s | identical |
| + round 9 (macro table and substitution), LTCG + PGO | 7.7 s | identical |
| + round 10 (assembler operands, data fill, UTF-8 scan cache), LTCG + PGO | **7.5 s** | identical |

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

4. Round 4 (after removing the forced re-parse pass in the gamemode itself, see below): 16.7 s -> 13.9 s.
   * Source files are read once into memory (`libpawnc.c`: `pc_opensrc` & co.), with text-mode semantics
     (`
` -> `
`, `^Z` ends the data, `feof` only after a read ran into the end). Every pass and every
     re-included file reuses the buffer; `plungequalifiedfile` no longer calls `stat`.
   * The AMX is written through a 64 KB buffer of its own (15 million tiny `fwrite`s before).
   * `findopcode` is a hash lookup on the lower-case name (the assembler calls it three times per asm line).
   * `max_stacksize_recurse` walked every call path again; a sub-tree without recursion is now computed once.

   * Round 5: `mfgets` copies just the line (it copied 256 bytes and seeked back); the assembler's first pass records\n     every instruction line, so the code and data passes no longer lex the whole file again. 13.9 s -> 13.2 s.\n
   The biggest single gain is not in the compiler: `check_reparse()` raises warning 208 and a whole extra
   first pass (~6 s) when a function with a tag is called before it is defined and has no prototype. Two
   `forward` declarations in the gamemode remove it; the generated program differs from the 2-pass build by one
   padding cell and the data addresses it shifts (checked cell by cell).

## Round 10: assembler and source scanning, 7.83 s -> 7.48 s (both LTCG + PGO; 9.28 s -> 8.59 s without PGO)

* `write_encoded_n()` (the `dumpn` of zero-filled arrays: 181 MB of data, ~45 million one-byte cells under
  compact encoding) called `pc_writebin()` once per cell; the copies are now gathered in a 4 KB block.
* The assembler's first pass already parsed every operand to compute code sizes. It now keeps the parsed
  operands (a value, or a label number resolved through `lbltab[]` when written), and the code and data
  passes write them without seeking, copying and parsing the text again. Function names are resolved in the
  first pass, which runs the `code` directives (current file for static functions) in the same order as the
  code pass; `data` directives still only take effect in the data pass. Cells are written one by one through
  `write_encoded()` as before, so the compact-encoding overflow check and its restart behave the same.
  Assembler 1.74 s -> 1.21 s (trace build).
* `scan_utf8()` read every included file completely (to classify it as UTF-8 or not) on every `#include` in
  both passes: ~39 MB per pass, one `get_utf8_char()` per byte. The answer depends only on the bytes from the
  start of the file, so it is kept with the cached file; `pc_getpossrc()` is still called, since it sets the
  one static position that the compiler hands out.

Checks: gamemode AMX identical; `ctest` 90/93; `tools/compare-tests` identical, with new scripts for the data
fill (zero and value fills above one 4 KB block, multi-byte encodings), the compact-encoding restart
(warning 232) and source encodings (UTF-8 without BOM, CP1251, BOM, malformed UTF-8 after a BOM); the fill,
float, macro and restart scripts also with `-C-` (no compact encoding).
Found on the way, unchanged: the BOM is skipped with `pc_readsrc(fp,bom,3)`, which reads only two bytes
(fgets semantics), so a file with a UTF-8 BOM fails with error 010 on the stock compiler too.

## Round 9: macro lookup and substitution, 8.55 s -> 7.72 s (both LTCG + PGO)

After round 8 the macro engine was the largest single block (~15%: `find_stringpair` 5.2%, `substpattern` ~7%).

* `find_subst()`: a macro prefix is unique (`#define` of an existing prefix deletes the old macro first), so a
  hash table from (prefix, length) to the list node replaces the "may exist" key set; a real macro use no
  longer walks every macro with the same first character. Deletion removes the entry by backward shift
  (linear probing). If the table cannot be allocated, the list search is used for the rest of the run.
* `substpattern()`: the expansion was inserted one character at a time (`strins(s,e,1)`), each moving the
  whole rest of the line. It is now built once (its length was already computed for the overflow check),
  then the tail is moved once. Warning 236 comes in the same order.

Non-PGO 10.06 s -> 9.35 s; with PGO 8.55 s -> 7.72 s. Gamemode AMX identical, `ctest` 90/93, and
`tools/compare-tests` (now also `macros.pwn`: define/undef/redefinition with warning 201, `#if defined`,
parameters, chains; `macros_subst.pwn`: long and nested expansions, `%0` inside a string; `macros_236.pwn`)
identical.

## Round 8: array initializers without the expression parser, 9.81 s -> 8.86 s (both LTCG + PGO)

A time-per-source-file profile (`PAWNCC_FILE_PROFILE=<file.tsv>` with the `-D PERF_TRACE` build: the time from
one source line to the next goes to the file of that line) showed one 14 MB file of animation tables,
`new const Float:x[401][31][7] = {{{0.1480000,-0.0334300,...}}}`, taking 1.97 s of 11 s: ~1.4 million
literals, each parsed in both passes by `constexpr()` -> `expression()` -> `hier14()` ... `hier2()` -> `primary()`
(an operator test at every level, `ldconst` code that `stgdel` throws away). Data files like it were ~20%.

`init()` now first asks `lex_initconst()`: `initvector()` has already read the element's first token and pushed
it back; when that token is a number followed by `,` or `}`, or `-` followed by a number and then `,` or `}`
(on the same line), the element is taken directly. The value is what `hier2()`/`constant()` compute (a negated
integer, or a negated `float` for a rational); the same `stgset`/`stgget`/`stgdel`/`errorset` bracket as
`constexpr()` runs; the lexer is left as after lexing the number. Anything else (expressions, symbols, tags,
strings, a value continued on the next line, fixed-point mode where `ftoi()` can warn, documentation or
recording mode) consumes nothing and goes the old way.

That file alone: 1.46 s -> 0.53 s. Without PGO the gain on the whole gamemode is ~2 s; with PGO 0.95 s
(PGO had already sped up the general path). Checks: gamemode AMX identical; `ctest` 90/93 as before; five
test scripts (negative/hex/binary/char literals, `- 0.5`, `-0.0`, `-7.25e2`, expressions, `...`, enum fields
with braces, strings, tag overrides, trailing commas, values split across lines, fixed point, no rational
support, syntax errors such as `--2`, `-`, `1 2`, `-1.0.5`, `-1x`) give the same messages and the same AMX.

## Round 7: whole-program and profile-guided build, 12.39 s -> 10.04 s

No source change: `tools/build-pgo.cmd` builds with `/GL` + `/LTCG` and Profile-Guided Optimization.
Median of 5 alternating runs each: round 6 12.39 s, LTCG alone 11.47 s (-7%), LTCG + PGO 10.04 s (-19%).
AMX identical, `ctest` the same 90/93, the PGO `pawnc.dll` imports the same DLLs (no `pgort140.dll`).

Rebuilding it (the profile belongs to the exact objects, so retrain after any source change):

1. `tools\build-pgo.cmd build-pgo gen` - instrumented `pawnc.dll` plus `pawnc.pgd`.
2. Compile the real gamemode once with `build-pgo\pawncc.exe` (same arguments as a normal build), with
   `...\VC\Tools\MSVC\<ver>\bin\Hostx86\x86` (it holds `pgort140.dll`) on `PATH`. It runs ~4x slower and
   leaves `pawnc!1.pgc` next to the DLL.
3. `tools\build-pgo.cmd build-pgo use` - relinks with the profile (`pawnc.pgd` grows when it is merged).

A file's cold paths can only get slower under PGO, never wrong: other scripts compile correctly, just with
less of the gain.

## Phase breakdown (round 5 build, `-D PERF_TRACE`)

`build-trace` prints the wall and thread-CPU time of every phase, plus the time spent inside `preprocess()`
(reading lines, directives, macro substitution) and `stgout()` (peephole optimizer + asm text output).
Gamemode, thread CPU seconds (machine loaded by an antivirus scan, so ~1.6x the quiet 13.2 s; the shares
were the same in three runs):

| Phase | CPU s | Share |
|---|---:|---:|
| first pass | 9.75 | 46% |
| - of which `preprocess` | 4.30 | |
| second pass | 8.48 | 40% |
| - of which `preprocess` | 2.56 | |
| - of which `stgout` | 0.64 | |
| assembler: tables 0.08, labels 0.92 (4.8M instructions), code 0.73, data 1.11, debug 0.08 | 2.92 | 14% |
| rest (testsymbols, max_stacksize, freeing) | ~0.2 | 1% |

Threads would not pay off: the passes are strictly sequential, and the preprocessor cannot run ahead of the
parser because `#if defined` sees symbols the parser has declared (YSI relies on it). Only the assembler (14%)
could overlap with the second pass. The preprocessor (~1/3 of the total) is the next target.

## Sampling profile after round 5 (quiet machine, 6986 samples)

Self time plus the CRT/ntdll samples attributed to their pawnc caller, grouped:

| Group | Share | Main entries |
|---|---:|---|
| preprocessor and source files | ~24% | `substpattern` 5.8, `srccache_load` (failing `stat`) 5.5, `find_stringpair` 3.9, `substallpatterns` 2.0, `stripcomment` 1.8, `skim` 1.7 |
| assembler | ~18% | `assemble` 5.9 (3.1 of it CRT calls), `mfgets` 2.7, `pc_writebin` 2.8, `write_encoded` 2.2, `hex2long` 1.2, `findopcode` 1.1 |
| parser and symbols | ~16% | `plnge` 2.6, `find_symbol` 2.7, `nextop` 1.6, `hier*` ~4 |
| lexer | ~15% | `lex` 6.4, `matchtoken` 3.0, `number`/`ftoi` 4.2 |
| code generator text | ~12% | `stgwrite` 6.3, `stgget` 1.3, `itoh` 1.3 |

`-D PERF_TRACE` also counts the source loader: 1691 files (38.8 MB) read in ~0.2 s, but 14,342 `stat` calls on
missing paths take 0.7 s (the missing-path cache is per exact string; every include dir x extension x name
misses once). Under an antivirus scan `srccache_load` waits much longer (18% of a loaded run's samples).

## Round 6: 13.21 s -> 11.99 s (median of 5 alternating runs each)

* Missing include paths are answered from a per-directory listing (`FindFirstFileExA`, once per directory,
  ASCII-case-folded and sorted) instead of a `stat()` each: 11,945 of the 14,342 failing `stat` calls go.
  Only a definite "missing" is answered; names the listing cannot judge exactly (non-ASCII, wildcards, `~`
  short names, `:` streams, trailing dots or spaces, device names, roots, doubled separators) and
  directories that cannot be listed still go to `stat()`. The 2,397 left are Cyrillic file names.
* `lex` tests for a digit once before `number()`/`ftoi()`; all of their parsers return 0 otherwise, and
  every symbol went through both.
* `isdigit`/`isalpha`/`isspace`/`isupper`/`isalnum`/`tolower`/`toupper` are ASCII inline functions under
  MSVC (`sc.h`). The compiler never calls `setlocale()`; in the "C" locale they match the CRT for every value
  from -128 to 255 (checked by a test program against ucrtbase).

Checks: gamemode AMX identical to round 5 apart from the time string; `ctest` the same 90/93; an include
test (other letter case, `/` and `\`, `#tryinclude` into a missing directory, a missing file) gives the
same messages and the same AMX on both builds.

## Round 12: speculative second pass (scpass.c)

The second pass depends only on the state the first pass leaves: the global symbol table (usage flags,
tags, prototypes, array sizes), the tag, library, automaton, state and state-combination tables, the
heap list and the settings a #pragma changes. `PAWNCC_PASS_SAVE=<file>` writes that state where the
second pass starts; `PAWNCC_PASS_LOAD=<file>` skips the first pass and starts from a saved state.

A build runs both at once (MODS ZONA `tools/pawn_build.py`): a normal compile that saves the new
state, and a second pass from the previous build's state. Besides the state, the save writes
`<file>.key`, the state without what the second pass sets again before reading it: declaration line
numbers, code addresses, function and global variable addresses (a variable cannot be used before
its declaration, error 017, also in `#emit` and `sizeof`; calls go through labels), stack sizes,
referrer lists, `__line`, `__timestamp` and the data address of the string constants. Equal keys,
equal second passes: the build takes the speculative AMX if its compile printed no message (message
positions are not part of the key) and stops the normal one.

Gamemode (PGO build): normal compile 7.1 s, second pass from a state 4.65 s. An edit inside function
bodies keeps the key (number, text, added lines, a new branch; 14 random edits across the gamemode:
14 hits, AMX byte-identical to a normal compile each time). A new global symbol changes it and the
build waits for the normal compile. `PAWNCC_FIXED_NOW=<unix time>` fixes `__timestamp`/`__date`/
`__time` for reproducible output. Tests: `tools/pass-tests/run.py`; why a build missed:
`tools/pass_key_diff.py`. `PAWNCC_PASS_DEBUG=1` prints the progress of a load.

Found on the way: the slots of `arginfo.dim`/`idxtag` past `numdim` are left uninitialized by the
compiler (the key writes zeros there); the second pass stores `__date`/`__time`/`__file` only if the
first pass marked them used, so the usage of predefined symbols is part of the state.

## Round 13: the include cache judges Cyrillic names and skips stat() for listed files

`-D PERF_TRACE` on the gamemode after round 12: 2,405 `stat()` calls on missing paths were left, 2,388 of
them Cyrillic file names (the listing compared ASCII only) and 17 names of directories; the 1,692 files
that do exist each had a `stat()` before the `fopen()` as well. "miss+stat" was 0.275 s per compile.

* A non-ASCII name is converted as the file API does (`AreFileApisANSI`) and looked up in a wide listing
  of its directory (`FindFirstFileExW`, read on the first such name). Both sides are upper-cased by the
  fixed case pairs of ASCII and U+0400..U+045F, which every NTFS upper-case table has; a name with any
  other non-ASCII letter (in the query or in the directory) still goes to `stat()`.
* A directory in the ANSI listing ends in '/', so a name that is a directory is answered "missing" (a
  directory is no source file, as the `stat()` check said).
* `dircache_lookup()` also answers "a file": a file of the listing is opened without `stat()`; names it
  cannot judge (device names, roots, wildcards, short names...) keep the `stat()`.

Trace: stat failures 2,405 -> 0, "miss+stat" 0.275 -> 0.076 s (760 listings, 125 of them wide, 0.054 s).
The PGO build compiles the gamemode 6.90 -> 6.83 s (median of 4 alternating runs; a warm file cache
answers `stat()` faster than the trace suggests). Checks: gamemode AMX identical to round 12 with
`PAWNCC_FIXED_NOW`; `compare-tests` 0 mismatches; `pass-tests` all OK; `tools/include-tests/run.py`
(new: ASCII and Cyrillic names in other letter case, Ё/ё, both separators, missing files and directories,
a directory namesake, a directory with Greek and Latin-1 names, `-i` paths) gives the same messages and
AMX as round 12.

Found on the way: a Cyrillic file name without its extension (`#include "mod/ёлка"`) is not found in any
version; the gamemode always writes `.inc`.

## Round 14: the assembler's label pass on parallel chunks

The assembler already parsed the intermediate text once (the code and data passes replay the records of
the label pass), but that label pass went over 4.5 million lines on one thread: 0.66 s, the longest serial
step after the parser. The intermediate file is in memory (`pc_asmbuffer`), so it is cut after a '\n' into
chunks that threads parse like the serial loop (pieces as `mfgets()` returns them, the same handlers in a
chunk-local form). The merge appends the chunks in file order: labels get the code size of the chunks
before them, and the two order-dependent things -- "code" file directives (`fcurrent`) and the lookup of
function names, which depends on `fcurrent` -- run in file order there. Anything the serial pass would
report or assert on drops the parallel result and the serial pass runs. `PAWNCC_ASM_THREADS=n` sets the
chunks (1: serial); by default half the processors, 2 to 8, for an intermediate file of 1 MB or more.

Trace on a loaded machine (other builds running), median of 3 alternating runs: label pass 0.89 s serial,
0.40 s on 4 chunks, 0.27 s on 8 (of which the merge ~0.09 s); quiet, the serial pass is 0.66 s. MODS ZONA
stand: speculative compile 4.3 -> 3.7 s. Checks: gamemode AMX identical with 1, 4 and 8 chunks and to
round 13; `compare-tests` 0 mismatches, `include-tests` PASS and `pass-tests` ALL OK with the parallel pass
forced (`PAWNCC_ASM_THREADS=4` and `=2`, small scripts included).

## Remaining profile (not done)

`stgwrite`/`assemble` text handling (the code generator writes text, the assembler parses it back three times),
`lex`/`substpattern` on the YSI-heavy sources, and `find_stringpair`.

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

