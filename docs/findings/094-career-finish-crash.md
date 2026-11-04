# 094: Crash after a Super Career finish that does not advance (issue #8)

Status: current
Checked: 2026-11-04

2026-10-27. Public issue #8: "The game always crashed whenever I didn't finish in first place in
Super Career mode". No log was attached. Reproduced on 0.5.0 (`284ad39a3`, exe sha256 `e3f017f4…`)
and on main (`067fb5061`). Fixed by a port patch.

## Cause

`func_0017A678` draws the race-end text over the finishing rider. When a career race does not
advance the career, it does this (in the runs below: 6th and 10th in Super Career race 1; a win
shows "YOU ADVANCE TO THE NEXT RACE" instead):

```
char buf[0x20];
...
sprintf(buf, "%d RACE CREDITS AWARDED", ...);
...
sprintf(buf, "%d MORE CREDITS REQUIRED TO ADVANCE", remaining);   /* func_00254D68 */
```

"12 MORE CREDITS REQUIRED TO ADVANCE" (what both runs showed) is 35 characters plus the NUL: 36
bytes into a 32-byte buffer. Any value of `remaining` overflows it ("0 MORE ..." is already 35
bytes). This is a bug in the original game. On the EE (and in the old i686 build) the extra bytes
land in the frame above, which the retail game evidently survives.

In the wasm32 core, `buf` is an address-taken local, so it lives on the module's shadow stack. Each
EE thread has a 256 KB shadow-stack slot at 0x06000000 + n * 0x40000, with a no-access guard page at
the bottom of each slot (`port/host64/fiber_x64.c`). The main game thread uses slot 0, [0x06000000,
0x06040000). The frames above `func_0017A678` (the main loop, `func_00176ED8`) keep no address-taken
locals there, so `buf` is the topmost shadow frame. The overrun runs off the top of the slot into
slot 1's guard page:

```
host64 memory: write of uncommitted guest address 0x006040000
  w2c_core_memcpy
  w2c_core_rc_func_00258390      libc string output (vfprintf's writer)
  w2c_core_rc_func_00257460      vfprintf
  w2c_core_func_00254D68         sprintf
  w2c_core_func_0017A678         career finish text
  w2c_core_func_00176ED8
  w2c_core_rc_func_001B40A0 / rc_func_001B6708   main loop
```

(RVAs from `host64_rva.py` with the build's `dhd64.map`.) The process ends with 0xC0000005 a few
fields after the rider crosses the line. A 1st place takes the "YOU ADVANCE TO THE NEXT RACE" branch
instead, which prints only short strings, so the crash happens only on a non-advancing finish. In
Single Event the results came up after 1st, 2nd and 10th places with no crash.

This is not the wraparound class of finding 086 / commit edf2dc201, and `wasm_offset_scan.py` cannot
catch it: the address is computed correctly, and the bug is a real overflow that the EE's stack
layout happened to hide.

## Fix

`port/patches.py`: the port's copy of `func_0017A678` gets `char buf[0x40]`. That is enough for the
longest string the function can print, including `remaining` = INT_MIN (46 bytes with the NUL).
`src/` (the matching decomp) is unchanged.

## Reproduction

All runs used the scripted harness on a hidden or minimised window, with the deterministic recipe:
virtual clock, seed 81257, `DHD_SETTINGS=none`, no memory card, sound mixed but not played. The
scratch runner is outside the repo.

- Route to Super Career race 1 (Mt. Middle, Italy FR), using DHD_NAV:
  `0:triangle|cross,15:cross,3:cross,16:cross,1:down,1+30:cross,12+30:cross,18@4C7F38=0:start,18@4C7F38=1:cross,9/5:cross`.
  Menu 1 is SINGLE PLAYER GAME MODE (Single Event, Career, …) and menu 12 is CAREER MODE (Super
  Career, Specialized Career, Custom Tournament). race_compare's standard route takes Single Event.
- Pad: `DHD_RACE_PAD=2:1200:cross` (pedal only). With no steering the rider gets stuck before the
  line, so a new test hook sets the finish place: **`DHD_RACE_FINISH_AT=seconds`** (`autopad.c`,
  needs `DHD_RACE_SYNC`). At that race-clock time it sets the game's own finish-now flag for player
  1 (0x43EF22, the flag online.c's `DHD_NET_TEST_FINISH` uses). In this event the AI winner finishes
  at about 4:48-4:50.

| Run | Finish | Before the fix | After the fix |
|---|---|---|---|
| SC282, SC289 | 4:42, 4:49 (1st: "YOU ADVANCE TO THE NEXT RACE") | results, no crash | not re-run (that branch is unchanged) |
| SC296 | 4:56, 6th | crash, write of 0x006040000, exit 0xC0000005 | "0 RACE CREDITS AWARDED / 12 MORE CREDITS REQUIRED TO ADVANCE", RACE RESULTS; Cross on RACE AGAIN starts the race again; 27000 of 27000 fields, exit 0 |
| SC330 | 5:30, 10th | same crash | the same text and RACE RESULTS; 25714 of 25714 fields, exit 0 |

Not reproduced (no crash): Single Event finishes in 1st, 2nd and 10th; Single Event with the player
widescreen defaults, the memory card and real-time pacing; Single Event TRY AGAIN and QUIT on the
results.

## Checks

- The fixed exe on the two crashing runs: no fault, and both reach the results (table above).
