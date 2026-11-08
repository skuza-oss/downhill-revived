# 028 - Round 20: the late-race crash and the GPU readback wait (2026-10-15)

Status: current
Checked: 2026-11-04

## 1. The late-race crash in func_00214BA0

Every build crashed 40-125 s into a race, as recorded in finding 027:

- an access violation in ps2asm `_run`;
- reached from the first loop of `func_00214BA0` (the spatial batch query);
- loading through `records[k].x0`, which held a float.

### Cause

A temporary probe on entry to `func_00214BA0` logged its caller, its records and the host return
address whenever a record pointer had the top bit set. Two paced races reached the bad call
~90 s and ~100 s after race start:

```
PROBE214BA0 records 0414FA70 count 2 ret 40030EB1        (func_00131630 + 0x2D1)
rec0 C51E4F06 C4A5E68F 45C1CE71 3FA00000 C51E569C C4A5E5F1 45C1D48C 3FA00000 ...
rec1 0414FA60 0414FA40 3F800000 3F800000 00000000 00000000 00000000 0000000A ...
```

The first 32 bytes of `groups[0]` held two position vectors (w = 1.25), the wheel probes' end
points.

`func_00131630` (`src/unsorted/00131630.cpp`) works as follows:

- It keeps the probe segments in `struct { Vector before[2]; Vector after[2]; } ends`.
- It stores each end point with `ADD(bp[2], bp[0], scaled)`, where `bp` walks `ends.before`.
  Retail relies on the struct layout: `bp[2]` is `ends.after[i]`.
- On the PC, indexing past `before` is out of bounds. LLVM's scalar replacement of aggregates
  split `ends` into two separate stack slots: `after` at esi+0x70 and `before` at esi+0x90.
  `bp[2]` then landed on `groups` at esi+0xB0.

So every call overwrote `groups[0].x0..x1C` with the end points.

- **Positive x:** the float bits formed a readable host address, and the query silently read
  garbage.
- **Negative x (0xC5...):** the read faulted.

The crash time depended only on where the rider was on the course.

### Fix (e115dfa7)

A port patch in `port/patches.py` changes that store to `ADD(ends.after[i], bp[0], scaled)`. The
recompiled frame keeps `ends` in one piece (x4 = x0 + 0x20) and `groups` separate. The patch:

- doesn't touch `src/`, the matching build or any shared-header file;
- passes the image check (MATCH).

### Proof

- Three races ran 282 s, 282 s and 298 s past race start with no crash (two paced, one
  `DHD_PACE=wall`). Before, every paced run crashed by ~90 s and the wall run by 125 s.
- Race dumps change, and now match retail. Against the retail PCSX2 dumps
  (`ee-g+N.bin`), rider positions (+0x3530..) compare as follows:

  | Moment | Before the fix | After the fix |
  |---|---|---|
  | GO+0, +2, +12, +16 | 0/10 riders bit-exact, worst error 36.0 | 10/10 bit-exact |
  | GO+30 | | 10/10 |
  | GO+60 | | 9/10 (worst 0.0005) |
  | GO+120 | | 8/10 (0.32) |
  | GO+180 | | 3/10 (5.3) |

  The remaining drift is the round-18 contact normal.
- A control run of the previous exe with the same setup matched the round-19 dumps exactly, so
  the change comes from the fix alone. The fix also changes the boot: it reaches GO 438 fields
  later.
- The race is ~1 fps slower (24.5 → 23.5, paced): the wheel probe now runs real collision
  queries.

### The bug class

Any matching source that reaches a struct member by indexing past an adjacent array can break
the same way on the PC. The compiled code is legal: the source has undefined behaviour in C.
Next step: audit for such accesses (array index past the bound, inside a struct local), for
example with clang's `-fsanitize=array-bounds` in a test build.

## 2. The GPU readback wait

### What the game reads

- `func_00228328` (lens flares) projects each light. For each one on screen it calls
  `func_00244228` (`sceGsExecStoreImage`) to read a 4x4 patch of the Z buffer
  (`sceGsSetDefStoreImage(..., bp 0, bw 8, PSMZ32, 4, 4)`) into `D_0072DC00[index][k]`.
- `func_002286F8` counts the pixels with `Z <= limit` and scales the flare by the square root
  of the visible fraction.
- `func_002286F8` is called from `func_0022E9A0` ← `func_00215B68` ← `func_002161B8`, the next
  frame's world packet build. So each read's result is used one frame later.

Before, each read made the CPU wait for the GPU to finish all the raster work queued before it:
~13 ms of a 41 ms race frame (finding 027).

### Change (9c03fe29)

The read is deferred:

1. `gs_defer_read_image` (`port/gs/gs.c`) captures the pages the rectangle needs:
   - GPU-written pages by an ordered copy into a second staging buffer, then submitted
     (`stage_pages`, `port/gs/gpu.c`);
   - the others from the CPU image.
2. It leaves the GS transfer state the synchronous read would leave.
3. `sceGsExecStoreImage` (`port/hle/graphics.c`) makes the destination's RAM pages
   inaccessible in all six views of PS2 RAM (finding 020: base, `|0x20000000`, `|0x30000000`,
   and the low views).
4. The first access, from game code or from the port, faults. A vectored handler on the game
   thread then settles every pending read:
   - it maps the staging buffer (waiting only for that copy);
   - it releases the pages;
   - it writes the bytes with the packing `gs_read_image` uses.
5. Kernel calls on PS2 RAM, which fail instead of faulting on such pages, settle first:
   CD reads (`cdvd.c`) and the EE dumps (`autopad.c`).
6. A read of a page that changed since an earlier pending read captured it settles the earlier
   reads first, using a per-page VRAM epoch: draws, local copies and CPU writes.
7. `DHD_DEFER_READS=0` keeps the synchronous read.

### Checks

- A check build did the synchronous read as well, after the GPU copy was queued, so it read the
  same state. It compared the bytes at settle time: 2000 deferred reads, 0 mismatches,
  0 synchronous fallbacks. 1998 of the 1999 settles came from the guard fault, about one per
  frame.
- `test_gpu` compares deferred reads with synchronous ones across overdraw, for Z32 and CT32,
  and checks the refusal when a captured page changed: 0 failures.
- Race dumps at GO+0, +2, +12 and +16 are identical to the crash-fix build's.
- The image check prints MATCH.

## 3. Results

Measured as in finding 027: presented frames per second, menu 20-40 s after boot, race start
+20..+80 s. Each build was measured twice while the user played another game.

| Build | Pacing | Menu fps | Race fps |
|---|---|---|---|
| round 19 (exe-r19j) | paced | 29.5 / 29.5 | 24.5 / 24.5, crashed at ~90 s |
| round 19 | wall | 32.5 | 27.6, crashed at 125 s |
| crash fix | paced | 29.5 / 29.4 | 23.5 / 23.5 |
| crash fix and deferred reads | paced | 27.2 / 29.6 | 35.8 / 34.9 |
| crash fix and deferred reads | wall | 32.8 / 30.4 | 27.8 / 27.9 |

- In wall mode a race can't pass 30 fps: any frame over 16.7 ms spans two fields. That mode is
  now close to real-time game speed.
- Paced mode runs at 0.58x real time, steady (was 0.41x).
- The menus do no store-image reads, so their spread is noise.

### Where a paced race frame goes now (~28.5 ms)

| Cost | Share |
|---|---|
| VU1 microprograms | 60% |
| GS on the CPU | 12% |
| VIF1 | 9% |
| ps2asm | 5% |
| Inside D3D11 calls (flush 4.1%, draw 2.7%, stage_pages 1.4%, others 0.4%) | 8.6% |
| Game code | 3% |
| Everything else | ~2% |

## 4. VU1 flags: the ceiling, not implemented

- The replay benchmark of finding 027 takes 1.59-1.65 s with the current code.
- With every MAC/status computation and queue entry removed (the clamp kept) it takes
  1.16-1.20 s. Flag work is therefore at most ~27% of VU1 time.

An exact elimination can recover much less, because the status flag's sticky bits are an OR
over every FMAC result:

- A dropped operation still needs its four lane tests to merge its sticky bits into a later
  entry. That is most of the cost.
- The merge is only exact when no reader can see the window between the two landing times. The
  flags land 4 cycles after issue, so the later writer must sit at least 3 pairs before the end
  of its block, with no FM/FS reads in the block. Blocks are at most 8 pairs.
- The end-of-program status is part of the PCSX2 oracle comparison, so a microVU-style flag
  hack would weaken the check.

Estimated exact gain: 5-10% of VU1 time (3-6% of the frame), for a medium-high effort. Left for
a later round.

## Sources

- The PCSX2 retail dumps (finding 024) as the referee for the crash fix.
- Microsoft documentation:
  - `AddVectoredExceptionHandler`;
  - `VirtualProtect` on mapped views;
  - kernel calls failing with `ERROR_NOACCESS` instead of faulting;
  - D3D11 staging `Map`, which waits only for the work that uses the resource.
- PCSX2 microVU `mVUsetFlags`, for the flag analysis discussion.
