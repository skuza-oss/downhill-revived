# 205: a state trace that does not depend on the core's layout

Status: in progress

Branch `fix/state-trace-core-layout` from `main` 1578b6c5, started 2026-11-11. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

The state trace (`port/hle/state_trace.c`, `port/tools/state_trace_run.py`, `state_diff.py`;
finding 081) of two builds whose game behaviour is equal must be equal, whatever the wasm core's
own layout (where its data, bss, shadow stack, heap and function table entries sit), and it must
still report every real difference. Found while checking lane 203 (finding 203): main 1578b6c5
against its parent f2dd2d4b gives boot, race, race2 and race6 equal, but race3 differs in 32 of
14000 rows, race4 in 6 and race5 in 5, although no game state differs: the patch moved the core's
data by 16 bytes.

Acceptance:
- main against main without lane 203's code: every recipe (`boot`, `race`, `race2` to `race6`)
  equal, with this lane's trace in both builds;
- a test that flips real game state (a rider word, a RAM word, a register) in a run and shows the
  diff reports it, and one that moves the core's layout and shows the diff stays empty;
- `DHD_STATE_NORM=host` works or is gone;
- the players' game is the same game (the trace is a debug facility).

Out of scope: the online rider code (lane 202) and online Mosh Pit (lane 204).

## Progress

- [x] 2026-11-11: finding reserved (d497b2db).
- [x] 2026-11-11: reproduced with the old trace: main 1578b6c5 (exe sha256 05648f8f...) against
  f2dd2d4b (b23d3e95...), `state_trace_run.py <recipe>` on both and `state_diff.py`: race3 32 of
  14000 rows differ (first field 2141, ram), race4 6 (first 5173, cpu), race5 5 (first 2655, ram);
  boot, race, race2 and race6 PASS.
- [x] 2026-11-11: the SIF packets moved to a fixed guest address (697308a1), the trace's register
  rule, core identities and DHD_STATE_NORM=host removal (9757e173), the test switches and
  `state_trace_layout_test.py` (cde55ba0, 8d3c07b0), the SIF unit test in its own memory
  (bf93b041).
- [x] 2026-11-11: the layout test found 276 function-pointer words of guest RAM and one of the
  scratchpad that the code-pointer list lacked; listed (696510ad, see Surprises).
- [x] 2026-11-11: proofs on the final list (below), references regenerated, `docs/TESTING.md`.

## The two mechanisms

**RAM, once at a race load.** `sceSifCallRpc` (`port/hle/sif.c`) gave the game's RPC client the
address of a packet in the core's own `.bss` (`pending[i].packet`), and the game keeps the client
in its RAM, so that word held a core address while the RPC was in flight (EE 0x004C8940 on race3
and race5; 0x00760A10 was masked for the same reason since finding 090). The core's data moved
16 bytes with lane 203's patch, and the trace hashed the word raw.

**Registers, during a race.** The old trace classified a register's low lane by value: a value
inside a core data symbol became (tag 2, symbol id, offset). r8 held 0x7049544E (text, "NTIp") in
both builds; it fell inside a bss object whose base moved 16 bytes, so the hashed offset differed
(0x19A9B6 against 0x19A9A6) although the raw registers were identical.

## What the trace does now

- **Nothing the HLE gives the game points into the core's data.** The SIF packets live in a 16 KB
  HLE window right above the scratchpad (`memory.h` `PS2_HLE_WINDOW`, 0x70004000, committed by
  `ps2_memory_init`), at the same address in every build. The layout test (below) moved the
  core's rodata, data and bss by 4 KB each and found no other core data address anywhere in guest
  RAM, the scratchpad or the hardware pages, on any recipe.
- **Code addresses in guest memory are listed, then hashed by identity** (as before): the data
  image's fixups plus `port/tools/state_trace_codeptrs.txt`. A code address in wasm is a
  function-table index (from 0x12100); RAM and scratchpad words are not classified by value,
  because game integers between 74000 and 139000 would then be compared as functions.
- **Register lanes** (every lane of the 32 EE GPRs, and of VU1's register snapshot in vu0): a lane
  whose value lies in the core's fixed ranges (the function table, [0x12100, 0x22100); the module
  memory, [0x70100000, 0x80000000): the same bounds in every build) is hashed in `cpu` as a
  placeholder and written to the new `regptrs` column with its raw value and its identity in this
  build: tag 1 a function, 2 a data symbol, 3 the shadow stack (offset below its top), 4 the heap
  (offset from `__heap_base`), each with an id and an offset; tag 0 none. `state_diff.py` and
  `trace_columns.py` count two lanes equal when the raw values are equal or both have an identity
  and the identities are equal; a lane in one list only, or another value with another identity,
  is a difference. So a pointer that moved with the layout is the same pointer, and a value that
  only looks like one (text, a float) is equal only when it is the same value. With equal
  layouts an identity can only match when the raw value matches, so the rule adds no blind spot
  there.
- `DHD_STATE_NORM=host` is gone, with `host_memory_regions` and its callback: it walked the 64-bit
  host process's address space, which a wasm32 core cannot hold (its callback trapped at start,
  "host process addresses have no wasm32 representation"). `fibers` stays; any other value ends
  the run with a message.
- Trace format 2: the first line says `state-trace 2` and records `core=` (the shadow stack's
  top, the heap's base, the function table's rows; not compared); the column `regptrs` is new.
  **Every stored reference trace of format 1 is invalid** and must be regenerated with its own
  executable.

## Decisions

- Registers by "raw or identity", not by provenance: a register's history is unknown at a field
  boundary, and value-only classification is wrong one way or the other (identity breaks text,
  raw breaks moved pointers). The comparison needs both builds, so it is in `state_diff.py`, and
  the hash columns keep a placeholder. Measured on the padded layout: 63 to 70 moved function
  pointers per race matched by identity, and on race2 to race5 between 261 and 712 rows had a
  lane with the same raw value but another identity, each of which the old rule would have
  reported.
- The SIF window rather than a trace rule for RAM: the address was a port choice; a fixed one makes
  the RAM word an ordinary value, also for the register that reads it. The mask of 0x00760A10 is
  removed: the word is compared again.
- Guest RAM stays raw outside the lists: a per-word raw-or-identity list for 32 MB would be too
  large for a row (any word in the module range, texture data included).
- The SIF unit test sets `PS2_HLE_WINDOW` inside its 16 MiB guest: at 0x70004000 the test's linear
  memory would be 1.8 GB.
- References: release 0.7.16's game code with this lane's trace commits on top (the game state is
  the release's; only the format changes).

## Surprises

- The function table's first entries never moved between the builds that taught the code-pointer
  list, so 276 RAM words and one scratchpad word that the game fills with function pointers at run
  time (callbacks in rider and object structs: func_00122E58, func_00123350, func_001B1748,
  func_00208C50 and others; 0x004D029C, func_00206E40, was already noted unlisted in finding 082
  W7h) were hashed raw. Any change that adds an address-taken function early in the link would
  have made every race trace differ. The layout test moved them by 64 entries; each is now listed
  with the recipes that showed it. One of them, EE 0x0093C800, is a stack slot that holds the
  pointer only sometimes.
- Between the base and main builds of this lane only 46 rodata switch tables moved; the bss did
  not. The padded build is the stronger layout proof.

## The proofs (2026-11-11, branch at 696510ad)

Builds (`python port/tools/build.py -j 6`, `DHD_BUILD_SLOTS=2`): **tip**, this branch (dhd64.exe
sha256 79023c4a..., core 0ef72929...; the SIF unit test commit and the list do not change the
core); **base**, this branch with `port/patches.py`, `port/hle/online.c` and `port/net/nettool.c`
taken from f2dd2d4b, i.e. main without lane 203 (2f92d3bc...); **pad**, the tip built by
`state_trace_layout_test.py build-pad --pad 4096` (70ba3366...: 4 KB more rodata, data and bss and
64 more table entries in front); **rel**, f0971070 (0.7.16) with this lane's code commits
(dd05a12a...).

(a) The lane-203 pair, every recipe, each `state_trace_run.py <recipe> --exe <build>` then
`state_diff.py base.tsv tip.tsv`:

    boot  rows: A 600, B 600  all 600 rows match  PASS
    race  rows: A 5000, B 5000  PASS
    race2 rows: A 14000, B 14000  PASS
    race3 rows: A 14000, B 14000  PASS
    race4 rows: A 14000, B 14000  PASS
    race5 rows: A 14000, B 14000  PASS
    race6 rows: A 14000, B 14000  PASS

The same base against rel: 7 of 7 PASS.

(b) The core's layout moved (`state_trace_layout_test.py layout --exe tip --pad-exe pad --recipes
boot,race,race2,race3,race4,race5,race6`):

    boot: core layouts 70C29920:70C29920:2068 / 70C2CD50:70C2CD50:2132 (moved); diff PASS
    race:  ... (moved); diff PASS        race2 .. race6: ... (moved); diff PASS
    layout: PASS

Before the list grew (9757e173's list) the same run failed every row from field 1 (ram); every
differing word was a function-table index moved by exactly 64 (EE dumps of boot fields 25, 300,
599 and of each race at 4 or 5 fields, `ee.bin` of both builds compared word by word).

(c) Real changes are reported (`state_trace_layout_test.py flips`, race recipe, 4100 fields):

    the player rider's position: EE 0x0078CA90
    rider flip (4050:ram:0x78CA90:0x00400000): reported; columns that differ: ram, scratch, hw, cpu, vu0, gs_stream, gs_regs
      FIRST DIFFERENCE: field 4050 (row 4050): ram, combined
    ram flip (4050:ram:0x500000:0x1): reported; columns that differ: ram
      FIRST DIFFERENCE: field 4050 (row 4050): ram, combined
    register flip (4100:reg:16:0x1): reported; columns that differ: cpu
      FIRST DIFFERENCE: field 4100 (row 4100): cpu, combined
    flips: PASS

`port/tools/test_state_diff.py` (run by `test_runtime.py`) checks the comparison rule on made-up
rows: the lane-203 r8 case, a moved data pointer, a moved function index and a moved stack address
are equal; changed text, a changed tag-0 value, a lane in one list only, another offset of the
same object, a RAM or cpu hash and a missing row are differences (13 cases).

(d) The players' game is the same game. Everything in `state_trace.c` runs only under
`DHD_STATE_TRACE` (`state_trace_init` returns first without it; `DHD_STATE_FLIP` is read there);
`DHD_CORE_LAYOUT_PAD` exists only in `build_wasm.py` and is unset in every normal build (the core
cache keys on it). The one change a player runs is the SIF packets' address: the game only reads
the packet through the pointer. Against main's own format-1 traces (old main exe), the tip's
traces of race2 to race6 have menu, phase, clock, hw, gs_stream, gs_regs, gs_vram and sound
identical in all 14000 rows (`trace_columns.py`); ram, scratch, cpu, vu0 and code differ from
field 1 because their hashed content changed (the listed words, the placeholder lanes).

Not checked: `split` and `memcard` (and the tracks the recipes do not race) on the padded layout:
they may store function pointers in words not yet listed; `state_trace_layout_test.py layout
--recipes split,memcard` finds them. Linux and the browser trace were not run (the code is
shared). No online runs (the trace is offline).

## Result

To fill in at the merge. Every format-1 reference trace is invalid; the regenerated ones (format 2,
release 0.7.16's game code, the list of 696510ad) are outside the repository with the lead.
Finding 081's line "opt-in only (`DHD_STATE_NORM=host`)" and its pointer description are then
partly superseded by this finding.
