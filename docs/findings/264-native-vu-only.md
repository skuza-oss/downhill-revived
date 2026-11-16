# 264: Native VU only: nothing left on the emulated VU1, then the emulated path deleted

Status: current

Branch `feat/native-vu-only-264` from `main` e9bba855 (release 0.8.7), started 2026-11-16. This
file is the lane's plan while it runs and its finding when it merges. Someone who has only this
file and the git log must be able to pick the lane up.

## Goal and acceptance

Finding 253 left a work list of what still runs on the emulated VU1 with Native VU on (outside
races, and beside the translated batches). This lane finishes it in stages that each stand on
their own:

1. **Census**: every VU1 program and entry run across the game (menus, the poster maker, a race
   on two mountains, the other event types, a replay, split screen), with counts, native or
   emulated: `DHD_VU1_EMULATED=1` (finding 253) on each scene, as below. The yardstick.
2. **Translate what is left**, largest count first, each proven equal by the existing tools
   (`DHD_NATIVE_VU1_CHECK=1`, `native_vu_pictures.py`). One commit per group.
3. **Only when the census shows zero emulated runs** everywhere it can reach: delete the emulated
   VU1 path, the `native_vu` setting and its rows (settings page, launcher, PLAYING.md, tests).
   If anything cannot be reached or proven, the path stays and this file lists what blocks it.

Acceptance per stage: `python tools/check.py quick`, `port` and `docs`; boot (600 fields) and race
(5000) state traces equal to e9bba855's.

Out of scope: gameplay, the picture's content, speed work.

## Progress

- [x] 2026-11-16: finding reserved; the census counter records VI13 at every `JR vi13` door (ec9fceda)
- [x] stage 1: the census, nine scenes, before and after the translations (below)
- [x] stage 2, group 1: the first batch after a pool pass or 30A0's setup (doors 06E0, 3258) (8a43581d)
- [x] stage 2, group 2: programs 0030 and 0050 in C (d85bc7a7)
- [ ] stage 2, what is left: overlay B's four quad loops, overlay A's eight programs, 1708's join
  value, the setup programs (not started: see "What blocks the deletion")
- [x] stage 3: not done; what blocks it is listed below

## How the census is run

Each scene is the state trace's recipe (`state_trace_run.py`'s routes, pad scripts and seed 81257)
in a window on a hidden desktop (`hidden_game.py`; headless has no Native VU), with
`DHD_NATIVE_VU1=on DHD_VU1_EMULATED=1 DHD_FRAME_CLOCK=1`, Native VU in skip mode as players have
it (no `DHD_EXACT_PROBE`). The game stops at `DHD_STATE_STOP`, which writes no end reports, so a
run stops just past a 3000-field report and the census takes the last one: the menus to field
3100 (title, main menu), the races to 9100 (the menus, rider choice, the bike shop, the load, about
90 s of race). The scenes: the six tracks a fresh save offers (Italy FR, MX, TD: track IDs 0, 6,
24, `race1`-`race3`; Utah FR, MX, TD: 1, 7, 13, `race4`-`race6`), track 0's replay
(`DHD_REPLAY_TEST=20` with `DHD_STEP_CLOCK=1`, as `replay_check.py` runs it; without the step
clock the replay does not start) and the 2-player split screen. The runner is a scratch script
around `hidden_game.Game`; the recipe above is all it does.

Not a scene of its own: OPTIONS > POSTERS (with Unlock All) draws no translated batch past field
3000, so it prints no report; the Poster Maker is the race's paused scene (finding 250), which the
races cover. Not reachable by the test inputs: the other mountains' tracks (locked on a fresh save
without the cheat code), the Mosh Bowl and online races (they draw the same overlay B programs).

## The census

Runs on the emulated VU with Native VU on, all kinds / those that kick (draw) / those that kick
nothing (setup):

| Scene | Before (ec9fceda) | After (d85bc7a7) |
|---|---|---|
| menus | 1,287,154 / 368,177 / 918,977 | 937,995 / 19,017 / 918,978 |
| race1 (Italy FR) | 2,801,221 / 349,419 / 2,451,802 | 2,602,442 / 150,640 / 2,451,802 |
| replay | 2,983,992 / 436,376 / 2,547,616 | 2,766,965 / 219,351 / 2,547,614 |
| race2 (Italy MX) | 4,047,201 / 466,400 / 3,580,801 | 3,792,308 / 211,507 / 3,580,801 |
| race3 (Italy TD) | 4,810,167 / 1,029,343 / 3,780,824 | 4,597,913 / 817,256 / 3,780,657 |
| race4 (Utah FR) | 3,050,794 / 543,517 / 2,507,277 | 2,839,955 / 332,678 / 2,507,277 |
| race5 (Utah MX) | 3,254,860 / 559,277 / 2,695,583 | 2,995,833 / 300,340 / 2,695,493 |
| race6 (Utah TD) | 2,236,657 / 512,868 / 1,723,789 | 2,015,561 / 291,772 / 1,723,789 |
| split | 4,068,322 / 482,787 / 3,585,535 | 3,861,545 / 276,010 / 3,585,535 |

The drawing runs left on the emulated VU fell by 52 % (race5) to 95 % (menus). Gone from every
scene: program 0030 (148,000-301,000 runs a scene), 0050 (20,000-26,000 in the races), and the
first batches through 06E0 (VI13 0E0, 20C, 260, 2FE, 616: 3,000-48,000) and 3258 (VI13 616:
9,000-12,000).

What still runs on the emulated VU (after; per scene the runs; "kicks a run" over all scenes):

| Overlay | Program | Entry | VI13 | Kicks a run | menus | race1 | replay | race2 | race3 | race4 | race5 | race6 | split |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| B | 06F0 | 06F0 |  | 0.0 | 350,625 | 575,531 | 506,083 | 1,477,244 | 635,385 | 896,076 | 1,047,655 | 474,492 | 824,029 |
| B | 07F0 | 07F0 |  | 0.0 | 45,584 | 271,832 | 222,599 | 432,069 | 1,002,073 | 168,234 | 220,333 | 70,827 | 381,235 |
| B | 17E0 | 17E0 |  | 0.0 | 17,192 | 188,396 | 214,291 | 596,110 | 445,516 | 335,839 | 304,768 | 370,132 | 277,663 |
| B | 0078 | 0078 |  | 0.0 | 42,761 | 506,110 | 634,507 | 109,657 | 57,479 | 38,288 | 114,103 | 31,656 | 536,912 |
| B | 0040 | 0040 |  | 0.0 | 22,795 | 231,926 | 251,669 | 215,080 | 214,033 | 269,661 | 213,418 | 204,293 | 381,432 |
| B | 1638 | 1638 |  | 0.0 | 11,403 | 182,693 | 162,953 | 137,642 | 307,202 | 72,894 | 63,690 | 59,003 | 452,927 |
| B | 12F0 | 12F0 |  | 0.0 | 14,245 | 122,374 | 139,066 | 144,371 | 115,933 | 146,223 | 143,345 | 111,955 | 300,073 |
| B | 0020 | 0020 |  | 0.0 | 158,260 | 94,591 | 108,815 | 148,130 | 111,510 | 114,066 | 117,505 | 117,208 | 130,356 |
| B | 1050 | 1050 |  | 0.0 | 87,678 | 72,528 | 87,499 | 80,885 | 77,068 | 87,720 | 81,114 | 76,260 | 107,665 |
| B | 28D0 | 28D0 |  | 0.0 | 2,850 | 2,517 | 2,467 | 32,252 | 630,898 | 3,821 | 3,901 | 2,857 | 2,381 |
| B | 28D0 | 1CB0 (MSCNT) | 51C | 1.0 | 2,850 | 2,517 | 2,467 | 32,252 | 630,898 | 3,821 | 3,901 | 2,857 | 2,381 |
| B | 0010 | 0010 |  | 0.0 | 94,024 | 40,630 | 42,224 | 42,078 | 38,961 | 38,091 | 80,294 | 45,964 | 60,241 |
| B | 30A0 | 30A0 |  | 0.0 |  | 59,421 | 59,579 | 59,721 | 53,377 | 50,156 | 52,843 | 46,454 |  |
| B | 1430 | 1430 |  | 0.0 | 19,943 | 42,760 | 49,394 | 30,522 | 35,850 | 36,594 | 28,814 | 30,104 | 80,782 |
| A | 0E10 | 0E10 |  | 2.9 |  | 30,716 | 56,741 | 28,827 | 43,149 | 42,246 | 29,516 | 33,758 | 67,448 |
| A | 2E48 | 2E48 |  | 6.3 |  | 33,406 | 53,486 | 34,060 | 35,656 | 35,257 | 28,416 | 30,670 | 72,382 |
| B | 2C20 | 1CB0 (MSCNT) | 586 | 1.0 |  | 7,832 | 9,580 | 21,633 | 3,448 | 89,188 | 84,152 | 33,552 | 14,472 |
| B | 2C20 | 2C20 |  | 0.0 |  | 7,832 | 9,580 | 21,633 | 3,448 | 89,188 | 84,152 | 33,552 | 14,472 |
| B | 2B10 | 2B10 |  | 0.0 |  | 13,788 | 14,817 | 10 | 3,496 | 113,249 | 100,773 | 7,072 | 10,440 |
| B | 2B10 | 1CB0 (MSCNT) | 564 | 1.0 |  | 13,788 | 14,817 | 10 | 3,496 | 113,249 | 100,773 | 7,072 | 10,440 |
| B | 2C20 | 1040 (MSCNT) | 586 | 1.0 |  | 8,504 | 12,594 | 388 | 394 | 1,135 |  | 130,255 | 17,112 |
| B | 0000 | 0000 |  | 0.0 | 48,438 | 13,029 | 14,223 | 13,568 | 14,070 | 13,757 | 14,080 | 14,617 | 8,090 |
| B | 28D0 | 1040 (MSCNT) | 51C | 1.0 | 14,250 | 5,400 | 5,400 | 30,814 | 60,003 | 10,300 | 11,650 | 13,000 | 2,765 |
| B | 2DC8 | 2DC8 |  | 0.0 |  | 16,787 | 18,684 | 22,184 | 25,729 | 13,340 | 15,408 | 18,135 |  |
| A | 29C0 | 29C0 |  | 1.0 | 660 | 14,622 | 14,290 | 12,240 | 11,448 | 10,288 | 10,662 | 10,250 | 31,828 |
| A | 29C0 | 2CE8 (MSCNT) |  | 1.0 | 660 | 14,622 | 14,290 | 12,240 | 11,448 | 10,288 | 10,662 | 10,250 | 31,828 |
| A | 02D0 | 02D0 |  | 1.1 | 596 | 12,019 | 25,011 | 13,756 | 8,075 | 7,213 | 11,818 | 6,360 | 19,737 |
| A | 0000 | 0000 |  | 0.0 | 330 | 7,626 | 7,612 | 6,865 | 6,048 | 5,700 | 6,421 | 5,661 | 16,424 |
| A | 26D8 | 26D8 |  | 18.0 |  | 4,102 | 6,828 | 4,083 | 3,129 | 3,388 | 4,219 | 4,263 | 4,552 |
| B | 0C20 | 0C20 |  | 0.0 | 2,850 | 1,277 | 1,357 | 1,635 | 1,919 | 12,694 | 2,445 | 3,199 | 164 |
| A | 1240 | 1240 |  | 1.1 |  | 2,917 | 3,667 | 4,956 | 4,551 | 3,766 | 4,460 | 2,370 | 522 |
| B | 2738 | 1CB0 (MSCNT) | 4E9 | 1.0 |  | 112 | 112 | 8,818 | 630 | 1,686 | 109 | 53 |  |
| B | 2738 | 2738 |  | 0.0 |  | 112 | 112 | 8,818 | 630 | 1,686 | 109 | 53 |  |
| B | 2738 | 1040 (MSCNT) | 4E9 | 1.0 |  |  |  | 7,428 | 28 |  |  |  |  |
| B | 2B10 | 1040 (MSCNT) | 564 | 1.0 |  |  |  |  |  | 395 |  | 6,401 |  |
| A | 1010 | 1010 |  | 1.0 |  | 81 | 66 |  | 901 | 393 |  | 659 | 541 |
| B | 0A90 | 0A90 |  | 0.0 |  | 42 | 83 | 327 | 32 |  | 322 | 295 | 249 |
| A | 1460 | 1460 |  | 3.4 |  |  |  |  |  | 63 |  |  |  |
| B | 1638 | 1CB0 (MSCNT) | 2C9 | 1.0 | 1 | 2 | 2 | 2 | 2 | 2 | 2 | 2 | 2 |
| | | | | **all** | **937,995** | **2,602,442** | **2,766,965** | **3,792,308** | **4,597,913** | **2,839,955** | **2,995,833** | **2,015,561** | **3,861,545** |

Overlay A 1460 (63 runs on Utah FR) is a kind finding 253's survey did not see. The translated
loops' batches drawn natively in the same runs (and, in brackets, any that ran on the VU):

| Loop | menus | race1 | replay | race2 | race3 | race4 | race5 | race6 | split |
|---|---|---|---|---|---|---|---|---|---|
| 0170 | 2,323,298 | 3,618,530 | 4,457,250 | 980,638 | 908,119 | 1,096,385 | 1,557,938 | 1,067,545 | 3,373,517 |
| 1708 | 199,523 (1 on the VU) | 904,278 (2 on the VU) | 807,688 (2 on the VU) | 774,501 (2 on the VU) | 1,766,484 (2 on the VU) | 1,342,810 (2 on the VU) | 1,302,059 (2 on the VU) | 1,292,377 (2 on the VU) | 2,048,486 (2 on the VU) |
| 08B0 | 74,074 | 786,272 | 668,540 | 1,062,997 | 2,847,140 | 425,111 | 696,300 | 294,599 | 1,131,440 |
| 0798 | 2,313,739 | 1,171,868 | 1,109,149 | 4,643,785 | 1,491,579 | 2,793,809 | 3,165,485 | 1,787,313 | 1,320,538 |
| 18B0 | 65,894 | 819,863 | 942,289 | 2,868,338 | 2,071,859 | 1,851,750 | 1,706,930 | 3,001,206 | 1,404,764 |
| 1110 | 1,092,928 | 557,713 | 654,445 | 682,070 | 570,210 | 729,162 | 767,076 | 689,250 | 671,664 |
| 13A0 | 31,339 | 825,082 | 922,916 | 1,047,069 | 801,855 | 857,953 | 1,045,495 | 750,050 | 2,050,503 |
| 1510 | 131,054 | 187,570 | 218,265 | 139,691 | 155,303 | 154,902 | 132,005 | 133,628 | 359,349 |
| 3140 | 0 | 430,399 | 430,858 | 432,202 | 389,637 | 368,476 | 386,464 | 343,876 | 0 |
| 0AA0 | 0 | 252 | 498 | 1,962 | 192 | 0 | 1,932 | 7,770 | 1,494 |
| 0C30 | 133,950 | 56,657 | 57,137 | 76,845 | 89,619 | 114,601 | 114,915 | 140,061 | 7,708 |
| 2DE8 | 0 | 213,414 | 207,699 | 341,476 | 435,418 | 139,522 | 197,948 | 243,834 | 0 |

The 1 or 2 batches of 1708 on the VU are the title screen's "join value" strips (finding 253).

## Group 1: the first batch through 06E0 and 3258 (kept, 8a43581d)

The census showed MSCNT runs at 06E0 (after program 0000's pool pass, 05A0) and 3258 (after 30A0's
setup, 3190) with VI13 of translated loops: both are `JR vi13`, the same dispatch as 1CB0, so the
object's first batch entered its loop through a door `mesh_before` did not take. Both doors are
taken now; 06E0's delay slot (`SQ vf25, 190(vi8)`) is made first, as 0590's is. 3258's slot is a
NOP.

Check: `DHD_NATIVE_VU1_CHECK=1` (shadow mode, `DHD_EXACT_PROBE=1`) on race1, both builds. The
added batches add 958,000 checked vertices (0798 +162,000, 1110 +105,000, 13A0 +38,000, 18B0
+110,000, 3140 +543,000) with no new difference: XY, colour and ADC 0 in both builds, Z, fog and
STQ the same counts as before (0170 374, 1110 5,290, 18B0 2,578, 2DE8 66 / 49,273 / 520, all from
batches both builds translate). The clipper's fans: 0 missing, 0 different. Menus: 0798 87.75 ->
89.26 million vertices, 1110 43.75 -> 44.06 million, no difference of any kind.

## Group 2: programs 0030 and 0050 in C (kept, d85bc7a7)

Overlay B's 0030 (`B 0050`) and 0050 only kick the GIF packet the EE uploaded at data 0x6C: 0050
`IADDIU vi8, vi0, 0x6C; XGKICK vi8`, then the end. The run is now C (`packet_program`,
`vu1_mesh.inc`): the same kick, VI8 = 0x6C, TPC 0078, and the flag registers written back as
`ps2_vu1_run` writes them (status 12 bits, MAC 16, clip 24). It runs for every renderer, the exact
one and the headless state trace included, because its output is the VU's to the bit.

Check: with `DHD_NATIVE_VU1_CHECK=1` each run also runs on the VU from the same state, and the
registers, the data memory and the kicked qword are compared: menus 300,722 and race1 173,781
runs, none different. The census runs count it too: 173,000-301,000 runs a scene in C.

## Pictures

`native_vu_pictures.py` at 4x (finding 156's route, eleven fields, shadow mode with the exact Z
probe, so every run has the same game state), worst field a scene (MAE / pixels off by more than 8
/ maximum):

| Scene | Lane's head on, against the base on | Lane's head on, against off |
|---|---|---|
| title | 0.000 / 0.000 % / 0 | 0.004 / 0.002 % / 77 |
| shop | 0.000 / 0.000 % / 0 | 0.005 / 0.005 % / 73 |
| race start | 0.000 / 0.000 % / 1 | 0.004 / 0.006 % / 112 |
| mid-race | 0.000 / 0.000 % / 0 | 0.010 / 0.007 % / 126 |

Against off the numbers are finding 253's head's, in every scene; against the base with Native VU
on one pixel differs by one level (field 6400).

## What blocks the deletion (stage 3)

The emulated VU1 path is not deleted, and the `native_vu` setting stays. Two kinds of reasons:

**Programs that still run on it** (the table above), in the order the brief asks for them:

1. The setup programs that kick nothing: 0.9 to 3.8 million runs a scene, overlay B 06F0, 07F0,
   17E0, 0078, 0040, 1638, 12F0, 0020, 1050, 28D0, 0010, 30A0, 1430, 2C20, 2B10, 0000, 2DC8, 0C20,
   2738, 0A90 and overlay A 0000. Most are `B 1AE0` with their VI13 (the shared setup: two 4 x 4
   matrix products, data 27-30 and 176-179, vf1-vf4, vf13, vf18, vf19, vf31) or a pool pass
   (0000, 0010, 0020, 0040: skinning and lighting, partly native kernels already). The translated
   batches read what they leave, so they must run, as C or on the VU. They are already compiled
   blocks (`vu1_aot.py`); a C version must keep every register, flag and memory word the VU
   leaves, as group 2 does, with the same check.
2. Overlay B's four quad loops, VI13 51C (28E0), 586 (2C30), 564 (2B20) and 4E9 (2748): up to
   691,000 batches a scene (51C on Italy TD). Each input vertex gives two output vertices offset
   sideways in screen space by its w times 277 (times Q), each pair of inputs a quad (ADC on its
   first two vertices), a quad with any vertex off the guard band dropped by ADC, colour `c vf31`
   clamped, ST pairs from one UV qword. This fits the mesh item (the offset is `x -+ 277 w` in clip
   space) but needs its own layout (six qwords a pair of outputs), ST mode and culling rule in
   `mesh_build` and the check.
3. Overlay A's eight drawing programs (particles and effects: 02D0, 0E10, 1010, 1240, 1460, 26D8,
   29C0 with 2CE8, 2E48; 1,800 instructions, 16 kick sites), up to 72,000 runs a scene. Each
   needs its own translation; 0E10 and 2E48 already have native kernels inside the VU run.
4. 1708's two "join value" strips of the title screen (finding 253's item 1).

**Uses of the emulated VU that no translation removes**, each a decision before a deletion:

5. The Exact renderer is a player setting (`renderer = exact`, REVIVED SETTINGS). It draws GS words
   only: with it `gs_mesh_mode` stays 0 (`native_core.inc` sets it, the exact front end never
   does) and every VU1 program runs on the VU. The brief says the exact front end must keep
   working, so it needs the VU's kicks of every program, translated or not.
6. VU1 output feeds game state: the lens flare's Z probe reads the depth the VU1 batches draw
   (native depth for players; the GS stream with `DHD_EXACT_PROBE`, finding 097). The state
   traces (headless, `DHD_EXACT_PROBE=1`) and the picture check therefore run the VU's own kicks
   (shadow mode). Without the emulated VU they lose their reference.
7. The PCSX2 referee replays GS streams (`DHD_GS_CAPTURE` dumps), which hold GIF transfers. A
   translated batch is a mesh item, not GS words, so a capture made with Native VU on in skip
   mode lacks those batches; referee captures need the VU's kicks (Native VU off, shadow mode or
   the exact renderer).
8. The GPU fallback: a device that refuses the mesh draws (`dhdr_native_meshes`,
   `gpu_rust_shared.inc`) and the native renderer without indexed fields run every loop on the
   VU. Finding 253 item 4.

So with this design the emulated VU1 (the interpreter, the compiled blocks and the native
kernels in `vu0_interp.c`) is the reference implementation, not a legacy path: the exact
renderer, the state trace and the referee need its kicks. Deleting it needs either C versions of
every drawing program that kick the VU's exact words (what the compiled blocks already are) or a
decision to drop the exact renderer and the GS-stream references. What can go once the four
groups above are translated is the `native_vu` setting's off (Native VU always on where the
renderer can), not the VU code.

## Decisions

- 2026-11-16: a translation whose output is the VU's to the bit (group 2) runs for every renderer,
  not only with Native VU on: one code path, and the headless state trace then proves it too.
- 2026-11-16: the census runs Native VU in skip mode, as players have it; the checks run shadow
  mode, which is what `DHD_NATIVE_VU1_CHECK` needs.
- 2026-11-16: overlay B's quad loops were not started in this lane: each is a new item layout with
  its own check, the same size of work as one of findings 164/167's groups, and stage 3 is blocked
  by items 5-8 above whatever they cost.

## Surprises

- The first batch of each object after a pool pass or 30A0's setup (shadows) never went native,
  in every scene (6,500-48,000 a scene, 0.3-3 % of those loops' batches in a race, 1.5 % of 0798's and
  1.3 % of 1110's in the menus): it enters its loop through 06E0 or 3258, not 1CB0. Finding
  253's survey listed them as "programs 0000 and 30A0" because its counter kept VI13 only at 1040,
  1CB0 and 12E0.
- `DHD_REPLAY_TEST` does nothing on the frame clock without `DHD_STEP_CLOCK=1` (the recording grid
  is off): the first replay run had exactly race1's counts.
- Overlay A 1460 runs on Utah FR only (63 runs).

## Result

Stage 1 and two groups of stage 2 are done; stage 3 is not (the list above says why).

- The census of nine scenes is the yardstick above, before and after.
- Drawing runs left on the emulated VU with Native VU on: menus 368,177 -> 19,017, Italy FR's race
  349,419 -> 150,640, Italy TD's 1,029,343 -> 817,256 (51C's quads are most of what is left),
  split screen 482,787 -> 276,010. The setup runs did not change (0.9-3.8 million a scene).
- Checks: `check.py quick`, `port` and `docs` OK; boot (600 rows) and race (5000 rows) state traces
  equal to e9bba855's (`state_diff.py`: PASS); `DHD_NATIVE_VU1_CHECK=1` and the pictures as above.
- Not run: phones, the Mac, the browser, timing. Nothing per GPU changed and no shader changed;
  the phones draw more batches as mesh items (the first batch of lit, skinned and shadow objects),
  through the same pipelines.

Next, in order: overlay B's quad loops (51C first), then a decision on items 5-8 before any work
on the setup programs or overlay A is worth it for the deletion.
