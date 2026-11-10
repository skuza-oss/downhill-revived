# 167: Native VU1 rendering, phase 2: the remaining VU1 programs

Status: in progress

Branch `native/vu-gs` (local only: never pushed or merged until the owner decides), after
finding 164's phase 1 (`43d6ce2`), started 2026-11-06. This file is the lane's plan while it runs.

## Goal and acceptance

The owner approved phase 2 (2026-11-06, overnight): translate the rest of what finding 162's table
lists, in this order (cheapest and biggest first), behind the same switch (`DHD_NATIVE_VU1`, a list,
off by default), with anything unusual left on the VU and counted by reason:

1. the prelit world variants 1638, 07F0, 06F0, 17E0 (finding 164's family: two-sided, some without
   clipping);
2. the lit programs: 0040 and its variants 1430, 1050, 12F0, 0C20, 0A90, 0000, 30A0 (per-vertex
   lighting, rigid skinning, shadow projection);
3. the skinned riders and bikes, 0020 and 0010 (3-bone skinning, lighting, shadow silhouettes into
   the shadow render targets);
4. overlay A: particles and sprites (2E48, 29C0, 0E10, 02D0, ...);
5. 2DC8 (the second matrix, likely an environment map).

Per program or family: correctness with finding 164's tolerance (pictures switch off vs on across
finding 156's scenes at 2x and 4x, per field MAE <= 0.1 and at most 0.1 % of pixels off by more than
8; PCSX2 where in doubt), the boot state trace with it on, one clear commit per program. A program
that cannot be matched within tolerance stays on the VU, with the reason recorded. Timing at
milestones only (after 1, after 2, at the end): Windows RTX at 8x, and the Mi 11i on the t612
profile at 2x only (the owner, 2026-11-06), on an idle PC and phone. The race state trace once at
the end with every translated program on, and `check.py port` before handing back. The final
report: per program what was translated (share, % of its batches on the GPU), correctness, the
timing table (off against each milestone), commits, and what is left.

## Progress

- [x] 2026-11-06: finding reserved
- [x] batch loops keyed by VI13 (whatever setup or pass ran), sideways clipping on the GPU
  (`02e48c5`)
- [x] 1. the prelit variants: loops 1708 (1638), 08B0 (07F0), 0798 (06F0), 18B0 (17E0) (`1b79009`)
- [x] 2. the lit loops: 1110 (1050, and the 0x20C batches of 0040, 0010, 0020), 13A0 (12F0; their
  0x260 batches) (`c08668c`); 1510 (1430) (`e184b05`); the shadow silhouettes 3140 (30A0; their
  0x616 batches) (`fe081ec`); the environment maps 0AA0 (0A90) and 0C30 (0C20) (`d4373c3`)
- [x] 3. the skinned riders and bikes: their batches go through 1110, 13A0 and 3140; the skinning
  and lighting passes stay on the VU (Decisions)
- [x] 4. overlay A: not translated in this phase (Decisions)
- [x] 5. 2DE8 (2DC8): the projected second texture (`39533bb`)
- [x] pictures switch off vs every loop on, 2x and 4x: within the tolerance; shadow check per loop
- [x] boot and race state traces (shadow, skip) with every loop on
- [x] timing: Windows RTX 8x (milestone 1 session, then one build for every milestone); Mi 11i
  t612 2x (one APK, every milestone, and 0170 alone for finding 164)
- [x] PCSX2 referee (2.8.2, four fields, off and `all`: the same distance)
- [x] `check.py port` (`39533bb`: every step OK)

## The remaining loops (the translation's specification)

Each has overlay B's batch loop shape (finding 164's "How loop 0170 works"): a prologue, a
software-pipelined loop over the strip's vertices that writes STQ, RGBAQ and XYZF2 in place, and
the GIF tail at 0FF8. What differs is in `vu1_mesh.inc`'s table. The loops go by their loop
address (finding 162's "17F0" is 17E0's prologue, its loop is at 18B0; its "1440" is 1430's
prologue, the loop is at 1510); a batch names its loop in VI13 (the loop address / 8 is the
prologue's).

- **1708, 08B0, 0798, 18B0** (prelit): 0170 without the back-face test; 08B0 and 0798 without
  CLIP (the GS takes what they kick); 0798 and 18B0 with the fog at its maximum (vf18.y), the
  batch's colour as it is and no vf19 offset.
- **1110, 13A0** (pool-lit): the colour from the pool entry's lighting L at 373 + index, which
  0040's lighting loop (1280) or 0010/0020's skinning and lighting (0468) wrote:
  c d5 L + d20 L.w^2 + d1015 (1110) or c L + d20 L.w^2 + d1015 (13A0), clamped to 255 and
  truncated, d1015 only on RGB, computed on the CPU in the VU's order into the item's colours.
- **1510** (1430, finding 162's untextured 02C draws): a highlight, c vf31 + d20 max(N.H, 0)^16
  with N the pool normal (12-bit fixed point) and H = (d6.w, d7.w, d8.w): four squarings.
- **3140** (30A0, and the 0x616 batches of 0040, 0010 and 0020): the shadow silhouettes into the
  shadow targets. Setup 3190 makes the matrix data 1008-1011 times the object's; the loop does not
  divide; its colour is FTOI0(d1013); its strip tag is the I register's constant (LOI at 30C0:
  PRIM 00C, untextured), stored over the header's second word. Translated while every vertex comes
  out with w = 1 exactly (the shader's divide then changes nothing), counted otherwise (never on
  the route).
- **0AA0, 0C30** (0A90, 0C20): an environment map. S, T = E N + (x/w + d997.zw) d998.zw, with E
  the 2 x 3 product of d997-d999.xy and the object's rotation d0-d2, and Q = 1: S and T are linear
  in screen space, not perspective-correct. The item carries (S w, T w, w); the shader's
  perspective-correct interpolation divided by the interpolated w gives the GS's screen-linear
  values (as it does for colours). 0AA0: the batch's colour, fog at its maximum, no CLIP; 0C30:
  c vf31, the computed fog, CLIP.
- **2DE8** (2DC8): a second texture projected onto the object (setup 2FD8 stores d5-d8 = data
  1008-1011 times the object's matrix): STQ = (d5-d8) (x y z 1) / w, the texture's Q being that
  product's z. RGB is 128, the low byte of the float 1 + 2^-16 the loop stores as its colour; alpha
  is d1013.w's low byte where N.d1013 > 0, else 0 (its FSAND at 2F30 sees 2F10's MAC, as
  `vu1_native_2dc8.h` has it). Its strip tag is the I constant at 2E98 (PRIM 05C: textured,
  blended). Fog at its maximum, CLIP.

## The translation (what phase 2 changed)

- **Keyed by the loop, not the setup** (`02e48c5`): a batch is translated when VI13 names a
  translated loop, whichever setup or pass ran before; the constants are read from the registers
  (vf1-vf4, vf13 or data 996, vf18, vf19, vf31) and data memory at each batch. So the lit and
  skinned objects' batches (0040, 0010, 0020) go to the GPU through 1110, 13A0 and 3140 while
  their passes run on the VU.
- **Sideways clipping on the GPU** (`02e48c5`, finding 164's near-ground answer): a CLIP loop's
  batch with a vertex off the guard band only in x or y is drawn and the GPU clips it; one with a
  vertex below the near bound runs on the VU.
- **Colours and texture coordinates on the CPU** where a loop does more than c vf31 (lit,
  highlight, constant, 2DE8's lit alpha; the environment map and the projection): the item's vertex
  colour and (u, v, uz) carry the result, so the shader is phase 1's (one mesh vertex shader, the
  same pixel entries, no ABI change).
- **What a skipped batch leaves**: besides finding 164's (the rotating buffer, VI3, VI4, TPC,
  0170's parity), each loop's constants it loads into registers and keeps (`mesh_skipped`): 1110's
  vf31, vf8, vf26; 13A0's vf13, vf14; 1510's vf7, vf8, vf10, vf14; 3140's vf5, vf10 and the tag
  word over the header; 0AA0's vf13 (its MR32s overwrite the guard band's xyz), vf14-vf17, vf8.z,
  vf5.w; 0C30's vf7, vf8, vf10, vf14, vf17.z, vf27; 2DE8's vf6, vf7, vf8, vf10, vf14, vf19, vf23.y
  and the tag word. Working registers (each prologue sets them before it reads them) are not kept.
- **The shadow check per loop**: `DHD_NATIVE_VU1_CHECK=1` reports each loop's own counts.
- **Groups**: `prelit` (0170, 1708, 08B0, 0798, 18B0), `lit` (1110, 13A0, 1510, 0AA0, 0C30),
  `shadow` (3140), and `all`; a loop also by its name.

## Correctness

### Pictures

Finding 164's method (finding 156's route, deterministic, `DHD_EXACT_PROBE=1` so the switch is in
shadow mode and the game state is the switch-off run's; GS captures at eleven fields; native
renderer, `DHD_NATIVE_SPEC=sync`), switch off against `DHD_NATIVE_VU1=all` (every loop above).
Worst field of each scene:

| scene | 2x: MAE / > 8 / > 32 / max | 4x: MAE / > 8 / > 32 / max |
|---|---|---|
| title (700, 950) | 0.005 / 0.003 % / 0.001 % / 95 | 0.005 / 0.003 % / 0.001 % / 95 |
| shop (3200, 3500) | 0.005 / 0.009 % / 0.002 % / 57 | 0.005 / 0.007 % / 0.001 % / 188 |
| race start (6400-7100) | 0.021 / 0.018 % / 0.006 % / 176 | 0.020 / 0.017 % / 0.005 % / 192 |
| mid-race (9000-9600) | 0.066 / 0.082 % / 0.011 % / 145 | 0.066 / 0.087 % / 0.013 % / 169 |

Every field is within the tolerance at both scales, and the numbers are those of phase 1's 0170
alone (finding 164) and of the step-2 set before 1510, 3140, 0AA0, 0C30 and 2DE8 joined (the 4x
fields equal to the third decimal): what differs is still 0170's texel noise and edge pixels.

### PCSX2 as the referee

Each scene's field (700, 3300, 6600, 9300) captured as a GS dump (`DHD_GS_CAPTURE`, 1x, shadow mode,
so the stream holds the translated loops' packets as the VU kicks them) with the switch off and with
`DHD_NATIVE_VU1=all`, replayed in stock PCSX2 2.8.2's software renderer (`compare_pcsx2.replay_pcsx2`),
against the port's native picture of the same field. 2.8.2 saves this game's field buffers (0x8C0,
0x1180) with the picture 224 rows down, where `compare_pcsx2.scanout` crops at DBY mod 8 (black);
the field's window was found by search instead (row 224 in every field), line-doubled as the
harness does. PCSX2 against the port, MAE / % of pixels off by more than 8:

| field | switch off | `all` |
|---|---|---|
| title 700 | 2.903 / 9.34 % | 2.903 / 9.34 % |
| shop 3300 | 11.503 / 45.37 % | 11.503 / 45.37 % |
| race start 6600 | 6.052 / 19.67 % | 6.053 / 19.67 % |
| mid-race 9300 | 6.326 / 27.58 % | 6.328 / 27.60 % |

The switch moves the port neither toward PCSX2 nor away from it (at most 0.002 MAE): what PCSX2
sees differently is the native renderer's own (finding 162: mip levels, filtering), the same in both
modes. The port's two pictures differ by MAE 0.001-0.073 at 1x.

### The shadow check, per loop

`DHD_NATIVE_VU1_CHECK=1` at 2x to field 9600 (shadow mode: each item through the shader's maths
against the packet the VU wrote): vertices, then those off in X or Y by more than 1/16 pixel, Z by
more than 1, fog, colour, STQ beyond 1e-4 relative; triangles, ADC differences.

| loop | vertices | XY | Z | fog | colour | STQ | triangles | ADC |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 0170 | 161.4 M | 6,923 | 233,502 | 0 | 0 | 317 | 148.2 M | 24,255 |
| 1708 | 18.6 M | 5 | 4,804 | 0 | 0 | 0 | 17.3 M | 0 |
| 08B0 | 24.5 M | 0 | 0 | 0 | 0 | 0 | 22.9 M | 0 |
| 0798 | 48.0 M | 0 | 1,088 | 0 | 0 | 0 | 44.9 M | 0 |
| 18B0 | 48.4 M | 966 | 527,763 | 0 | 0 | 8 | 45.2 M | 0 |
| 1110 | 50.0 M | 0 | 6,869 | 0 | 0 | 0 | 47.5 M | 0 |
| 13A0 | 24.7 M | 0 | 15 | 0 | 0 | 0 | 23.4 M | 0 |
| 1510 | 6.7 M | 1 | 61 | 0 | 0 | 0 | 6.4 M | 0 |
| 3140 | 17.8 M | 0 | 5,099,035 | 0 | 0 | (untextured) | 16.9 M | 0 |
| 0AA0 | 28,728 | 0 | 0 | 0 | 0 | 0 | 27,216 | 0 |
| 0C30 | 5.7 M | 0 | 0 | 0 | 0 | 1,335 | 5.2 M | 0 |
| 2DE8 | 8.4 M | 4,325 | 78,908 | 13,266 | 0 | 3,705 | 7.7 M | 0 |

Colours (every lit, highlight, constant and 2DE8 colour computed on the CPU) never differ by more
than 1. The Z counts are the VU's toward-zero rounding at a 1-unit step, as in phase 1 (0.1-1 %),
except 3140's 29 %: the shadow pass leaves z unscaled, near 2^24 (median 16.73 M in field 9000's
GS dump), where one float step is a whole Z unit, and its draws neither test nor write depth (all
2,325 of that field's vertices: TEST ZTST 1, always; ZBUF ZMSK 1; into the 64-wide shadow target
at FBP 1FC). 2DE8's few fog, XY and STQ differences (0.04-0.2 %) sit with its Z ones; the pictures
show none of them.

### Game state

State traces (headless, finding 164's method) with `DHD_NATIVE_VU1=all`: boot (600 fields)
against finding 164's base trace, and the race (`state_trace_run.py race`, 5000 fields, the first
track) against finding 162's base build. In shadow mode every row of both equals the base. In skip
mode EE RAM, scratchpad, CPU, the hardware registers, GS memory, sound, code and the stack words
are equal in every row of both; the VU memory and registers, the GS stream and the GS registers
differ from field 29 on, by design (the batches do not run, their packets never reach the GS). The
race in skip mode translated batches of every loop (0AA0 only 36), and ran on the VU 18 % of
0170's, 32 % of 1708's, 17 % of 18B0's, 32 % of 0C30's, 12 % of 1510's and 3 % of 2DE8's, all for
the near bound; 1110 2 % (5,741 near bound, 589 a lit colour of -1 or less, which FTOI0 would
hand the GS wrapped), 13A0 47 batches (colour), 1708 2 (a join value other than 0 or 2048.0).

## Timing

### Method

Finding 164's (finding 156's route, `DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`, 8x, skip mode, two
runs of each interleaved, the timed game lock, each run started on an idle PC: six 5-second windows
under 30 % within 6 points of each other after 4 minutes, the owner's background apps keeping it at
about 11-24 %; recorded per run). Milestones are switch values of one build: off, `prelit`
(step 1), `prelit,lit,shadow` (step 2, which with the batch keying covers step 3's batches), `all`
(the end: with 2DE8). The phone session ran at the same time as part of the last Windows session
(its PC side is adb polling only); no game or build of another lane ran.

### Windows, RTX 3070, 8x

The end build (`39533bb`, release with line tables). Mean of two runs (each in brackets); ms a field,
columns as finding 156's.

| scene | switch | fps | field ms | game work / busy | vu work / busy | gs work | render CPU / busy | GPU ms |
|---|---|---:|---:|---|---|---:|---|---:|
| title | off | 180 (175/186) | 5.55 | 1.39 / 0.93 | 5.54 / 5.55 | 2.64 | 0.80 / 0.47 | 1.54 |
| title | prelit | 249 (243/255) | 4.02 | 1.33 / 0.95 | 4.01 / 4.02 | 1.76 | 0.76 / 0.54 | 1.40 |
| title | lit | 300 (296/303) | 3.34 | 1.32 / 0.92 | 3.34 / 3.34 | 1.30 | 0.74 / 0.52 | 1.37 |
| title | all | 290 (292/288) | 3.45 | 1.34 / 0.93 | 3.46 / 3.45 | 1.13 | 0.84 / 0.53 | 1.36 |
| shop | off | 233 (212/255) | 4.33 | 1.12 / 0.75 | 4.27 / 4.33 | 1.32 | 0.97 / 0.66 | 1.72 |
| shop | prelit | 315 (313/317) | 3.18 | 1.02 / 0.74 | 3.16 / 3.18 | 0.74 | 0.89 / 0.65 | 1.85 |
| shop | lit | 342 (346/338) | 2.92 | 1.04 / 0.73 | 2.92 / 2.92 | 0.89 | 0.91 / 0.66 | 1.89 |
| shop | all | 310 (278/342) | 3.26 | 1.02 / 0.75 | 3.13 / 3.26 | 0.84 | 1.01 / 0.73 | 1.86 |
| start | off | 221 (216/226) | 4.52 | 3.40 / 3.13 | 4.49 / 4.52 | 2.54 | 1.90 / 1.58 | 1.91 |
| start | prelit | 289 (291/287) | 3.46 | 3.05 / 3.00 | 3.42 / 3.46 | 1.79 | 1.72 / 1.56 | 1.95 |
| start | lit | 308 (305/310) | 3.25 | 3.07 / 3.00 | 3.23 / 3.24 | 1.67 | 1.87 / 1.54 | 1.97 |
| start | all | 307 (308/306) | 3.26 | 3.00 / 3.02 | 3.22 / 3.25 | 1.49 | 1.84 / 1.54 | 1.96 |
| mid | off | 277 (265/288) | 3.62 | 2.86 / 2.71 | 3.58 / 3.62 | 1.96 | 1.51 / 1.22 | 1.53 |
| mid | prelit | 357 (357/357) | 2.80 | 2.58 / 2.56 | 2.78 / 2.79 | 1.49 | 1.50 / 1.20 | 1.58 |
| mid | lit | 368 (364/372) | 2.72 | 2.59 / 2.59 | 2.70 / 2.71 | 1.31 | 1.41 / 1.19 | 1.58 |
| mid | all | 366 (367/364) | 2.74 | 2.59 / 2.59 | 2.70 / 2.72 | 1.27 | 1.45 / 1.20 | 1.60 |

Whole route (fields 300-14000): off 240.7 / 261.1, `prelit` 333.0 / 334.7, `prelit,lit,shadow`
355.0 / 359.7, `all` 348.6 / 352.5 fields/s. The milestone 1 session before (`c08668c`, the same
method): off 261.5 / 254.1, `0170` 297.4 / 313.5, `prelit` 325.3 / 337.6, and that build's `all`
(prelit with 1110 and 13A0) 341.9 / 355.4.

- **Off is the base** (finding 164: the switch-off build equals 162's); its four runs over the two
  sessions spread 241-262 fields/s.
- **Step 1 (prelit): +31 %** over the route (all four runs: 332.7 against 254.4 fields/s); the
  title +38 %, the shop +35 %, the race start +31 %, mid-race +29 %.
- **Step 2 (lit, shadow; step 3's batches): +40 %** (357.3); title +67 %, shop +47 %, race start
  +39 %, mid-race +33 %.
- **The end (`all`, with 2DE8): +38 %** (350.5): 2DE8 gains nothing measurable on Windows (its
  native kernel was already about as cheap as the translation, below), and its two runs sit within
  the spread of step 2's.
- **The VU thread is still the limit, by a little.** At the race start it is busy 3.25 ms a field
  against the game thread's 3.02, mid-race 2.72 against 2.59; the GS thread does half the work it
  did (race start 2.54 -> 1.49 ms). The GPU does not notice (1.4-2.0 ms a frame either way).

### VU1 time per program (Windows 8x, `DHD_VU1_PROFILE`)

The end build's profiled runs, switch off against `all`; ms of VU1 time a field by object entry
(the MSCAL that started the object: its setup, its passes, the batches left on the VU and, with
the switch, the translation of the others).

| entry | title off / all | shop | race start | mid-race |
|---|---|---|---|---|
| 0078 (0170) | 1.64 / 0.97 | 1.56 / 1.13 | 1.30 / 0.68 | 1.05 / 0.69 |
| 0020, 0010 (skinned: pass, 1110/13A0/3140 batches) | 1.41 / 0.79 | 0.75 / 0.54 | 0.36 / 0.27 | 0.19 / 0.15 |
| 0040 (lit: pass, 1110/13A0/3140 batches) | 0.07 / 0.06 | 0.26 / 0.18 | 0.51 / 0.41 | 0.24 / 0.20 |
| 1638 (1708) | 0.10 / 0.11 | 0.10 / 0.10 | 0.29 / 0.26 | 0.27 / 0.25 |
| 07F0, 06F0, 17E0 (08B0, 0798, 18B0) | 0.58 / 0.41 | 0.38 / 0.29 | 0.38 / 0.26 | 0.39 / 0.28 |
| 1430, 1050, 12F0 (1510, 1110, 13A0) | 0.05 / 0.04 | 0.01 / 0.01 | 0.15 / 0.10 | 0.06 / 0.04 |
| 2DC8 (2DE8) | | | 0.09 / 0.08 | 0.10 / 0.10 |
| overlay A (stays) | | 0.07 / 0.07 | 0.22 / 0.21 | 0.25 / 0.25 |
| total | 4.01 / 2.53 | 3.17 / 2.36 | 3.32 / 2.29 | 2.59 / 1.97 |

The translation's own cost on the VU thread (the same runs): 0.39-0.50 us a batch and 12-14 ns a
vertex for the prelit loops, 0.66-0.82 us and 17-20 ns for the lit ones (their colours and
texture coordinates on the CPU), 0.46-0.50 us for 3140, 0C30 and 2DE8 (2DC8's native kernel cost about as much). What is
left: 0078's setups and its near-bound batches, 1638's (42 % of its batches cross the near bound),
the skinning and lighting passes, overlay A.

### Mi 11i on the t612 profile, 2x

The phase's APK (release-signed, the shipped builds' PGO profile, this branch at `39533bb`),
finding 156's run script (the run's keys in the app's `dhd.ini`, put back and checked after each
run; other apps closed, screen on, 60 s cooling), the frozen t612 profile (`phone_throttle.py
--apply t612`, restored after the session), two rounds of off, `prelit`, `prelit,lit,shadow`,
`all`. Mean of the two runs (each in brackets); ms a field from the run's timeline: the game, VU
and GS threads' CPU time, the render thread's CPU time, the GPU (sampled).

| scene | switch | fps | field | game | VU | GS | render | GPU |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| title | off | 49.6 (49.3/49.8) | 20.18 | 15.34 | 18.71 | 17.53 | 4.65 | 0.40 |
| title | prelit | 62.0 (61.9/62.1) | 16.12 | 11.61 | 14.57 | 10.72 | 5.35 | 0.45 |
| title | lit | 70.2 (70.0/70.5) | 14.24 | 9.99 | 12.58 | 8.32 | 5.34 | 0.46 |
| title | all | 70.6 (70.3/70.8) | 14.17 | 9.98 | 12.52 | 8.28 | 5.41 | 0.46 |
| shop | off | 63.0 (63.0/63.1) | 15.87 | 10.71 | 14.36 | 9.73 | 6.38 | 0.51 |
| shop | prelit | 71.5 (71.6/71.4) | 13.99 | 9.26 | 12.17 | 7.20 | 7.07 | 0.55 |
| shop | lit | 76.4 (76.2/76.5) | 13.09 | 8.28 | 11.19 | 5.88 | 7.25 | 0.56 |
| shop | all | 76.8 (76.7/76.9) | 13.02 | 8.27 | 11.16 | 5.84 | 7.27 | 0.56 |
| start | off | 58.8 (58.5/59.0) | 17.01 | 14.36 | 14.72 | 15.04 | 13.33 | 0.83 |
| start | prelit | 62.9 (62.7/63.2) | 15.89 | 13.38 | 12.33 | 11.51 | 14.23 | 0.87 |
| start | lit | 62.3 (61.3/63.2) | 16.06 | 13.39 | 11.68 | 9.17 | 14.42 | 0.88 |
| start | all | 62.2 (62.6/61.7) | 16.08 | 13.40 | 11.66 | 9.18 | 14.44 | 0.89 |
| mid | off | 72.7 (72.9/72.6) | 13.75 | 11.78 | 11.89 | 11.83 | 10.69 | 0.64 |
| mid | prelit | 81.3 (80.8/81.9) | 12.30 | 10.42 | 10.09 | 8.63 | 11.43 | 0.69 |
| mid | lit | 81.2 (80.7/81.7) | 12.31 | 10.46 | 9.91 | 7.39 | 11.50 | 0.69 |
| mid | all | 81.5 (81.5/81.5) | 12.27 | 10.43 | 9.79 | 7.33 | 11.48 | 0.70 |
| route | off | 63.9 (63.7/64.0) | 15.66 | 12.06 | 13.06 | 12.71 | 9.49 | 0.59 |
| route | prelit | 72.4 (72.1/72.8) | 13.81 | 10.29 | 10.56 | 9.02 | 10.43 | 0.64 |
| route | lit | 74.4 (74.1/74.7) | 13.44 | 9.92 | 10.01 | 7.65 | 10.49 | 0.64 |
| route | all | 74.7 (74.9/74.5) | 13.39 | 9.87 | 9.89 | 7.56 | 10.53 | 0.65 |

- **The route: 63.9 -> 74.7 fields/s (+17 %)**; the title +42 %, the shop +22 %, mid-race +12 %,
  the race start +6 %. Most of it comes with step 1 (+13 %: the prelit world), step 2 adds 3 %,
  2DE8 nothing measurable.
- **The VU and GS threads** do 3.2 and 5.2 ms less a field over the route (title: 6.2 and 9.3 ms).
- **The race start is the render thread's now**: its CPU time grows by 1.1 ms a field with the
  switch (13.3 -> 14.4 ms, the mesh draws; not taken apart further; title 4.7 -> 5.4, shop 6.4 -> 7.3)
  and it is the busiest thread there, next to the game thread (13.4 ms). Mid-race too the render
  thread is the busiest (11.5 ms), then the game thread (10.4) and the VU (9.8).

## Decisions

- **The phone only on the t612 profile at 2x** (the owner, 2026-11-06), and **Windows at 8x only**,
  as finding 164.
- **Batch loops by VI13, passes on the VU.** The lit and skinned objects' passes (0040's lighting
  loop 1280, 0010/0020's skinning and lighting 0468, 0000's 05A0) write the vertex pool, which the
  translation reads on the CPU: positions for the near-bound test and the item's vertices, lighting
  and normals for the colours. Moving a pass to the GPU would move the pool there and with it the
  near-bound decision (today a batch below the near bound goes back to the VU, which needs the pool
  in VU memory). That is a different design (a GPU skinning pass and a GPU-side fallback), not a
  loop translation; the passes stay on the VU, and their objects' batches are translated.
- **Overlay A stays on the VU in this phase.** Its loops are not strip loops over an index pool: 2E48
  builds a screen-space quad per particle from two projected points with widths and clamps and
  culls per particle by flags (FMAND), 29C0 is a recurrence with random numbers, 0E10/02D0 are
  sprites with their own register packets. Moving them needs a new item kind (instanced quads) and
  its own vertex shader; their native kernels already run them. Its share: 0.25 ms a field mid-race
  at 8x (about a tenth of what is left on the VU), nothing in the title and shop.

- **The small programs stay on the VU.** 28D0, 2B10, 2C20 and 2738 have loops of their own
  (VI13 0x51C, 0x564, 0x586, 0x4E9) and take at most 0.02 ms a field each at 8x; 0280 and 0948
  (0x52, 0x12B) never run on the route; 0000 is a pass (05A0, bone palette). Not translated: no
  measurable gain, and no route to check them on.

## Surprises

- **Finding 162's program names are prologues, the loops sit elsewhere.** 17E0's loop is at 18B0,
  1430's at 1510, 0A90's at 0AA0: VI13 holds the prologue's address / 8, and the translation keys
  on it.
- **A loop can rewrite its own GIF tag.** 3140 and 2DE8 store an LOI constant over the header's
  PRIM word (3140's untextured strip, 2DE8's blended one); the header in VU memory is then not what
  the GS draws with.
- **2DE8's colour is a float.** It stores 1 + 2^-16 as R, G and B (low byte 128) and d1013.w's bits
  as A; whether A is kept depends on the sign flags of the dot product four instructions earlier
  (FSAND at 2F30 against the MADD at 2F10), as the native kernel of 2DC8 models it.

- **PCSX2 2.8.2's saved field buffers sit 224 rows down.** `compare_pcsx2.scanout` crops this
  game's fields (DISPFB 0x8C0 and 0x1180, DBY 0) at row 0 and scores black; the referee here found
  the window by search. The tracked harness is unchanged (validated with other captures, finding
  024); a fix belongs with it.
- **2.3.232 cannot replay these dumps headless** (the replay timed out with no log); the referee
  used the 2.8.2 release the harness names.

## Result

Per program, what is on the GPU now (the shares of VU1 time are finding 162's, Windows 4x, title /
shop / race start / mid-race; "on the GPU" counts batches over the route at 2x):

| program (loop) | share of VU1 time | translated | on the GPU | commit |
|---|---|---|---:|---|
| 0078 (0170) | 42 / 49 / 37 / 38 % | the loop | 84.7 % | `43d6ce2`, `02e48c5` (164) |
| 1638 (1708) | 3 / 4 / 11 / 12 % | the loop | 58.3 % | `1b79009` |
| 07F0 (08B0) | 0 / - / 3 / 9 % | the loop | 100 % | `1b79009` |
| 06F0 (0798) | 10 / 1 / 5 / 4 % | the loop | 100 % | `1b79009` |
| 17E0 (18B0) | 4 / 11 / 4 / 3 % | the loop | 88.5 % | `1b79009` |
| 0020, 0010 | 32 / 24 / 10 / 7 % | their batches (1110, 13A0, 3140); the skinning pass stays | 99.7 % of 1110's, 13A0's | `c08668c` |
| 0040 | 2 / 7 / 15 / 9 % | its batches (as 0020's); the lighting pass stays | (with 0020's) | `c08668c` |
| 1050, 12F0 (1110, 13A0) | (in 1430's row) | the loops | 99.7 % | `c08668c` |
| 1430 (1510) | 1 / 0 / 5 / 2 % with 1050, 12F0, 0C20, 0A90 | the loop | 97.8 % | `e184b05` |
| 30A0 (3140) | - / - / 0.4 / 0.5 % | the loop, and every 0x616 batch | 100 % | `fe081ec` |
| 0A90, 0C20 (0AA0, 0C30) | (in 1430's row) | the loops | 100 %, 77.3 % | `d4373c3` |
| 2DC8 (2DE8) | - / - / 3 / 4 % | the loop | 83.2 % | `39533bb` |
| overlay A | - / 3 / 6 / 14 % (loops) | nothing (Decisions) | 0 % | |
| 0000 (05A0) | 0.1 ms a field (title) | nothing: a bone-palette pass; its time does not change | | |
| 28D0, 2B10, 2C20, 2738; 0280, 0948 | <= 0.02 ms a field each; the last two not on the route | nothing (their own loops: VI13 0x51C, 0x564, 0x586, 0x4E9; 0x52, 0x12B) | 0 % | |

- **Correct:** every field of finding 156's route within the tolerance at 2x and 4x (the worst
  MAE 0.066, 0.087 % of pixels off by more than 8; the same as 0170 alone), PCSX2's replay as far
  from the port with the switch as without (at most 0.002 MAE apart), no colour or fog
  difference in the shadow check but 2DE8's 0.16 % fog, and the game state the switch-off run's
  (boot and race state traces).
- **Faster:** Windows RTX 8x, finding 156's route: 254 -> 333 (step 1) -> 357 (step 2) -> 351
  (all) fields/s, +38 % at the end (title +61 %, shop +33 %, race start +39 %, mid-race +32 %). The
  Mi 11i on the t612 profile at 2x: 63.9 -> 72.4 -> 74.4 -> 74.7 fields/s, +17 % (title +42 %,
  shop +22 %, race start +6 %, mid-race +12 %).
- **What limits now:** on Windows the VU thread still, by 0.1-0.25 ms over the game thread in
  races; on the phone the render thread (the mesh draws add 0.5-1.1 ms a field there) and the game
  thread.
- **What is left on the VU, largest first:** the batches with a vertex below the near bound
  (15 % of 0170's, 42 % of 1708's, 12 % of 18B0's, 23 % of 0C30's, 17 % of 2DE8's on the route:
  the clipper's near-plane fans would have to be reproduced, finding 164); the skinning and
  lighting passes (0020, 0010 and 0040 together 0.35-0.7 ms a field in races and 0.85 ms in the
  title at 8x, with their batches' translation: a GPU pass and a GPU-side near-bound fallback);
  overlay A (0.2-0.25 ms in races: a new item kind); the setups; the small programs (above).
