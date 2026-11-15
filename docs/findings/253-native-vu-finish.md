# 253: Native VU finished: the near batches clipped on the VU thread

Status: current

Branch `perf/native-vu-finish-253` from `main` 7987ff50 (release 0.8.5 plus lanes 247 and 249),
started 2026-11-15. This file is the lane's plan while it runs and its finding when it merges.
Someone who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

Finding 249's work list: on finding 156's route, 1.78 million of 16.4 million batches (11 %) of
the translated VU1 loops still run on the emulated VU, nearly all because a vertex of the batch is
below the near bound and the loop's own clipper has to cut its triangles (a GPU's clipping draws
something else there, finding 164).

Target: those batches run natively with the clipper's own result (the triangles the VU program
kicks), so that in a race no batch of a translated loop runs on the emulated VU, or only a short
list with a reason each. Then a survey beyond the race (menus, rider select, replays, split
screen, other tracks, the poster maker) of what still reaches the emulated path. Deleting the
emulated VU1 path is not this lane.

Acceptance: `python tools/check.py quick` and `port`; boot (600 fields) and race (5000) state
traces equal to `main` 7987ff50's; pictures against Native VU off no worse than the base's
(`native_vu_pictures.py`, near geometry included); the Android unit tests; the throttled Mi 11i
(`t612`, 4x) no slower than the base with Native VU on.

Out of scope: the deletion of the emulated path, gameplay, the picture's content.

## Progress

- [x] 2026-11-15: finding reserved
- [x] change 1: the batches that cross the near bound are clipped on the VU thread (2db8f22f)
- [x] change 2: the lit loops take any colour word (b24e6a8d)
- [x] `DHD_VU1_EMULATED=1`, the list of VU1 runs the emulated VU still makes (693c4320)
- [x] change 3: a clipper fan vertex below the loop's near bound is drawn (5fb0a245)
- [x] the survey beyond the race, the throttled Mi 11i before and after, the checks

## Result in one table

Batches of the translated loops on finding 156's route (fields 300-14000, the phone's own
counters), and fields a second on the throttled Mi 11i (`t612`) at 4x, the mean of two runs each:

| Build | Route: batches on the emulated VU | Race part: on the emulated VU | Race, on | Race, off | Start, on | Mid-race, on | Route, on | Route, off | VU thread a field (race, on) |
|---|---|---|---|---|---|---|---|---|---|
| base (7987ff50) | 1,777,293 of 16,355,739 (10.9 %) | 785,126 of 8,837,537 (8.9 %) | 64.45 | 65.27 | 51.31 | 67.87 | 66.16 | 65.85 | 9.59 ms |
| the lane's head | 2 of 16,355,786 | 0 of 8,837,667 | 65.02 | 65.32 | 51.53 | 68.58 | 67.13 | 65.87 | 8.60 ms |

The race part is fields 6000 to 14000 (the counters are logged every 3000 fields). The VU thread's
time is its CPU time a field from the game's timing lines; with Native VU off it is 11.1 ms. Native
VU on is now 0.5 % behind off in the race (it was 1.3 %) and 1.9 % ahead over the route. The GPU's
span a picture did not grow (13.9 ms before, 13.75 after).

The 2 batches left are 1708's "join value" pair of the title screen (a strip whose join word is
neither 0 nor 2048.0), the same two in every run.

## Change 1: the batches that cross the near bound are clipped on the VU thread (kept, 2db8f22f)

A loop with CLIP sends a triangle whose vertices carry a flag (off the guard band in x or y, below
the near bound) to its clipper (1948, then 1CC0-2738) and sets its ADC in the strip. The clipper is
now C in `vu1_mesh.inc` (`mesh_clip_triangle`, `mesh_clip_plane`), step for step:

- 1948: the three pool positions through the clip matrix (data 27-30; vertex a summed from the
  fourth column, b and c toward it), CLIP against each vertex's own w, the trivial reject when all
  three share a flag, STQ divided to (u, v, 1). 0C30 has its own copy at 0E18 (the environment
  map: it carries the pool normals), 2DE8 enters with the projected-texture mode (its colour words
  are float bits read as integers).
- 1CC0: the planes near, +y, -y, +x, -x in the space of data 176-179, in or out by the CLIP flag,
  the cut at t = d0 / (d0 - d1) with d = w - 1, w -+ y, w -+ x, buffers of 10 vertices.
- The output: each vertex through the loop's matrix (vf1-vf4) again, fog = clamp((d4.w + d21.x) +
  d21.w Q, 0, d21.y), Z times d4.z^4, at most 16777215, FTOI4, kicked as a fan before the strip.

The VU's float rules are kept: round toward zero, a divide by zero gives the largest number,
CLIP compares bits, FTOI saturates. An item carries the fans' vertices after the strip's (at most
9 a fan, `GS_MESH_FAN_VERTICES`) and their triangles first in its index list; in a culled item
(0170) each fan triangle is ordered by its own area, because the clipper's fans are drawn from
both sides.

`DHD_NATIVE_VU1_CHECK=1` (shadow mode) now also compares every fan the VU kicks with the one made
here, bit for bit (a watch on `kick()` in `vif.c`). On the route to field 9600 at 2x: 0170 116,471
fans with 441,590 vertices, 1708 35,632 / 133,812, 18B0 8,026 / 30,383, 2DE8 28,184 / 106,407; no
fan missing, none with another count, no XYZF2, RGBA or STQ word different. 0C30's 40,874 near
batches have every flagged triangle rejected by the clipper's own test, so its environment-map
output has run on no data yet.

The `:gpuclip` measurement option of finding 249 is deleted.

## Change 2: the lit loops take any colour word (kept, b24e6a8d)

1110 and 13A0 refused a batch whose vertex colour word or lit sum was out of a byte's range
(3,716 batches on the route). The route's cases are pool entries the lighting pass left alone:
their words are integers, which read as floats are tiny numbers or, for a negative one, exponent
255. A PC makes a NaN of that; the VU reads a huge number, and its sum saturates: it stores
0x80000000, of which the GS takes the low byte, 0. `mesh_vu` gives the sum the VU's rule, FTOI0
stores the word, the item takes its low byte and the clipper the whole word. The check run shows
no colour different on those batches.

## Change 3: a clipper fan vertex below the loop's near bound is drawn (kept, 5fb0a245)

Change 1 refused a batch when a fan vertex, projected through the loop's matrix, was below the
loop's CLIP bound. The route has none; the survey found them: 17,844 of split screen's 0170
batches, about 500 a run on tracks 2 and 3 (18B0, 2DE8, 1708), 148 in the replay. The clipper's
near plane is its own space's (data 176-179), not the loop's bound, so such a vertex is what the
VU kicks. The shader draws it as the plain projection; the item gives it the Z the clipper
clamped. Only a fan vertex behind the eye (w <= 0) still refuses, and no run has one.

## Pictures

`native_vu_pictures.py` at 4x against the base with Native VU off, worst field a scene (MAE /
pixels off by more than 8 / maximum):

| Scene | Base, on | Head, on |
|---|---|---|
| title | 0.004 / 0.002 % / 77 | 0.004 / 0.002 % / 77 |
| shop (the near ground fills the picture) | 0.005 / 0.005 % / 73 | 0.005 / 0.005 % / 73 |
| race start | 0.004 / 0.006 % / 112 | 0.004 / 0.006 % / 112 |
| mid-race | 0.006 / 0.007 % / 126 | 0.010 / 0.007 % / 126 |

Head against the base with Native VU on: MAE at most 0.005, no pixel off by more than 8 but in
one field (maximum 11). Split screen (five fields of the 2-player race at 4x, the state trace's
recipe in a window): MAE 0.003-0.004 before, 0.008-0.011 after; the share of pixels off by more
than 8 (0.003-0.011 %) and the maximum are the same in every field. The added difference is one
colour level here and there over the near ground: the last bit of a texture coordinate through
the mesh shader, as on every other translated batch, now also on the 11 % the VU used to kick.
All of it is far inside the tool's tolerance (MAE 0.1, 0.1 % off by more than 8).

## The survey: what still reaches the emulated VU

`DHD_VU1_EMULATED=1` counts every VU1 run that goes through the emulated VU while Native VU is on,
per kind (overlay, program, entry, MSCNT, VI13), in the log every 3000 fields and at the end. The
runs: the state trace's recipes in a window on a hidden desktop (headless has no Native VU), skip
mode as players have it; 3100 fields of menus, 9100 fields each of track 1, its replay
(`DHD_REPLAY_TEST=20`), tracks 2 and 3 and the 2-player split screen, and `options_walk.py`'s
POSTERS row. The bike shop and rider select are in the route's check run above and add no kind.

Batches of the translated loops on the emulated VU: 2 in every run (1708, join value; 1 in the
menus), nothing else.

Runs that draw (they kick), runs a scene:

| Overlay | Program | Entry | VI13 | Kicks a run | Menus | Posters | Track 1 | Replay | Track 2 | Track 3 | Split |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A | 02D0 | 02D0 | | 1.0 | 596 | 375 | 12,019 | 25,011 | 13,756 | 8,075 | 19,737 |
| A | 0E10 | 0E10 | | 2.8 | | | 30,716 | 56,741 | 28,827 | 43,149 | 67,447 |
| A | 1010 | 1010 | | 1.0 | | | 81 | 66 | | 901 | 541 |
| A | 1240 | 1240 | | 1.1 | | | 2,917 | 3,667 | 4,956 | 4,551 | 522 |
| A | 26D8 | 26D8 | | 18.0 | | | 4,102 | 6,828 | 4,083 | 3,129 | 4,552 |
| A | 29C0 | 29C0, then 2CE8 (MSCNT) | | 1.0 | 660 | 432 | 14,622 | 14,290 | 12,240 | 11,448 | 31,826 |
| A | 2E48 | 2E48 | | 6.5 | | | 33,406 | 53,486 | 34,060 | 35,656 | 72,381 |
| B | 0000 | 06E0 (MSCNT) | | 1.0 | 48,438 | 46,796 | 13,029 | 14,223 | 13,568 | 14,070 | 8,090 |
| B | 0030 | 0030 | | 1.0 | 300,722 | 315,125 | 148,250 | 165,262 | 203,776 | 164,541 | 198,683 |
| B | 0050 | 0050 | | 1.0 | | | 25,536 | 25,528 | 25,528 | 22,868 | |
| B | 28D0 | 1040 and 1CB0 (MSCNT) | 51C | 1.0 | 17,100 | 14,040 | 7,917 | 7,867 | 63,066 | 690,901 | 5,146 |
| B | 2C20 | 1040 and 1CB0 (MSCNT) | 586 | 1.0 | | | 16,336 | 22,174 | 22,021 | 3,842 | 31,584 |
| B | 2B10 | 1CB0 (MSCNT) | 564 | 1.0 | | | 13,788 | 14,817 | 10 | 3,496 | 10,440 |
| B | 2738 | 1040 and 1CB0 (MSCNT) | 4E9 | 1.0 | | | 112 | 112 | 16,246 | 658 | |
| B | 30A0 | 3258 (MSCNT) | | 1.0 | | | 11,964 | 12,012 | 12,021 | 10,568 | |

The four overlay B kinds with a VI13 are batch loops of the same family as the translated twelve
(the dispatch at 1040 / 1CB0), not yet translated: 51C is the big one (691,000 batches on track
3). 0030, 0050, 06E0 and 3258 are overlay B's other drawing programs, and overlay A's seven are
all untranslated.

Runs that kick nothing (setup: they load registers and data for the batches that follow) are most
of the emulated runs, 0.8 million (menus) to 3.8 million (track 3) a scene: overlay A 0000;
overlay B 0000, 0010, 0020, 0040, 0078, 06F0, 07F0, 0A90, 0C20, 1050, 12F0, 1430, 1638, 17E0,
2738, 28D0, 2B10, 2C20, 2DC8, 30A0. A translated batch reads what they leave in the VU's registers
and memory (vf1-vf4, vf13, vf18-vf20, vf31, VI13, data 0-30, 176-181, 996-1015).

## Decisions

- 2026-11-15: in a batch that crosses the near bound, the triangles off the guard band sideways
  are drawn as the clipper's fans too, not left to the GPU. With the GPU cutting them beside a
  fan, the bike shop's near ground differed from the emulated picture over a wedge (MAE 0.041
  against 0.005); with the whole batch as the VU kicks it, the picture is the base's. Batches
  that do not cross the near bound keep finding 164's rule (the GPU clips sideways).
- 2026-11-15: the fans are checked against the VU's kicked words, not through the shader's maths:
  a fan is what the clipper kicked or it is wrong, there is no tolerance to choose.
- 2026-11-15: the survey counter is a test variable, not a line in every player's log: it is a
  lookup on each of 3 to 5 million runs.

## Surprises

- The small picture difference of the first version was not where the fans are placed. Putting
  the fan vertices, and then the strip vertices of a near batch, exactly on the kicked 1/16 pixel
  changed no number; the sideways triangles of the same batch were the cause (the decision above).
- "Colour" was not a negative lit sum but a NaN: integer bits in the lit pool entry (change 2).
- The headless state trace runs without Native VU (it needs the native renderer with indexed
  fields), so the boot and race traces prove only that the shared code did not move. What Native
  VU draws is proven by the check run and the pictures; game state does not read the mesh path.
- The game's field stop (`DHD_STATE_STOP`) ends without the end reports, so the survey runs stop
  just after a 3000-field report.
- A fan vertex can lie below the loop's near bound (change 3): the clipper and the loop's CLIP use
  different spaces. The route never shows it; split screen does, thousands of times.

## Result

On the route no batch of a translated loop runs on the emulated VU but 1708's two "join value"
strips of the title screen (base: 1,777,293 of 16,355,739, 10.9 %); in the race part none (base:
785,126 of 8,837,537, 8.9 %). The same holds in the menus, the poster maker, a replay, tracks 2
and 3 and split screen.

Throttled Mi 11i at 4x, Native VU on: the race 64.45 -> 65.02 fields a second (off: 65.3), the
race start 51.31 -> 51.53 (off: 51.87), the route 66.16 -> 67.13 (off: 65.86). The VU thread's
time a field in the race went from 9.59 to 8.60 ms (off: 11.1 ms).

Checks: `check.py quick` and `check.py port` OK; boot (600 rows) and race (5000 rows) state traces
equal the base's (`state_diff.py`: PASS); the Android unit tests pass; pictures as above.

Not run: the Redmi 9 (Mali-G52), the Mac, the browser. Nothing in the lane is per GPU; an item
now holds up to 64 + 62 x 9 vertices (16-bit indices as before).

What is left before the emulated VU1 path can be deleted, in order:

1. 1708's "join value" strips (2 a run): read the join word as the loop's FTOI4 does.
2. Overlay B's four untranslated batch loops (VI13 51C, 586, 564, 4E9) and its programs 0030,
   0050, 06E0 and 3258; overlay A's seven drawing programs. Each needs its own translation and
   check, as findings 164, 167 and 194 did for the twelve.
3. The setup programs: they must still run, or become C, because the translated batches read the
   registers and data they leave. They are most of the emulated runs.
4. The GPU fallback (a driver that refuses the mesh draws) and the setting's off need another
   answer than the emulated loops before those can go: the root AGENTS.md decision.
5. 0C30's clipped environment-map fans and a fan vertex behind the eye have code but no data that
   reaches them; a capture that does is worth a look before the deletion.
