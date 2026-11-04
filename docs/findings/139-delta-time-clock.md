# 139: one delta-time game clock

Status: current
Supersedes: parts of 049, 052, 067
Checked: 2026-11-04

Branch `feat/delta-time-clock` (from main at 0.7.7 + findings 137/138), 2026-11-02.

## The report

A player on a weak laptop (i7-7500U 2 cores / 4 threads, GeForce 920M, 120 Hz screen), 0.7.7 at
FPS=120 SCALE=3. Their dhd.log: races at 66-84 fps of 120, 1% lows 26-60 fps, `limit=cpu:vu` or
`gpu`. At 66 fps the game and the sound ran in slow motion, then fast for a second or two (a
"speed ramp"); near other riders 45 fps, also slow motion; V-Sync on at 120 locked to 60 (the
surface offered Mailbox, Fifo, Immediate).

What 0.7.7 did (findings 026, 052, 067, 114):

- **At 60** every frame was one fixed 1/60 s step (`kernel.c` `pace`, the original clock). A frame
  slower than a field moved the game only one field: below 60 fps the game ran slower than real
  time (45 fps: x0.75). The sound was fed one field per VBLANK, so it slowed with it and the
  output time-stretched.
- **Above 60** (`fps.c` `clock_interval`, wall mode) each step covered the wall time since the
  last frame, but at most one field. The rest was kept as debt (offline up to 30 fields) and
  repaid by up to a whole field extra per step: a fast-forward after every slow stretch. Below 60
  fps the cap made it the same slow motion as at 60. The sound got at most a field per VBLANK.
- **Online at 60** the shared pacing gave a late PC an extra field per frame (two fields a
  frame); above 60 the debt was repaid in full (finding 114).

The decision (the user's): one delta-time clock, as variable-rate games have and as the original
engine has. Finding 049: the PS2 clock (`func_00238C00` / `func_00238D98`) steps by the fields
since the last frame, clamped to 1..2 (`D_0029E3D0`), with `D_0029E3FC` steps a second and
`D_0029E400` seconds a step, so the original keeps its speed down to 30 fps.

## What the clock does now

`port/hle/fps.c`, `port_clock_wait` / `clock_interval` (called through `func_00238C00`):

- Every paced frame, at every selected rate (30 to 240, 60 included), offline and online, the
  step is the real time since the last frame, clamped to **[one step at the selected rate, two
  fields]**: 1/30 s, the original's own limit. It goes through the game's own step variables
  (`D_0029E3D0` whole fields 0-2, `D_0029E3D4`/`D_0029E3FC` steps a second, `D_0029E400`
  seconds a step), so the game's code for a two-field step is the code that runs.
- **Smoothing that conserves time.** The time owed to the game gains each frame's wall time and
  gives half of it as the step: an exponential average of the frame times with weight 1/2. Jitter
  reaches the physics at a third of its size (frames alternating 9 / 17 ms give steps of 11.7 /
  14.3 ms; unit test), a hitch is spread over the next three or four frames, and the steps add
  up to the wall time less what is still owed (one frame's worth). Weight 1/2 is the least
  smoothing that takes most of the alternating jitter out; more would make a real change of frame
  rate show as a ramp.
- **No debt, no fast-forward.** Nothing is kept beyond the filter's own lag. What the two-field
  cap leaves over (frames slower than 1/30 s, a stall, a window drag) is dropped offline: below 30
  fps the game runs slower than real time (25 fps: x0.83), the cap keeps every system within the
  steps it was made for.
- **Online** (race screen, phases 1-4) the time the cap leaves over is kept and given back at up
  to two fields a step once the frames are quicker, so this player's race clock stays true to wall
  time below 30 fps too (finding 114's reason: the others see this rider where its own race clock
  is). Above 30 fps there is nothing to catch up. At a selected 30 the step is always two fields
  and nothing can be caught up.
- **Deterministic runs** (`DHD_FRAME_CLOCK=1`: the state trace, `race_compare.py`,
  `test_fps_race.py`, benches) keep the rational clock: 60/N fields a step, whole fields from an
  integer phase. At 60 that is the original arithmetic, untouched (`port_delta_clock()` is 0):
  the state trace is unchanged (below). `DHD_STEP_CLOCK=1` runs the delta-time code paths
  deterministically at 60 (the drift check's reference).
- **Sound** (`kernel.c` `vsync`, `port_vblank_seconds`): on the delta-time clock the mixer gets
  the wall time since the last VBLANK, at most two fields per VBLANK (with a 0.1 s catch-up, as
  before), in 59.94 Hz ticks of `sound_tick`. The sound plays at its speed whenever the game does,
  and below 30 fps sound and game slow down together.
- **Pictures**: one per game step, as before. Nothing interpolated or synthesized (finding 049).
- dhd.log says `fps: requested N steps/s at most`, and after every 30 s of race a new **`clock:`**
  line: the race clock against wall time (`game x1.000`), the slowest and fastest whole second
  (a fast-forward shows as a fastest second over 1), the smallest and largest step in fields,
  and the sound rendered against wall time. A player's report now shows directly whether the
  game kept real time.

## What was replaced (deleted)

- `fps.c`: the wall debt (`wall_error`), its 30-field offline cap and eighth-a-step repayment, the
  one-field step cap; `port_fps_wall_mode` (now `port_clock_wall` at start and
  `port_clock_rebase` for waits that are no game time); the "60 keeps NTSC steps" split.
  `port_fps_high()` is now `port_delta_clock()` (about 120 guards in `patches_fps.py` and
  `recomp.py`): the 067 code paths run whenever the delta-time clock does, at 60 too.
- `kernel.c`: the shared pacing's extra fields (`kernel_pace_extra_fields`, `extras`,
  `frame_waits`), the per-VBLANK `sound_tick` while paced, and `DHD_PACE=wall` (wall-time
  fields that the game measured as 2 or 3: replaced by the clock). The online log line has no
  "extra fields" now; its "ms late" is the time owed.
- `DHD_FPS` takes 30-59 too (the settings still offer 60 and up; the setting is a cap).

## V-Sync above 60 Hz

The halving was the swapchain, not the present mode: 0.7.7 already took Mailbox with V-Sync on
where offered (Fifo otherwise), but with frame latency 1 wgpu makes two buffers (DXGI flip model:
`BufferCount = latency + 1`; Vulkan `minImageCount`; Metal `maximumDrawableCount`). One is on the
display, one is drawn: a picture that misses a refresh holds the next acquire until the refresh
after, so 85 fps on a 120 Hz screen shows 60, with Mailbox too. `port/render/src/present/surface.rs`
now asks for latency 2 (three buffers) whenever the present waits for the display (Mailbox or
Fifo); Immediate (V-Sync off) keeps 1; `DHD_FRAME_LATENCY` still overrides. It is the same surface
code on Windows (DXGI), Linux and Android (Vulkan) and macOS (Metal, which offers no Mailbox: Fifo
with three drawables no longer halves either). The browser's canvas has no present modes (frames
follow requestAnimationFrame) and is unchanged. Finding 108 measured latency 2 on the Mi 11i as no
loss. Not measured on a 120 Hz screen here (the test PC's hidden desktop has none): it needs a
player's check.

## Two-field and fractional steps in what finding 067 fixed

Before this change the 067 paths only ever saw steps of at most one field (60/N <= step <= 1).
Now they also see 1 to 2 fields (45 fps: 1.33; 30 fps: 2), with up to two whole fields a step.

| System (067) | How 1-2 field steps are handled | Checked by |
|---|---|---|
| Clock, whole-field timers (class A, `func_00238F60`) | accumulator gives 0-2 whole fields a step; retail handles 2 | `test_fps.py` at 30/45: 600 timer fields in 10 s |
| **Per-frame counters 067 made whole-field** | **13 broke** with 2 whole fields (below): countdowns stop at 0 (`port_count_down`), exact marks fire once when passed (`port_count_reached`), the AI's rotating slot moves one rider a field-ending update | unit tests at 30/45; source read of every converted counter |
| Rider physics: impulses, damping, steering, air counter, boost | float step, `powf` decays, fields-elapsed air age; **the take-off step counts at most one field** (it counted 2 at 30: every bump was "airborne", `00138560` `x4A4 > 1`, and riders crashed every few seconds) | `test_fps.py` (thrust 300.000 at 30, 300.0001 at 45; decay, blend); drift race |
| Ragdoll (Verlet) | dt and dt^2 for any step, previous span converted | `test_fps.py` paced: point and accelerated point exact at steps up to 2 |
| Animations | 1024 subticks a field, any step | `test_fps.py`: 600 x 1024 subticks in 10 s at 30/45 |
| Particles | fractional sums, decays, lifetimes | `test_fps.py` at 30/45: displacement 8.983825 (reference 8.983825), lifetimes within a step |
| AI | whole-field gating, rotating slot (fixed), timers | drift race (AI finishes) |
| Sound triggers / envelopes | envelopes by step | `test_fps_envelopes.py` at 30/45 and 60 on the delta clock: within one step of retail 60 |
| Menus / input | input edges per update, action lock by fields | `test_input_edges.py` at 30/45/60 with jittered steps up to 2 |
| Mixer | wall time per VBLANK, up to two fields | `test_fps.py` (VBLANK time), slow-PC runs (below) |
| Replays / ghost sampler | off on the delta-time clock (as above 60 since 067) | now also off at paced 60: see Open |

The counters that two whole fields broke (retail took one off a frame, and tests for exactly
0 or an exact value; 067's whole-field step jumps over it):

- `001B4900` the post-race countdown (`x508 == 0` starts the results): 1 - 2 = -1, never 0.
- `0017EB00` the action lock (`counts != 0`): stuck below 0, the action never comes back.
- `0017C168` the menu flag countdown (`--x1318 == 0`).
- `00142508` an item timer (`if (timer) timer--`, `!timer` changes state): stuck.
- `001425E0` a creature cooldown (`x64 == 0` re-arms): never re-armed.
- `001207F8`, `0015B018` NPC timers (`if (--x)`, `--x != 0`): ran on forever.
- `00161B10` the bike trail's unsigned short countdown (`x2724`): 1 - 2 wrapped to 65535.
- `0021F8E0` the input repeat (`next <= 0` calls the handler): below 0 it called it every frame.
- `00161B10` the trail counter's marks `== 0x79` / `== 0xF0` (also counted again on updates
  with no whole field above 60, now fixed too) and `0017C638` the item list's `state == 60`
  event: jumped over. (Review, 2026-11-03: its `x1310 == 1` flag clear too, which the first fix
  missed: from 2 a two-field step never calls it at 1. Both marks now fire when the update
  covers them, and the copy into `x1318` is the last value covered, not 61; `test_fps.py`
  checks it at every rate. Second round, Muse N1: `x1310` itself came off as `-= whole`, so a
  two-field step from 1 stored -1 where retail, whose per-field call is inside `x1310 > 0`,
  rests at 0; no reader told them apart (all test `> 0`), but on the delta-time clock it now
  counts down with `port_count_down`, stopping at 0; the original clock keeps `--x1310`.)
- `0011A100` the AI's rotating query slot (`x501B4`): moved by two, every other rider was never
  queried.
- `00135A30` the airborne age (`x4A4`, `port_air_tick`): the take-off step counted the whole two
  fields, so `00138560` (`x4A4 > 1`, "in the air") was true on the first airborne step of every
  bump; retail counts that frame as 1 at any step. Found by the drift race (below), not by
  reading: the counters above were found by reading every converted counter's tests.

The other converted counters test `<= 0`, `< 0`, `>` or `>=` and are right with any step (listed
in the commit's source read: `0011A100 x501C0`, `001B4688`, `001B4718`, `0012F6A0`, `0012CF68`,
`00155380`, `00147BD8`, `0012AD60`, `0013DF28`, `0011FF38`, `001517D0`, `0017B3C0`, `00176ED8`,
`00136F80`, `00228BE8`/`0022B708` lifetimes, and the up-counters `0021F8E0` repeat, `0017E440`,
`001EB1D8`, `001B0410`, `001100D0`/`001103B0`, `0012CF68`, `00155208`, `00154058`, `00178730`,
`001A4F70`, `001E6970`, `001E6F58`, `00165B90`). Retail's own two-field code (`func_00238F60`
users) needs nothing.

## Slow PC on a fast one: before and after

`DHD_SLOW_FRAME=MS,JITTER` (new test switch, `kernel.c`): each game frame lasts at least MS ms,
give or take JITTER (uniform, fixed random sequence), busy on the EE thread at its VBLANK.
`port/tools/slow_race.py` (new) rides the race route (seed 81257, Cross held) on the hidden
desktop, paced as players run it (threaded graphics, scale 1, timed run), the sound device open at
zero gain (`DHD_SND_MUTE=1`, new) with its per-second trace, 60 s of race each. Before = main +
the instrumentation commit (c5d00e8); after = the new clock (b090116; the later counter and
take-off fixes don't touch the clock). RTX 3070 desktop, 144 Hz display, V-Sync off.
"Seconds" is the slowest and fastest 1-s window of race clock against wall time (a fast-forward
shows above 1); "stretched" is how long the sound output time-stretched because the game rendered
too little sound.

| FPS setting | frames | fps (low 1%) | game speed before -> after | seconds before -> after | steps after (fields) | sound before -> after | stretched before -> after (s / 60 s) | underruns before -> after |
|---|---|---|---|---|---|---|---|---|
| 60 | 22.2 +- 6 ms | 44.9 (30) / 44.5 (18) | **x0.746 -> x0.998** | 0.706-0.777 -> 0.928-1.017 | 0.99-1.99 | x0.746 -> x0.999 | 57.1 -> 5.3 | 0 -> 1 |
| 60 | 15.2 +- 9 ms | 59.8 / 59.2 | x0.996 -> x0.999 | 0.956-1.030 -> 0.967-1.045 | 0.99-1.88 | x0.996 -> x0.999 | 8.9 -> 2.4 | 0 -> 0 |
| 60 | 11.8 +- 4 ms | 59.6 / 59.4 | x0.993 -> x0.999 | 0.916-1.025 -> 0.957-1.049 | 0.99-1.99 | x0.993 -> x0.999 | 6.0 -> 0.8 | 4 -> 0 |
| 120 | 22.2 +- 6 ms | 44.9 / 44.8 | **x0.747 -> x0.999** | 0.706-0.784 -> 0.986-1.012 | 1.01-1.84 | x0.747 -> x0.999 | 57.0 -> 6.1 | 1 -> 0 |
| 120 | 15.2 +- 9 ms | 65.5 / 64.8 | x1.000 -> x0.999 | 0.947-1.042 -> 0.981-1.020 | 0.49-1.99 | x0.999 -> x1.000 | 7.7 -> 4.3 | 0 -> 0 |
| 120 | 11.8 +- 4 ms | 82.9 / 83.2 | x0.999 -> x0.999 | **0.866-1.125** -> 0.965-1.035 | 0.50-1.74 | x0.999 -> x0.999 | 1.9 -> 0.7 | 2 -> 0 |

- At 45 fps the game ran at three quarters of its speed at both settings, and the sound
  time-stretched the whole race; now it keeps real time and the sound is mostly copied.
- At 66 and 85 fps 0.7.7 at 120 kept real time on average only by repaying its debt: seconds at
  x0.87 then x1.13 (the reported slow motion and speed ramp; a player whose 1% lows are 26 fps
  sees more of it). Now the worst second is within 2-5%, which is the 1-s window's own noise
  (the race clock counts whole fields).
- "fps 60" with frames of 15.2 +- 9 ms: 0.7.7's pacing already let quick frames repay slow ones
  within two periods (`pace`), so it was close to real time there.
- The sound still stretches a little at 45 fps (5-6 s a minute): the device's speed estimate sees
  sound arriving in 22-ms bursts. Underruns: 0-1 a minute, as before.

## Does the race drift with the frame rate?

`port/tools/rate_drift.py` (new): the same scripted race (race_compare's menu route, seed 81257,
Cross held: full pedal) on the deterministic stepped clock at 30, 36, 40, 45, 60, 90, 120 and 144
steps a second (60/N fields a step), headless, recording every rider each race tick, until the
race ends (at most 500 s).
"60" is the original arithmetic; "60d" is the delta-time code paths at one field a step, the
reference for the others (paced players at 60 now run those paths).

| Steps a second (fields a step) | Player finish | vs 60d | AI finished, mean | vs 60d | Player crashes in the first 167 s |
|---|---|---|---|---|---|
| 30 (2.00), before the take-off fix | no finish in 500 s | | 0 of 9 | | 41 (crashed 27% of the time) |
| **30 (2.00)** | 444.900 s | +6.20% | 9, 327.94 s | +1.38% | 9 |
| 36 (1.67) | 388.450 s | -7.27% | 9, 321.99 s | -0.46% | 7 |
| 40 (1.50) | 424.950 s | +1.44% | 9, 336.86 s | +4.14% | 8 |
| 45 (1.33) | 414.467 s | -1.06% | 9, 327.07 s | +1.11% | 6 |
| 60, original arithmetic | no finish (held at checkpoint 9 from about 120 s) | | 9, 298.59 s | -7.69% | 6 |
| **60d (1.00), reference** | 418.917 s | | 9, 323.47 s | | 6 |
| 90 (0.67) | 438.450 s | +4.66% | 9, 324.61 s | +0.35% | 4 |
| 120 (0.50) | 417.183 s | -0.41% | 9, 325.06 s | +0.49% | 8 |
| 144 (0.42) | 410.367 s | -2.04% | 9, 322.40 s | -0.33% | 8 |

(90/120/144 and the original 60 ran on the build before the take-off fix, which changes nothing
at steps of a field or less; 60d gave the same 418.917 s on both builds.)

- **Two-field steps were broken, now they ride.** Before the take-off fix every rider at 30
  crashed every few seconds (next section), and nobody finished. Now 30 finishes like the others.
- **No drift with the rate.** The differences have no trend (30 +6.2%, 36 -7.3%, 40 +1.4%, 45
  -1.1%, 90 +4.7%, 120 -0.4%, 144 -2.0% for the player; AI means -0.5% to +4.1%), and they are
  no larger than the difference between the two arithmetics at the same 60 (the original path's
  player hangs on a wall at checkpoint 9 for the rest of the run, its AI are 7.7% quicker). The
  full-pedal race is chaotic, as finding 067 found: one crash or wall contact that differs moves
  a finish by tens of seconds, and positions at the same race-clock moment are hundreds of units
  apart after the first such event (OUT/summary.txt lists them; 10 s in, 34-316 units). These
  are one seed and one script: a statistical equivalence (many seeds) is finding 067's open
  question, now including steps of 1-2 fields.
- The crash count is the measure that showed the broken two-field step clearly (41 against 4-9),
  and it now sits with the others.
- The 0.7.7 player on a 66-fps laptop now gets steps of 0.9-2 fields with the same rules.

## Online

Each game times its own race (finding 044); with game time equal to wall time down to 30 fps the
online catch-up is not needed above 30, and below 30 the clock keeps the time and gives it back
(above). Puppets, hits and finishes count race time in seconds with the part of a field
(`port_field_seconds`, finding 114), which is now 1/60 s a field at 60 too (0.7.7 at 60:
1001/60000 s; the offsets track either). `rematch_fields` and the puppet rings use whole fields
and race seconds, which any step size keeps.

One-PC puppet check on the hidden desktop (puppet.c's test hooks, finding 114): rider 0 recorded
at 60 (`DHD_PUPPET_RECORD`, 3607 steps), then replayed as a puppet onto slot 1 of a game whose
frames take 22.2 +- 6 ms (45 fps, 1.33-field steps) with `DHD_PUPPET_TRACE`. Over 1345 puppet
steps the remote race time shown follows the local race clock with slope 1.00002 and a steady lag
of 111 ms (p5-p95 109.6-113.2: the 100 ms delay plus the snapshot spacing), never going back; the
puppet's position against the recording at the time it shows: median 0.007, p95 0.046, max 0.39
game units (114 measured 0.010 / 0.048 / 0.57 between two PCs at 60 and 144). Hits travel as
events with the race time and the finish is the local race clock's (finding 044), so neither
depends on the step. Not run: a two-PC race with a slow peer.

## The steps a second as a time step (review, 2026-11-03)

About twenty units take their time step from the integer steps a second (`func_00238F30`,
`D_0029E3D4`) as `1/rate`: the rigid-body integrator `002327B0` (through `001460D0`), the contact
solver (`00131998`, `00133DE0`), `00209248`'s callers (`00147F40`, `00148560`, `0016F198`,
`0016FC00`, `00210868`, `00148F60`) and `00157BE8`, `001DCE58`, `00165E68`, `001AB418`,
`00154058`, `001457A8`, `00180A10`; `00145E80`, `00170278` and `00131998`'s angular step read the
rate itself. Retail's rate is exact (60, or 30 on a two-field frame). The delta-time clock set it
to `(int)(60/step)`, truncated: paced 60 holds pictures 1001/60000 s, so the step is about 1.001
fields and the rate was 59 on nearly every step, a time step 1.6 % longer than the step itself
(the review's probe: 1.2 % over a run at 60, 0.4 % at 144). Bodies moved that much too far per
unit of speed wherever the clock is not on the offline race's field grid (whose codes give exact
rates): online races, the attract demo, the menus.

Now every reader but `0021F210`'s `== 60` test (already off on this clock) reads the exact step
on the delta-time clock: `patches_fps.py` rewrites `1.0f / (float)func_00238F30()` as
`port_step_seconds()` (`D_0029E400`) and the other reads as `port_step_rate()` (`D_0029E3FC`),
22 reads in 18 units, found by a search of every unit (the asm-only callers are the same
functions). On the original path both return retail's own expressions. `D_0029E3D4` itself is
now rounded, for any integer reader. `test_fps.py` checks it at every rate on the paced clock:
3000 frames paced on the selected period, each 10-200 us late as `pace()` leaves them, give the
readers' time step equal to `D_0029E400` within 0.1 % on every step, and an integer rate of 60
at 60 (it was 59). No `rate_drift.py` row: that check runs the rational clock, whose rate is
exact.

The original path has to keep the division in the unit: a first version returned
`1.0f / (float)D_0029E3D4` from `fps.c` on that path, and the state trace's race left main's at
field 3214. The game's float code runs in the EE's rounding mode (toward zero), the port's own
code to nearest, so `fps.c`'s 1/60 is 0x3C888889 where retail's is 0x3C888888. With
`(port_delta_clock() ? port_step_seconds() : <retail expression>)` in the unit, the race (5000
fields) and boot (600) traces match a build of 60ba1c2 row for row, and `replay_check.py` passes
at 45 (frame clock, 27010 rider positions) and paced 144 with `--slow 9,4` (35980), every
coordinate equal.

## Checks

- State trace (`state_trace_run.py`, deterministic, the original clock): race 5000 fields
  **identical** to main's on the clock build and on the final build (dfa9df0; main = the before
  build: all 5000 rows match), boot 600 fields identical (also to `next077-ref`). The shared
  next/0.7.7 race reference differs from main itself from field 3231 (sound, then RAM): main has
  later core commits (`9b6d871`, `cf6b425` sound, online ones); main's own trace is the
  comparison.
- Unit tests: `test_fps.py` at 30, 45, 60, 61, 75, 90, 100, 120, 144, 165, 240, each on the fixed
  and the paced clock: time conserved within a frame over 40-200 s of jitter and a 50-ms hitch,
  jitter attenuated to a third, a hitch repaid within ten frames with no step over two fields, 25
  fps offline x0.83 with at most two fields owed and no fast-forward after, 25 fps online owing
  1.68 s and catching it up (not at 30, where nothing can be), the mixer's VBLANK time, the
  countdown/mark helpers, the take-off step. `test_fps_envelopes.py` (30, 45, 60 on both clocks),
  `test_input_edges.py` (30-240, jittered steps up to two fields), `test_defined_producers.py`,
  `test_proximity_float.py`: pass. `test_runtime.py`: every test up to `queued-vif` passes
  (keyboard ramps, sound, timers, SIF, GS, VU1, VIF); `queued-vif` does not compile
  (`graphics_queue.c`: `timeline_enabled` undeclared under `GRAPHICS_QUEUE_TEST`), a file this
  branch does not change.
- Builds: core and Windows x64 (`build.py`), four times on the branch. Not built: macOS, Linux,
  Android, the browser (shared code only; the renderer change is one function in the shared
  surface code).

## Open

- **Ghosts and replays** (`001A8DE8` recorder/playback, `00138E08` ghost sampler, mode 18) are
  off on the delta-time clock since 067 (their format holds one sample a frame with a step of 1-2
  fields). Paced 60 now runs the delta-time clock, so they are off for every player. Whether the
  ghost mode is reachable in the menus, and a recorder that samples on whole fields, are open.
- **Animation timestamps** count 1/1024 field in a 32-bit int on the delta-time clock: they wrap
  after about 9.7 hours of play in one session (above 60 since 067; now at 60 too).
- Finding 067's open question (physics equivalence between step sizes) now covers steps of 1-2
  fields too: see the drift table.
- V-Sync with three buffers is not measured on a 120 Hz screen, and online races with a slow
  peer were not run between two PCs (`cross_race.py` both ways before a release, as for every
  pacing change).
