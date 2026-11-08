# 151: Faster builds on every platform

Status: current
Checked: 2026-11-05

Branch `perf/build-speed` from `main` 6d232ea, started 2026-11-04. Measured on the Windows PC
(12 threads, 31 GB; the owner's game ran during most runs, so local numbers are CPU time of the
whole process tree plus cache counts, wall time only where marked quiet), the Mac build host (M1,
8 GB, -j4; wall time, unaffected) and WSL Ubuntu 22.04 on the same PC.

## Goal and acceptance

Shorter builds of the PC port for Windows, Linux, Android, the Mac and the browser: cold, no
change, a one-line change in a game unit under `src/`, a one-line change in a `port/hle` file and
a new function in port code. Nothing changes for players: the same core module for the same
source, the same game state, the shipped code's flags unchanged. Out of scope: the phone (PGO
profiles come from a phone run), the matching build, the server.

## Where the time went (before)

- **Every platform's one-line loop starts with the core** (`build_wasm.py`): a one-line game
  change rebuilt 2 objects of 10,741 and still took 331 s on the quiet PC (388 s CPU, 6,748
  processes): clang's AST dump for every recompiled function's callers (`recomp.signatures`, the
  signature cache was deleted every build), `wasm-objdump` on all ~4,500 objects (the ABI stage),
  then 47-65 s of the object cache walking its 460,000 entries to prune (after any build with a
  miss).
- **Windows compiles the module as one unit**: `core.c`, about 160 MB of C, one compiler thread:
  470 s uncached, 315-345 s after any core change (1 miss). Linux, the Mac and Android compile
  wasm2c's 8-part split instead.
- **wasm2c's own split churns**: its parts are equal function counts in module order and all
  include `core-impl.h`, which declares every function. A code-only change misses only the part
  that holds the function (plus the first part, which holds the data and so changes with every
  core), but a new function, import or declaration moves the cuts: the Mac recompiled all 8
  parts for a one-function addition (109.6 s); Android's "2 hits, 62 misses" was this.
- **Every native build re-made the module's C** (wasm2c, imports, `prepare_wasm2c`, 160 MB
  through Python): 9 s on the Mac, 23-31 s on Windows, also after a host-only or Rust change.
- **SDL3 per checkout**: downloaded into each checkout's `build/downloads` (a new worktree
  without network failed) and built again: 186 s Windows, 222 s WSL, 36 s Mac, 128 s Android.
- **The browser build's wasm-opt passes** ran on every build: a no-change browser build took
  93.5 s wall, 400 s CPU (quiet PC).
- Rust is already cached (rustgs's library in the object cache); cargo runs only for Rust changes
  (`-j 2` fixed in rustgs; a cold renderer build is 73-225 s, unchanged here).

## What changed (commits on the branch)

1. `build_host64.py` prints each step's time (`step NAME: S s`, `steps:` at the end).
2. The object cache prunes at most once a day (`DHD_OBJCACHE_PRUNE_HOURS`, a stamp in the cache):
   -30 s (quiet) to -65 s (busy) after every build with a miss.
3. The core build remembers, in `DHD_DEVCACHES/dhd-modulecache`: each recompiled name's signature
   keyed by every input of it (`recomp-signatures.json`), each object's signatures by its SHA-256
   (`object-signatures.json`), and the core key's file hashes by path, size, mtime and file id
   (`file-hashes.json`). The candidate files per name come from one identifier index (checked:
   the same files as the old per-name regex for all 712 names).
4. A split module's C in content-defined chunks (`prepare_wasm2c.stable_units`): a chunk ends after
   a function whose name hashes to a cut (256 KB to 4 MB), is named after its first function and
   includes a header with only the declarations it uses (~165 KB instead of 2.9 MB). The host's
   and the module's C compile in one pool, the largest first, linked in the same order.
   `port/tools/test_wasm2c_chunks.py`.
5. The module's C is made again only when the module, its tools, WABT or the target changed
   (`build/<target>/prepared.json`).
6. SDL3's download in `DHD_DEVCACHES/downloads`, each built SDL3 in `DHD_DEVCACHES/dhd-sdl3`.
7. The browser build's wasm-opt outputs in the object cache (`objcache.tool_output`).
8. Android compiles with half the machine's threads by default (was 2: wasm2c's 54 MB part needed
   the memory; chunks are small).

## Before and after

Local numbers are CPU time of the whole build's process tree (Windows job accounting; WSL
`time`), with the object cache's hits and misses; "wall (quiet)" only for the runs made while the
PC was idle. The Mac numbers are wall time. Scenarios: a one-line change in a game unit
(`src/unsorted/0012C9E0.c`, a constant), in a `port/hle` file (`cdvd.c`, an array size), and a
new non-inlined function in `cdvd.c` (new port code: a new function in the module).

**Core (`build_wasm.py`, Windows; every platform's build of a changed core):**

| | before | after |
|---|---|---|
| no change (core cache hit) | 4.2 s CPU, 4.4 s wall (quiet) | 4.3-5.7 s CPU |
| one-line game change | 388 s CPU, 6,748 processes, 331 s wall (quiet); 2 misses | 224 s CPU, 832 processes; 2 misses |
| no object cache (cold) | 1,165 s CPU, 21,664 processes | not measured again (same compiles; the memos start empty) |

**Windows native (`build_host64.py`; the default keeps one `core.c`):**

| | before | after | after, `--num-outputs 8` (development loop) |
|---|---|---|---|
| no change | 26.3 s CPU, 24.7 s wall (quiet) | 4.7 s CPU | 19.0 s CPU |
| one-line game change | 319 s CPU, 332 s wall (quiet): core.c, 1 miss | the same work (one unit) | 73 s CPU, 2 of 177 units |
| one-line port/hle change | 385-492 s CPU, core.c | the same | 67 s CPU, 4 units |
| new port function | 466 s CPU, core.c | the same | 97 s CPU, 14 units |
| no object cache | 1,013 s CPU: SDL3 186, Rust 225, host C 151, module C 470 s (one thread) | | fill: 563 s CPU, C step 188 s instead of 470 + 151 |
| SDL3 in a new checkout | 186 s | 0.3 s | |
| after any build with a miss | + 30 s (quiet) to 65 s pruning walk | 0 (once a day) | |

**Android (`build_apk.py`, debug APK, `--jobs 6` both; CPU):**

| | before | after |
|---|---|---|
| no change | 70.4 s | 58.7 s (Gradle's packaging is ~50 s of both) |
| one-line game change | 215 s (2 of 64 misses) | 120 s (2 of 181) |
| new port function | 643 s (8 misses: every part) | 92 s native (14 of 181) + packaging |
| no object cache, host units cached | 886 s | 784 s |
| SDL3 in a new checkout | 128 s | a copy |

**Mac (M1, -j4, wall):**

| | before | after |
|---|---|---|
| no change | 11.5 s | 2.0 s |
| one-line game change | 42.4 s (2 misses, one part: 31 s) | 22.9 s (2 misses) |
| one-line port/hle change | 81.2 s (3 misses, 70 s) | 22.7 s (4 small) |
| new port function | 109.6 s (8 misses: every part) | 22.4 s (14 of 176) |
| no object cache (C step) | 176 s (host 78, module 98) | 144 s (one pool) |
| SDL3 in a new checkout | 36 s | 0.1 s |

**Linux (WSL Ubuntu 22.04, the 8-part C, CPU user+sys):**

| | before | after |
|---|---|---|
| no change | 19.4 s (wall 22.9 s) | 4.9 s (wall 8.8 s) |
| one-line game change | 244 s (2 misses, module C 135 s) | 72 s (2 of 174) |
| one-line port/hle change | 463 s (3 misses, module C 268 s) | 60 s (4) |
| new port function | 979 s (8 misses: every part, module C 370 s) | 96 s (14) |
| SDL3 in a new checkout | 222 s (146 s on a later run) | a copy |

**Browser (`port/web/build.py`, CPU):**

| | before | after |
|---|---|---|
| no change | 455 s (wall 93.5 s quiet) | 9.5 s |
| after a core change | 452 s | 402 s (the core's own wasm-opt pass) |

## Proven unchanged

- **Core module:** main's source built with the old and the new tools gives the same
  `dhd_core.wasm`, sha256 `b0a6059a32f7b07d...` (core id d126b9ca0280397d); the one-line game
  change gives `9eedaba4cdcfeabb...` with both.
- **Windows game:** `dhd64.exe` from main's `build_host64.py` and from the branch's, same core:
  identical apart from the link stamp (`objcache.py pe-sha`: 829f5bc1... both). State traces
  (`state_trace_run.py`, on WARP, `DHD_WGPU_ADAPTER=fallback`, so no GPU was used while the PC's
  owner played): boot 600 and race 5000, all rows match. A Windows build of the chunked C
  (`--num-outputs 8`, the path Android, Linux and the Mac now take) against the same base: boot
  600 and race 5000, all rows match.
- **Checks:** `python tools/check.py port -j 6` on the final branch: check_docs, check_tree,
  build_wasm (core sha256 `b0a6059a...` again), test_runtime, build_game, net_tests, test_gs_wgpu
  all OK; `python port/tools/test_wasm2c_chunks.py` PASS.
- **Android:** not run on a phone (not this lane's). Its code is the same C in other units (the
  x64 trace of the chunked build above covers the C); the inlining across the 8 parts and the
  chunks differs, its speed was not measured. A PGO profile keeps applying: wasm2c's function
  names are the same in both splits.

## Decisions

- Windows keeps its one-unit `core.c` by default: chunking it changes the shipped code (functions
  no longer static to one unit, fewer inlined; the dispatch specialisation of `prepare_wasm2c`
  only reaches calls in the unit that holds the table wrappers), and a speed measurement of the
  game needs the PC to itself and the GPU. `build.py -- --num-outputs 8` is the development loop;
  making it the default is a separate decision with a speed run (numbers below).
- Content-defined chunks rather than a hash bucket per function: neighbours in the module stay
  together (callers and callees of one source unit), as in wasm2c's own split.
- **Release builds keep wasm2c's own split** (`build_host64.py --release`, 2026-11-05): the chunks
  cost the game on the phone. Throttled Mi 11i (t612), 2x, unlocked, the same core (b0a6059a) and
  PGO profile, 4 runs a build in both orders: race start (6400-7300) 69.80 -> 68.69 fields/s
  (-1.6 %), the 9000-14000 window 88.37 -> 87.23 (-1.3 %); the new build was slower in all 8
  pairs, whichever ran first. The chunks stay the development default (Android, Linux, the Mac);
  `split.txt` beside the game (in an APK, `assets/dhd-split.txt`) says which C it was built from
  (`one`, `wasm2c`, `chunks`), and `package_release.py` and the APK's release check refuse
  `chunks`.
- The memo files are plain JSON next to the core cache, keyed by content, shared by checkouts;
  each starts again past a size cap (no pruning walk).
- Android's default job count follows the machine (half its threads, at least 2); the Mac keeps 4
  (8 GB).

## Surprises

- The core build's fixed cost, not compiling, dominated every one-line change: 2 misses, 331 s.
- 5,099 `wasm-objdump` and thousands of `clang -ast-dump` processes per core build; on this PC each
  Windows process costs ~25 ms of kernel time, half of a core build's CPU.
- wasm2c's split is stable for code-only changes (2 misses on the Mac); it is new functions and
  imports that recompile everything.
- The Mac's command-line tools carry an SDK (27.2) newer than their `ld`: every link failed with a
  tapi error; `SDKROOT=$(xcrun --sdk macosx --show-sdk-path)` (Xcode's 26.5 SDK) works
  (docs/BUILDING.md).
- A test edit must survive the optimiser: a noinline identity function was folded away (the module
  came out byte-identical), so the "new function" case uses one with an empty volatile asm.

## Left

- **The quiet-PC wall-time round** for Windows, Linux (WSL), Android and the browser: the PC's
  owner played during almost every local run, so the local tables are CPU time and cache counts
  (the measuring scripts were local; the runs are described above). Only the base's no-change
  and one-line runs got quiet wall times.
- **Windows' one-unit `core.c`** (315-345 s of one thread after any core change): the biggest
  local cost left. `--num-outputs 8` turns it into 67-97 s of CPU across threads, but it changes
  the shipped code; switching the default needs a speed run of the game on an idle PC.
- **The core's own fixed cost** after a one-line change, ~125 s: the recompiler's fixed point (10
  rounds of `recomp.recompile` over all ~700 names, ~7 s each; recompiling only the new names
  each round would change its contract), transforming 3,815 units (16 s), the offset scan (16 s).
- **Gradle** packages every APK without a daemon (~50 s of CPU per build).
- **The object cache keys include the output folder**: `build_host64.py --out` elsewhere misses
  everything.
- **rustgs builds with `-j 2`** (a cold renderer build: 73 s Mac, 225 s Windows); only Rust
  changes and new cargo target dirs pay it.
- **Data moves**: a change that adds data to the module shifts the addresses later functions use,
  so those chunks change too; not measured separately.

## Result

Every platform's loop after a one-line change is shorter: the core 388 -> 224 s CPU (Windows),
then the native build of the chunked C (Mac 42-110 -> 22-23 s wall; Linux 244-979 -> 60-96 s CPU;
Android 215 -> 120 s CPU with packaging), or on Windows still the one `core.c`. No-change rebuilds:
Mac 11.5 -> 2.0 s, Windows native 26 -> 5 s CPU, Linux 19 -> 5 s CPU, browser 455 -> 9.5 s CPU. A new
checkout copies SDL3 (186 s Windows, 36 s Mac). Players get the same core and, on Windows, the
same executable.
