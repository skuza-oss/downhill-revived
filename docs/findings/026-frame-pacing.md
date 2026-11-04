# 026 - Frame pacing: the vblank wait and the virtual clock (2026-10-14)

Status: current
Checked: 2026-11-04

The port (under `DHD_FRAME_CLOCK=1`, the deterministic virtual clock of finding 024) stepped
the race at delta 2 (30 fps, dt 1/30) where PCSX2 steps at delta 1 (60 fps). From GO+1 on,
the game state diverged. This finding records the retail measurement, the cause and the fix.

## Retail measurement

The PCSX2 comparison adapter (`port/tools/prepare_pcsx2_compare.py`) logs the game's frame
counters on every field with `DHD_COMPARE_FRAMELOG`. The port does the same with
`DHD_RACE_FRAMELOG` (`port/hle/autopad.c`). The counters are:

- `D_0029E10C`: main-loop frames (incremented in `func_001B5130`);
- `D_00663108`: load-bar frames (the loading screen's thread, `func_00221608`);
- menu `0x29DED8`, race phase `0x43F348`, race clock `0x43EEC8`.

Same navigation, pinned race seed and same race pad; frames per field from
`(field, counter)` pairs. For loads, the first step of the load-bar counter is skipped
because it is set from the call's argument.

| | Retail (PCSX2) | Port, before | Port, after |
|---|---|---|---|
| (a) boot and menus, main-loop frames/field | 0.974 | ~90 (no vblank wait) | 0.997 |
| (b) load 1, load-bar frames/field | 1.000 (370 fields) | - | 1.000 (41 fields) |
| (b) load 2 (the level), load-bar frames/field | 1.000 (483 fields) | hangs (see below) | 1.000 (92 fields) |
| (c) race GO..GO+660, race clock/field | 1.000 | 0.5 (delta 2) | 1.000 |

Retail is locked to one frame per field throughout. The port's loads are shorter in
fields because CD read time is not modelled (the IOP replies after a flat 1 ms,
`port/hle/sif.c`); the load-bar rate during them matches.

## Cause: no INTC_STAT, so no vblank wait

Each main-loop frame ends in `func_00238C00` → sceGsSyncV (`func_00243CE8`), which calls
`func_0025AB70` or `func_0025AC00`. These disable interrupts, write 4 to INTC_STAT
(`0x1000F000`, write 1 to clear) to clear VBLANK_START, re-enable interrupts and spin
until bit 2 is set again (`func_0025AC00` also stops on the SetVSyncFlag word). They are
the only readers of INTC_STAT in the game.

The port had no INTC_STAT. The clearing store left 4 in the mapped word, so the spin
exited at once and the frame rate was set by the virtual clock's per-call charges. Early
boot ran about 90 frames per field. The race frame, with about 400 kernel boundaries
charged a quarter-millisecond each (`func_0021B8A8` alone calls FlushCache about 375 times
per frame, before each VIF1 DMA kick), overran a field, so `func_00238D98` measured 2
vsyncs per frame and stepped physics with dt 1/30.

## Fix

- **INTC_STAT model (`port/hle/kernel.c`).** A shadow of the register: vsync raises
  VBLANK_START and VBLANK_END, and a store clears the bits written (write 1 to clear, as
  PCSX2's `HwWrite.cpp`).
- **INTC_STAT reads.** Recompiled loads go through `ps2_load_address`
  (`port/runtime/ps2asm.h`, `recomp.h`). A load of INTC_STAT calls the kernel. If
  VBLANK_START is clear, the kernel lets time run to the next interrupt (the frame clock
  jumps to it, as in the idle loop), delivers it and returns. As in PCSX2's `HwRead.cpp`,
  the game's own loop then re-checks the bit or the flag word. The EE does nothing else
  during that spin, so the jump is exact rather than a cost estimate.
- **Per-boundary charge (`port/hle/timer.c`).** Down from 36864 bus cycles (0.25 ms) to
  1024 (about 7 us). The charge no longer paces frames; it only has to keep polling loops
  moving and leave a race frame well under a field.

Per-call cost models (a GS-work cost for sceGsSyncPath, a separate FlushCache cost) were
tried first and dropped. A cheaper sceGsSyncPath without the vblank wait made boot about
10x slower in wall time (more frames per field), and the retail measurement showed every
phase vblank-locked. No GS timing model is needed while frames stay under a field.

## The loading-screen hang (ReleaseWaitThread race)

With cheaper per-call charges but no vblank wait, the second level load hung forever:
the main thread waited in `func_00230FC8` for loader thread 4 (`func_00231628`), which
slept. This is not a kernel-semantics bug.

- Thread 4 (priority 2) starts with SleepThread (syscall 0x32).
- The main thread (priority 1) releases it on the first chunk with ReleaseWaitThread
  (0x2D, `func_00231230`), not WakeupThread.
- On the EE, ReleaseWaitThread on a thread that isn't waiting returns an error and changes
  nothing. ps2sdk's `syscallnr.h` confirms the numbers. Play!'s HLE kernel (`PS2OS.cpp`)
  has the same rules, and only WakeupThread counts wakeups.

A kernel trace showed the order. Main blocked on a synchronous CD RPC. The
vblank-released load-bar thread (priority 0) took the CPU. The RPC completed during its
render, and main (priority 1) then outran thread 4 (priority 2), releasing it before it
had slept. Retail never shows this because its timing always lets thread 4 reach
SleepThread first.

With the vblank wait and the 1024-cycle charge, all loads complete: 4 boots to the race
of the same build, with identical fields.

## Race state after the fix

Comparing equal race-clock dumps (retail `ee-g+N`, port at the same clock):

- delta, rate, dt and the vsync counter `D_0029E3D8` match at every moment from GO to
  GO+600 (260 → 860 on both).
- Rider positions match to 0.00024 at GO+0 and to 0.0073 at clock 2. The error is 0.006
  at clock 5 and 0.035 at clock 10, then grows: 1.76 at GO+30, 30 at GO+120.

At GO+0 the rider structs already differ slightly in sub-state while positions agree:
pose vectors at +0x3540..+0x3640 (e.g. 0.7922 / 0.7903), and the words at +0x354C and
+0x355C (0 in retail, 6 in the port). The animation start time +0x3B38 is 157 / 18; the
elapsed animation time is equal (1761 on both). These pre-GO differences are the next lead
for the first divergence.

## Round 17: EE float rounding (the GO+0 differences)

A whole-RAM diff of the countdown-start dumps (retail `ee-c+0` against the port) showed the
pattern: retail values end truncated where the port's are rounded to nearest (`3F7FFFFF` /
`3F800000`, `42EAAAAA` / `42EAAAAB`). A hardware watchpoint (x86 debug registers on the EE address,
a temporary probe) found the writers of the GO+0 rider differences:

- The pose vectors at +0x3540..+0x3640 are stored once, at the end of the level load, by the VU0
  macro code of `func_00221888` (a matrix-vector product) called from `func_001B3300`. Its w lane
  is copied from the node position's w, which gives +0x354C / +0x355C (0 retail, 6 port).
- The start-grid positions (1 ulp apart at the countdown) are written by inline asm too.
- +0x3B38 and similar stamps are written by `func_002033E0` from the animation clock `D_0029E160`.
  That clock is ticked only by the load-bar thread (`func_00221608` -> `func_00202D78`) while the
  loading music fades, so it depends on load length: the port's is 139 lower at the countdown.
  Elapsed animation times are equal; only the stamps differ.
- +0x3FB8 is a timer stamp taken during the level load (`func_001CFEE0`, `func_00238F48`): 0xB
  retail, 9 port. Also load-timing dependent.

PCSX2 runs the EE FPU and VU0 with MXCSR set to round toward zero with DAZ/FTZ
(`Pcsx2Config.cpp` DEFAULT_FPU_FP_CONTROL_REGISTER, DEFAULT_VU_FP_CONTROL_REGISTER). Its EE FPU
recompiler (`x86/iFPU.cpp`) also:

| Operation | PCSX2 |
|---|---|
| MUL, RSQRT, CVT.S.W | the FPU mode: toward zero |
| ADD, SUB (and MADD/MSUB, ADDA/SUBA) | one guard bit: the smaller operand loses the bits alignment shifts out past it (`FPU_ADD_SUB`), then toward zero |
| DIV | to nearest (`FPUDivFPCR`) |
| SQRT | to nearest |

The port ran all of it at the host's round-to-nearest. Fixes:

1. EE fibers and the kernel loop run in that mode (`kernel.c ee_float_mode`); host VIF1/GIF
   execution switches to IEEE nearest (`graphics_queue.c`).
2. Interpreted and recompiled COP1 ops use `ps2_ee_add/sub/div/sqrt` (`ps2float.h`); VU0 macro
   DIV/SQRT keep chopping.
3. Native game units: `port/tools/eefloat.py` rewrites each unit's LLVM IR in `build.py` so float
   add/sub/div call the always-inline helpers of `port/runtime/ee_native_float.h`. Double
   arithmetic and double-to-float conversion round to nearest, as the game's IEEE soft-float did.
   2,046 operations in 3,797 units; most game float math is VU0/COP1 asm or recompiled code.

Equal-race-clock rider position error (max over 10 riders, `poserr.py`):

| Build | GO+0 | clock 10 | clock 30 | clock 60 | clock 120 | clock 300 | first > 0.01 | first > 1.0 |
|---|---|---|---|---|---|---|---|---|
| round 16 | 0.00024 | 0.035 | 1.76 | - | 30 | 71 | 8 | <= 30 |
| + EE MXCSR mode | 0 | 0.018 | 0.56 | 1.59 | 7.4 | 80 | 10 | 60 |
| + per-operation rounding | 0 | 0 | 0 | 0.00049 | 0.30 | 5.7 | 120 | 300 |

With per-operation rounding, finer samples: 0 at clocks 31, 36 and 42 (bit-exact), 0.00012 at 48,
0.00049 at 60, 0.0063 at 75, 0.12 at 90, 0.30 at 120. So the error first exceeds 0.01 between 75
and 90 (the coarser table sampled 60 and 120 only) and 1.0 between 120 and 300.

### What still differs, and the CD-latency question

Watchpoints on the remaining rider differences (the same temporary probe):

| Field | Retail / port | Writer |
|---|---|---|
| +0x354C, +0x401C, +0x46DC, +0x5CAC (w lanes) | 0 / 6 | VU0 macro asm during the level load (copies of the node position's w) |
| +0x41CC | 0 / 0.2, then 2.04 | VU0 macro asm from `func_001304F8` / `func_00136038`, from GO+0 |
| +0x7B2C (riders 1, 5) | 0 / 1.9 | VU0 macro asm from `func_001304F8`, from clock 10 |
| +0x4890 | about 1e-9 / up to 0.08 | `func_001AE358`, the camera angle smoother (`CameraAngles` xA8, from `func_002224E0`'s matrix-to-angle decomposition) |
| stamps (+0x3B38, +0x3634, +0x4634, ...) | offset 139 | animation clock `D_0029E160`, load-length dependent |
| +0x3FB8 | 0xB / 9 | load timer stamp, load-length dependent |
| +0x3B4C, +0x4578, +0x484C | EE / host address | function pointers: expected |

The load-length stamps keep the same offset from GO to clock 48, and positions are bit-exact to
clock 42 despite them, so they don't drive the early divergence. Modelling CD/RPC read latency
would align them, but there is no evidence yet that it changes the race, so it was not done.

The first position-relevant drift is rider 4 (the player) at clock 42: orientation vectors at
+0x3CF0..+0x3CF8 and +0x4030 differ in the 5th digit, and +0x4130/+0x4138 (168.826 / 168.829,
18.737 / 18.743). Positions follow at 48. The next step is a watchpoint on rider 4's +0x3CF0 between
clocks 36 and 42, then the instruction that first gives a different result.

## Round 18: the first differing value (2026-10-14)

Dumps are paired by the race clock stored in them (0x0043EEC8), not by name. The port's dumper
lands one clock late when two moments are only one clock apart (its g+13 holds clock 14), which
made odd clocks look shifted. With NAV and the pinned seed:
- Every rider struct is bit-exact against PCSX2 through clock 15, excluding the GO+0 load stamps,
  the camera and the sound tail.
- The first differences appear in the frame that ends on clock 16, all in rider 1:
  - +0x4030, +0x4138, +0x4198 and +0x4314 are off by 8 ulp.
  - +0x4234/+0x43F4 and +0x4248 are off by about 0xC00 ulp.
- The transient at clock 2 (rider 9 +0x4030) was a 1-ulp value that later vanished.
- An earlier "+0x43D4 = FLT_MIN" lead was the diff tool removing 0x08000000 from a flag word.

Tracing it back used two temporary probes, both removed:
- PCSX2: write memchecks that log the clock, PC and old value instead of pausing (the recompiler
  only emits checks for breaking memchecks). The probe build reproduces the rider physics bit for
  bit.
- Port: a ring of `ps2asm` `lqc2`/`sqc2` values.

The chain, from the first differing write back to its source:

1. `func_00231CC0` stores obj+0x70 += cross(b − obj+0x50, a) (obj = rider+0x40C0).
   - Retail z = C0D77B62, port C0D77B6A. Both start from the same C0CD4852.
   - Delta matches, but the cross product differs: retail 42680DD2 C3798922 BEA33100, port
     42680DD1 C3798922 BEA33180.
   - The VU0 ops are right: with retail's inputs, chopped products and subtraction give retail's
     bits exactly.
2. Its input `a` differs only in y: retail C02A21C0, port C02A21C9. `a` is `func_00133408`'s
   projection v = in + dir·d.
   - `d` (42357A8C) and `dir` match.
   - `in.y` is retail BBD55EEE, port BBD57077.
3. `in` is the impulse from `func_00130C08`, normal × scalar. Its x and z match, so the contact
   normal's y differs: about −3.48e-5, retail B81203C7, port B8120FC7. That is the value seen at
   rider +0x4234.
4. The normal is already different (port B8120FC9) before the collision matrix-stack transform
   in `func_00215140` (rows at 0x00515700, scale 0.99999988).
   - It comes from a normalize in the collision primitives (`func_00213098`'s `VecNormalize`:
     asm `madd.s`/`sqrt.s`/`div.s`, then native `x * inv`).
   - A y of 3.5e-5 in a unit vector means its input y is a cancellation, so a 1-ulp difference
     in an upstream coordinate gives this 3e-4 relative error.

Item 3 of the brief: `+0x4890` (camera smoothing) is not on this chain. Every value from the
contact normal to the rider's +0x4138/+0x4198 comes from collision and impulse data (obj, the
contact event, impulse, dir), and none of it reads the camera rig at +0x46D0..+0x48A0.

Next: find the native or asm operation upstream of the normalize where retail and port first
differ. Either probe the collision primitive's inputs in the same way, or replay the closest-point
code offline with the logged inputs.

## Sources

- PCSX2 2.8.2 `pcsx2/HwRead.cpp`, `pcsx2/HwWrite.cpp` (INTC_STAT read and write-1-to-clear).
- ps2sdk `ee/kernel/include/syscallnr.h` (syscall numbers).
- Play! `Source/ee/PS2OS.cpp` (ReleaseWaitThread, SleepThread, WakeupThread rules).
- PCSX2 2.8.2 `pcsx2/Pcsx2Config.cpp` (default EE/VU MXCSR: chop, DAZ, FTZ) and `pcsx2/x86/iFPU.cpp`
  (FPU_ADD_SUB guard bit, DIV/SQRT at nearest); independently implemented in `ps2float.h` and
  `ee_native_float.h`.
- PCSX2 2.8.2 `pcsx2/x86/ix86-32/iR5900.cpp` (recMemcheck/dynarecMemcheck) and
  `pcsx2/DebugTools/Breakpoints.cpp`, used only in a temporary local probe build.
