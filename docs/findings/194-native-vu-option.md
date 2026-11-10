# 194: Native VU (experimental), a Graphics setting on every platform

Status: current
Checked: 2026-11-10

Branch `feat/native-vu-option` from `main` cdee456, started 2026-11-10. This file is the lane's
plan while it runs and its finding when it merges. Someone who has only this file and the git log
must be able to pick the lane up.

## Goal and acceptance

The owner's decision (2026-11-10): ship the native VU1 path (VU1 microprograms run as GPU shaders
instead of being emulated; findings 162, 164, 167, 168, 172, the local branch `native/vu-gs`) as an
experimental option on every platform, **off by default**.

- Bring the native work onto this branch from today's `main`: merge `native/vu-gs` (26 commits on
  `main` 9ecb8e1), keeping what finding 172 kept and leaving out what it dropped. `native/vu-gs`
  itself is not rewritten.
- One setting: **Native VU (experimental)** in REVIVED SETTINGS > Graphics, next to the Performance
  group; a dhd.ini key, default off, in the settings crate, the game's page and the launcher's
  settings, the way finding 183 added its settings. `DHD_NATIVE_VU1` stays as its override (its
  per-program values for testing).
- Safety: a native pipeline or shader a driver refuses (PowerVR, Mali, Intel, WebGPU limits) sends
  that draw path back to the emulated one for the session, with one dhd.log line saying why; the
  game never closes over it. Finding 188's Intel dual-source rule holds in the native path.
- Platforms: Windows fully; macOS on the owner's Mac (build, a harness run with the option on and
  off); Linux build; Android release-signed APK, the phone run with the option on and off; the
  browser built, the option working or hidden there with the reason.

Acceptance:

- Option off: byte-for-byte today's `main`: boot and race state traces equal to `main`'s, the
  picture unchanged.
- Option on: the game's state unchanged (state traces), pictures within finding 164's tolerance
  (per field MAE <= 0.1, at most 0.1 % of pixels off by more than 8) against the emulated path.
- Timing: Windows RTX at 8x, off against on; the Mi 11i on the t612 profile at 2x, off against on
  (the recipes of findings 146-149 and 172).
- `python tools/check.py port` passes; the release APK builds.

## Progress

- [x] 2026-11-10: finding reserved (`36be34c`)
- [x] 2026-11-10: `native/vu-gs` merged (`32df819`): four conflicts, all beside each other (main's
  pass trace fields and the mesh group flag in `render.rs`, the raster scanout stage and the mesh
  vertex stage in `native_spirv.rs`, two docs tables); the native findings keep their numbers (main
  has no 162, 164, 167, 168 or 172). Builds on Windows; core id unchanged by the merge (the VU1
  code is the host's, `vif.c`).
- [x] the setting, live, and the fallback (`053fd86`, `1d2b00a`; below), the picture tool
  (`fe46beb`), the ten-row tab's layout (`3f8818b`)
- [x] pictures: main against off (identical), off against on (finding 164's tolerance), 2x and 4x;
  the fallback; dual-source blending off
- [x] state traces: boot and race, off against main; on (forced skip and shadow) against off
- [x] Windows RTX 8x timing, off against on (interleaved, three off and three on runs)
- [x] macOS: built on the owner's Mac; the picture route there, off and on
- [x] Linux: built (WSL Ubuntu 22.04, the release recipe); headless boot traces
- [x] Android: release APK; the Mi 11i (t612, 2x) off against on, two rounds
- [x] the browser: built, the row hidden
- [x] `check.py port` (every step OK); the renderer crate's own tests, which it does not run
  (three were stale from `native/vu-gs`: `5d7ff18`), with Arm's offline compiler for two Mali cores

## What came over from `native/vu-gs`, and what did not

All 26 commits, by a merge (the branch itself unchanged): finding 162's tools (`route_profile.py`,
`gs_census.py`, `sample_profile.py --stacks`, `DHD_VU1_LAYOUT`), the twelve translated batch loops
(findings 164, 167: 0170, 1708, 08B0, 0798, 18B0, 1110, 13A0, 1510, 3140, 0AA0, 0C30, 2DE8),
finding 168's object records in the vertex buffer and `DHD_RSTAT`'s counters, and finding 172's one
kept step, the draw constants read by instance index (which also changes the GS path's draws: their
constants are one array bound per 256 draws instead of a dynamic offset per draw; the pictures are
identical, finding 172). What finding 172 dropped was never on the branch (kept outside the
repository): the shared pipeline layout, texture arrays, lazy constant reads, generic pipelines.

## The setting

- **dhd.ini `native_vu = on|off`** (`port/render/settings/src/table.rs`, a switch, default and
  built-in `off`), read into `DHD_NATIVE_VU1` as every setting is; `DHD_NATIVE_VU1` set outside the
  game wins (the page shows the row locked, "Set by DHD_NATIVE_VU1 outside the game"). Besides `on`
  and `off` it keeps the native lane's test values: a list of loops by address or group (`prelit`,
  `lit`, `shadow`, `all`), then `:skip`, `:shadow`, `:gpuclip` (`gs_mesh_parse`, `port/gs/gs.c`).
- **REVIVED SETTINGS > Graphics**: "Native VU (Experimental)", the row right above the Performance
  group, hint "VU1 programs run on the GPU: faster, but new" ("Only with the native renderer" with
  `renderer = exact`). On Android's SETTINGS too (`DHD_PC_SETTINGS_ROWS`). The launcher's Graphics
  page has the switch with the same words.
- **Live.** The page sets the variable and calls `gs_present_options_apply` (the host, the game
  thread), which reads it into `gs_mesh_setting` (an atomic word: the loops and flags); the VU
  thread reads that word before every VU1 run (`mesh_enabled`, `port/hle/vu1_mesh.inc`), so a change
  takes effect at the next batch. The VU thread reads no environment: `host_settings_load` reads the
  word first, before the game runs. Turning it at any batch is safe: a skipped batch leaves the VU
  as its program would (finding 164), and translated batches have always mixed with batches run on
  the VU (the near-bound ones, about 15 % of 0170's). One dhd.log line at each change ("native VU1:
  loops ... drawn by the native renderer, the VU skips them", "native VU1: off; the VU runs every
  program"); the `settings:` line names it as every setting.

## The fallback

- **The renderer decides whether it can draw meshes** (`port/render/src/native/init.rs`): the mesh
  layouts are made only where the device has storage buffers for the vertex stage
  (`DownlevelFlags::VERTEX_STORAGE`, at least one storage buffer a stage), inside a validation error
  scope; a refused mesh draw pipeline (`render.rs`'s `set_pipeline`, which already logs and skips a
  refused pipeline, finding 176) and a vertex buffer larger than one storage binding also refuse.
  Each writes one dhd.log line, "gs native: Native VU can't run on this GPU (why); the VU runs
  every program", and sets a flag for the session, `dhdr_native_meshes()` (`dhd_render_ext.h`).
- **The game goes on emulating.** C sets `gs_mesh_mode` from it at the renderer's start
  (`native_install`) and clears it after any picture where the flag says no
  (`gpu_rust_shared.inc`); the VU thread sees the cleared mode at its next batch and runs every
  program itself ("native VU1: Native VU needs the native renderer with indexed fields (or the GPU
  refused its draws); the VU runs every program"). A mesh draw already recorded when the refusal
  came is skipped (its pipeline failed), so at most a picture or two lack those objects.
- **Test:** `DHD_TEST_NO_MESHES=N` refuses at the renderer's N-th picture (0: at its start).
- **The browser** never makes the mesh layouts (no line: by design) and its VU is built without the
  translation; the row is not offered there (`port/web/www/settings.js`).
- **Intel's dual-source rule (finding 188)**: mesh draws choose their blend variant in the same
  `draw_passes` as every draw, so without dual-source blending (Intel, or `DHD_WGPU_DUAL_SOURCE=0`)
  a mesh draw blending on As takes the two-draw form like any other; the mesh entry points of the
  dual-source module exist only when that module is made.

## Correctness

### Pictures (Windows, RTX 3070 on Direct3D 12)

`port/tools/native_vu_pictures.py` (finding 164's method: finding 156's route, the frame clock, the
exact Z probe so every run's game state is the same, `DHD_NATIVE_SPEC=sync`, the native picture of
eleven fields; Native VU then runs in shadow mode). Main is a reference build of `cdee456` made for
this lane (kept in the shared scratch folder with its state traces, for other lanes).

- **Main against this branch, option off**: identical, every pixel of the eleven fields, at 2x and
  at 4x.
- **Option off against on**, worst field of each scene (MAE / % of pixels off by more than 8 / by
  more than 32 / largest difference):

| scene | 2x | 4x |
|---|---|---|
| title (700, 950) | 0.005 / 0.003 % / 0.001 % / 95 | 0.005 / 0.002 % / 0.001 % / 95 |
| shop (3200, 3500) | 0.005 / 0.009 % / 0.002 % / 57 | 0.005 / 0.007 % / 0.001 % / 188 |
| race start (6400-7100) | 0.021 / 0.018 % / 0.006 % / 87 | 0.020 / 0.016 % / 0.004 % / 112 |
| mid-race (9000-9600) | 0.066 / 0.082 % / 0.004 % / 139 | 0.066 / 0.087 % / 0.003 % / 136 |

  Every field within finding 164's tolerance, the same numbers finding 167 measured.
- **Dual-source blending off** (`DHD_WGPU_DUAL_SOURCE=0`, finding 188's Intel rule: "GS blending
  without dual-source blending" in dhd.log): off against on as above to the third decimal (worst
  mid-race 0.066 / 0.082 %).
- **The fallback** (`DHD_NATIVE_VU1=on`): with `DHD_TEST_NO_MESHES=0` the renderer refuses at its
  start and every picture is the option-off one, identical; with `DHD_TEST_NO_MESHES=900` field 700
  is the native one (MAE 0.002, as with the option on) and every field after the refusal is
  identical to option off. dhd.log: "gs native: Native VU can't run on this GPU
  (DHD_TEST_NO_MESHES); the VU runs every program", then "native VU1: Native VU needs the native
  renderer with indexed fields (or the GPU refused its draws); the VU runs every program"; the game
  went on to the route's end both times.
- **The page** (hidden desktop, keys only): Graphics shows Resolution, Scaling, Texture Pack, Font,
  Native VU (Experimental), then the Performance group; right turned it on ("native VU1: loops 0170
  ... 2DE8 drawn by the native renderer, the VU skips them" in the menus' next VU1 run), right again
  off ("native VU1: off; the VU runs every program").

### State traces

`state_trace_run.py`, headless (so no renderer: Native VU forced with `DHD_NATIVE_VU1=all:skip` or
`all:shadow`, as findings 164-172 ran it; the setting's `on` waits for a native renderer and stays
off there):

| run | boot (600 fields) | race (5000 fields) |
|---|---|---|
| option off against main `cdee456` | equal, every row | equal, every row |
| shadow (`all:shadow`) against off | equal, every row | equal, every row |
| skip (`all:skip`, what players get) against off | game state equal in every row | game state equal in every row |

In skip mode the columns menu, phase, clock, EE RAM, scratchpad, hardware registers, CPU, GS memory,
sound, code and stack words are equal in every row of both runs; the VU column (from field 29),
the GS stream and the GS registers differ, by design (the translated batches don't run on the VU
and their packets never reach the GS: findings 164, 167). The Linux build's boot traces (WSL) are
equal to Windows' in every row, off and in skip mode.

## Platforms

| platform | built | ran | timed | option |
|---|---|---|---|---|
| Windows (RTX 3070, Direct3D 12) | yes | pictures, traces, page, fallback, dual-source off | 8x (below) | offered |
| macOS (M1, Metal) | yes, on the owner's Mac from this branch and the Windows build's core C | the picture route off and on: within the tolerance (worst mid-race 0.058 / 0.085 %) | no | offered |
| Linux x64 | yes (WSL Ubuntu 22.04, the release recipe) | headless boot traces | no | offered |
| Android (Mi 11i) | release-signed APK (the release key's certificate, not debuggable) | the timed route off and on | t612, 2x (below) | offered (SETTINGS) |
| Browser (WebGPU) | yes (`port/web/build.py`; its import check names `dhdr_native_meshes`) | no | no | hidden: its VU is built without the translation, its renderer makes no mesh layouts |

- **Linux, without network in WSL**: WSL's mirrored networking failed to start ("Failed to configure
  network ... falling back to networkingMode None"), so cargo there could not fetch the crates main
  added since the last Linux build (str0m). The crates were vendored on Windows from its cargo cache
  (`cargo vendor --locked`, offline) and given to the WSL build through an untracked
  `.cargo/config.toml`, the folder under WSL's CARGO_HOME: in the tree, `check_rust_lib.py` refuses
  the library (its objects name the tree's path). WSL was not restarted (other lanes use it).
- **macOS's harness** started the game over ssh with a window (as `cross_race.py` does), not
  `DHD_HEADLESS` (which turns the renderer off).

## Timing

### Windows, RTX 3070, 8x

Findings 164 and 167's method: finding 156's route (`route_profile.py`: `DHD_FPS=240 DHD_VSYNC=0
DHD_FRAME_CLOCK=1`, 8x, windowed 1920x1080 on the hidden desktop, the timeline), the final build,
each run behind the PC's timed game lock and an idle gate (finding 164's `timed.py`). This PC now
idles between 2 and 14 %, which neither of that gate's rules (six 5-second windows under 10 %; or,
after 4 minutes, under 30 % within 6 points) reliably sees, so a third rule was added for this lane:
after 4 minutes, six windows under 15 %. Each run records the rule that let it in. Run order off,
on, off, on, on, off. During on-1 another lane's build compiled on one core (idle floor 20-24 %, run
load 38.8 % against 29.5-33.1 % for the others); it is shown apart and left out of the means.

| scene | Native VU | fields/s (runs) | field ms | game work / busy | VU work / busy | GS work | render CPU / busy | GPU ms |
|---|---|---:|---:|---|---|---:|---|---:|
| title | off | 180.5 (186.8/184.9/169.9) | 5.55 | 1.32 / 0.92 | 5.54 / 5.55 | 2.64 | 0.62 / 0.46 | 1.65 |
| title | on | 269.4 (268.6/270.1) | 3.71 | 1.30 / 0.87 | 3.70 / 3.71 | 1.37 | 0.80 / 0.51 | 1.37 |
| shop | off | 236.4 (231.7/262.2/215.4) | 4.26 | 0.86 / 0.69 | 4.25 / 4.26 | 1.39 | 0.76 / 0.57 | 1.82 |
| shop | on | 318.6 (331.3/305.8) | 3.14 | 1.10 / 0.67 | 3.15 / 3.14 | 0.95 | 0.93 / 0.62 | 1.92 |
| race start | off | 221.1 (234.5/220.5/208.4) | 4.53 | 3.33 / 3.27 | 4.47 / 4.53 | 2.69 | 1.74 / 1.52 | 2.07 |
| race start | on | 297.9 (294.8/301.0) | 3.36 | 3.17 / 3.11 | 3.35 / 3.35 | 1.69 | 1.77 / 1.54 | 2.16 |
| mid-race | off | 289.7 (291.6/306.1/271.4) | 3.46 | 2.76 / 2.69 | 3.44 / 3.45 | 1.96 | 1.44 / 1.15 | 1.63 |
| mid-race | on | 375.8 (376.0/375.6) | 2.66 | 2.60 / 2.60 | 2.64 / 2.64 | 1.29 | 1.42 / 1.19 | 1.72 |

Whole route (fields 300-14000): off 258.5 / 263.2 / 237.7, on 329.7 / 327.4 (on-1, beside the other
build: 312.3) fields/s.

- **The route: 253.1 -> 328.6 fields/s (+30 %)**; the title +49 %, the shop +35 %, the race start
  +35 %, mid-race +30 %. Finding 167 measured +38 % (254 -> 351) on `native/vu-gs`'s older main;
  this run's on scenes are within a few percent of finding 167's (title 269 against 290, shop 319
  against 310, race start 298 against 307, mid-race 376 against 366); the off runs spread 238-263 as
  finding 167's did (241-262).
- **The VU thread is still the limit with it on, by a little** (race start 3.35 ms a field against
  the game thread's 3.11; mid-race 2.64 against 2.60), as finding 167 found; the GS thread does
  about half the work.

### The Mi 11i on the t612 profile, 2x

Finding 172's recipe: this lane's release-signed APK (the shipped builds' PGO profile, line
tables), finding 156's route through the run's keys in the app's dhd.ini (put back after each run,
md5 checked), other apps closed, the screen on, 60 s cooling, `phone_throttle.py --apply t612
--big-max 1209600 --little-max 1497600` for the session (restored after; the phone's own thermal
caps were above t612's), two rounds interleaved (off, on, off, on); battery 34-37 C. Mean of the two
runs (each in brackets); ms a field from the run's timeline, route_profile.py's columns (work = CPU
minus spin; busy = the field minus the thread's waits):

| scene | Native VU | fields/s | field ms | game work / busy | VU work / busy | GS work / busy | render CPU / busy |
|---|---|---:|---:|---|---|---|---|
| title | off | 47.2 (46.6/47.7) | 21.22 | 5.47 / 3.46 | 19.38 / 21.22 | 17.71 / 17.70 | 4.71 / 3.49 |
| title | on | 61.8 (61.5/62.0) | 16.20 | 4.71 / 4.34 | 14.07 / 16.20 | 8.80 / 7.28 | 5.35 / 4.11 |
| shop | off | 58.2 (57.2/59.1) | 17.20 | 3.96 / 2.97 | 15.10 / 17.20 | 9.79 / 9.52 | 6.31 / 5.20 |
| shop | on | 67.0 (66.8/67.2) | 14.93 | 3.43 / 3.35 | 12.49 / 14.68 | 6.09 / 5.79 | 7.19 / 6.09 |
| race start | off | 50.9 (51.1/50.7) | 19.65 | 12.78 / 14.41 | 16.42 / 18.84 | 15.71 / 16.26 | 13.38 / 13.74 |
| race start | on | 56.7 (56.7/56.7) | 17.65 | 11.42 / 13.31 | 12.62 / 14.52 | 9.79 / 15.45 | 14.29 / 14.15 |
| mid-race | off | 62.2 (61.8/62.6) | 16.08 | 11.36 / 13.07 | 13.57 / 15.90 | 12.36 / 12.38 | 10.77 / 9.77 |
| mid-race | on | 71.7 (71.7/71.6) | 13.96 | 11.35 / 13.41 | 11.11 / 13.15 | 8.46 / 8.21 | 11.50 / 10.60 |
| route | off | 57.2 (56.9/57.4) | | | | | |
| route | on | 66.7 (66.6/66.8) | | | | | |

- **The route: 57.2 -> 66.7 fields/s (+16.6 %)**; the title +31 %, the shop +15 %, the race start
  +11 %, mid-race +15 %, as finding 167 measured (+17 % then).
- **Both levels are lower than finding 167's** (63.9 off, 74.7 on, on `native/vu-gs`'s older main):
  this lane did not time main against its own off build on the phone (the pictures and traces say
  the off path is main's; the restore APK on the phone is main's build without the PGO profile, so
  it is not a like-for-like base). Not traced further: what main gained since 9ecb8e1 (finding 174's
  Mali barriers, finding 183's effects, P2P) is the likely cost.
- **The race start is the render thread's**: with the option on it is busy 14.2 ms of a 17.7 ms
  field and the GS thread waits behind it (busy 15.5, working 9.8), as findings 168 and 172 found.

## Decisions

- **Both paths are kept while native is experimental** (the owner, 2026-11-10): the emulated VU1
  path stays beside the native one, an explicit exception to the single-implementation rule (root
  AGENTS.md, decision 099), as for the P2P experiment (180). The plan: once native is proven on
  every platform (pictures, state, speed, drivers), it becomes the default and the emulated draw
  path for the translated loops is deleted.
- **Merged, not rebuilt**: the conflicts were few and side by side, so the branch's history stays
  as it was measured.
- **The row above the Performance group, not under a heading of its own**: an "Experimental"
  heading made the Graphics tab eleven rows, and the last ran into the "L1 / R1 Tabs" legend.
- **Live, not at the next start**: switching at a batch is safe (above), and it costs the VU thread
  two atomic loads a VU1 run.
- **The vertex buffer's storage use** is added where meshes are possible, whether the option is on
  or not (the buffer is made before the option can change); the off path's draws do not read it.

- **Finding 172's constants step stays, in the off path too**, as the brief asked (keep what 172
  kept): it changes the GS path's draws for every player (their constants by instance index), with
  identical pictures and traces. Finding 172 left one question open before any merge: the Redmi 9
  at 2x ran about 8 % slower after it, likely heat from the run order, unconfirmed. This lane had
  the Mi 11i only; the interleaved Redmi 2x run is still owed (Result).
- **Test values kept**: `DHD_NATIVE_VU1` still takes loops and flags (`0170,1708:skip`), so the
  native lane's per-loop checks run as before; `on` and `off` are the setting's words.

## Surprises

- `vu1_mesh.inc`'s browser stub had no `MESH_ON`, which `vif.c` uses: the browser build never
  compiled on `native/vu-gs`. Added.
- `DHD_PASS_TRACE` measured a mesh draw's bounds from its vertices, which are object-space: such a
  draw's bounds are now its scissor.
- **Three renderer tests were stale on `native/vu-gs`**: `check.py port` does not run the crate's
  own tests (`rustgs.py test`), and finding 168 changed the draw constants' size without them:
  `abi_layout` (224 bytes, the commands' prefix 296), `native_arenas` (96-byte records) and
  `native_spirv`, which gave the mesh vertex stage the draw layout, so its storage binding had no
  slot and the stage was never checked for Mali's and PowerVR's driver rules. Fixed (`5d7ff18`);
  with the right layout the mesh stages pass both rules and Arm's offline compiler for Mali-G57
  and Mali-G52.
- **WSL had no network** (its mirrored networking failed to start), so the Linux build took its
  crates from the Windows cache (Platforms).

## Result

Lane state 2026-11-10 (not merged; the lead merges and closes the finding).

- **What changed**: `native/vu-gs` merged as it was (findings 162, 164, 167, 168, 172), then the
  setting (dhd.ini `native_vu`, REVIVED SETTINGS > Graphics "Native VU (Experimental)" above the
  Performance group, the launcher's Graphics page, Android's SETTINGS; hidden in the browser), live
  at the next VU1 batch, off by default; the fallback to the emulated VU when the GPU can't draw
  meshes (one dhd.log line, the game goes on); `native_vu_pictures.py`; the renderer tests the
  native branch left stale.
- **Off is main**: pictures identical at 2x and 4x, boot and race state traces equal to `cdee456`'s
  in every row.
- **On**: the game's state equal (shadow mode in every row; skip mode in every game-state column),
  pictures within finding 164's tolerance on Windows at 2x and 4x (worst MAE 0.066, 0.087 % of
  pixels off by more than 8), with dual-source blending off too, and on the Mac (M1, Metal: worst
  0.058, 0.085 %).
- **Faster**: Windows RTX 3070 8x, finding 156's route 253.1 -> 328.6 fields/s (+30 %); the Mi 11i
  on the t612 profile at 2x 57.2 -> 66.7 (+16.6 %).
- **Platforms**: Windows built, ran, timed; macOS built and ran (pictures off and on); Linux built
  (WSL Ubuntu 22.04) with headless boot traces equal to Windows'; Android release-signed APK built,
  timed on the phone; browser built, option hidden.

What is left:

- **Finding 172's open question**: the interleaved Redmi 9 2x run (before/after the constants step,
  which this lane keeps in the off path too). Only the Mi 11i was attached.
- **A main-against-off timing on the phone** (the off level is below finding 167's; not traced).
- **Real drivers that refuse**: the fallback is shown with `DHD_TEST_NO_MESHES` and the mesh
  shaders pass the Mali and PowerVR checks and Arm's offline compiler; no PowerVR, Mali-G52 or Intel
  device ran the option here.
- **No timing on the Mac or Linux**; Linux did not run with a window (WSL).
- **Before native becomes the default**: the near-bound batches still run on the VU (15-42 % of
  some loops' batches, finding 167), overlay A and the skinning passes stay on the VU, and the
  phone's render thread limits with it on (findings 168, 172).
