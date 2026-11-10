# 195: the phone's biggest remaining costs, measured and cut

Status: in progress

Branch `perf/phone-fable` from `main` f06ffba, started 2026-11-10; `main` 959c5f4 merged into it
(`48de78a`), which is the base every change is measured against. This file is the lane's plan
while it runs and its finding when it merges. Someone who has only this file and the git log must
be able to pick the lane up.

## Goal and acceptance

The owner's minimum target on Android is 60 fps at 2x on a Redmi 9 (Helio G80, Mali-G52). The
Redmi 9 is not attached, so this lane measures on the Mi 11i (Snapdragon 888, Adreno 660) with the
stand-ins earlier findings validated:

- the Mi 11i throttled to the t612 profile at 2x (`port/tools/phone_throttle.py --apply t612
  --big-max 1209600 --little-max 1497600`, findings 146-149, 172, 194): the CPU-bound stand-in,
  "t612 2x" below;
- the Mi 11i at 4x on the profile's own clocks (`--apply t612`: 1.44 / 1.80 GHz, the Adreno held at
  315 MHz; findings 174, 183): the GPU-bound stand-in, "4x" below.

The work: a fresh profile of `main` in a race (start and middle) on both stand-ins, with Native VU
off (what players get) and on; the costs ranked; the biggest ones that can be fixed, fixed; each
gain proven by timed runs (uncapped, the screen on, other apps closed, before/after/before
interleaved).

Acceptance:

- Each change keeps the game's state: boot and race state traces equal to `main`'s.
- The picture stays correct (identical where the change is meant to be invisible).
- Each kept change has a measured gain on a named stand-in, Native VU off and on.
- `python tools/check.py port` passes; the release APK builds.

Out of scope: gameplay, the desktop builds' speed (kept equal or better), anything findings 168 and
172 dropped, unless a new idea differs from what they tried (said where it does).

## Progress

- [x] 2026-11-10: finding reserved (`ee14da6`)
- [x] 2026-11-10: the base: `main` f06ffba as a release-signed APK (the shipped builds' PGO profile,
  line tables), the Windows build, its boot and race state traces (equal to the shared reference of
  cdee456 in every row) and its pictures at 2x and 4x, Native VU off and on
- [x] 2026-11-10: the base timed on both stand-ins, off and on ("The base")
- [x] 2026-11-10: the two drops traced without the phone: finding 172's constants step priced by
  Arm's offline compiler ("Surprises", 1), the shipped PGO profile's keys ("Surprises", 2)
- [x] change 1, `140a7d2`: the draw constants a uniform block again. Written before the pause;
  built, checked and timed after it (the entries below, "Change 1").
- [x] change 2, `3ec3717` and `b3d0959`: PGO keys by file name, a profile keyed by paths refused.
  Recorded and timed after the pause ("Change 2").
- [x] 2026-11-10: one diagnostics run of the base (t612 2x off: `DHD_RSTAT`, `DHD_GPU_TIMES`,
  `DHD_VU1_PROFILE`, the compositor's layer dump). Read: the compositor ("Surprises", 5).
- [x] **paused 2026-11-10 05:11** on the lead's word (the owner's usage limit). The phone as it was
  found: main's release APK (cdee456) back on with `adb install -r`, the throttle restored
  (`phone_throttle.py --restore`: every saved value read back, cpus 0-7 online), the app's dhd.ini
  byte-identical to the copy taken first (md5 41477ea7...), no run files left in the app's folder,
  the phone lock removed.
- [x] 2026-11-10 08:45: resumed on the lead's word. `main` 959c5f4 merged (`48de78a`: finding 196,
  bilinear sprites no longer snapped when upscaled, which changes pictures and the core), so the
  comparison base from here on is `main` 959c5f4 ("base2": its own APK, Windows build, traces and
  pictures, built in a scratch worktree of it); the first base's numbers above stay as measured.
- [x] 2026-11-10: base2 and change 1 ("c1": the branch at `48de78a`) built, APK and Windows, both
  with the shipped profile. Their 62 C objects hold the same code (41 identical files, 21 identical
  once the debug sections, which name the build folder, are stripped), so the two APKs differ in the
  Rust library alone, and change 2's compiler option changes nothing while the shipped profile is
  used (its static keys match in neither build).
- [x] 2026-11-10: change 1 checked on the PC against base2: boot (600) and race (5000) state traces
  equal in every row; the picture route's eleven fields identical at 2x and 4x, Native VU off and
  on, on Direct3D 12, and at 2x off and on on Vulkan; the renderer crate's tests pass (`rustgs.py
  test`, every stage through Arm's compiler for the Mali-G52).
- [x] 2026-11-10: change 1 timed at 4x, Native VU off (base2, c1, base2, c1): the route 55.5 ->
  61.5 fields/s (+10.9 %), "Change 1" below.
- [x] 2026-11-10: change 1 timed in the other three cells: 4x on +11.7 %, t612 2x off +0.3 %,
  t612 2x on -1.6 % (the binds, on the render thread, which limits there).
- [x] 2026-11-10: the first base's APK (`main` f06ffba) run again at t612 2x off: 64.5 fields/s
  over the route, not the morning's 56.9. The morning's runs were disturbed ("Surprises", 7), so
  "The base" below is base2's.
- [x] 2026-11-10: change 2's profile recorded on the phone (two unthrottled routes with sound on
  the profile-collecting APK, off and on, merged 2 : 1; 62,266 functions, 33,520 static, every
  static keyed by its file's name); `b3d0959`: `build_host64.py` refuses a profile keyed by
  paths, `docs/BUILDING.md` has the recording recipe. The APK with it ("p2") passes the release
  check.
- [x] 2026-11-10: change 2 timed at t612 2x against the same code with the shipped profile (c1,
  p2, c1, p2): off +4.2 % over the route (+4.9 % in the race), on +0.4 % (the render thread, which
  is Rust and takes no profile, limits there). "Change 2" below.
- [x] 2026-11-10: change 3, `9e87e9d`: a large picture's encoder cut once and its first part
  finished on a helper thread. Checked on the PC against base2 (boot and race traces equal, the
  picture route identical at 2x and 4x, off and on, on Direct3D 12 and at 2x on Vulkan; the cut is
  taken in every race picture); `check.py port` passes on it.
- [x] 2026-11-10: change 3 timed on the phone: +3.6 % over the route at t612 2x on, but -4.9 % at
  the race start; nothing off. Its two parts timed apart (temporary timers): the helper had a third
  of the work, because the cost is in the short passes after the scene, not in the draws.
- [x] 2026-11-10: change 3 reworked, `bf60d5e`: the cut where a pass ends, at 60 % of the picture's
  work with a pass weighed as 27 draws. PC checks as before. Timed (off, on, off, on): +7.4 % over
  the route at t612 2x on, +11.1 % in the race, +3.4 % at the race start.
- [x] 2026-11-10: why the race start gains least: the render thread sleeps in `queue.submit`, in
  the Adreno kernel driver's queue for the context (simpleperf with off-CPU time; "Change 3").
- [x] 2026-11-10: `147ae46`: `phone_throttle.py --restore` accepts a thermal cap that came on during
  the throttle ("Surprises", 8).
- [ ] change 4: the conversions that run back to back in one compute pass (10 -> 4 compute passes a
  race picture). Written; PC checks pass (traces equal, pictures identical). Phone timing: running.

## Method

- **The phone**: Mi 11i (M2012K11G, SM8350, Android 13), rooted, the owner's game data on it. Every
  APK is release-signed and goes on with `adb install -r`; the app is never uninstalled and its data
  never cleared; the app's dhd.ini is saved first and put back after every run (md5 checked).
- **The runner** (scratch, not committed; lane 194's, which is findings 147-172's): writes the run's
  `DHD_*` keys into the app's dhd.ini by root, closes third-party apps, cools 60 s with the screen
  on, starts the app, samples the clocks and the battery temperature every 3 s, pulls dhd.log and the
  `DHD_TIMELINE` CSV. Keys: finding 156's route (`bench_race.py`'s race scene: its NAV, RACE_PAD,
  `DHD_RACE_SEED=81257`), `DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1 DHD_RENDER=native
  DHD_WIDESCREEN=0 DHD_ASPECT=4:3 DHD_SKIP_MOVIES=1 DHD_MEMCARD=0 DHD_SND_OFF=1
  DHD_BENCH=300-14000 DHD_BENCH_EXIT=1`, `DHD_SCALE=2` or `4`, Native VU through
  `DHD_NATIVE_VU1=on`.
- **Scenes** (finding 156's field windows; with the frame clock every run draws the same fields):
  title 450-1100, shop 2950-3750, race start 6400-7300 (GO is field 6321), mid-race 9000-14000; and
  the race 6321-14000 and the route 300-14000. Columns as `route_profile.py`'s: work = a thread's CPU
  minus its measured spin; busy = the field minus the thread's measured waits; GPU = the span of the
  timed frames.
- **APKs**: `build_apk.py --release --line-tables --pgo-use <profile>` (the release recipe with
  symbols kept beside it). The clocks sat at the profile's caps in every run (cpus 0-5 online, the
  A78s at 1.21 or 1.44 GHz, the A55s at 1.50 or 1.80 GHz, the GPU at 315 MHz).

## The base

`main` 959c5f4 ("base2"), two runs of each cell, interleaved with change 1's (battery 27-33 C).
Fields/s and ms a field:

| scene | run | fields/s | field | game work / busy | VU work / busy | GS work / busy | render CPU / busy | present wait | GPU |
|---|---|---:|---:|---|---|---|---|---:|---:|
| race start | t612 2x off | 59.2 | 16.89 | 11.15 / 12.31 | 14.49 / 16.19 | 14.86 / 15.66 | 13.19 / 12.19 | 2.67 | 8.19 |
| race start | t612 2x on | 61.1 | 16.38 | 10.34 / 11.60 | 11.58 / 12.72 | 9.34 / 15.93 | 14.09 / 13.14 | 3.23 | 8.86 |
| race start | 4x off | 41.6 | 24.05 | 11.06 / 9.59 | 13.48 / 14.14 | 15.74 / 24.05 | 13.06 / 16.79 | 7.26 | 21.45 |
| race start | 4x on | 38.8 | 25.79 | 10.50 / 8.97 | 9.30 / 9.12 | 12.21 / 25.79 | 15.08 / 17.31 | 8.48 | 22.29 |
| mid-race | t612 2x off | 73.6 | 13.59 | 9.57 / 10.91 | 11.74 / 13.49 | 11.79 / 12.10 | 10.58 / 9.56 | 1.82 | 6.35 |
| mid-race | t612 2x on | 82.7 | 12.09 | 8.93 / 10.66 | 9.67 / 11.13 | 7.48 / 11.67 | 11.25 / 10.31 | 1.74 | 7.11 |
| mid-race | 4x off | 57.7 | 17.34 | 8.23 / 9.33 | 10.82 / 11.82 | 10.36 / 17.33 | 10.51 / 15.52 | 1.81 | 16.01 |
| mid-race | 4x on | 54.3 | 18.42 | 8.24 / 9.56 | 8.48 / 9.42 | 7.30 / 18.41 | 11.75 / 16.54 | 1.88 | 17.04 |
| the race | t612 2x off | 68.5 | 14.60 | 9.92 / 11.14 | 12.63 / 14.35 | 12.97 / 13.42 | 11.13 / 10.12 | 1.96 | 6.70 |
| the race | t612 2x on | 77.4 | 12.92 | 9.20 / 10.88 | 10.15 / 11.57 | 8.20 / 12.54 | 11.85 / 10.92 | 1.97 | 7.47 |
| the race | 4x off | 54.3 | 18.41 | 8.74 / 9.41 | 11.58 / 12.54 | 11.77 / 18.41 | 10.96 / 15.79 | 2.61 | 16.86 |
| the race | 4x on | 50.8 | 19.67 | 8.64 / 9.57 | 8.79 / 9.60 | 8.40 / 19.66 | 12.38 / 16.82 | 2.84 | 17.89 |
| the route | t612 2x off | 64.4 | 15.53 | | | | | | |
| the route | t612 2x on | 75.4 | 13.26 | | | | | | |
| the route | 4x off | 55.5 | 18.03 | | | | | | |
| the route | 4x on | 52.7 | 18.97 | | | | | | |

What limits, by the busy columns:

- **t612 2x, off: the VU thread** (busy 16.2 of a 16.9 ms field at the race start, 13.5 of 13.6
  mid-race), the GS thread next (15.7 / 12.1), then the render thread (12.2 + 2.7 in the present at
  the race start) and the game (12.3 / 10.9). Four threads within 15 % of each other: a cut in one
  gains little until the next is cut too.
- **t612 2x, on: the render thread** (13.1 busy + 3.2 in the present of a 16.4 ms field at the race
  start, 10.3 + 1.7 of 12.1 mid-race; it never waits for a field, and the GS thread waits behind
  it). The game and the VU thread are next (10.7 and 11.1 of 12.1 mid-race).
- **4x, off and on: the GPU** (21-22 ms a frame at the race start, 16-17 mid-race); the render
  thread's busy time there is the Adreno's submit blocking while the GPU is behind (finding 156), and
  the GS thread waits behind it. Native VU on is slower than off at 4x (52.7 against 55.5 over the
  route): its draws cost the GPU more, and the CPU it saves is not what limits.
- **Against the last measurements**: t612 2x off is where finding 172 left it (64.0 over the route
  on `native/vu-gs`; 64.4 here). 4x off is not: 61.2 over the route and 13.6 ms of GPU mid-race on
  `main` 0574b05 the day before Native VU was merged (finding 183), 55.5 and 16.0 ms here. That drop
  is finding 172's constants step ("Surprises", 1) and change 1 takes all of it back (61.5, 13.9 ms).

The first base (`main` f06ffba, the lane's first morning) measured 56.9 / 67.9 (t612 2x off / on)
and 51.7 / 49.2 (4x) over the route, 8-13 % lower in every cell with every thread's work up alike.
Those runs were disturbed ("Surprises", 7): its APK run again later gave 64.5 at t612 2x off, base2's
64.4. Nothing below uses them.

## Change 1: the draw constants a uniform block again (`140a7d2`)

Finding 172's step (each draw reading its constants out of a 256-slot array by its instance index,
handed to its pixels as a flat varying) is undone: `native.wgsl`'s `nat` is a uniform block at group
0's dynamic offset again, bound again only when a draw's constants differ from the last one's. The
draw constants keep their own arena (256 bytes apart) and layout (minimum size given). Why: Surprises
1. Pictures, state and the crate's tests: Progress.

Timed against base2 (`main` 959c5f4), both APKs built with the shipped profile and holding the same C
code; interleaved (4x: base2, c1, base2, c1; t612 2x: base2, c1, c1, base2, the second pair with
`DHD_RSTAT=1`), two runs each (in brackets), 60 s cooling, the clocks at the profile's caps in every
run. Fields/s, and the GPU's ms a frame:

| stand-in | Native VU | scene | base2 | change 1 | gain | GPU base2 -> change 1 |
|---|---|---|---:|---:|---:|---|
| 4x | off | race start | 41.6 (41.6 / 41.5) | 47.2 (47.3 / 47.2) | +13.6 % | 21.45 -> 18.00 |
| 4x | off | mid-race | 57.7 (57.7 / 57.7) | 64.8 (64.9 / 64.8) | +12.4 % | 16.01 -> 13.93 |
| 4x | off | the race | 54.3 (54.3 / 54.3) | 61.0 (61.0 / 61.0) | +12.4 % | 16.86 -> 14.59 |
| 4x | off | the route | 55.5 (55.5 / 55.4) | 61.5 (61.6 / 61.5) | **+10.9 %** | 15.64 -> 13.55 |
| 4x | on | race start | 38.8 (38.7 / 38.8) | 43.2 (43.1 / 43.4) | +11.3 % | 22.29 -> 18.76 |
| 4x | on | mid-race | 54.3 (54.2 / 54.4) | 61.0 (61.0 / 61.0) | +12.3 % | 17.04 -> 14.88 |
| 4x | on | the race | 50.8 (50.8 / 50.9) | 57.0 (57.0 / 57.1) | +12.2 % | 17.89 -> 15.53 |
| 4x | on | the route | 52.7 (52.7 / 52.8) | 58.9 (58.9 / 58.9) | **+11.7 %** | 16.63 -> 14.52 |
| t612 2x | off | race start | 59.2 (59.3 / 59.1) | 59.3 (59.4 / 59.3) | +0.2 % | 8.19 -> 7.14 |
| t612 2x | off | mid-race | 73.6 (73.4 / 73.7) | 73.7 (73.8 / 73.6) | +0.1 % | 6.35 -> 5.71 |
| t612 2x | off | the race | 68.5 (68.4 / 68.6) | 68.8 (68.8 / 68.8) | +0.4 % | 6.70 -> 5.98 |
| t612 2x | off | the route | 64.4 (64.4 / 64.4) | 64.6 (64.7 / 64.5) | **+0.3 %** | |
| t612 2x | on | race start | 61.1 (61.6 / 60.5) | 60.7 (61.6 / 59.9) | -0.7 % | 8.86 -> 7.77 |
| t612 2x | on | mid-race | 82.7 (83.4 / 82.1) | 80.7 (80.8 / 80.5) | -2.4 % | 7.11 -> 6.39 |
| t612 2x | on | the race | 77.4 (77.9 / 76.9) | 75.6 (76.0 / 75.3) | -2.3 % | 7.47 -> 6.71 |
| t612 2x | on | the route | 75.4 (75.6 / 75.3) | 74.2 (74.1 / 74.4) | **-1.6 %** | 6.56 -> 5.88 |

- **Where the GPU limits (4x, off and on) the change is worth 11-12 %**: the Adreno pays for the
  indexed constants too, 13-16 % of its frame. 4x off is back where finding 183 had it before Native
  VU was merged (61.2 over the route, 13.6 ms of GPU mid-race; 61.5 and 13.9 now).
- **Where the CPU limits it gains nothing, and with Native VU on it costs 1.6 % over the route**
  (2.3 % in the race): the GPU's frame is 10-13 % shorter there too, but the render thread, which
  limits with Native VU on, pays for the binds. `DHD_RSTAT`, the render thread's ms a picture
  (base2 -> change 1):

  | Native VU | scene | group 0 binds a picture | the picture | `encoder.finish` | render CPU a field |
  |---|---|---|---|---|---|
  | off | race start | 18 -> 251 (of 420 draws) | 11.16 -> 11.35 | 5.17 -> 5.48 | 13.19 -> 13.58 |
  | off | mid-race | 12 -> 157 (of 318) | 8.68 -> 8.78 | 3.99 -> 4.20 | 10.58 -> 10.86 |
  | on | race start | 18 -> 292 (of 452) | 12.15 -> 12.37 | 5.97 -> 6.19 | 14.09 -> 14.52 |
  | on | mid-race | 12 -> 209 (of 358) | 9.53 -> 9.72 | 4.59 -> 4.81 | 11.25 -> 11.59 |

  About 1 us a bind on an A55 at 1.5 GHz, and wgpu-core makes it two Vulkan binds: a group set at
  index 0 is flushed at the next draw together with every group after it (`flush_bindings_helper`
  binds from the lowest index set up), so each constants bind binds the texture's group again.
- **Kept**: the Redmi 9, the target, lost 8 % at 2x to finding 172's step (the GPU limits it there,
  as at 4x here), and Native VU is an experimental setting, off by default. The binds are the next
  thing to cut ("What is left").

Which constants differ between a draw and the one before it, counted on the PC over the same race
(a temporary counter, not committed; copies a picture at the race start, off / on): 252 / 299 in
all. 108 / 99 of them differ in `tex_size` alone (a texture of another size, everything else the
same); 55 / 55 in `flags`, `atst` and `aref` together (an alpha-tested draw after a plain one, or
back); 50 with Native VU on in `flags` and `mesh_offset` (a mesh draw after a plain one, or back);
28 / 25 in `flags` alone. Without `tex_size`, 143 / 199 would be left; without `tex_size`, the wrap
modes, `flags` and the alpha test (all of which a specialised pipeline already has as constants),
40 / 97.

## Change 2: the PGO profile keyed by file name (`3ec3717`, `b3d0959`)

clang's profile keys a static function by its unit's path as the compiler was given it, then its
name, and `-ffile-prefix-map` does not change that (Surprises 2). The shipped profile (recorded
2026-11-04 in another lane's worktree) therefore matched no static function in any build made
elsewhere, and the hot external ones had changed since. `build_host64.py` now compiles both the
profile-collecting and the profile-using build with `-mllvm -static-func-full-module-prefix=false`
(the key is the file's name, the same in every checkout and build folder), and refuses a profile
whose static keys hold a path (`pgo_keys`: it would silently match nothing).

What a build finds in a profile, counted by compiling today's units again with the compiler's
profile warnings on (functions without profile data; functions refused because their control flow
changed):

| unit | shipped profile | the profile recorded here |
|---|---|---|
| `port/runtime/vu0_interp.c` | 781 (`native_clip_run`, `native_finish`, every translated block) | 69 (blocks the route never runs) |
| `port/hle/vif.c` | 13, and `vif_feed` refused | 2 |
| `port/gs/gs.c` | 12 (`vertex_kick`, `load_clut`), and `gs_feed` refused | 3 |
| `port/gs/gpu_rust.c` | 63 (`native_draw_strip`, `native_state`, `native_palette`, ...), 3 refused | 12 (inlined everywhere, or never run) |
| `port/hle/graphics_queue.c` | 21, 1 refused | 14 |
| the module's C (eight units) | 49,138, 105 refused | 16,544 (functions the route never runs) |

Of the new profile's 99 / 299 / 995 hottest functions, a build with the shipped profile had usable
data for 44 / 185 / 741; with the new one, for all.

The profile: the profile-collecting APK (`build_apk.py --release --pgo-generate`, Gradle's signed
output), two unthrottled runs of the route with sound on the Mi 11i, Native VU off and on (99.1 and
115.7 fields/s instrumented), merged with weights 2 : 1; 62,266 functions, 33,520 static. Recipe:
`docs/BUILDING.md`. It is in the lane's scratch folder (`pgo/dhd-195.profdata`), not in the
repository: the release needs it (the shipped file is now refused).

Timed: the branch's code built with the shipped profile (c1) against the same code with the new one
(p2); c1, p2, c1, p2 (on: p2, c1, p2, c1). Fields/s, and the threads' work in ms a field over the
race:

| stand-in | Native VU | scene | shipped profile | new profile | gain |
|---|---|---|---:|---:|---:|
| t612 2x | off | race start | 59.7 (59.7 / 59.7) | 62.1 (62.2 / 62.0) | +4.0 % |
| t612 2x | off | mid-race | 73.3 (73.5 / 73.2) | 77.1 (77.1 / 77.1) | +5.2 % |
| t612 2x | off | the race | 68.7 (68.8 / 68.6) | 72.1 (72.0 / 72.2) | +4.9 % |
| t612 2x | off | the route | 64.6 (64.7 / 64.5) | 67.3 (67.3 / 67.4) | **+4.2 %** |
| t612 2x | on | race start | 61.3 (60.7 / 61.8) | 61.0 (60.7 / 61.3) | -0.5 % |
| t612 2x | on | mid-race | 80.8 (80.3 / 81.2) | 80.1 (80.2 / 80.1) | -0.9 % |
| t612 2x | on | the race | 76.0 (75.6 / 76.5) | 75.3 (75.0 / 75.6) | -0.9 % |
| t612 2x | on | the route | 74.5 (73.9 / 75.0) | 74.8 (74.6 / 75.0) | **+0.4 %** |
| 4x | off | the route | 61.6 | 62.0 | +0.6 % |
| 4x | on | the route | 59.0 | 58.8 | -0.3 % |

| Native VU | thread, work over the race | shipped | new | |
|---|---|---:|---:|---:|
| off | game | 9.99 | 9.63 | -3.6 % |
| off | VU | 12.62 | 11.88 | -5.9 % |
| off | GS | 12.94 | 12.28 | -5.1 % |
| off | render (Rust: no profile) | 11.33 | 11.39 | |
| on | game | 9.22 | 8.94 | -3.0 % |
| on | VU | 10.27 | 9.79 | -4.7 % |
| on | GS | 8.16 | 8.01 | -1.8 % |
| on | render | 12.19 | 12.29 | |

- **Off, where the VU and GS threads limit: +4.2 % over the route, +4.9 % in the race.** Less than
  the +8 to +12 % findings 146-149 measured for a profile against none: the shipped one still held
  the external functions' data.
- **On: nothing** (inside the runs' spread): the three C threads do 2-5 % less work, and the render
  thread, which limits there, is Rust and takes no profile.
- **4x: nothing** (one run each): the GPU limits; the C threads' work falls as at 2x (game 8.78 ->
  8.39, VU 11.55 -> 11.00, GS 11.41 -> 11.02 ms a field over the race, off).

## Change 3: a large picture's encoder cut once (`9e87e9d`, `bf60d5e`)

The render thread's picture at the race start is 11-12 ms on the throttled phone, 5-6 ms of it in
`encoder.finish` (wgpu-core replays the picture's passes into Vulkan commands there) and 2.8-3.0 in
`queue.submit`. wgpu-core 29's `finish` takes only a read lock on the device, so two encoders can be
finished at once. A picture with 96 draw commands or more is now cut once: its encoder so far goes to
a helper thread ("dhd-encode") that finishes it while the render thread records the rest into a
second encoder, and both command buffers are submitted together, in order. `DHD_ENCODE_CUT=N` sets
where (0: never, the picture as before); on Android the helper takes the render thread's affinity
mask (the little cores on a phone with two big ones). Not in the browser (no threads there).

**Where the cut is made.** A race picture is one scene pass of 300 to 400 draws, then about thirty
short passes (the riders' shadow and haze textures: 64 x 64 targets drawn, converted and copied in
turn) with 50 draws between them, and finishing those short passes costs more than the scene.

- First form (`9e87e9d`): inside the scene pass, after 60 % of the picture's draw commands. Timed
  (below), it gave the helper a third of the work, added a pass (the target stored and loaded
  again: 0.3-0.5 ms more GPU a frame) and lost at the race start. Replaced.
- As committed (`bf60d5e`): at the first pass that begins once 60 % of the picture's encoder work is
  behind, a pass's begin counted as 27 draws. No pass is added. The weight: on a build with
  temporary timers (not committed), t612 2x, Native VU on, the two parts' `finish` in ms a picture:

  | weight of a pass | race start: helper / render thread | mid-race: helper / render thread | the route, fields/s |
  |---|---|---|---:|
  | no cut | - / 6.08 | - / 4.95 | 76.0 |
  | 5 | 2.76 / 4.04 | 2.38 / 3.04 | 79.5 |
  | 12 | 3.10 / 3.61 | 2.36 / 3.05 | 80.2 |
  | 27 | 4.24 / 2.58 (waits 0.25) | 2.79 / 2.71 | 81.3 |

Checked on the PC against base2 (both forms): boot and race state traces equal, the picture route
identical at 2x and 4x, Native VU off and on, on Direct3D 12 and at 2x on Vulkan.

Timed on one APK (the branch with the new profile), `DHD_ENCODE_CUT=0` against the default,
interleaved (off, on, off, on). Fields/s:

| form | stand-in | Native VU | scene | no cut | cut | gain |
|---|---|---|---|---:|---:|---:|
| committed | t612 2x | on | race start | 61.5 (61.7 / 61.4) | 63.6 (63.6 / 63.7) | +3.4 % |
| committed | t612 2x | on | mid-race | 81.9 (81.7 / 82.1) | 92.0 (91.8 / 92.1) | +12.3 % |
| committed | t612 2x | on | the race | 76.8 (76.7 / 76.8) | 85.3 (85.1 / 85.4) | +11.1 % |
| committed | t612 2x | on | the route | 75.9 (75.8 / 76.0) | 81.5 (81.3 / 81.7) | **+7.4 %** |
| first | t612 2x | on | race start | 61.0 (61.5 / 60.5) | 58.0 (58.1 / 58.0) | -4.9 % |
| first | t612 2x | on | mid-race | 80.5 (80.8 / 80.3) | 86.5 (86.4 / 86.5) | +7.5 % |
| first | t612 2x | on | the route | 75.2 (75.8 / 74.5) | 77.9 (78.1 / 77.7) | +3.6 % |
| first | t612 2x | off | race start | 61.9 (62.0 / 61.9) | 60.2 (60.3 / 60.1) | -2.7 % |
| first | t612 2x | off | the route | 67.3 (67.3 / 67.3) | 67.2 (67.0 / 67.3) | -0.2 % |

The render thread's picture with the committed form (ms; `DHD_RSTAT` on the build with the timers):
race start 12.32 -> 11.35 (`finish` 6.09 -> 2.84, `queue.submit` 2.79 -> 4.93), mid-race 10.04 ->
8.20 (`finish` 4.96 -> 2.73, `queue.submit` 2.46 -> 2.78).

**Why the race start gains so little: the submit sleeps.** `queue.submit` takes 2.1 ms longer there
with two command buffers, and none of it is CPU. simpleperf with off-CPU time on the render thread
at the race start (t612 2x, Native VU on) shows where it sleeps:

| | no cut | cut |
|---|---:|---:|
| asleep in `queue.submit`, in the Adreno kernel driver's `_wait_for_room_in_context_queue` (`adreno_dispatcher_queue_cmds`) | 64 ms a second (1.1 a picture) | 193 ms a second (3.1 a picture) |
| asleep in the present (binder) | 20 ms a second | 22 ms a second |

The kernel's queue for a GPU context has a fixed number of places (this phone:
`context_drawqueue_size` 50, `inflight` 15, read from its sysfs), and a picture with this many
passes fills it: the submit sleeps until the GPU has worked through enough of the picture's first
commands. With the cut the submit comes 3 ms sooner in the field and brings two more command
buffers, and it sleeps 2 ms longer: the picture sits at that limit. Fewer command buffers a picture
is the thing to try against it ("Change 4").

## Where to resume

Measured: the base (base2), change 1 in all four cells, change 2 at t612 2x off and on. Open, in
this order:

1. **Change 3 on the phone** (p3; `DHD_ENCODE_CUT=0` against the default, `DHD_RSTAT=1`): t612 2x
   on (where the render thread limits), then off, then 4x (no loss allowed there). Kept only with a
   measured gain; else reverted with a commit of its own and listed as dropped.
2. **Change 2 at 4x** (c1 against p2; the GPU limits, so little is expected).
3. **The ranked cost table** on p2 (changes 1 and 2): simpleperf at the race start and mid-race with
   the stage of every sample (`stages.py`), `DHD_RSTAT`, `DHD_GPU_TIMES`, `DHD_VU1_PROFILE`; t612 2x
   off and on, 4x off and on.
4. Before handing back: `python tools/check.py port` on the final tree; boot and race traces against
   `main`'s; the release APK; on the phone main's release APK (`cdee456`) back with `adb install -r`,
   the throttle restored, the app's dhd.ini equal to the first copy, the lock removed.

Scratch (not committed, in the lane's scratch folder): the runner `run195.py` and `session.sh` (a
plan of `put` / `thr` / `run` lines; takes the phone lock, cleans up a killed run first
(`phone_clean.py`, at every session's start and end), restores the throttle and frees the lock on any
exit), `scenes.py` and `rstat.py` (the tables here), `stages.py` (a simpleperf record's samples by
stage), `build_apk.sh`, `build_gen.sh` (the profile-collecting APK: Gradle's signed output, which
`build_apk.py`'s release check rightly refuses), `spv/mali.sh` (SPIR-V of every stage from a WGSL
file, then malioc's cycles for the race's stages), `pgo_applies.py` and `pgo_hot.py` (what a build
finds in a profile; never while a build runs in the same folder). APKs with their `libmain.debug.so`:
base2 (`main` 959c5f4, built in a scratch worktree of it), c1 (the branch at `48de78a`, shipped
profile), p2 (the branch, new profile), p3 (the branch with change 3, new profile); the Windows
builds, state traces and pictures of base2, c1 and c3; the profile `pgo/dhd-195.profdata`.

## What is left

Ranked by what the measurements say it is worth:

1. **The render thread with Native VU on** (it limits there on the CPU-bound stand-in, and change 1
   added 0.3-0.4 ms to it). Fewer constants binds, not the array again: (a) a draw whose pipeline is
   specialised reads neither its flags, wrap modes nor alpha test from the block, and a non-mesh
   draw never reads `mesh_offset`; left as the last block had them, such draws share it (252 / 299
   copies a picture at the race start would become 168 / 168 at best, with every draw specialised,
   without touching a shader; the vertex stage's two flag bits must stay). (b) `tex_size` alone is 108 / 99 more, but asking the
   texture for its size instead costs the pixels (finding 174's bit 19 exists because of that), so
   only with malioc saying it is free. (c) wgpu-core flushes every group after the one that was
   set: the texture's group is bound again with each constants bind. Constants in the last group
   would save that second bind but make every texture bind (379 a picture) rebind the constants, so
   no order of the two is free; one group holding both would be one bind a draw.
2. **Rust PGO for the renderer library** (the render thread is 11-14 ms of CPU a field at the race
   start and no profile reaches it; the C threads got 4-6 % from theirs).
3. **The GS thread's two loops** (`packed_strip` 24 % and `native_draw_strip` 21 % of its samples at
   the race start: a GsVertex list written by the first and read by the second), and its palette
   path (`load_clut`, `gs_palette`, `native_palette`: 11 %; the game alternates textures with
   different CLUTs, and the one-entry memos of finding 148 miss then). The GS thread is the second
   limit with Native VU off.

## Decisions

- **Change 1 goes back to finding 168's form instead of keeping 172's and trimming it.** The cost
  is in reading the constants through a varying index at all (they leave the uniform registers,
  and what the driver computes once a draw is computed every pixel); finding 172's own lazy-reads
  variant, which read fewer fields through the same index, got none of it back.
- **The draw constants keep their own arena and layout** (finding 172's), 256 bytes apart, the
  layout's minimum size given: no draw waits for wgpu's late size check.
- **PGO keys by file name, not by a path relative to the checkout**: the compiles run in the
  build's work folder with absolute paths, and the module's C lies in that folder; one compiler
  option covers both. Two host units share a name (`port/gs/memory.c`, `port/runtime/memory.c`): a
  record is found by name and control-flow hash, so two statics of one name there keep their data.
- **Each change is to be timed alone**: change 1 against the base with the shipped profile in
  both, the new profile against the shipped one on the same code, the encoder cut against itself
  switched off on one APK.
- **Change 1 is kept although it costs 1.6 % with Native VU on at t612 2x.** It is worth 11-12 %
  wherever the GPU limits, which is the Redmi 9 at 2x (finding 172's 8 %) and every phone at a
  higher scale; the cell it costs is an experimental setting on the stand-in whose GPU does not
  limit, and the cost has a known cause (the binds) with its own next step.
- **The new profile is not committed** (no profile ever was: it is derived from the game's code and
  a run of the game). It is in the lane's scratch folder for the lead; the release must be built
  with it, or with one recorded the same way after the last C change before a release.
- **The profile is recorded on the Mi 11i unthrottled**, as the shipped one was: the counters say
  which paths run and how often, not how long they take, so the phone's speed does not matter; both
  Native VU settings are in it (2 : 1 for the default).
- **The encoder is cut once, not into three**: the helper would need a second thread to gain from
  two cuts, each cut is one more render pass (a store and a load of the target on a tile GPU), and
  a phone with two big cores has two spare little ones at most.
- **The cut's place is a share of the picture's draw commands (60 %), not of its time**: known
  before recording starts, and the first part is the one that runs beside the rest of the
  recording, so it may be the larger.

## Surprises

1. **Finding 172's constants step costs the GPU, and it is in the default path since Native VU was
   merged.** Finding 172 lost 8 % on the Redmi 9 at 2x after it and left "heat from the run order,
   or a GPU cost of the per-draw lookup" open. Arm's offline compiler (malioc 8.7.1, Mali-G52; the
   stages' SPIR-V as `native_spirv` writes it for a Vulkan 1.1 device; `native.wgsl` of cdee456
   against f06ffba) answers it without the phone. Cycles a pixel or vertex, shortest / longest path:

   | stage | | arithmetic | load/store | varying | uniform registers |
   |---|---|---|---:|---:|---:|
   | `native_pixel_blend_u`, flags 0x31 (textured, fogged, bilinear: finding 174's 53 % of race pixels) | before 172 | 2.58 / 4.25 | 0 | 1.12 | 40 |
   | | with 172 | 2.97 / 4.42 | 2.00 | 1.25 | 8 |
   | the same with the texture's alpha (0x33), `native_pixel_u` too | before 172 | 3.00 / 4.42 | 0 | 1.12 | 40 |
   | | with 172 | 3.42 / 4.87 | 2.00 | 1.25 | 6 |
   | `native_vertex` | before 172 | 1.08 | 11 | | 18 |
   | | with 172 | 1.42 | 16 | | 16 |
   | change 1's shader, every stage above | | as before 172 | | | |
   | `native_mesh_vertex` (Native VU) | with 172 / change 1 | 3.13 / 3.00 | 24 / 20 | | |

   The scene's pixels are arithmetic-bound on the Mali-G52 (finding 174: 90 % ALU utilisation, 9-10
   of a 21 ms frame): +15 % there and +45 % on the vertex stage's loads is the size of the 8 %.
   The Adreno pays the same: at 4x, finding 183 measured 13.6 ms a frame mid-race on `main` the day
   before the merge (the route 61.2 fields/s); base2 has 16.0 (55.5), and change 1, interleaved
   with it, 13.9 (61.5). "Change 1" has the runs.
2. **The shipped PGO profile reaches almost none of the VU and GS threads' code.** A static
   function's key in a profile is its unit's path as the compiler was given it, then its name;
   `-ffile-prefix-map` does not change it (checked with the NDK's clang 21). The shipped profile
   (2026-11-04) holds 33,466 such keys of its 62,139 functions, all under one lane's worktree and
   build folder; 70 of its 120 hottest functions are static. Today's base, compiled again with the
   compiler's profile warnings on (they are off in the build: `-w`), unit by unit:

   | unit | functions without profile data | refused: control flow changed |
   |---|---:|---|
   | `port/runtime/vu0_interp.c` | 781 (`native_clip_run`, `native_finish`, every translated block) | |
   | `port/hle/vif.c` | 13 (`run_micro`, `kick`, the unpack helpers) | `vif_feed` |
   | `port/gs/gs.c` | 12 (`vertex_kick`, `load_clut`) | `gs_feed` |
   | `port/gs/gpu_rust.c` | 63 (`native_draw`, `native_draw_strip`, `native_state`, `native_set`, `native_palette`, `native_decode`, ...) | 3 |
   | `port/hle/graphics_queue.c` | 21 (`idle_wait`, `feed_words`, `consume`, `gs_consume`) | `graphics_timeline` |

   `ps2_vu1_run` still finds its data. So a build made outside that lane's folder never had the
   statics' data, and since the hot external functions changed (`vif_feed`, `gs_feed`), little of
   the VU and GS threads' profile is left. Measured ("Change 2"): a profile that matches is worth
   4.2 % over the route at t612 2x off. It was not the reason for the first base's low numbers, as
   this entry first said: those runs were disturbed (7), and finding 172's builds had the same
   unmatched statics at 64.0.
3. **The phone is a Snapdragon 888** (SM8350, Adreno 660), as `phone_throttle.py` says, not an 870.
4. **Native VU on is slower than off at 4x** (base2: 52.7 against 55.5 fields/s over the route;
   with change 1: 58.9 against 61.5): GPU-bound there, and its frame costs the GPU 1 ms more.
5. **The compositor does not draw the game's layer with the GPU on the Mi 11i.** wgpu-hal asks for
   no pre-rotation (`pre_transform(IDENTITY)`), which on some phones makes the system rotate every
   frame on the GPU; here the layer's composition type is DEVICE (the display hardware), and the
   Redmi 9's counters in finding 174 show one full-screen pass a frame, so not there either. No
   gain to look for; dropped.
6. **A phone run that is killed leaves the phone dirty.** The runner puts the app's dhd.ini back in
   a `finally` and the session restores the throttle in a trap; when the session itself was cancelled
   (2026-11-10 08:53, mid-run) neither ran to the end: the bench keys stayed in the app's dhd.ini (a
   reinstall with `adb install -r` keeps the app's data), three recordings and a timeline stayed in
   its folders, and `phone_throttle.py` kept its saved state ("RESTORE INCOMPLETE"). Found and put
   right at the next start (dhd.ini back to the first copy, md5 equal; `--restore` printed RESTORED).
   The session script now looks for a killed run's keys before it backs anything up, so they can
   never become "the original", and runs the clean-up at every session's start and end.
7. **The first morning's runs were 8-13 % slow in every cell, and not because of the code.** `main`
   f06ffba measured 56.9 fields/s over the route at t612 2x off then, and 64.5 when its APK was run
   again at midday between base2's runs (64.4): the same APK, clocks and battery temperature, every
   thread's work 13-15 % up in the morning. A screen-mirroring server (scrcpy's) was running on the
   phone when the lane paused at 05:11 and was gone at the resume; it encodes the screen on the
   same cores, and is the likely cause (not shown: it was not started again to prove it). Lane
   194's 57.2 for the same cell the night before may be the same thing. So: before a timed batch, look at the phone's process list for anything of the
   owner's or another session's that draws or encodes (`ps -A | grep -E "scrcpy|app_process"`), and
   compare against a base measured in the same batch, never against a number from another day.
8. **`phone_throttle.py --restore` can report INCOMPLETE on a warm phone**: the phone's own thermal
   cap had lowered policy7's maximum since the apply, and the tool compares every value it saved.
   That cap came back within seconds, and the session script tries the restore again (up to twelve
   times, 15 s apart). After the 4x batch in the afternoon it was policy4 (the A78s: cooling device
   `thermal-cpufreq-4` at state 3, 2.11 of 2.42 GHz), and MIUI kept it there for more than ten
   minutes on an idle phone at 30 C; the restore failed twelve times and the next batch stopped at
   "a throttle is already applied". The user request had been written back each time: the phone was
   in its own state, only the tool could not say so. `147ae46`: it now accepts a maximum below the
   saved one while that policy's cooling device is engaged and the value is not the throttle's own,
   and names the cap. The cap is above every throttled clock, so no timed run was touched by it.
9. **A background job of the harness is killed at two hours.** A chain of phone plans longer than
   that would be killed mid-run (6); chains are kept under 100 minutes, and a `STOP` file ends one
   between two plans.

## Result
