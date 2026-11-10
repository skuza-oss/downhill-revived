# 195: the phone's biggest remaining costs, measured and cut

Status: in progress

Branch `perf/phone-fable` from `main` f06ffba, started 2026-11-10. This file is the lane's plan
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
- [ ] change 1, `140a7d2`: the draw constants a uniform block again. Written; the Rust library
  builds for Android with it and its stages compile for the Mali-G52 at the cost they had before
  finding 172. **Not yet done**: an APK or Windows build of it (the one APK build failed in its C
  step by this lane's own doing: a diagnostic compile had `vu1_native_clip.inc` open while the
  build wrote it), any run of the game, the pictures and state traces against the base, the
  renderer crate's tests, the phone timing.
- [ ] change 2, `3ec3717`: PGO keys by file name. Written; checked on a two-function unit only.
  **Not yet done**: the profile-collecting APK (its build was stopped at the pause), the recording
  runs on the phone, the merged profile, the APK built with it, the timing against the shipped
  profile.
- [x] 2026-11-10: one diagnostics run of the base (t612 2x off: `DHD_RSTAT`, `DHD_GPU_TIMES`,
  `DHD_VU1_PROFILE`, the compositor's layer dump), 55.3 fields/s with the diagnostics on. Read so far:
  the compositor only ("Surprises", 5); the rest is in the scratch folder, its per-scene parse not
  yet checked against finding 172's numbers.
- [ ] **paused 2026-11-10 05:11** on the lead's word (the owner's usage limit): "Where to resume".
  The phone as it was found: main's release APK (cdee456) back on with `adb install -r`, the
  throttle restored (`phone_throttle.py --restore`: every saved value read back, cpus 0-7 online),
  the app's dhd.ini byte-identical to the copy taken first (md5 41477ea7...), no run files left
  in the app's folder, the phone lock removed.
- [x] 2026-11-10 08:45: resumed on the lead's word. `main` 959c5f4 merged (`48de78a`: finding 196,
  bilinear sprites no longer snapped when upscaled, which changes pictures and the core), so the
  comparison base from here on is `main` 959c5f4 ("base2": its own APK, Windows build, traces and
  pictures, built in a scratch worktree of it); the first base's numbers above stay as measured.
- [x] 2026-11-10: base2 and change 1 ("c1": the branch at `48de78a`) built, APK and Windows, both
  with the shipped profile. Their 62 C objects hold the same code (41 identical files, 21 identical
  once the debug sections, which name the build folder, are stripped), so the two APKs differ in the
  Rust library alone, and change 2's compiler option changes nothing while the shipped profile is
  used (its static keys match in neither build).

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

`main` f06ffba, one run each, in this order: t612 2x off, on, then 4x off, on (battery 26-31 C).
Fields/s and ms a field:

| scene | run | fields/s | field | game work / busy | VU work / busy | GS work / busy | render CPU / busy | present wait | GPU |
|---|---|---:|---:|---|---|---|---|---:|---:|
| race start | t612 2x off | 49.1 | 20.35 | 13.09 / 14.71 | 16.67 / 19.22 | 15.76 / 16.44 | 13.38 / 14.00 | 2.66 | 9.35 |
| race start | t612 2x on | 57.7 | 17.32 | 11.61 / 13.62 | 12.57 / 14.54 | 9.72 / 14.76 | 13.91 / 13.78 | 3.44 | 10.10 |
| race start | 4x off | 37.5 | 26.68 | 11.15 / 9.61 | 13.78 / 14.61 | 15.94 / 26.67 | 13.42 / 19.38 | 7.30 | 24.19 |
| race start | 4x on | 35.3 | 28.32 | 10.14 / 9.03 | 9.06 / 8.83 | 11.38 / 28.32 | 14.95 / 21.13 | 7.19 | 24.91 |
| mid-race | t612 2x off | 62.9 | 15.91 | 11.01 / 12.71 | 13.47 / 15.80 | 12.37 / 12.43 | 10.74 / 9.80 | 1.88 | 6.86 |
| mid-race | t612 2x on | 73.2 | 13.67 | 10.98 / 13.02 | 10.95 / 12.98 | 8.42 / 8.21 | 11.17 / 10.33 | 1.86 | 7.64 |
| mid-race | 4x off | 53.5 | 18.68 | 8.74 / 10.14 | 11.39 / 12.76 | 10.48 / 18.64 | 10.83 / 16.80 | 1.86 | 16.66 |
| mid-race | 4x on | 49.7 | 20.10 | 8.41 / 9.95 | 8.67 / 9.71 | 7.41 / 20.10 | 11.84 / 18.09 | 2.01 | 17.66 |
| the race | t612 2x off | 59.1 | 16.93 | 11.43 / 13.10 | 14.30 / 16.69 | 13.56 / 13.81 | 11.27 / 10.55 | 2.01 | 7.29 |
| the race | t612 2x on | 69.6 | 14.37 | 11.14 / 13.22 | 11.33 / 13.40 | 9.02 / 9.65 | 11.75 / 11.03 | 2.10 | 8.09 |
| the race | 4x off | 50.1 | 19.94 | 9.19 / 10.12 | 12.16 / 13.49 | 11.94 / 19.92 | 11.32 / 17.27 | 2.67 | 17.77 |
| the race | 4x on | 46.6 | 21.48 | 8.75 / 9.90 | 8.90 / 9.79 | 8.40 / 21.48 | 12.44 / 18.73 | 2.75 | 18.79 |
| the route | t612 2x off | 56.9 | 17.56 | | | | | | |
| the route | t612 2x on | 67.9 | 14.73 | | | | | | |
| the route | 4x off | 51.7 | 19.35 | | | | | | |
| the route | 4x on | 49.2 | 20.31 | | | | | | |

What limits, by the busy columns:

- **t612 2x, off: the VU thread** (busy 19.2 of a 20.4 ms field at the race start, 15.8 of 15.9
  mid-race), the GS thread next (16.4 / 12.4), then the game and the render thread (14.7 and 14.0 +
  2.7 in the present at the race start). Lane 194 measured the same route (56.9 and 57.4).
- **t612 2x, on: the render thread at the race start** (13.8 busy + 3.4 in the present of a 17.3 ms
  field; it never waits for a field), **the game and the VU thread mid-race** (13.0 each of 13.7).
- **4x, off and on: the GPU** (24-25 ms a frame at the race start, 16.7-17.7 mid-race); the render
  thread's busy time there is the Adreno's submit blocking while the GPU is behind (finding 156), and
  the GS thread waits behind it. Native VU on is slower than off at 4x (49.2 against 51.7 over the
  route): its draws cost the GPU more, and the CPU it saves is not what limits.
- **Both stand-ins are slower than the last time they were measured**: t612 2x off 64.0 over the
  route on `native/vu-gs` four days ago (finding 172), 56.9 now; 4x off 61.2 over the route and 13.6 ms
  of GPU mid-race on `main` 0574b05 the day before (finding 183), 51.7 and 16.7 ms now. Both drops are
  traced below (the first two entries of Surprises).

## Where to resume

Nothing is measured for either change yet: the base is, and the two causes are shown off the
phone. Scratch (not committed, in the lane's scratch folder): the runner `run195.py` and
`session.sh` (a plan of `put` / `thr` / `run` lines; takes the phone lock, restores the throttle
and frees the lock on any exit), `scenes.py` (the tables here), `build_apk.sh`, `build_gen.sh` (the
profile-collecting APK: Gradle's signed output, which `build_apk.py`'s release check rightly
refuses), `spv/mali.sh` (SPIR-V of every stage from a WGSL file, then malioc's cycles for the
race's stages), `pgo_applies.py` (recompiles recorded units with the compiler's profile warnings on
and counts the functions without data; never while a build runs in the same folder). The base's APK
with its `libmain.debug.so`, its Windows exe, state traces and pictures (2x and 4x, off and on).

1. **Change 1**: the APK (`build_apk.sh c1`) and the Windows build; `state_trace_run.py` boot and
   race against the base's traces; `native_vu_pictures.py` at 2x and 4x, off and on, against the
   base's with `--same` (and with `DHD_WGPU_BACKEND=vulkan`, the phones' backend); `rustgs.py
   test`. Phone: 4x nominal, off, base / c1 / base / c1 (`b1-n4-off` is the first base run), then
   on; t612 2x off and on with `DHD_RSTAT=1` for the render thread's `finish` (finding 172 measured
   0.3-0.4 ms a picture for the binds on the Mi 11i). If the binds matter there, the next step is
   fewer of them, not the array again: a draw whose pipeline is specialised reads neither its
   flags, CLAMP modes nor alpha test from the block, and a decoded texture's size can come from the
   texture, so most consecutive draws could share a block (count first which words of the block
   differ between draws).
2. **Change 2**: `build_gen.sh gen`, install, two unthrottled recording runs (`run195.py --kind
   pgo`: the route with sound; again with `DHD_NATIVE_VU1=on`), `llvm-profdata merge` (off weighted
   2, on 1), an APK with `--pgo-use` of it, then timed against the same code with the shipped
   profile on t612 2x (off, on) and at 4x. Record it last, after every C change of the lane. If it
   gains what findings 146-149 measured for a matching profile (+8 to +12 %), the release needs the
   new file (the old one matches no static function with the new keys), `docs/BUILDING.md`'s
   PGO paragraph the recipe, and `package_release.py` a check that a profile still matches (the hot
   units' functions without data, as `pgo_applies.py` counts them).
3. **Then the profile** the plan started with (simpleperf at the four scenes, `DHD_RSTAT`,
   `DHD_GPU_TIMES`, `DHD_VU1_PROFILE`), on the build with both changes: the ranked cost table, off
   and on. One candidate noted, not measured: wgpu-core replays a picture's passes inside
   `encoder.finish` under a read lock only, so a picture split over two or three encoders could be
   finished on idle little cores (the render thread limits Native VU on at the race start, and the
   Redmi 9 at 1x).

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
  both, the new profile against the shipped one on the same code.

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
   On the Adreno the same step may be what shows as GPU time: at 4x, finding 183 measured 13.6 ms a
   frame mid-race and 17.4 at the race start on `main` the day before the merge; the base here has
   16.7 and 24.2 (the route 61.2 -> 51.7 fields/s). That comparison is across days and builds: the
   interleaved run of change 1 is what proves or refutes it (not yet run).
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

   `ps2_vu1_run` still finds its data; the module's C (the game thread) is not counted yet. So a
   build made outside that lane's folder never had the statics' data, and since the hot external
   functions changed (`vif_feed`, `gs_feed`), little of the VU and GS threads' profile is left.
   Findings 146-149 measured +8 to +12 % from a matching profile; this is the likely reason the
   t612 2x level fell from finding 172's 64.0 to 56.9 (game and VU work up 13-16 %; the render
   thread, which is Rust and never profiled, unchanged). Not yet measured (change 2).
3. **The phone is a Snapdragon 888** (SM8350, Adreno 660), as `phone_throttle.py` says, not an 870.
4. **Native VU on is slower than off at 4x on the base** (49.2 against 51.7 fields/s over the
   route): GPU-bound there, and the mesh draws' pixels cost a little more.
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
   never become "the original".

## Result
