# 172: Fewer state changes per draw on the render thread

Status: partly superseded by 195 (the constants per draw by instance index: undone, the draw constants are a uniform block at a dynamic offset again)
Checked: 2026-11-10

Branch `native/vu-gs`, after 168's `22a5c2d`, started 2026-11-06. Merged into `main` with the rest of `native/vu-gs` at 32df819b on 2026-11-10, behind finding
194's Native VU setting (off by default); ships in 0.7.15. The lane was wrapped up on the lead's word the same night: one step
kept (the constants), the others measured and dropped (Decisions).

## Goal and acceptance

Finding 168: with the native VU1 path on, the Mi 11i on the t612 profile at 2x is limited by the
render thread, and mostly by wgpu turning the recorded draws into Vulkan commands
(`encoder.finish` about 6.3 ms and `queue.submit` about 2.8 ms a picture, about 13 µs a draw), driven
by the state changes of every draw, not only the mesh draws: about 383 texture binds, 255-295
constants binds and 180-225 pipeline changes a race picture. Batching the mesh draws (168) cut
draws but not time.

Goal: cut the render thread's cost per picture for all draws, one code path for every platform:
- **Texture binds:** textures in arrays or atlases (by format and size class), the draw selecting
  its layer/region from its constants or vertex data, so consecutive draws with different
  textures share one bind group; or bindless-style descriptor indexing where wgpu offers it on
  every platform we ship (check Vulkan on Adreno/Mali/PowerVR, D3D12, Metal; fall back to arrays).
- **Constants:** looked up per draw from one storage buffer (an index per draw: a push constant or
  a vertex attribute) instead of a dynamic-offset rebind per draw.
- **Pipelines:** one shared layout for the mesh and GS draw pipelines (168's
  layout patch, kept outside the repository), fewer pipeline variants switched per picture
  where specialisation constants can become uniform branches without a GPU cost.
- Measure each step with `DHD_RSTAT`'s counters and the render thread's ms (encoder finish,
  submit) before keeping it.

Acceptance: pictures identical to before (or within finding 167's tolerance; say which) at 2x and
4x over finding 156's route, switch off and on; boot trace while iterating, race trace once at the
end; `check.py port`; the Mi 11i on t612 at 2x only (the owner's rule), switch on before/after, and
switch off before/after (the GS path's draws change too); desktop timing only if the owner's PC is
free and no other session uses the Mac.

## Progress

- [x] 2026-11-06: finding reserved
- [x] the upper bounds first: experiment switches in a scratch build (not in the branch) that give
  every draw the same texture or the same constants (wrong pictures, timing only), and 168's
  shared-layout patch; timed on the Mi 11i
- [x] constants per draw (`1b9e925`): pictures identical at 2x and 4x, switch on and off; boot
  traces equal; Mi 11i timed
- [x] texture arrays: built, dropped (the race load hangs at 4x and with the switch off; few binds
  saved); kept outside the repository
- [x] the Redmi 9 (Helio G80, Mali-G52 MC2) as the low-end phone: before/after at 1x and 2x, switch
  on and off, and the generic pipelines; a shader variant reading the constants where used
- [x] race state traces; `check.py port` (every step OK, at two build slots)

## The change kept: constants per draw

**Undone by finding 195** (`140a7d2`): indexing the constants per draw cost the GPU (Mali-G52: more
cycles a pixel and a vertex, the constants out of uniform registers; the 8 % below on the Redmi 9
at 2x). The draw constants keep this step's own arena, 256 bytes apart, but are a uniform block at
a dynamic offset again, group 0 bound only when a draw's constants differ from the last one's. The
rest of this section describes the step as it was.

Before, each draw's constants (112 bytes since 168) sat at its own dynamic offset in the uniform
arena, so group 0 was bound again at every draw whose constants changed: 295 group 0 binds a
race-start picture with the switch on, 255 off. Now the draws' constants go into their own arena,
packed 112 bytes apart; the draw pipelines' group 0 binds a window of 256 of them (28 KB, its own
layout with the minimum size given), and each draw reads its own by its instance index
(`first_instance`), which its vertex shader passes to its pixels as a flat varying; `native.wgsl`'s
draw entry points load them first. Group 0 then changes once per 256 distinct constants and at a
new pass: **295 -> 19 binds a race-start picture (switch on), 255 -> 18 (off)**. One path for every
platform (no feature check: the instance index and a uniform array exist everywhere wgpu runs).

## Timing: the Mi 11i on the t612 profile, 2x

Finding 168's method (release-signed APKs with the shipped builds' PGO profile, finding 156's run
script, the frozen t612 profile, the clocks at its caps in every run). Two sessions: the
experiment build (`x`: the switches above, 168's code otherwise) and before/after (168's APK
against `1b9e925`). Per scene, ms a field from the timeline, route_profile.py's columns:

| scene | run | fps | field | game work / busy | VU work / busy | GS work / busy | render CPU / busy |
|---|---|---:|---:|---|---|---|---|
| start | before, on | 61.5 | 16.27 | 10.37 / 11.67 | 11.61 / 12.74 | 9.27 / 15.93 | 14.41 / 13.52 |
| start | after, on | 60.1 | 16.65 | 10.53 / 11.75 | 11.74 / 12.90 | 9.40 / 16.12 | 14.28 / 13.42 |
| start | before, off | 59.0 | 16.95 | 11.26 / 12.45 | 14.60 / 16.35 | 14.95 / 15.67 | 13.62 / 12.65 |
| start | after, off | 58.4 | 17.14 | 11.24 / 12.31 | 14.76 / 16.48 | 15.02 / 15.73 | 13.22 / 12.22 |
| start | x: shared layout, on | 58.9 | 16.98 | 10.78 / 12.23 | 12.01 / 13.20 | 9.41 / 15.93 | 14.63 / 13.91 |
| start | x: one texture, on | 67.4 | 14.83 | 10.45 / 11.95 | 11.29 / 12.63 | 9.21 / 13.71 | 13.28 / 12.37 |
| start | x: one constants, on | 67.1 | 14.90 | 10.42 / 11.87 | 11.41 / 12.68 | 9.22 / 13.82 | 13.76 / 12.85 |
| start | x: both, on | 71.6 | 13.96 | 10.72 / 12.31 | 11.33 / 12.81 | 9.77 / 10.40 | 12.10 / 11.19 |
| route | before, on | 74.8 | 13.37 | 7.03 / 8.05 | 9.83 / 11.22 | 7.62 / 11.14 | 10.50 / 9.56 |
| route | after, on | 74.7 | 13.39 | 7.03 / 8.04 | 9.86 / 11.24 | 7.65 / 11.14 | 10.42 / 9.44 |
| route | after + one texture, on | 78.5 | 12.74 | 7.16 / 8.17 | 9.64 / 11.15 | 7.83 / 9.28 | 9.47 / 8.46 |
| route | before, off | 64.0 | 15.61 | 7.74 / 8.01 | 12.94 / 14.47 | 12.65 / 13.55 | 9.62 / 8.59 |
| route | after, off | 64.0 | 15.62 | 7.78 / 8.02 | 12.98 / 14.50 | 12.66 / 13.53 | 9.35 / 8.34 |
| route | x: base, on | 74.0 | 13.52 | 6.98 / 7.98 | 9.91 / 11.26 | 7.66 / 11.24 | 10.68 / 9.69 |
| route | x: shared layout, on | 74.0 | 13.52 | 7.07 / 8.10 | 9.94 / 11.32 | 7.64 / 11.21 | 10.62 / 9.65 |
| route | x: one texture, on | 77.7 | 12.87 | 7.14 / 8.20 | 9.68 / 11.18 | 7.75 / 9.79 | 9.75 / 8.76 |
| route | x: one constants, on | 75.8 | 13.18 | 6.99 / 7.99 | 9.81 / 11.20 | 7.61 / 10.77 | 10.22 / 9.18 |
| route | x: both, on | 79.2 | 12.63 | 7.20 / 8.24 | 9.63 / 11.16 | 7.90 / 8.78 | 9.15 / 8.11 |

`DHD_RSTAT` at the race start, ms a picture and counts a picture (group 0 = constants, group 1 =
textures):

| run | picture | record | finish | submit | present | draws | group 0 | group 1 | pipelines |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| before, on | 12.44 | 2.02 | 6.30 | 2.91 | 1.63 | 457 | 295 | 383 | 225 |
| after, on | 12.34 | 2.04 | 6.02 | 3.02 | 1.97 | 457 | 19 | 383 | 225 |
| before, off | 11.45 | 1.98 | 5.61 | 2.81 | 1.25 | 425 | 255 | 383 | 180 |
| after, off | 11.19 | 1.93 | 5.20 | 2.97 | 1.44 | 425 | 18 | 383 | 180 |
| x: shared layout, on | 12.76 | 2.04 | 6.46 | 3.05 | 1.64 | 457 | 295 | 383 | 225 |
| x: one texture, on | 11.17 | 1.83 | 5.35 | 2.81 | 1.25 | 317 | 295 | 50 | 225 |
| x: one constants, on | 11.48 | 1.83 | 5.81 | 2.70 | 0.94 | 457 | 18 | 383 | 225 |
| after + one texture, on | 10.73 | 1.77 | 4.85 | 2.89 | 1.59 | 317 | 19 | 50 | 225 |

So on the Mi 11i the constants step takes 0.3-0.4 ms off `finish` and leaves the fields a second
where they were (74.8 -> 74.7 on, 64.0 -> 64.0 off): the render thread still limits there, and the
texture binds (383) are the larger part of what it replays. The experiment switches overstate what
a real change gives: one texture or one set of constants for every draw also cuts the GPU's work
(fewer texture fetches, one set of fog and colour values), so "one constants" (+1.8 fields/s) is
above what `1b9e925` gave (none), and "one texture" (+3.7 to +3.8) is a ceiling for the textures.

## Timing: the Redmi 9 (Helio G80, Mali-G52 MC2), 1x and 2x

The low-end phone from 2026-11-06 (not rooted): debuggable APKs (debug key, the shipped builds' PGO
profile, release-optimised native code), the disc and `dhd.ini` in the app's own files through
`run-as` (finding 091), stock clocks (no throttle profile), a 90 s cool-down with the screen on and
the game closed before each run, other apps closed; CPU clocks and the battery temperature sampled
every 5 s (28.8-34.3 C over the runs); the GPU clock is not readable without root. Same route and scenes as above; race start = fields 6400-7300, mid-race 9000-14000.
Session 1 ran in table order, session 2 (`r2`) ran before, the variant, after.

| run | route | race start | mid-race | race start: game / VU / GS work, busy / render busy, present wait (ms) | picture / finish (ms) |
|---|---:|---:|---:|---|---|
| before, off, 2x | 48.7 | 34.5 | 48.5 | 15.7 / 19.1 / 16.6, 28.9 / 18.4, 10.6 | 12.50 / 7.20 |
| before, on, 2x | 47.2 | 33.5 | 46.1 | 15.9 / 18.0 / 11.1, 29.8 / 19.1, 10.7 | 13.56 / 7.85 |
| before, off, 1x | 56.3 | 49.6 | 59.2 | 14.1 / 16.5 / 13.9, 20.0 / 16.1, 4.1 | 14.62 / 7.50 |
| before, on, 1x | 55.4 | 47.1 | 56.2 | 14.2 / 12.0 / 9.6, 21.1 / 16.9, 4.3 | 15.18 / 7.58 |
| after, on, 2x | 43.3 | 30.1 | 42.3 | 17.3 / 21.3 / 11.5, 33.2 / 18.4, 14.8 | 12.43 / 7.35 |
| after, off, 2x | 44.8 | 30.9 | 44.1 | 16.4 / 21.4 / 18.1, 32.3 / 18.3, 14.1 | 11.82 / 6.92 |
| after, on, 1x | 57.4 | 49.3 | 57.8 | 14.2 / 12.1 / 9.5, 20.2 / 16.2, 4.1 | 14.50 / 7.23 |
| after, off, 1x | 57.3 | 51.2 | 60.9 | 13.9 / 16.6 / 14.0, 19.3 / 15.2, 4.3 | 13.72 / 6.96 |
| after, on, 1x, generic pipelines | 40.6 | 29.0 | 39.2 | 17.7 / 23.3 / 11.8, 34.5 / 17.6, 17.0 | 11.56 / 7.07 |
| r2: before, on, 2x | 47.0 | 33.6 | 46.3 | 15.8 / 17.7 / 10.8, 29.7 / 19.4, 10.3 | 14.15 / 8.11 |
| r2: lazy reads, on, 2x | 42.3 | 29.4 | 41.4 | 17.7 / 21.4 / 11.4, 34.0 / 18.9, 15.2 | 12.41 / 7.41 |
| r2: after, on, 2x | 43.3 | 30.1 | 42.2 | 17.4 / 21.1 / 11.5, 33.3 / 19.0, 14.3 | 12.59 / 7.45 |

What limits the Redmi 9 (race start):
- **At 2x, the GPU.** The render thread is busy 18-19 ms of a 30-34 ms field and waits 10-15 ms
  in the present (for the GPU); the GS thread is busy the whole field behind it, the game thread
  works 16-18 ms. The native VU1 path's savings do not show here (on and off within about 3 %).
- **At 1x, the render thread.** Busy 15-17 ms plus about 4 ms in the present, the whole 20-21 ms
  field; wgpu's `finish` is about half of its picture (7.0-7.6 of 13.7-15.2 ms). The game thread
  (14 ms) and the VU thread (12 on, 16.5 off) have room; on and off within 2 %.

The constants step on the Redmi:
- **The render thread's picture is cheaper in every pair**: 0.7-1.6 ms less a race-start picture
  (`finish` 0.3-0.7 ms less), at both scales, on and off.
- **1x (render-thread bound): +2 to +4 %** over the route (on 55.4 -> 57.4, off 56.3 -> 57.3).
- **2x (GPU bound): about 8 % fewer fields a second, not explained.** On 47.2 / 47.0 -> 43.3 / 43.3,
  off 48.7 -> 44.8, the present wait up by about 4 ms a field: the GPU took longer per picture.
  Two readings fit and these runs cannot tell them apart: **heat from the run order** (the lead's
  read, the more likely one: every "after" 2x run ran after the "before" runs, 1-2 C warmer by the
  battery, and the GPU clock cannot be read without root to rule throttling out), or **a GPU cost
  of the per-draw lookup on the Mali** (a uniform array indexed per draw instead of a uniform block
  at a fixed address, which Mali drivers can load into registers ahead of the shader). The 1x
  runs cannot separate them (at a quarter of the pixels a GPU cost or a warm GPU shows less); the
  lazy-reads variant, which loads only the fields each pixel uses, did not get back what was lost,
  which speaks a little against a per-pixel load cost. **Not confirmed either way**; an
  interleaved before/after/before run at 2x on the Redmi decides it.

## Tried and dropped

- **One pipeline layout for mesh and GS draws** (168's follow-up): `finish` 6.39 -> 6.46 ms, the
  route 74.0 -> 74.0 on the Mi 11i. The layout changes between mesh and GS pipelines were not the
  cost 168 suspected. Not kept.
- **Texture arrays** (decoded textures as layers of arrays by format and size class, the layer in
  the draw's constants): the race-start group 1 binds went only 383 -> 341 (only decoded
  textures went into arrays; the rest, the textures read from the frame's own targets among them,
  stay separate binds), and the uploads into array layers raised the copies 294 -> 437 a picture.
  The race load hung: the first version grew the arrays by doubling and asked for too much memory;
  with a cap, 2x with the switch on loaded (pictures identical, all eleven fields) but 4x and the
  switch off still hung (cause not found). Dropped, kept outside the repository. The "one texture" ceiling (above) needs every texture source in one
  scheme (descriptor indexing over the targets too), not decoded textures alone.
- **Lazy reads** (the shader reads each constant where it uses it instead of copying the block
  first): no gain on the Redmi at 2x (42.3 against 43.3 for the kept step). Not kept.
- **Generic pipelines** (`DHD_NATIVE_SPEC=0`) to cut pipeline changes: 229 -> 176 a race-start
  picture, but the Redmi at 1x went 57.4 -> 40.6 fields a second (the GPU waits 17 ms a field in the
  present). The specialised pipelines stay; fewer pipeline changes would have to come without
  their GPU cost.

## Correctness

- **Pictures:** finding 156's route, the eleven fields at 2x and 4x, switch on and off, against
  168's captures: identical, every pixel of every field (`1b9e925`; so within 167's tolerance of
  the switch-off pictures as before).
- **Boot state traces** (600 fields, headless): shadow and skip mode equal to 168's in every row.
- **Race state traces** (5000 fields, the first track, headless, `DHD_NATIVE_VU1=all`): shadow and
  skip mode equal to 168's in every row.
- **`check.py port`:** every step OK at two build slots.

## Decisions

- **Wrapped up early** (the lead, 2026-11-06 night): no more phone sweeps; the Redmi runs taken are
  the record. The interleaved Redmi 2x run above is the open question for the constants step.
- **The constants step stays on the branch** (`1b9e925`): cheaper render-thread pictures on both
  phones, +2-4 % on the Redmi at 1x, no change on the Mi 11i; to be settled by the interleaved
  Redmi 2x run before any merge.
- **No desktop timing**: the owner was using the PC and another session the Mac.

## Surprises

- **The constants binds were not where the time was.** Taking 276 of 295 group 0 binds out of a
  picture saved 0.3 ms of `finish` on the Mi 11i: about 1 µs a bind, against 13 µs a draw overall.
  The time per replayed draw is mostly elsewhere (texture binds, the draw itself, the pipeline
  changes).
- **On the Redmi 9 at 2x the GPU limits, not the render thread.** The Mi 11i on t612 at 2x and the
  Redmi at 1x are render-thread bound; the Redmi at 2x waits on the Mali-G52, so render-thread
  work cannot show there, and anything that adds GPU work per pixel costs it directly (the generic
  pipelines: -29 % at 1x).

## Result

- **Kept (undone since by finding 195):** constants per draw by instance index (`1b9e925`): group 0 binds a race-start picture
  295 -> 19 (on), 255 -> 18 (off); pictures identical, traces equal, `check.py port` OK.
- **Mi 11i, t612, 2x:** `finish` 6.30 -> 6.02 ms (on), 5.61 -> 5.20 (off); fields a second unchanged
  (74.8 -> 74.7 on, 64.0 -> 64.0 off). Render-thread bound; texture binds are the bigger lever
  ("one texture" ceiling +5 %).
- **Redmi 9, Helio G80:** 1x render-thread bound, +2-4 % (55.4 -> 57.4 on, 56.3 -> 57.3 off); 2x
  GPU bound, 47.2 / 47.0 -> 43.3 / 43.3 on, 48.7 -> 44.8 off, likely heat from the run order but
  unconfirmed (above). Native VU1 on and off within about 3 % at both scales.
- **Dropped:** the shared pipeline layout (no gain), texture arrays (hang at 4x and switch off,
  341 of 383 binds left), lazy reads (no gain), generic pipelines (a large Mali GPU cost).
- **Left for the render thread:** the texture binds with every texture source in one scheme
  (descriptor indexing, including the frame's own targets), and on the Mali the GPU's work per
  picture at 2x.
