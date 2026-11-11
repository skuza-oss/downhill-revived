# 198: Native VU is the default

Status: current
Supersedes: parts of 194 (off by default, "experimental", both paths kept beside each other), parts of 195 (the encoder cut on Mali GPUs)
Checked: 2026-11-11

Branch `feat/native-vu-default` from `main` 652a734b (release 0.7.15), started 2026-11-11. Merged into `main` at b3f8496a on
2026-11-11; ships in 0.7.16. This file is the lane's plan while it runs and its finding when it merges. Someone who has only this
file and the git log must be able to pick the lane up.

## Goal and acceptance

The owner's decision (2026-11-11): Native VU (finding 194: the VU1 programs' batch loops run as GPU
shaders; dhd.ini `native_vu`, override `DHD_NATIVE_VU1`) becomes the default on every platform
except the browser. The emulated VU path stays only as the automatic fallback (a pipeline the
driver refuses, `dhdr_native_meshes`) and as the browser's path; a later lane deletes it once
players' logs show no fallbacks. This lane measures first, then flips the default.

- Measure on the Redmi 9 (Helio G80, Mali-G52 MC2), the low-end phone: finding 156's route at 2x
  (and 1x), Native VU off against on, interleaved, one debuggable APK of `main` 652a734b. dhd.log
  with it on: no fallback line, no crash.
- `native_vu` on by default: the settings crate, REVIVED SETTINGS, the launcher; hidden in the
  browser as before. Players who already have a dhd.ini get the new default unless they chose off.
- The label loses "experimental"; the switch stays while the emulated path exists.
- Acceptance: `python tools/check.py port`; boot and race state traces equal to `main`'s; pictures
  (`port/tools/native_vu_pictures.py`): the new default identical to `main` with the option on, the
  fallback (`DHD_TEST_NO_MESHES`) identical to `main` with the option off; the settings tests.
- Out of scope: deleting the emulated path, the near-bound batches still run on the VU, the
  browser's Native VU (lane 199).

## Progress

- [x] 2026-11-11: finding reserved (`6654e191`)
- [x] 2026-11-11: the Redmi 9, 2x and 1x, off against on (below). Found on the way: `main` loses
  the GPU device there (below), fixed in `3c42021b`
- [x] 2026-11-11: the default (`67cf649d`): settings crate, REVIVED SETTINGS, launcher, the browser
  left out; existing dhd.ini files need no migration (Decisions)
- [x] 2026-11-11: docs: AGENTS.md, STATE.md, finding 194, PLAYING.md, the release texts
- [x] 2026-11-11: `check.py port`, state traces, pictures, settings tests (below)
- [x] 2026-11-11: the integrated GPU at 4x, off against on (below); the Mac was not idle: not timed

## The Redmi 9 (Helio G80, Mali-G52 MC2, Android 11)

Finding 174's recipe: a debuggable APK of `main` 652a734b (`build_apk.py --line-tables --pgo-use`
with finding 195's profile), installed over the debuggable build on the phone, finding 156's route
(fields 300-14000) through the run's keys in the app's dhd.ini (the file found there put back after
every run, md5 checked), `DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`, the screen on, no throttle
(the phone is not rooted), 90 s between runs (30 s before the first), the option switched per run by
`DHD_NATIVE_VU1` in dhd.ini, order off, on, off, on at 2x, then off, on at 1x. Battery 31.7-36.0 C
over the six runs. Every run with `DHD_ENCODE_CUT=0`: without it no run of this build finishes on
this phone (next section).

| scale | Native VU | fields/s (runs) | p50 ms | p99 ms | fallback lines | device lost |
|---|---|---:|---|---|---:|---:|
| 2x | off | 50.6 (51.0 / 50.2) | 18.9 / 19.3 | 36.2 / 34.9 | 0 | 0 |
| 2x | on | 48.7 (49.0 / 48.5) | 19.7 / 19.9 | 35.4 / 35.3 | 0 | 0 |
| 1x | off | 57.1 | 17.2 | 27.9 | 0 | 0 |
| 1x | on | 58.4 | 16.4 | 25.7 | 0 | 0 |

By scene (fields a second; ms a field from the run's timeline, finding 156's columns: work = CPU
minus spin, busy = the field minus the thread's waits; "race" is fields 6400-14000):

| scene | Native VU | 2x fields/s | 2x VU work | 2x GS work / busy | 2x render CPU / busy | 1x fields/s |
|---|---|---:|---:|---|---|---:|
| title | off | 53.4 | 18.72 | 14.96 / 14.36 | 8.11 / 5.68 | 53.2 |
| title | on | 65.9 | 12.99 | 6.94 / 15.18 | 10.77 / 8.50 | 66.6 |
| shop | off | 60.0 | 15.37 | 7.28 / 13.63 | 8.62 / 6.57 | 64.4 |
| shop | on | 56.7 | 14.18 | 4.68 / 17.62 | 9.90 / 8.09 | 66.4 |
| race start | off | 38.7 | 17.40 | 14.10 / 25.70 | 19.21 / 17.07 | 50.7 |
| race start | on | 37.0 | 15.43 | 10.30 / 26.89 | 20.07 / 17.97 | 49.6 |
| mid-race | off | 49.2 | 14.75 | 10.94 / 20.33 | 17.37 / 14.61 | 58.5 |
| mid-race | on | 46.3 | 11.69 | 7.62 / 21.58 | 17.96 / 15.58 | 58.9 |
| race | off | 47.3 | 15.59 | 12.12 / 21.10 | 18.03 / 15.19 | 56.5 |
| race | on | 44.8 | 12.26 | 8.54 / 22.30 | 18.70 / 16.24 | 56.3 |
| route | off | 50.6 | 15.10 | 11.40 / 18.52 | 14.74 / 12.24 | 57.1 |
| route | on | 48.7 | 11.99 | 7.45 / 20.46 | 15.80 / 13.70 | 58.4 |

- **At 2x Native VU is slower on this phone: the route -3.7 %, the race -5.3 %** (race start
  -4.4 %, mid-race -5.9 %, the shop -5.5 %); only the title gains (+23 %). The CPU side does get
  cheaper (VU work 15.6 -> 12.3 ms a race field, GS work 12.1 -> 8.5), but the GPU limits this
  phone at 2x (finding 174): the GS thread is busy the whole field waiting behind the picture, and
  the picture takes about 1.2 ms longer with the mesh vertex stages on the Mali-G52.
- **At 1x: the route +2.3 %, the race level** (56.5 against 56.3; the title +25 %, the shop +3 %).
- **With the option on**: no fallback line ("gs native: Native VU can't run on this GPU"), no
  crash, the twelve loops reported as drawn ("native VU1: loops 0170 ... 2DE8 drawn by the native
  renderer, the VU skips them"); two race screenshots (`adb screencap`, race seconds 6 and 10)
  show the track, trees, banners, the cable cars, the helicopter, the riders and the HUD as they
  should be. This is the first Mali-G52 run of the option (finding 194 had none).
- **Finding 172's open question** (the Redmi at 2x about 8 % slower after the per-draw constants:
  heat?) can no longer be run as asked: finding 195 undid that step (the constants are a uniform
  block at a dynamic offset again), so `main` has no "after". What these runs say: with the step
  undone the 2x route is 50.6 off / 48.7 on, above finding 172's "before" (48.7 / 47.2) and well
  above its "after" (44.8 / 43.3), and two interleaved pairs here differ by under 2 % at battery
  temperatures 2-4 C apart. That fits the GPU-cost reading (the per-draw lookup), not heat.

## `main` loses the GPU device on the Mali-G52 (fixed here)

The first runs of `main` 652a734b on the Redmi 9 ended with the lost-device box ("The graphics
driver stopped responding", finding 171): dhd.log "gs: the GPU device was lost (Unknown): Device is
lost" at the title (field about 416, 10 s after the start) or right after the route enters the Bike
Shop (after field 2877, about 57 s). Five of five starts of the full route, with Native VU off
(four) and on (one); three short runs to field 1500 passed. Nothing about it in logcat.

The cause is finding 195's encoder cut (a large picture recorded into two encoders, the first
finished on a helper thread): with `DHD_ENCODE_CUT=0` (one encoder a picture) six of six full
routes ran through, and the branch's APK with the fix below two more, on default settings. Finding
195 timed the cut on the Mi 11i (Adreno 660) only and noted that the Redmi 9 was not attached; the
release APK of 0.7.15 is built from the same source, so it is expected to do the same on this
phone (not run here: only debuggable builds may be installed on it).

**Fix (`3c42021b`)**: on Arm's Mali GPUs (Vulkan vendor id 0x13B5) a picture stays in one encoder
unless `DHD_ENCODE_CUT` asks for a cut; one dhd.log line says so ("gs native: a picture stays in
one encoder on this GPU (Mali)"). Every Mali, not only the G52 on r26p0: no other Mali has run the
cut, and the gain was measured on an Adreno. What in the cut the driver cannot take (two command
buffers recorded at once, or the scene split over two) was not looked for: the phone had to go
back.

## Checks

- `python tools/check.py port`: every step OK, on the default change and again on the branch's
  last code commit (`67cf649d`, with the Mali rule); the browser target compiles
  (`rustgs.py check --target wasm32-unknown-unknown`).
- **Settings tests** (`rustgs.py test -- -p dhd_settings`): pass. `web_corpus.json` was
  regenerated on purpose (`DHD_SETTINGS_BLESS=1`): its eight cases' `DHD_NATIVE_VU1` default went
  from `off` to `on`, nothing else. `settings_table.h` is unchanged (it carries the built-in value,
  still off). New assertions in `corpus.rs`: on with a dhd.ini without the line, off with
  `native_vu = off` or `DHD_NATIVE_VU1 = 0`, on with an empty line, built-in off.
- **State traces** (`state_trace_run.py`, headless, against `main` 652a734b's build): boot 600
  rows and race 5000 rows equal, both as the tools run (no dhd.ini) and with `DHD_NATIVE_VU1=on`
  given to both builds (a headless run has no renderer, so the setting waits and the VU runs every
  program, as finding 194 found).
- **Pictures** (`native_vu_pictures.py`, 2x, eleven fields, RTX 3070, each run with a new dhd.ini
  holding only the header line, so the players' defaults apply):
  - the new default against `main` with `DHD_NATIVE_VU1=on`: identical, every pixel;
  - the fallback (`DHD_TEST_NO_MESHES=0`, new default) against `main` with its default (off):
    identical; dhd.log has the two fallback lines;
  - `main` off against the new default: within finding 164's tolerance (worst mid-race MAE 0.063,
    0.078 % of pixels off by more than 8), as finding 194 measured.

## Other timings

- **The PC's integrated GPU (Ryzen 5 7600's Radeon, `DHD_WGPU_ADAPTER=integrated`), 4x**:
  `bench_race.py`, the route's fields 300-14000, uncapped, the window focused (100 % in every
  run), interleaved off, on, off, on: **off 122.1 fields/s (119.6 / 124.7), on 118.2 (118.5 /
  117.9): -3.2 %**, p99 13.7 against 13.4 ms, the worst 1 % 23.5 against 17.7 ms. The GPU limits
  in both (`limit=gpu`, about 7.9 ms of GPU a frame); the CPU side is cheaper with it on (VU 3.07
  -> 2.24 ms a field, GS 1.23 -> 0.78). The off runs spread 4 %, so the loss is at the edge of what
  two pairs can show.
- **The Mac (M1)**: reachable, not idle (three system processes of the machine's own at 95 % of a
  core each, load 6.2): not timed.

So where the GPU limits (the Redmi at 2x, the integrated Radeon at 4x) Native VU costs 3-5 %, and
where the CPU limits it gains (the Mi 11i on the t612 profile +16.6 %, the RTX at 8x +30 %, finding
194).

## Decisions

- **The flip goes ahead as the owner decided**, with the Redmi's -5 % in the race at 2x said
  plainly here and to the lead: the brief was to measure, then flip, and the switch stays for
  whoever is GPU-bound. Whether a GPU-bound low-end phone should start with it off is the owner's
  call (Result, what is left).
- **No migration for existing dhd.ini files.** A save writes only the line of the setting it
  changes (`Ini::set`; REVIVED SETTINGS commits only rows that changed, the launcher saves one
  key), and neither the game nor the launcher ever writes a full file (`create` writes the header
  only). So a 0.7.15 player's dhd.ini has a `native_vu` line only if they moved that row; everyone
  else has none and gets the new default at the next start. A line that says `off` is kept: it is
  there because the player turned the option on and off again (or backed out of the page with
  Triangle after trying it, which writes the opened value back). Those two cannot be told apart,
  and both saw the option and left it off, so off stays. 0.7.15 is one day old.
- **The built-in value stays off**: what the tools get without a dhd.ini (`DHD_SETTINGS=none`:
  state traces, benchmarks, picture checks) is unchanged, as `renderer` is `exact` there and
  `native` for players. Tools that should measure what players get pass `DHD_NATIVE_VU1=on`. The
  lane that deletes the emulated path flips it.
- **The browser**: `DHD_NATIVE_VU1` joins `settings.js`'s `BROWSER_WITHOUT`, so the default never
  becomes a variable there (its VU has no translation; the `settings:` line would otherwise claim
  it on). The row stays hidden as before.
- **Words**: the row is "Native VU:", its hint "Faster. Turn OFF if the picture looks wrong"; the
  launcher's "Native VU" with the same advice. The switch stays as long as the emulated path does.
- **The Mali fix is in this lane** although it is finding 195's code: the phone that shows it was
  here for an hour, and without it the default could not be measured at all.
- **Every Mali, not only the G52** (above).

## Surprises

- **`main` could not finish one route on the Redmi 9** (above). The measurements were taken with
  `DHD_ENCODE_CUT=0`, which is also what the fix makes of every Mali.
- **Native VU is not a gain where the GPU limits**: -3.7 % on the Redmi at 2x, -3.2 % on the
  integrated Radeon at 4x. Finding 172 had seen the same sign on the Redmi (47.2 on against 48.7
  off) without saying so.
- **The dhd.ini on the Redmi 9 was not a settings file**: 46 bytes, the text of a `cat` error an
  earlier lane's backup had captured and "restored". It was put back as found; the runs here
  started from an empty file instead of appending to it.
- **A system dialog of the phone's own ("USB connection") came up over one validation run**; it
  was left alone (HOME sent it to the background) and the screenshots were taken again.
- With `renderer = exact` (or headless) and the new default, dhd.log gets finding 194's one line
  "native VU1: Native VU needs the native renderer with indexed fields (or the GPU refused its
  draws)" after 600 fields. The lane that reads players' logs for fallbacks must count "gs native:
  Native VU can't run on this GPU" (the renderer's refusal), not this line.

## Result

Merged into `main` at b3f8496a on 2026-11-11; ships in 0.7.16.

- **What changed**: `native_vu` on by default (settings crate; the tools' built-in stays off), the
  row and the launcher's switch without "experimental", the browser leaves the variable out; on
  Mali GPUs a picture stays in one encoder; the docs.
- **Measured**: the Redmi 9 at 2x 50.6 -> 48.7 fields/s with the option on (race 47.3 -> 44.8), at
  1x 57.1 -> 58.4; the integrated Radeon at 4x 122.1 -> 118.2. No fallback, no crash, the race
  looks right on the Mali-G52.
- **Checked**: `check.py port`, boot and race state traces equal to `main`'s, pictures identical
  to `main` with the option on and, in the fallback, to `main` off.

What is left:

- **The owner's call on GPU-bound devices**: on by default costs the Redmi 9, the stated minimum
  for 60 at 2x, about 5 % in a race at 2x. Options: accept it, or start with it off where the GPU
  limits (nothing in the game measures that today).
- **0.7.15 on Mali**: the released APK is expected to lose the device on the Mali-G52 (r26p0) as
  the debuggable build of the same source did; the fix is `3c42021b`, in 0.7.16. Other Malis (G57 and newer
  drivers) were not run with or without the cut.
- **Why the cut loses the device** on that driver.
- **No timing on the Mac**; the launcher's own tests were not run (one label and hint changed).
- **The Redmi 9 now carries the branch's debuggable build** (0.7.15, the fix, the new default):
  the 0.7.12 debuggable build it had cannot be put back without uninstalling.
