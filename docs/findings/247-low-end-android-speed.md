# 247: more race frames a second on weak Android phones

Status: current

Branch `perf/low-end-android-247` from `main` 02a8bb3e (release 0.8.4), started 2026-11-14. This
file is the lane's plan while it runs and its finding when it merges. Someone who has only this
file and the git log must be able to pick the lane up.

## Goal and acceptance

More fields a second in a race on weak Android phones, without changing what the game does. The
product target is 60 fps at 2x on the Redmi 9 (Helio G80, Mali-G52 MC2). That phone is not
attached, so the lane measures on the stand-in finding 174 calibrated: the Mi 11i with
`port/tools/phone_throttle.py --apply t612` (the profile's own clocks) at `DHD_SCALE=4`, Native VU
on (the players' default since finding 198). Finding 174's limits apply: the stand-in is good for
the whole-race gain of a change, not for absolute fps; mid-race the render thread limits it, at the
race start the GPU.

Method: the base first (the published 0.8.4 release APK, which is `main` 02a8bb3e built with the
profile `dhd-197.profdata`), then a profile of where a field goes, then one change at a time, the
biggest measured cost first. A change is kept only when the whole race gains about 3 % or more,
clearly outside the runs' spread, and the boot and race state traces on the PC equal the base's.

Acceptance: `python tools/check.py quick` and `port`; boot (600 fields) and race (5000) state
traces equal to `main` 02a8bb3e's; the Android unit tests.

Out of scope: gameplay, the picture's content, players' settings, anything findings 146-149, 172,
174, 183, 195, 197 and 198 tried and dropped, unless a new idea differs from what they tried.

## Progress

- [x] 2026-11-14: finding reserved (`b7069877`)
- [x] 2026-11-14: the base measured (the published 0.8.4 release APK, two runs) and profiled
  (`DHD_TIMELINE`, `DHD_RSTAT`, `DHD_GPU_TIMES`, `DHD_GPU_STATS`): the GPU limits the race.
- [x] 2026-11-15: change 1, `de0c0f00`: the present samples the picture from its display surface,
  the scanout's copy is not made. Kept: the race +7.9 %, the race start +6.5 %.
- [x] 2026-11-15: review fix on change 1: once the copy is made (`materialize`) the picture is
  the display texture's and the present no longer samples the surface, which may have been drawn
  into since (a field that scans nothing out, then the held picture presented again);
  `test_native_transfers.py --presented` has that sequence. No copy is made in a race field, so
  the measured gain is untouched.
- [x] 2026-11-15: change 2 (the frame in one pass): +1.1 % over the race, +2.2 % at the race
  start. Under the bar, not on the branch (kept aside, see "Tried and dropped").
- [x] 2026-11-15: hypothesis 3 (a swapchain of three buffers with V-Sync off): nothing.
- [x] 2026-11-15: `check.py quick` and `port` pass on `de0c0f00`; boot (600) and race (5000) state
  traces equal `main` 02a8bb3e's; `port/android/test_build_apk.py` and `test_disc_image.py` pass.

## How it was measured

The Mi 11i (Snapdragon 888, Adreno 660), `phone_throttle.py --apply t612` (two big cores off, the
A78s at 1.44 GHz, the A55s at 1.80 GHz, the Adreno held at 315 MHz), `DHD_SCALE=4`, Native VU on,
finding 156's route (`bench_race.SCENES['race']`, `DHD_BENCH=300-14000`, `DHD_FPS=240`,
`DHD_VSYNC=0`, `DHD_FRAME_CLOCK=1`, no sound, 4:3), release-signed APKs built by
`package_release.py --platform android --pgo-use` with the profile `dhd-197.profdata`, installed
over the release with `adb install -r`. Scenes are fields of the route, read from the run's
`DHD_TIMELINE`: the race 6321-14000, the race start 6400-7300, mid-race 9000-14000. Every run
starts after 60 s of rest with the other apps closed; the battery was 28 to 32 degrees C.

Two things differ from the earlier lanes:

- The Mi 11i runners of lanes 195 and 197 were in their scratch folders and are gone. The lane's
  runner was written again from lane 198's Redmi 9 runner (the same keys in `dhd.ini`, the app's
  files read as root). It is not in the repository.
- A screen mirror of another session was running on the phone for the whole lane (not this
  lane's, so left alone). It took 25 to 28 s of CPU in a run of 225 to 245 s, the same in every
  run, so the numbers are a little lower than without it and the comparisons hold.

Two runs of the same build differ by 0.3 % or less over the race.

## The base and where its field goes

The published 0.8.4 APK, two runs, fields a second:

| scene | run 1 | run 2 | Native VU off (one run) |
|---|---:|---:|---:|
| the race | 56.96 | 57.09 | 60.41 |
| race start | 46.65 | 47.02 | 48.76 |
| mid-race | 59.65 | 59.75 | 63.36 |
| the route | 58.64 | 58.80 | 61.37 |

The GPU limits every race scene on this stand-in. Over the race a field is 17.5 ms and the GPU
works 15.9 ms of it; the game thread works 9.0 ms, the VU thread 9.1, the GS thread 6.3, the render
thread 9.8 (busy 15.2: it waits for the GPU in the submit and the present). With Native VU off the
VU and GS threads work 2 ms more each and the race is still 6 % faster: with the GPU at its limit
Native VU's mesh draws cost the GPU more than the emulated draws did. That is finding 198's
subject, not this lane's.

The GPU's frame by kind of pass over the race (`DHD_GPU_TIMES=1`, ms a frame):

| | draw passes | scanout | present | conversions |
|---|---:|---:|---:|---:|
| base | 12.75 (12.8 passes) | 1.09 | 1.08 | 0.16 (4 passes) |
| change 1 | 12.59 (12.1 passes) | 0 | 0.83 | 0.16 |

At the race start the draw passes are 15.9 ms in 18 passes. `DHD_GPU_STATS` there: 15.5 million
pixels shaded a picture on a 2048 x 896 frame (8.5 layers), 8.3 million of them blended, fogged,
filtered and Z-tested (the scene's haze and smoke). No full-frame clear sprite stands out, so
"draw the clear as a pass clear" was not tried.

## Change 1: the present samples the display surface (`de0c0f00`)

Every field, a compute pass copied the picture from its display surface into the display texture
(doubling the rows of the interlaced display), and the present pass then sampled the display
texture. On the stand-in the copy was 1.09 ms of the GPU's 15.9 (on the Redmi 9, finding 174
measured about 1.5 of 21).

Now, when the picture is one plain rectangle of the surface (no wrap at the GS's 2048 pixels,
nothing outside the texture: `scanout.rs`'s `plain_rect`), no copy is made. The present's new
fragment shader (`present_direct` in `native.wgsl`) samples the surface with the same sampler. For
the doubled rows it moves the sample's row so the sampler mixes the same two surface rows with the
same weight as it mixed the doubled picture's, and it holds the sample to the picture's outer
texel centres as the display texture's clamped edges did. What reads the display texture itself
makes the copy first (`Display::materialize`): a read-back (`DHD_SCREENSHOT`, the tools), the
widescreen fill, and a picture that shows nothing new while the surface is drawn into again.
Movies, black and a wrapped scanout go the old way. `DHD_DIRECT_PRESENT=0` keeps the copy
(comparisons).

Checked on the PC: a numeric model of both paths over six sizes (doubled and not, with an origin,
odd row counts, non-integer scales) agrees to 1e-13; `test_native_transfers.py --presented` (the
surface read back must be the picture, nearest) passes with and without; `native_vu_pictures.py`
at 4x, eleven fields, the copy path against the new one (whose read-back goes through
`materialize`): identical; the presented pictures of a hidden race look right in both; boot and
race state traces equal the base's.

Timed on one APK, off and on in turn (fields a second):

| scene | copy (`DHD_DIRECT_PRESENT=0`) | direct | gain |
|---|---:|---:|---:|
| the race | 56.90, 56.89 | 61.33, 61.48 | +7.9 % |
| race start | 46.78, 46.70 | 49.66, 49.92 | +6.5 % |
| mid-race | 59.74, 59.58 | 64.46, 64.58 | +8.2 % |
| the route | 58.62, 58.57 | 63.30, 63.36 | +8.1 % |

The GPU's frame over the race fell from 15.9 to 14.7 ms: the copy's 1.09 ms and 0.25 ms of the
present (one large texture less to read).

## Tried and dropped

**The frame in one pass (+1.1 % over the race, under the bar).** A race picture draws its scene
into the frame, then the riders' shadow and haze textures (64 x 64 targets), then more of the
frame with them, more small textures, and the last of the frame: three draw passes into the frame
mid-race and five at the race start, each a load and a store of the whole frame and its Z on a
tile GPU. The change records what lies between two runs of the frame's draws before the first or
after the second when neither reads or writes what the other writes (a pure ordering of the
picture's command list, with unit tests), and lets a pass that begins with depth-disabled draws
take the Z surface of the draws that follow. Result: one frame pass mid-race, two at the race
start (one small texture there is drawn from the frame itself, so that split is real); pictures
identical in eleven fields at 4x, traces equal. Timed off and on in turn:

| scene | game's order | reordered | gain |
|---|---:|---:|---:|
| the race | 61.45, 61.29 | 62.10, 62.01 | +1.1 % |
| race start | 49.67, 49.73 | 50.74, 50.84 | +2.2 % |
| mid-race | 64.54, 64.39 | 64.88, 64.72 | +0.5 % |

The GPU's frame fell by 0.26 ms only (the Adreno's load and store of a 2048 x 1024 frame cost
about 0.13 ms each, far less than the 0.3 to 0.5 ms finding 195 saw for a pass added in the middle
of the scene), and the render thread's work by 1.3 ms a field mid-race (fewer passes to encode),
which does not show while the GPU limits. Real and outside the runs' spread, but under 3 %: not on
the branch. The commit is kept on the local branch `lane247-pass-order` (`e0a5fc59`, on top of
change 1) for the Redmi 9: a Mali's load and store may cost more, and at 2x its render thread
limits mid-race, where the 1.3 ms would count.

**A swapchain of three buffers with V-Sync off (`DHD_FRAME_LATENCY=2`): nothing.** After change 1
the GPU works 13.9 ms of a 15.5 ms field mid-race, and the render thread waits 2.2 ms in the
present. With one more buffer the route was 63.37 and 63.07 against 63.39 fields a second on the same APK.
The idle time is not the swapchain's. With V-Sync on (the 120 Hz screen, three buffers) the route
was 63.18: the players' setting runs as the timed one does.

## Decisions

- **The copy is skipped only for a plain rectangle.** A scanout that wraps or reads outside the
  texture keeps the compute pass: the present's shader would need the same wrap, and no race or
  menu picture is such a scanout.
- **The display texture stays** (same size, made as before) and is filled on demand: the
  read-back, the fill and a picture that shows nothing new need the picture after its surface is
  drawn into again. That is one code path with a lazy step, not a second renderer.
- **Change 2 is not merged** although it is correct and gains a little: the lane's bar is 3 % over
  the race on the stand-in.
- **The lane stops here**, at about six agent hours. The other ideas on finding 195's list (the
  constants binds, Rust PGO) save render-thread time, which does not limit at 4x; the 8.3 million
  blended pixels at the race start are the game's own picture.

## Surprises

1. **The scanout copy was worth 8 %, more than its 1.09 ms of 15.9.** The present pass got cheaper
   too, and the field shortened by the whole 1.25 ms.
2. **A pass into the frame costs the Adreno 660 little** (about 0.13 ms at 2048 x 1024 with the GPU
   held at 315 MHz). Finding 195's 0.3 to 0.5 ms was a cut in the middle of the scene pass.
3. **Native VU on is slower than off on this stand-in at 4x** (the race 57.0 against 60.4 on the
   base) because the GPU limits and the mesh draws cost it more. At 2x, where the threads limit,
   finding 198 measured the opposite.
4. **`test_disc_image.py` needs `DHD_JNI_HEADERS`** on a PC whose `JAVA_HOME` is a runtime without
   `include/`: it failed to compile until the variable named a folder with `jni.h`.

## Result

Whole race on the stand-in (Mi 11i, `t612`, 4x, Native VU on): **57.0 -> 61.4 fields a second
(+7.9 %)**, the race start 46.8 -> 49.8 (+6.5 %), mid-race 59.7 -> 64.5, the route 58.7 -> 63.3.
One change kept (`de0c0f00`), two hypotheses dropped with their numbers above.

What the Redmi 9 must still confirm, at 2x: that change 1 gains there too (its scanout was about
1.5 ms of a 21 ms GPU frame, so 5 to 7 % is expected where its GPU limits); that the Mali driver
takes `present_direct` (a plain fragment shader with one sample); and whether
`lane247-pass-order` is worth more on a Mali than the 1 % here.

What to try next: Native VU's cost on the GPU at 4x (surprise 3) is the largest number in this
lane's data: 6 % of the race between on and off with the same picture.
