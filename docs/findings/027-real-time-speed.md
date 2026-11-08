# 027 - Real-time speed: round 19 (2026-10-15)

Status: history

The 32-bit D3D11 build's first speed round, from about 15-19 fps to a steady
24.5 fps paced race (29.5 in menu 33) on an RTX 3070. Its numbers, its pacing rule and its open
crash are all replaced: the crash is fixed (finding 028), pacing is the delta-time clock (finding
139, `port/hle/kernel.c` `pace`, `port/hle/fps.c`), and speed is findings 066, 089 and 090.

## Where a paced race frame went then (~41 ms)
VU1 microprograms 38-45 %; waiting for the GPU at the game's store-image reads ~34 % (~13 ms);
GS on the CPU 9-13 %; VIF1 unpack ~6 %; ps2asm 3-4 %; game code 2-3 %; sound ~1 %. The GPU was
about 53 % busy: the readbacks (`func_00228328` → `sceGsExecStoreImage` → `gs_read_image`) made
the frame's GPU time add to its CPU time.

## What landed (all still in the port in some form)
- VU1: translated-block lookup cached per PC (reset by a code version VIF MPG bumps), hot
  register helpers inlined, **MAC/status flags retired lazily** (only when read or the flag ring
  fills), delay slots as single-pair blocks. Replay bench of 32 race VU1 programs x3000: 2.1 s →
  1.45 s (plain interpreter 2.7 s), result hash unchanged (`6DB1972798276582`).
- VIF1: a whole-word UNPACK path for 32-bit-aligned data.
- `func_00202760`'s fourth argument (an ABI fix in `port/patches.py`).
- GS: VRAM-to-VRAM transfers on the GPU in draw order (they used to read all 4 MB back); store
  image reads copy back only the GPU-written pages they touch.

## Rejected, with the reason
- A context Flush after every dispatch to start GPU work earlier: the readback wait fell to
  5 ms/s, but the CPU then blocked in Flush, the race was no faster and the menu fell to 21.5 fps.
- Wall-time fields (`DHD_PACE=wall`, then the default): a slow frame spanned 2-3 fields and the
  race stepped by 1 or 2, so speed wobbled. The round's replacement, one fixed step per field at
  59.94 Hz, ran a slow PC uniformly slow (0.41x real time at 24.5 fps); finding 139 replaced both
  with one delta-time clock, and `DHD_PACE=wall` is gone.

## Determinism rule it set
Race EE dumps at GO+0/+2/+12/+16 must equal the round's first build, except host pointers and dead
native stack frames (the state-trace tools of findings 081/082 now do this per field).

Full text: `git show 860ab6b:docs/findings/027-real-time-speed.md`
