# 256: the slow frames on weak phones

Status: current

Branch `perf/phone-stutter-256` from `main` c2e92600 (release 0.8.5 plus lanes 247 and 249-253),
started 2026-11-15. This file is the lane's plan while it runs and its finding when it merges.
Someone who has only this file and the git log must be able to pick the lane up.

## Goal and acceptance

The speed lanes (findings 195, 247, 249, 253) raised the average. Their run lines also show what a
player feels on the throttled Mi 11i (`phone_throttle.py --apply t612`, 4x, Native VU on): a p99
frame of about 29 ms, a worst 1 % of 77 to 95 ms over the route, and one frame of about 1.5 s in
every run. This lane lowers the slow frames without changing what the game does.

Method: the base first (p50, p99, the worst 1 %, the frames over 33, 50 and 100 ms and the longest,
for the race start and mid-race, from the runs' `DHD_TIMELINE`); then every frame over 50 ms
attributed to a cause before anything changes; then one change at a time, the largest share of the
lost time first. A change is kept when the count of slow frames falls clearly outside the runs'
spread, the average does not drop and the boot and race state traces on the PC equal the base's.

Acceptance: `python tools/check.py quick` and `port`; boot (600 fields) and race (5000) state
traces equal to `main` c2e92600's; the Android unit tests.

Out of scope: gameplay, the picture's content, players' settings, the average speed (findings
247, 249 and 253), what those lanes tried and dropped.

## Progress

- [x] 2026-11-15: finding reserved
- [x] the base measured and every frame over 50 ms attributed
- [x] changes, one at a time: the pipeline list, the sharp font's cache, the touch controls' objects
- [x] the checks

## How it was measured

The Mi 11i with the low-end profile (`phone_throttle.py --apply t612`), release-signed APKs with
the profile-guided build (`dhd-197.profdata`), 4x, Native VU on, finding 156's route
(`DHD_BENCH=300-14000`, `DHD_FPS=240`, `DHD_VSYNC=0`, `DHD_FRAME_CLOCK=1`) with `DHD_TIMELINE`:
one row a field with each thread's time. Scenes by field: the race 6321-14000 (GO at 6321), its
start 6321-7300, mid-race 7301-14000, the route 300-14000 (title and menus, the track's load
screen from about 4064, the flight over the track 4573-6060, the countdown to 6321). "Lost over
50" is the sum of each slow frame's time above 50 ms. Causes: `DHD_SPIKES=1` (one line for every
render-thread step over 1 ms, with the pipeline's key) and simpleperf on the process with the
build's unstripped library. Two or three runs a build; the runs of one build agree within 0.2
fields/s and one or two slow frames.

## The base (c2e92600)

| Scene | fields/s | p50 ms | p99 ms | worst 1 % ms | longest ms | over 33 | over 50 | over 100 |
|---|---|---|---|---|---|---|---|---|
| race start | 52.0 | | | 82-84 | 170-177 | | 3-4 | 3 |
| mid-race | 67.4 | | | 24.8-25.3 | 164 | | 1 | 1 |
| race | 64.9-65.0 | 14.8 | 28.2-28.5 | 38.7-39.1 | 170-177 | 21-25 | 4-5 | 4 |
| route | 66.1-67.0 | 14.5 | | | 1510-1590 | | 30-31 | 24-25 |

The route loses 8.3 to 8.6 s over 50 ms a run, the race about 0.45 s.

The 1.5 s frame is not in a race. It is field 347, the title screen's first pictures: 20 draw
pipelines compiled one after another on the render thread in fields 345-347 (2.7 s together).

## Causes

Every frame over 50 ms of the base, by cause, with its share of the time lost over 50 ms:

| Cause | Where | A frame costs | Share, route | Share, race | Now |
|---|---|---|---|---|---|
| A draw pipeline compiled by the driver at a draw state's first use, on the render thread (`Pipelines::get`) | title and menus (20 at once), every new screen, four times in the race (fields 6348, 6530, 7165, 10930) | 130-160 ms each | 77 % | 90 % | built ahead from the second run on |
| The sharp font (finding 058) made again for the same font pixels (`font_sharp_view`) | the title, the load screen's start, the flight's start | 290-340 ms | 11 % | 0 | made once a session (the title's stays) |
| The touch controls' shader and pipeline made when they first show | the flight's first picture | 380-390 ms | 4.5 % | 0 | made ahead on a thread |
| The kernel's GPU memory for new surfaces at a screen change (`kgsl_iommu_get_gpuaddr`, page clears, inside the render pass's encode) | the title, the load screen (fields 4191, 4373) | 170-280 ms | 4.5 % | 0 | left: the phone's driver |
| Texture uploads read pixel by pixel on the GS thread (`gs_image_pixel`, `native_decode`) | the track's load, the flight, the countdown's start (3944, 4572, 4863-4869, 6062) | 50-125 ms | 3 % | 0 | left: load time, see below |
| The start scene itself: game thread 20-30 ms and GPU 20-23 ms, with a 12 ms texture upload | race start, fields 6627 and 6733 | about 50 ms | 0 | 10 % | left: the scene's load, not a hitch |

The 33 to 50 ms frames of fields 6587-6763 are the race start's own load (GPU-bound at 4x,
findings 195 and 249), in every run alike.

## What changed

1. **The pipeline list** (`port/render/src/native/pipelines.rs`, `pipelines.txt` beside dhd.log).
   Every draw pipeline that builds is written to a text file: one key a line under a header that
   names the format, backend, GPU and driver. The next start reads it and sends the keys to the
   `dhd-pipelines` thread, which builds them while the game boots (127 keys after one route: 50
   generic and 77 specialised, done 8 to 9 s after the renderer's start on the throttled phone).
   A draw whose pipeline is ready no longer compiles. The file is only a cache: none, another GPU
   or driver, or a damaged file mean first use builds as before, and the file is written afresh.
   `DHD_PIPELINE_LIST=0` turns it off. The browser has none.
2. **The sharp font's cache** (`port/gs/font_sharp.c`) is keyed by the font's pixels. It was keyed
   by the decoded texture's key, which also hashes everything else in the font's memory pages (the
   menus' font is the top bytes of a depth buffer), so each screen change made the same font
   again. The copy is the same bytes as before.
3. **The touch controls' shader and pipeline** (`port/render/src/present/touch.rs`) are made on a
   thread of their own from a touch screen's first picture on; the first picture with the controls
   takes them, and waits as before only when they are not ready. The browser makes them at first
   show as before.

## Result

Throttled Mi 11i, 4x, Native VU on, the list of an earlier run in use (two runs each):

| Scene | | base | now |
|---|---|---|---|
| race start | fields/s | 52.0 | 53.2-53.4 |
| | worst 1 % ms | 82-84 | 38.6-38.9 |
| | frames over 50 ms | 3-4 | 0-1 |
| mid-race | fields/s | 67.4 | 67.5 |
| | worst 1 % ms | 24.8-25.3 | 22.7-22.8 |
| | frames over 50 ms | 1 | 0 |
| | longest ms | 164 | 26-29 |
| race | fields/s | 64.9-65.0 | 65.3 |
| | p99 ms | 28.2-28.5 | 27.9-28.2 |
| | worst 1 % ms | 38.7-39.1 | 31.6-31.7 |
| | longest ms | 170-177 | 45-51 |
| | frames over 50 / over 100 ms | 4-5 / 4 | 0-1 / 0 |
| route | fields/s | 66.1-67.0 | 69.5-69.6 |
| | longest ms | 1510-1590 | 324 |
| | frames over 50 / over 100 ms | 30-31 / 24-25 | 9 / 3-4 |
| | lost over 50 ms | 8.3-8.6 s | 0.9-1.0 s |

Step by step, the route's time lost over 50 ms: 8.3-8.6 s, with the list 2.0-2.1 s, with the font
cache 1.2-1.3 s, with the touch objects 0.9-1.0 s. The one race frame over 50 ms left (50.7 ms in
one of two runs) is the start scene's load at field 6686.

Limits:
- The first run after an install, a GPU driver update or a cleared app has no list: it stutters
  as the base does, and each scene visited for the first time adds its pipelines.
- What is left on the route (9 frames): the title's one font copy (320 ms, field 343), the
  kernel's GPU memory at the load screen (170 ms), texture uploads at the track's load and the
  flight (50-125 ms).

Checks: `check.py quick` and `port`; boot (600 fields) and race (5000) state traces of 75225fd4
equal to c2e92600's; the Android unit tests; on the PC a second hidden start built its 10 listed
pipelines ahead and only the four probe pipelines on the render thread.

A real weak phone must confirm (Redmi 9, Mali-G52, not throttled): that its second start logs
`gs native: pipelines.txt lists N draw pipelines of earlier runs` and `the N listed pipelines were
built ... s after the renderer's start`, with no refused pipeline; that the race has no frame
over 100 ms in the second run where the first has several; that building ahead on the worker
while the game boots does not slow its boot or its first menus (the throttled Mi 11i keeps more
CPU cores than such a phone's two fast ones); and that the touch controls appear with the
flight's first picture.

## Decisions

- The list is learned on the player's device, not shipped: the keys depend on the GPU's features
  and the player's settings (scale, Native VU, effects), and a shipped list would compile
  pipelines a device never draws on drivers that are slow or fragile at it (findings 170, 176).
- A draw never waits for or skips a pipeline that is still building: a generic pipeline missing
  at a draw is built on the render thread as before, so the picture is the same in every run.
- The list holds keys, not driver binaries: no Vulkan pipeline cache file, whose format and
  validity are the driver's. It would cut the worker's 8 to 9 s of compiling; a later lane can
  add it under the same header.
- A pipeline the device refused is not listed, and a listed one that fails ahead is dropped
  without a log line (the draw that needs it reports it as before).
- At most 4096 keys; the file is rewritten when its header does not match.

## Surprises

- The 1.5 s frame every speed lane's run line showed is the title screen, not the race.
- The sharp font was made up to four times a session for identical pixels: the cache was there
  (four entries) and never hit across screens.
- Not done, and why: skipping a draw until its pipeline is ready (the picture would change);
  moving texture decodes off the GS thread (the draws that follow need the texture in the same
  picture); the kernel's GPU memory time (the driver's, at screen changes only).
