# 124: PERU weather inherits particle floats as VU1 loop counts

The warning `vu1: microprogram at 0x1F50 didn't end` comes from an uninitialized
weather batch record in the EE particle pool. VU execution faithfully consumes
the bad input. The fix initializes the producer's four per-view records;
MAX_STEPS and VU dispatch are unchanged.

## Symptom and loop

Release 0.7.4 and 0.7.5 online logs repeatedly fail after loading PERU following
earlier races. The last block PCs repeat `2170 2270 2150`. The historical PC
log's VI file is:

```
0000 0001 0008 8000 0065 0001 0009 0000 0557 0000 8000 0011 0204 02FE 8065 000F
```

Overlay A loads data qword 18 into vf23 at `1F60`; `1FE0 MTIR vi8,vf23w`
reads its low 16 bits as the remaining particle count. `2160 IBLEZ vi8,2288`
tests it and `2178 ISUBIU vi8,vi8,1` decrements it. `2278 IBGTZ vi4,2150`
repeats the inner batch; `22A8 IBGTZ vi8,2140` starts another batch. VI4 is
remaining output capacity (`vi14 - vi10`). Clipped particles can leave VI10
unchanged, burning many steps without producing vertices. This is not zero
wrapping: a stale float supplies a large positive count. A finite loop then
exceeds the million-step guard within a frame.

## Input and upstream evidence

A natural ALPINE/MOAB/JUNGLE/PERU room rotation with the release executable
captured qword 18.w = `0x3F4845F6`, float 0.7823175. MTIR interprets it as
17,910 particles. Both the port and PCSX2 finish in **698,576 reference steps**,
emit 16 GIF bytes and agree on all checked state and memory.

A second rotation captured EE RAM alongside VU input. Emitter slots 42 and 43
have kind `0x8048` (masked mode `0x8008`), densities 2,000 and 200, and inline
position records at object + 0xF0. Slot 42's first record contains old coordinates
`(40.4357, -8154.2178, 9964)` and W = `0x3F77E348`, a particle float around
0.9683. The VIF input matches this EE record exactly; it also appears in the
emitted DMA packet. `func_0022E560` copies the four words with `0x6C08000E`
UNPACK: matrix at qwords 14–17, position/count at 18, velocity at 19, controls
at 20 and view data at 21. No UNPACK reinterpretation explains the stale W.

The producer, `func_001E0480` in `src/unsorted/001DF920.c`, calculates an
integer density count and stores its bits into position.W, but only inside
`if (count != 0)`. Captured slot 42 has radius 2,400 and view-offset distance
about 2,380.34: its density 2,000 gives a count below one, truncated to zero.
Slot 43 is outside its volume and also has count zero. Neither publishes a
new record. Their `0x1000` flag excludes normal particle updates.

`func_00226D08` builds the weather packet in reused emitter storage. It sets
x2830 to the four per-view position/count records but initializes only records
4–7 (history). Records 0–3 retain old pool contents. Clearing velocity phase
words in `func_001E1228` does not clear these position/count words. Old particle
ages pass the renderer's nonzero-W test before any weather count is published.
Fresh starts or different prior races can instead leave zero, a small count or
a negative signed VI value, explaining the varying symptoms.

## Deterministic reproduction and reference

To control unspecified pool contents, the hidden harness seeded slot 42's old
inline W **before PERU weather construction**, using `0x3F666984` (float
0.90004754) from a particle record in the captured EE pool. This models the
observed reuse without changing microcode, its limit or output. The baseline
game logs the exact warning. PCs repeat `2170 2270 2150`; VI8 ends at `0x007F`:
27,012 bogus particles minus the 26,885 iterations consumed. The failed call
is captured before execution. This is a controlled pool-reuse reproduction,
not a claim that every unconditioned run fails.

The project's pinned PCSX2 interpreter, commit
`0f71e668b8adc950db7de1b1b15c2f5080c102e9`, exhausts the guard on the same
failed input (`reference timeout at 2270`). An empty position/count record
makes both interpreters finish in 86 steps, 16 GIF bytes, zero differences.
At the EE level an empty W bypasses packet emission entirely. A valid captured
count of one still matches PCSX2 in 125 steps.

Reference tools: `port/tools/test_vu1_pcsx2.py`, `pcsx2_vu_oracle.cpp`,
`vudis.py`, and the local retail microcode. Source consulted:
https://github.com/PCSX2/pcsx2/blob/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2/VUops.cpp
No upstream implementation was copied. The oracle confirms VU execution;
the producer's zero count and uninitialized records explain the bad upload.

## Fix and checks

A build-time patch in `port/patches.py` clears the four position/count records
immediately after `func_00226D08` assigns x2830. Each view starts empty. A
nonzero update writes XYZ and the integer count before the renderer can consume
the record. History initialization and valid batches retain their retail
behavior. Matching sources stay unchanged.

Opt-in diagnostics added (captures contain game data and remain untracked):

- `DHD_VU_CAPTURE_FAILURE=DIR`: preserve failed inputs in the existing oracle
  format before VU stores overwrite them; at most 16 files, shared with the
  existing suspect-draw diagnostic limit.
- `DHD_VU_CAPTURE_ENTRY=HEX`: filter the ordinary first-call capture by entry.
- `DHD_VU_CAPTURE_EE=1`: add a 32 MiB EE RAM sidecar to an ordinary capture.

Checks (2026-10-31):

- The diagnostic-only Windows build (before the fix) matched all 600 boot trace
  rows against release 0.7.5, zero masked words.
- Pool-reuse case, online room rotation JUNGLE then PERU with a nettool bot and
  the stale W seeded before weather construction: the build without the fix logs
  `vu1: microprogram at 0x1F50 didn't end` twice (PCs `2170 2270 2150`, the
  players' logs); the fixed build logs none, its four per-view counts read
  `[0, 0, 0, 0]` after the constructor, and both races finish.
- PERU state trace, 1,600 fields (DHD_RACE_SETUP=3, the same seed): the fixed
  build differs from the unfixed one only in `ram`, from field 1102 (the
  constructor). The RAM dump there differs in one word: the seeded stale W,
  now zero. By field 1600 the differences are the renderer's bookkeeping: the
  texture/CLUT cache's LRU clock `D_0029E218` and slot tables
  (`D_00621310`, `D_00625190`), packet words and the weather emitter's scratch
  floats near the pool, because the bogus weather batch is no longer emitted.
  CPU, scratchpad and every other RAM word are equal.
- The decisive one: the fixed build **with** the stale seed against the unfixed
  build **without** it differs in exactly the 214 fields 888 to 1101 (from the
  seed's write to the constructor) and matches in every column from field 1102
  to 1600. The fix gives what the game does when the pool holds no stale data,
  as on a fresh start; it adds no behaviour of its own.

Not checked: the other PERU variants (PERU TDH, PERU MX) and a race to the
finish on PERU; their weather comes through the same constructor.
