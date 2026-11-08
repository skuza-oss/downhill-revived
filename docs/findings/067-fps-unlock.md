# 067 - Real offline simulation steps (experimental; equivalence not established)

Status: partly superseded by 139 (the fixed 60 clock, the 144 cap and debt, online forced to 60, replays off)
Checked: 2026-11-04

Shipped in 0.2.9 (finding 053 §13) and kept: the rate-aware game fixes, whole-field timers, the
sub-field animation clock and fractional particles, checked by `port/tools/test_fps_race.py`.
Changed since: rates go to 240 and Monitor (`fps` in `port/render/settings/src/table.rs`);
online races run at each player's rate (finding 114, no longer forced to 60); one delta-time clock
steps every rate, 60 included, with no one-field cap or debt (finding 139); replays and ghosts
work at every rate (finding 142). The physics/AI equivalence limits measured below are still the
known limits (finding 139: no trend against 60 at 30-144 steps a second).

Nothing was published or merged when this was written.

Cleanup 0.2.5: the measurement scripts, probes, traces and rejected trials named below were removed
from the tree and remain in git history at `99d04692`. The kept check for the high-rate path is
`port/tools/test_fps_race.py` (fixed seed, unpaced, DHD_FPS_TRACE state compared row by row against
a reference run).

Latest continuation (2026-10-18): the input-edge fix passes the live keyboard, SDL and DHD_NAV
matrix at 60/120/144, and clean 43b5c068 passes the unchanged default r46-plain gate (14 EE
comparisons, 1,143 readback/Z records). See the input-edge continuation at the end. The earlier
physics-equivalence acceptance still FAILS; cap/force-overlap trials remain OFF. Launch isolation
and all 81 candidate AI finishes are retained below as historical, explicitly failed physics
evidence. This input fix does not establish high-rate physics parity.

Round 52 handoff: clean `167fa3e2` passes the default r46-plain gate. The retained full-pedal
fixture finishes in 392.517 / 432.733 / 402.400 seconds at 60/120/144. Early player speed meets the
requested 0.3% window, isolated particle lifetimes pass, but **120 race time and AI behavior still
fail equivalence**. The grounded recovery watchdog is an explicit policy addition. Final provenance
and remaining work are recorded at the end of this finding.

The implementation follows finding 049: a fractional offline simulation clock, whole-field integer
timers, rate-aware forces/decays, a sub-field animation clock and fractional particles. `src/` and
the retail matching checks stay unchanged. `fps=60` retains the original NTSC clock and arithmetic
paths (until finding 139's one clock). Higher rates were nominal integer rates (120/144), capped at
144 (now up to 240); `monitor` uses the primary desktop refresh. Online was forced to 60 by
`pads_online()` (no longer, finding 114). The setting applies at next start to avoid converting
live animation timestamps or changing integrator history mid-race. Replay recording/playback and
ghost sampling were disabled above 60 (back since finding 142).

This is an experimental implementation until the race comparison below passes; real update/draw
cadence alone does not establish equivalent handling.

## Implemented changes

- Clock: `00238A68/C00/D98` use one real offline update per selected interval,
  float seconds/rate accessors and a rational whole-field accumulator. Timer 1
  accounting remains physical bus time. Original 60 code remains the fallback.
- Pacer: the existing present grid and just-in-time pad refresh use the selected
  interval. Audio production stays on its NTSC field cadence. Online overrides
  simulation/presentation to 60, including a room entered through the menus.
- Rider: four per-step force producers use the original 60-force tuning; the
  direct low-speed velocity push also scales with time. Angular damping, steering
  lean/release, contact recovery, airborne age/first-frame handling, boost hold,
  summed average speed, pedal phase and pedal fade are corrected.
  The extended audit also found per-update yaw/pitch acceleration and release
  damping in `00137950` / `00137B70`, and direction-history smoothing in
  `00137300`. Their downstream rotations already use `60/sps`; their producers
  now scale acceleration by the fractional step and decay exponentially.
  The later contact audit found a fifth impulse-rate dependency: `00132CE0`
  clamps tire friction to a constant impulse per contact update, then applies
  it directly via `00231CC0` / `00231C88`, bypassing dt integration. The cap now
  scales by `h`; the unconstrained collision impulse remains unchanged. The
  skid-effect threshold in `00161B10` uses the same impulse units.
- AI scheduling: `0011A100`'s rotating query slot and 420-field leader-selection
  refresh retain their field cadence. `0010EC80`, `0010F0D8`, `0010FB00` and
  `00112360` only consume the rotating slot on whole-field updates, preventing
  repeated queries while the slot is held between field boundaries. Physics,
  steering evaluation and their timers still run each real step. `0011A938`'s
  `x501C4` is a freshly recomputed count of attacking riders, so it is not scaled.
- Shared smoothing: raw linear/angle helpers correct blend rates and minima;
  their rate-aware wrappers avoid applying that correction twice.
- Animation: integer timestamp layouts are retained with 1024 subticks per field
  in high-rate processes, including when online selects 60. Elapsed-time readers,
  blend paths (including assembly-only `00204F38`), animation update scheduling
  and float-tween callbacks use the fractional clock. Joining online does not
  reinterpret timestamps accumulated in a menu.
- Ragdoll: dt²/drag/soft-constraint scaling, frame counters, settling speed,
  ground friction and held-input impulses are adjusted. Initial displacement
  already spans one real animation step (`001B1C20`); it must not be scaled twice.
- Particles: fractional emission/motion, integer lifetime counters, the
  `0022B708` generated-C decay/integration path, specialized spray/trail paths,
  signed size/envelope slopes, trail velocity and trigger/delay timing.
  The assembly remains local input; no generated C or assembly is committed.
- Further audit: stuck-AI timeouts, crash recovery/blink timers, projectile
  lifetime/gravity, NPC cooldowns, a world-object phase/envelope, object rotation,
  light fades and vibration envelopes. Ambient 1-in-101 random events draw only
  on whole fields. Allocation counts, traversal indices and event totals are
  excluded from timer rewrites after reading their callers/bodies.
- Extended audit: gauge recovery and drains `001634E0` (`x14`/`x18`, initialized
  to 0.2/0.45, plus the base recovery/depletion). Its provisionally named `dt`
  field is **1.0 at both 60 and 144**, verified in the clean 60 GO RAM and live
  144 state; it is not elapsed time, so the base terms also need `h`. Manual falling
  acceleration/motion in `0016FC00`; rising/descending NPCs `001737F0` and
  `00173CB0`; object movement/rotation `001926E0`; radial decay/forcing
  `00142940`; per-slot movement/fade/timers `0017C130/168`; rider pose age
  `001B0410`; post-race countdowns `001B4688/4718/4900`; pose envelope
  `001AF5E8`; camera transition phases and offset rates in merged `00148F60`.
  `00223578` is the partial quaternion blend used by seven per-frame callers
  (camera attitude and ground-following actors). Its blend weight is now
  rate-aware; `00165B90`'s five-field correction window also stays five fields.
  Camera editor key increments, temporary vector scaling, counts rebuilt each
  call, and one-shot initialization are not treated as elapsed-time operations.
- Replays/ghosts: high-rate recording/playback is disabled at the recorder's
  caller, so its sample index cannot advance over unwritten samples. Ghost
  sampling is disabled. At 60 the existing format and paths are unchanged.

`port/tools/compare_fps.py` freezes each executable, pins the existing race seed, uses menu
navigation and race-clock inputs, and writes per-rider CSV state, screenshots and JSON comparisons
under ignored `build/`. Full pedal is its default; `--race-pad` can use the older gate input
sequence. `--crash-tick` enters the real crash controller at the same race tick. `--stop-finish`
records the actual finish. It reports coverage gaps; raw position agreement alone is not a pass
condition for different integration timesteps.

_Cleanup 0.2.5: `compare_fps.py` was removed; still in git history at `99d04692`._

Sources: repository findings 036–038, 049, 050, 052, 054 and 061 (identification of pickup emitter
template 16); retail source and locally split assembly. No third-party implementation or game data
is committed.

Research: Macklin et al., *Small Steps in Physics Simulation* (SCA 2019),
https://matthias-research.github.io/pages/publications/smallsteps.pdf. Smaller position-based steps
change numerical damping and constraint stiffness; the ragdoll therefore needs physical dt²,
exponential drag and rate-adjusted soft constraints, not just a clock change. No paper code was
copied.

Clock/helper test (`python port/tools/test_fps.py`): over ten seconds, both 120 and 144 produce
exactly 600 integer timer fields and 614400 animation subticks. Constant-thrust final velocity:
300.000000 at 120, 299.999664 at 144 (analytic 300). Particle geometric displacement after one
second: 8.983818 / 8.983820, reference 8.983825. Original 60 helper arithmetic passes exact
comparisons. Online override returns 60 while preserving animation timestamp units. These are
isolated math/clock checks, not race equivalence evidence. Custom-rate math/clock checks also pass
at 61, 75, 90 and 100. Worst tested constant-thrust error is 0.002289 on 300 units of velocity (75
Hz).

## Validation log

An intermediate run lost trace writes when the volume filled. Its incomplete race/crash CSVs and
unfinished gate are invalid and excluded from the results. The comparison now checks free space
before/during each arm and requires every integer race tick and all ten rider records before
producing any verdict. Completed earlier gate JSON results remain; disposable generated captures
were archived locally to make room. The final results below use fresh output folders.

The first clean build, revision `925cd702`, passed the unchanged default `r46-plain` native renderer
gate: **14 EE comparisons MATCH**, and **1,159 readback/Z records MATCH** through field 9080.
Artifacts: `build/port/fps-unlock/gate-925cd702/result.json`. The reference was copied from the main
checkout, never re-recorded; no exclusions or matching checks changed. The final revision requires
its own clean gate after the later fixes.

The next clean build, `736fe98b`, also passed: 14 EE comparisons and 1,151 readback/Z records
through field 9072. Its full-pedal 60 race finished at official tick **23551 (392.5167 s)**, exactly
the earlier build's finish tick. Later audit changes remain guarded at 60; their clean gate is run
separately.

Clean `1b64aba6` passed the unchanged gate too: 14 EE comparisons, 1,135 readback/Z records through
field 9056 (`gate-checked/result.json`). Its complete 31-second forced-crash pair
(`checked-crash/comparison.json`) has exactly 1861 ticks × ten riders per arm: 60.0 / 144.0 real
updates and draws per game second, forced recovery at ticks 601–630 in both, pickup lifetimes
113–116 ticks at 60 and 112 at 144. Player active-clip phase coverage: 81 / 209 distinct sampled
phases; mean circular phase difference 0.075835. Observed unpaced busy host throughput: 98.67 /
99.73 real steps/s. It still fails the early handling screen (first-five-second speed ratio 1.06935,
maximum path separation 40.83). This is the build before the tire-friction cap and base gauge
corrections; those require new measurements, not reinterpretation of these results.

## Measured physics build: `85229f7d`

Clean build: 3,797/3,797 units compile; 714 assembly-only functions recompile; 23 existing SDK
stubs, no undefined port helpers. Executable: `build/port/clean/85229f7d/dhd.exe`, SHA-256
`54fbedd96ef79b3e7c4d4909f07912d8c5f3247d6e1c4a9359fa7a9a404f6835`.

**60 gate MATCH**: 14 EE comparisons and **1,116 readback/Z records** through field 9037. Default
`r46-plain`, no re-recording, no changed comparison functions or exclusions. Evidence:
`final-gate-85229f7d/result.json` in the private artifact directory. The tools gained
`--external-out` solely to permit a private data-drive destination when the build volume is
constrained. The option still rejects tracked locations inside the checkout; reference-recording
restrictions remain. All matching logic is unchanged. These final runs used that external
destination.

Final forced-crash pair (`final-crash-85229f7d/comparison.json`): complete 0–1860 tick coverage, ten
riders each tick, **60.0 / 144.0 real updates and draws per game second**. Forced crash recovery is
**601–630 inclusive, 30 ticks** at both rates. Pickup template-16 effects: four complete 60 events
at 113, 115, 115, 116 ticks, one complete 144 event at 120 ticks. Different events consume different
particle random draws; this measures their observed duration range. Player active-clip phase error
averages 0.032365, with clip/state differences included. Busy-host unpaced throughput in the
600–1800 window: **108.19 / 102.49 real steps/s**, respectively. This is not a quiet-machine paced
benchmark. The first-five-second speed ratio is **1.013227**, maximum path separation **7.78125
world units**. The tool flags its strict five-unit path threshold; this short scenario deliberately
has no finish-time coverage.

### Full-race results: the unlock is not acceptance-ready

All results here use **one frozen clean `85229f7d` executable**, fixed seed 81257, virtual frame
clock and identical race-time-keyed inputs. Every trace was checked for unbroken tick coverage and
all ten rider slots before comparison.

`final-race-85229f7d`: full pedal, no steering, no recovery input, limit 30,000 fields / 500 game
seconds:

| Measure | 60 | 120 | 144 |
|---|---:|---:|---:|
| Official player finish fields | 23551 | no finish | no finish |
| Official player finish seconds | 392.5167 | >500 | >500 |
| First-five-second mean speed / 60 speed | 1 | 1.007976 | 1.013227 |
| First-five-second maximum position separation, world units | 0 | 4.60438 | 7.78125 |
| Total observed player crash fields | 1362 | 72 | 144 |
| Real update and main-view draw calls / game second | 60 | 120 | 144 |

The high-rate player becomes nearly stationary near checkpoints 6 / 7. This is an unresolved
handling/contact/recovery failure, **not a finish-time pass**. The 60 finish is exactly the same
tick as the earlier clean 60 runs. No input was retuned to conceal the failed unsteered fixture.

AI official finish fields (same slots, rather than sorted finish order):

| AI slot | 60 | 120 | 144 |
|---|---:|---:|---:|
| 1 | 17072 | 17193 | 17117 |
| 2 | 20750 | 16949 | 18336 |
| 3 | 20750 | 18354 | 16940 |
| 4 | 18824 | 16928 | 17249 |
| 5 | 17056 | 17073 | 16891 |
| 6 | 18566 | 18233 | 18096 |
| 7 | 16849 | 16900 | 16809 |
| 8 | 22342 | 18785 | 18385 |
| 9 | 22084 | 18839 | 18189 |

AI progresses and finishes, but individual differences are substantial. The 31-second forced-crash
pair's final AI checkpoint deltas are `[0,0,0,0,0,0,1,0,0]`; its mean position separation is 139.61
world units, maximum 659.29. First-five-second AI mean speed ratios are 1.009797 at 120 and 0.965572
at 144. These do not establish AI equivalence.

A **separate** recovery fixture (`recovery-race-85229f7d`) holds the same full pedal input and
double-taps Select every 20 game seconds (at +0 / +0.2 s, 0.05 s presses), exercising the game's
ordinary bail/recovery control. Added `compare_fps.py --bail-every 20`; no production gameplay
change or hidden teleport was added. Both arms finish:

- 60: official 24087 fields = **401.45 s**.
- 144: official 24683 fields = **411.3833 s**, **+2.47436%**. Finish flag is
  sampled at field 24684; official stored time is one field earlier.
- Update/draw cadence: 60.0 / 143.99854 calls per game second over the full run.
- Total crash time: 1776 / 1338 fields. Equal-time mean player separation is
  715.72 world units, maximum 1374.53. Different crashes and trajectories make
  a single averaged speed ratio misleading; the comparison retains raw values.

This additional coverage does not replace or invalidate the original failed fixture. Both comparison
JSONs report `NEEDS_REVIEW` (the recovery fixture also fails the five-world-unit early-path screen).
The tool's broad screening limits are diagnostic and are not a claim that a 2.47% finish shift meets
the brief.

### Particle and animation measurement limits

The short controlled pickup event above has approximately two seconds of active emitter visibility
at both rates. The full unsteered high-rate runs also contain many short emitter episodes (2-33
fields), producing a >25% median-duration screening failure. This metric observes active
emitter/count visibility, not the lifetime of an individually tracked particle; culling,
retriggering and slot reuse have not been separated. Preserve that failure and raw particle CSVs; do
not interpret the short episode median as a proven particle decay constant. `0022DE40`'s active list
was checked against matching source (`D_0029E308`, node stride 12, `used`/next links). Kind-4
`life0` is an auxiliary float array, so the logger deliberately does not interpret it as a short
lifetime. Isolated identical-emitter/particle-ID tests remain needed.

Likewise, the active-clip phase statistic includes clip/state divergence. The 31-second player's 81
/ 214 distinct sampled phases and 0.032365 mean circular error demonstrate fractional evaluation,
not identical animation selection. Natural ragdoll behavior and sound still need gameplay review.

### Contact diagnostics and final small fix

Clean `6a063c72` adds read-only trace columns for player contact calls, persistent wheel contacts,
contact mask, airborne age, gauge and boost. In the first six seconds, contact calls are **95.8333 /
228.0 per game second**; persistent contacts are **63.1667 / 169.0**, at 60 / 144. This supports the
impulse-rate concern behind the friction cap correction; it does not prove the whole collision model
invariant. Every sampled rider position, speed, checkpoint, race/finish time, crash flag and
animation value in its 31-second pair matches `85229f7d` exactly (18,610 rows per arm).

`734a8cc2` additionally preserves `prev.w` in the port-only Verlet helper: retail CopyVector copies
four lanes, whereas the first helper copied xyz only. A production-helper unit check now verifies
the fourth lane as well as motion. This is the final simulation-code change; its own clean
validation is below.

### Final tested executable: `734a8cc2`

Clean build (`build_clean.py -j4`, no object basis): all 3,797 units compile, 714 assembly-only
functions recompile, 23 existing SDK stubs, no undefined port helpers. Executable:
`build/port/clean/734a8cc2/dhd.exe`, SHA-256
`804d2bd5c890e6888c9eb0b9fe7e840f4f16a5e6cca76a9e0fb93334b6fd493f`.

**Final 60 gate MATCH**: 14 EE comparisons and **1,110 readback/Z records** through virtual GS field
9031. Default `r46-plain`, original comparison functions/exclusions, no reference re-recording.
Private result: `final-gate-734a8cc2/result.json`. Both exact and native arms used the clean
executable above; native setting explicitly `DHD_FPS=60` (exact defaults 60).

`final-crash-734a8cc2/comparison.json`: both arms contain 18,610 rows (0-1860 fields, all ten
riders). All sampled positions, speeds, checkpoints, race/finish times, crash flags, animation
phases and particle counts match the measured `85229f7d` build exactly at both rates. The final
build verifies **60.0 / 144.0 actual updates and main-view draws per game second**, with the same
30-field forced crash and pickup/phase statistics reported above. Clock/helper and 60/144 keyboard
ramp tests pass. Final shared-host unpaced throughput is **111.37 / 104.48 real updates per wall
second**. This remains a correctness test under load, not a quiet paced FPS benchmark.

Final screenshots `144-race-final-734a8cc2.png` and `144-crash-final-734a8cc2.png` were inspected at
fields 1800 / 615: normal rider/bike, crash ragdoll, terrain, particles and HUD. Both are in the
private artifact root. The full-race/finish numbers above are explicitly from `85229f7d`; the final
helper's fourth-lane repair does not change any sampled 31-second state, but no new full-race
equivalence claim is inferred from that.

### Real throughput and pictures

No quiet-machine paced benchmark was available on the shared overnight host. Measured unpaced real
update throughput during the forced-crash test was **108.19 at 60 / 102.49 at 144 updates per wall
second**. Other full-race arms varied with contention (144: 117.43, 120: 121.71, 60: 90.58 in their
initial steady windows). These are real measured steps, not generated pictures, and are **not
evidence of sustained wall-clock 144 FPS**. Performance work remains separate from correctness under
`DHD_FRAME_CLOCK`.

Inspected private screenshots: `144-final-85229f7d.png` (normal race at field 1800) and
`144-crash-6a063c72.png` (forced crash at field 615). Rider, bike, terrain, particles and HUD render
normally; a screenshot cannot prove motion smoothness. Captures and all game-derived traces remain
untracked.

Remaining acceptance work: explain/fix high-rate unsteered wedging, reduce or explain early handling
and AI divergence, isolate particle lifetime tests, listen-test audio, test online override with
real peers and validate a quiet paced high-refresh run. Do not ship this as a proven same-feel
unlock.

The settings page was exercised through DHD_NAV: main menu → OPTIONS → PC SETTINGS → last row →
Right. It saved `fps = 120`, displayed `Frame Rate: 120 *` and `* Applies when the game starts
again`, and the eleventh row fitted without overlap. Local evidence:
`build/port/fps-unlock/settings/settings-001.png` and its log/INI. The test used an isolated
settings file, not the player's file.

An early fixed-seed full-pedal pair (31 game seconds) measured **60.0 / 144.0 real race updates and
draws per game second** using counters at `0011A100` and the main-view entry to `00215B68`. The
front-end frame log is unsuitable: its counter stops advancing during a race. These counters count
actual game calls. This pair exposed template-16, kind-4 particles lasting 49 ticks at 144 versus
108–119 ticks at 60. `0022B258` advanced the complete position/normalized-age vector by a full
velocity every update, even after its gravity was corrected. It now advances all four lanes by `h`
and rotates the orientation by `h` of its stored angle, once per real step. The original 60
generated instructions remain. The recompiled `0022B708` path also needed its branch delay-slot
store preserved before the high-rate coefficient replacement; omitting it left a signed slope
uninitialized. These defects were found and fixed before the final build.

Controlled crash run at `edd6e141` (`build/port/fps-unlock/crash`): trigger the real crash
controller at race tick 600. At both 60 and 144 the crash flag is set on ticks **601–630 inclusive:
30 ticks / 0.5 seconds**, then clears. The 144 trajectory also had a natural 72-tick crash earlier,
at tick 274. Thus this tests the timed recovery path; it does not prove all natural-impact ragdolls.
The active animation clip has 81 / 250 distinct sampled phases, respectively; mean equal-tick
circular phase difference is 0.10461. Different movement states can select different clips, so this
is coverage rather than an equivalence pass. This short run did not encounter a pickup at 144; the
tool reports that gap.

Screenshots are local only: `build/port/fps-unlock/144-visual.png` and the race captures under each
run's `port/`. The tick-1800 144 screenshot was inspected: rider, bike, terrain and HUD render
normally. A still image cannot prove motion smoothness. The 144 crash arm achieved 82.47 real
updates/s on the shared busy host (unpaced virtual-clock correctness run); an earlier arm achieved
111.27. Neither is a quiet-machine paced benchmark or evidence of sustained 144 FPS.

Reproduction (set `DHD_ISO` to the existing local disc first):

```
python port/tools/test_fps.py
python port/tools/build_clean.py -j 4
python port/tools/gate_native_renderer.py --exe build/port/clean/<revision>/dhd.exe --out build/port/fps-unlock/gate-final
python port/tools/compare_fps.py --exe build/port/clean/<revision>/dhd.exe --out build/port/fps-unlock/clean-race --rates 60,120,144 --ticks 30000 --stop-finish
```

_Cleanup 0.2.5: `compare_fps.py` was removed; still in git history at `99d04692`. The kept high-rate
state check is `port/tools/test_fps_race.py`._

The comparison freezes one executable for all arms, seed 81257, holds Cross on race-clock seconds
0–900, disables memory-card writes, and uses `DHD_FRAME_CLOCK`. Outputs stay ignored. It reports
equal-time position/speed and AI progress, official finish ticks, crash intervals, active animation
phase, particle-emitter visibility episodes, direct update/draw cadence and observed wall
throughput. Screens at race ticks 0/600/1800 permit visual checks. `--analyze` rescores existing
traces. The screening thresholds deliberately flag differences instead of declaring finer-step
trajectories identical. Sound is disabled in this automated harness; audio timing is code/unit
audited, not listening-tested by this comparison.

## Round 52 continuation: contact solver investigation

Acceptance remains failed until fresh full-pedal races establish otherwise. The first continuation
step adds opt-in `DHD_FPS_SOLVER` CSV diagnostics: real solver entry/exit, attempted integration dt,
pre-clear forces and tire friction before/after. Rows include velocity, wheel states, contact
normal, grip, boost and tire mode. It observes only the player and fields 0-6600; no telemetry is
written without the explicit environment variable. This is to distinguish fixed impulse terms from
discarded forces/zero-time contacts. The original comparison inputs, seed, thresholds and matching
checks remain.

Source audit found an additional fixed contact impulse in `0016AFC0`: tire mode 2 replaces the
friction vector with ten times projected gravity, bypassing the already corrected Coulomb cap. This
needs live mode coverage before attributing the wedge to it. `001336F8` clears force/torque after
the first wheel impact; traces will quantify its effect before changing it. All new evidence stays
under the private artifact root. A mistaken initial ISO path selected a different local image, which
the game's existing retail check correctly rejected; that aborted attempt is invalid. The retry uses
the existing USA image in place and does not copy or commit it.

### Sliding impulses and turbo duration (continuation experiment)

The baseline diagnostic executable reproduces all six recorded rider physics columns (position xyz,
speed, checkpoint and crash) exactly through field 300 at both 60 and 144 against clean `734a8cc2`.
Its complete 0-6600 comparison is in `solver-baseline-eff0b4e0/`. Early player speed ratio remains
1.013227; maximum separation remains 7.781247. Thus the new observer is inert in that window; this
is not a substitute for the final full compatibility gate.

Three sustained slide-mode impulses bypassed the cap correction: mode 2's projected-gravity impulse
in `0016AFC0`, mode 1's opposite lever impulses in `0016ABC8`, and its forward braking impulse in
`0016ACE8`. Their magnitudes now scale by elapsed fields above 60. `00136F80`'s turbo duration
`x7B00` was still decremented once per update, despite its force already being corrected; it now
consumes whole fields. Other writers identify it as the speed power-up/cheat duration, not a query
count. 60 arithmetic is unchanged.

`slide-experiment/144` is an incremental diagnostic build, not a clean release. The changed four
units compile. Against the unchanged baseline 60, its first five-second speed ratio is **1.015918**
and separation **9.218740**, worse than before. It again wedges at checkpoint 7 (field 7861: speed
0.395125, position approximately -188.05, 691.59, 1410.31). The diagnostic was stopped by its own
verified executable-path PIDs after the wedge was established; it is incomplete and supplies no
full-race finish claim. These fixes do not solve acceptance, and no input/recovery workaround was
introduced.

At the baseline stuck moment (144, field 6000), tire mode is 0, contact normal approximately
(0.7238,-0.1598,0.6712), boost is 1, and a zero-time impact clears a nonzero force
(-1563,-2285,-7314). The rest of the step advances with zero force. The solver consumes all
requested time: its exit remaining fraction is zero. This rules out exhaustion of the ten-pass limit
as the immediate stationary mechanism. Across the first five seconds, estimated fraction of
force-bearing time before the first clear averages 0.6091 / 0.6651 at 60/144. The force-clear
mechanism itself therefore changes with timestep; keeping all forces is a hypothesis to test, not
yet a retained gameplay correction.

Research used: Box2D's official dynamics documentation explains normal impulse as normal force times
timestep and distinguishes sustained forces from one-shot collision impulses:
https://box2d.org/doc_version_2_4/md__e_1_2github_2box2d__24_2docs_2dynamics.html and
https://box2d.org/documentation/group__body.html . No external code copied.

### Equal-time comparison correction

The original first-five-second ratio pairs the first update bearing each integer race tick. Those
are different elapsed times: the tick-0 60 state has advanced 1/60 s, whereas tick-0 144 has
advanced 1/144 s. The existing actual update counter permits alignment to `(frame - GO_frame + 1) /
hz`. `compare_fps.py` now brackets the reference instant using measured states; its old stamped-tick
ratio remains explicitly reported, and raw finishes, crash intervals and timing coverage remain
unmodified. Interpolation without the new dense sidecar spans up to one legacy field and is
identified as such. The observer is being extended to retain every real update for the first six
seconds, allowing the final early-handling measurement to use those samples. The synthetic
constant-velocity/linearly changing-speed reference catches the original sampling bias at 60/120/144
and verifies no extrapolation (2 tests).

The force-window experiment retains the retail rule that the first impact clears forces for the
remainder of a **legacy field**, across the fractional updates in that field. It still evaluates
real physics/collision queries at the selected rate. Together with elapsed integer airborne age
(floor, with a nonzero first-air-step sentinel), the 144 first-five-second results are: clock-stamp
ratio **0.994591**, actual-time ratio **0.999580**, actual-time maximum separation **2.857302**.
Before this force-window change, actual-time ratio was **1.018322**, separation **8.838042**.
Keeping all forces instead produced **1.063951** and **28.885854**, and is rejected. These
incremental experiments are not clean-build acceptance evidence.

The force-window 144 run passed checkpoint 14 by field 12000. However its 120 full run still lodged
near checkpoint 5 for a long interval before escaping; it reached checkpoint 13 only by field 20878.
Thus this candidate is not a full-race acceptance pass. New Euler drift/spring experiments aim to
preserve the original discrete integrator's numerical behavior at smaller steps rather than assuming
its continuous ODE has the same damping. Their live results are pending; no same-feel claim is made
from the early metric.

### Contact recovery and remaining validation

The field-force-window change alone failed both complete 500-second runs: 120 ended at checkpoint
13; 144 at checkpoint 28, still short of the finish. A read of the agent's own stuck 120 process
found two wheel contacts, velocity approximately (-0.023,-0.060,0.039), airborne age zero and
contact normal (0.227,-0.906,0.358). The crash controller's existing angle threshold was satisfied
and its impact measure was small, so no fall was pending. `00155208` only recovers an
airborne/out-of-world rider; it has no grounded immobility timeout. Finer contact steps therefore
admit a stable pedalling wall-contact equilibrium that the coarse retail solver can fall/rebound out
of.

The high-rate path now detects a grounded rider requesting propulsion, speed below 2 world units/s,
steep contact (normal z < 0.85), and less than one unit net movement over **two physical seconds**.
It invokes the existing collision crash/recovery controller (kind 2). It does not generate input,
write a position/velocity, depend on a checkpoint, or use a fixture-specific schedule. It applies to
rider physics above 60 only; moving slow climbs, countdown, finished riders, ordinary crashes and
online are excluded. This is an explicit high-rate recovery-policy addition, not proof that the
contact integrator itself is identical. The original full-pedal input is unchanged. The in-progress
120 run has ordinary recovery entries at fields 4774/5929 and is moving beyond checkpoint 8;
complete finish measurements are still pending.

The full-force, explicit-Euler drift and fractional spring-matrix experiments are not retained.
Although the latter two reproduce the isolated retail linear maps, they worsen the live early
handling metric and do not prove contact parity. No approximate spring/integrator replacement is in
the candidate.

Two more AI choice paths consumed RNG opportunities per update after a failed choice: `00110870`
retries trick selection with zero cooldown until successful; `0010C940` chooses a recovery direction
when neither previous direction is held. New decisions now wait for whole-field updates, while held
input, physics and trick evaluation still run every real step. One-shot trick-start rolls
(`00110DA0`/`00110F88`) are left unchanged. A counter reports opportunities per rider in optional
traces. Live AI/forced-crash comparisons are pending.

### Isolated particle lifetime correction

The probe pins both particle RNGs and creates a single 128-particle pickup burst, then advances only
the actual `0022B708` update and `0022C1C0` lifetime routines. Other emitters, rendering,
culling/retrigger and slot reuse cannot change the duration. A canonical-origin repeat is running to
remove a 2.4-mm initial-height difference introduced by the rejected drift experiment. The original
pre-update death test retires a particle one full legacy field after its normalized age crosses 1.
At high rates the old correction replaced that delay with one shorter update. The new death deadline
preserves the original float-accumulated, integer-field lifetime while motion stays fractional. The
lookup is bounded and cached; 60 executes the original generated comparison.

Initial measured burst lifetime: 60 **1.966667 s**, 120/144 **1.958333 s**. Maximum
death-order-statistic error across all 128 particles was **16.667 ms** at 120 and **25.000 ms** at
144 (both failed the selected-step bound). After correction: 120 **1.975000 s**, 144 **1.972222 s**;
maximum death-time errors **8.333334 / 6.944445 ms**, within one selected update. Initial age/rates
were identical; the canonical repeat must additionally establish identical positions before treating
this as the final isolated acceptance result.

### Audio and the four-slot envelopes

`00239778`/`002398C0` feed camera displacement through `00239858`, called by `00148F60`; they are
not audio envelopes. Actual transformed-unit synthetic-key tests give envelope end times **1.016667
/ 1.008333 / 1.006944 s** at 60/120/144; linear release **0.466667 / 0.458333 / 0.451389 s**. These
remain within one original 60-Hz field, not consistently within one high-rate update. The curve is
evaluated fractionally; the remaining boundary difference is explicit.

The audio backend advances grains, ADSR and pitch in samples; `sound_tick` remains on the NTSC field
grid instead of running once per high-rate update. The inspected riding-loop setters use current
spatial parameters, not a pitch increment per frame. A fresh standalone production-O2 `test_sound`
run passes (`sound-unit.log`). This is code/unit evidence; no listening or full-race PCM parity
claim is made. Sound remains disabled in the automated race comparison.

### Canonical particle acceptance and recovery refinement

The canonical-origin repeats are complete (`particle-canonical/`). All **128 initial particle
records are identical** at 60/120/144, including position, age and immutable lifetime rate.
`compare_particle_probe.py` follows those lifetime cohorts through array compaction and compares
exact rational `update_count / rate` times, so decimal formatting does not relax the bound. Every
cohort retires within one selected-rate update of 60: maximum differences **0 / 1/120 / 1/144
seconds**. Last-particle lifetimes are **1.966667 / 1.975000 / 1.972222 seconds**. This isolated
lifetime acceptance check passes at all rates. It does not establish parity of world-triggered
emission, culling or placement.

_Cleanup 0.2.5: `compare_particle_probe.py` and DHD_FPS_PARTICLE_PROBE were removed; still in git
history at `99d04692`._

The first contact-recovery candidate (`recovery-fix`, incremental build) completes both unchanged
full-pedal fixtures: 120 **26148 fields / 435.800 s (+11.027%)**, 144 **24197 / 403.283333 s
(+2.743%)**, versus 60 **23551 / 392.516667 s**. Total crash durations are **22.700 / 21.200 /
21.716667 s** at 60/120/144. The 120 trace still contains a **22.216667-second** near-stationary
episode at checkpoint 20 before recovery at field 19568. Its instantaneous velocity/ normal tests
kept resetting the timer, even though position barely changed. The refinement therefore measures
displacement directly over two seconds, allows brief contact chatter (under four fields), and
excludes intentional braking. Actual movement of one unit restarts the window immediately; no
velocity or contact-angle threshold can indefinitely reset immobility now. Production-helper
regressions verify moving slow riders and deliberate braking remain unaffected, while oscillating
velocity cannot defer the two-second recovery. Rates 61/75/90/100/120/144 pass; 60 returns before
touching a bike. The refined full-race result remains pending and is not inferred from 144's first
successful completion.

The AI-choice/forced-crash comparison (`ai-choice-fix`) is complete through 31 seconds. Dense
first-five-second player ratios are **0.997961 / 0.999356**, maximum separation **2.688554 /
2.857515** at 120/144. AI mean speed ratios are **0.998554 / 0.970064**, maximum separations
**92.8724 / 109.3214** units; AI equivalence is still not established. Forced recovery remains
fields **601-630 inclusive (30 fields)** at all three rates. Counts of AI decision opportunities
over this scenario are retained per rider, without interpreting different trajectories as proof of
timer parity. Default-60 game state still needs its clean r46 gate on the final refinement.

Clean `cb49164c` builds all 3,797 units and links successfully (no object basis). Its later
progress-window refinement requires a new clean build and gate. A fresh production-O2 sound-unit run
passes; optional camera-envelope tests also pass the explicitly stated one-original-field bound. No
listening claim.

The human controller's new progress watchdog is explicitly excluded from AI control mode 3. AI
already has grounded recovery in `001100D0`/`001103B0`; its original field timers remain
authoritative. A production-helper regression checks this exclusion. The current progress-recovery
fixture had no AI watchdog entries before this exclusion; only slot 0 invoked it. This change avoids
introducing a faster AI recovery policy in other scenarios.

### Clean progress-watchdog results and remaining partial-grip term

Clean `07ea5e18` (3,797 compiled units, 714 recompiled functions, 23 existing stubs; no object
basis) completes the full fixture at all three rates:

| Rate | Player finish (s) | Difference | Dense 5-s speed ratio | AI finishes | Total crash time (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| 60 | 392.516667 | reference | 1 | 9/9 | 22.700000 |
| 120 | 432.733333 | +10.2458% | 0.997961426 | 9/9 | 25.700000 |
| 144 | 402.400000 | +2.5179% | 0.999356153 | 9/9 | 22.916667 |

Evidence: private `final-clean-07ea5e18/`, identical executable hash in every arm
(`9e287b1093d7abd94e0972f30dc8be7623a888e1d7bc868a07c3dab8b004f824`). The human watchdog fires twice
at 120 (fields 4777/25337), once at 144 (23510). This removes indefinite grounded immobility but
**does not establish equivalent handling**: 120 still fails the finish-time target. AI
first-five-second mean ratios are 0.998554/0.970064, maximum separation 92.8724/109.3214 units. All
nine AI finishing is coverage, not AI equivalence.

Default r46-plain native gate on this clean executable: **MATCH**, including 1,159 readback
records/Z reads, through virtual GS field 9080. Reference and gate script unchanged. A fresh retail
SN build in `build/retail-r52` matches all eight checked sections; `src/` is unchanged. The clean
forced-crash repeat preserves fields 601-630 inclusive (30 fields). Clean canonical particle repeat
again gives identical 128 births and selected-step lifetime bounds.

One further sustained-contact term is in `00130D80`: below the Coulomb cap, partial grip cancels a
fraction of tangential velocity per invocation. Convert that fraction with `1-(1-f)^(60/rate)` above
60; full grip 1 and normal impact response are untouched. Actual transformed-unit tests with grip
0.01 give one-second residual velocity **5.471568108 / 5.471600533 / 5.471566677** from 10 at
60/120/144. The bound is 10 ppm, accounting for retail float `f-1+1` cancellation; full cancellation
remains exact in one call. This test originally exposed an unaligned test vector, corrected to the
EE's required 16-byte layout. The short 144 player result is unchanged (0.999356153; 2.857515-unit
maximum early separation); AI mean becomes 0.971754, still divergent. Full-race repeats are pending,
so no race-time benefit is inferred from this isolated correction.

Research: [Box2D simulation documentation](https://box2d.org/documentation/md_simulation.html) and
[body API](https://box2d.org/documentation/group__body.html) distinguish steady forces from
instantaneous impulses and describe timestep-sensitive damping/substeps. They support auditing these
separately; they do not prove this game's contact equivalence. No external code was adapted.

### Rejected partial-grip experiment; retained handoff behavior

The complete `grip-full-valid/` trial finishes at **25285 / 26029 fields**: 120 **421.416667 s
(+7.3627%)**, 144 **433.816667 s (+10.5218%)**. Both use the same frozen incremental executable hash
`bd3ed4e971ba6c6ec8ee056407cf2e4df7e0878ae46580bb5917d09fbec10dda`. All nine AI finish; total crash
time is 25.983333 / 29.000000 seconds. Player trajectory first differs from the retained build at
field 6693 / 6790; its five-second metric is unchanged. The conversion composes in an isolated
partial constraint, but the full contact/mode feedback produces a substantial 144 regression. **It
is removed from the handoff**, along with its prototype unit test. Commit `c01e5e9f` and private
evidence preserve the experiment; its clean build succeeded but is not the accepted handoff
executable.

The retained runtime is exactly `07ea5e18` (`git diff 07ea5e18 -- port` is empty after removal). The
full clean results table above therefore describes the handoff code, not the rejected trial. It
improves the original no-finish blockage to 402.400 seconds at 144 (+2.51794%) and brings the early
player speed ratios inside the requested 0.3% window at both 120 and 144. It does **not** meet the
complete brief: 120 is +10.24585%, AI routes/speeds differ, and the human grounded watchdog is an
explicit policy addition rather than a proof of contact-equivalent physics. No claim of matching
full-race trajectories, equal natural crash counts, world-emitter counts or audible PCM is made.

The contact mask is two wheel states (0 fresh impact, 1 supported/persistent, 2 no contact), not a
timer. `00135A30` reinitializes these each real update; fresh impacts and support probes then set
the actual contact state. Normal response `00130C08` cancels approaching relative normal velocity,
and terrain penetration `00131198` computes a geometric separation. Blindly multiplying either by
elapsed fields would leave penetration/approaching velocity behind; neither has been scaled. The
traced zero-time force clearing, persistent contact friction and missing human grounded recovery
explain the observed stall mechanism; resolving all remaining coupled integration differences still
requires work. Increased contact-call counts alone are not evidence that every collision impulse
should scale as a force.

Remaining acceptance work is to close the 120 full-race gap without fixture-specific tuning,
establish AI behavior beyond finish coverage, test more tracks/steering inputs and listen/compare
sound-enabled runs. The isolated particle-lifetime target passes. Camera-envelope boundary
differences remain within one original field, not always one high-rate update. Online remains forced
to 60 and replay/ghost paths remain disabled above 60.

### Final clean-build provenance

Clean `167fa3e2` builds all 3,797 units from scratch and links with no object basis (45-second
compile, 152-second link). Executable: `build/port/clean/167fa3e2/dhd.exe`, SHA256
`3e2a6f4dbc512b30f27562bafb1cb3e004beb60f99c1ebc568f0b734a9a9e419`. A byte comparison against
full-race-tested clean `07ea5e18` finds differences **only at offsets 128/129, inside the COFF
timestamp**. All eight loaded PE sections, virtual addresses and sizes match exactly. Private
evidence: `handoff-pe-header.json` and `handoff-pe-sections.json`. Thus the full-race, forced-crash
and canonical-particle numbers above describe this runtime too; they are not inferred from source
similarity alone. The final repeat of the unchanged default r46-plain gate **passes on this newly
built executable**: **14 EE MATCH comparisons, 1,106 readback/Z records, through virtual GS field
9027** (`final-gate-167fa3e2/`). The script, canonical reference and exclusions remain unchanged.
This is a game-state gate; pictures are not compared.

A final direct 120-Hz forced-crash repeat on this executable
(`final-crash-167fa3e2/120/`) covers all ten riders without a gap through field
1860. The forced interval is **601-630 inclusive: 30 fields / 0.500 s**, matching
the clean 60/144 pair. Its other automatic crash intervals are 467-538 and
1217-1252; natural crash histories are not being forced to match.

No game data is bundled, no player configuration is replaced, and the 144 script is for testing this
explicitly experimental build. The final validation game processes have exited normally; no owned
test game remains running.

Per-slot AI finish times (seconds; slots 1-9) make the remaining divergence visible beyond the 9/9
count:

| Rate | AI finish times |
| --- | --- |
| 60 | 284.533, 345.833, 345.833, 313.733, 284.267, 309.433, 280.817, 372.367, 368.067 |
| 120 | 282.800, 362.300, 376.767, 301.167, 281.200, 301.617, 283.100, 369.100, 372.717 |
| 144 | 280.283, 313.900, 374.400, 363.267, 281.833, 281.900, 328.567, 378.183, 379.033 |

### Round 52 chaos-aware continuation (2026-10-18)

New opt-in `compare_fps_windows.py` replays each window prefix at 60 real steps/s, then changes only
the diagnostic clock rate for 3-5 seconds. Every arm boots with identical 1024-subtick animation
storage; no existing timestamp is reinterpreted at the switch. Normal play and the compatibility
gate do not enable this facility. Prefix equivalence to ordinary 60 still requires a measured check;
it is not assumed from the timestep alone.

_Cleanup 0.2.5: `compare_fps_windows.py` was removed; still in git history at `99d04692`._

Before switching, the probe saves all ten full rider records, their controller records and both
particle/game RNG words. The analyzer rejects unequal starting bytes; it never turns divergent
prefixes into an accepted comparison. It uses actual update counts and brackets equal elapsed times
(no extrapolation), reports signed mean speed drift and endpoint forward drift for every rider, and
aggregates mean/median/sign counts across independent windows/seeds/scripts. Active-crash starts are
flagged and excluded from the riding aggregate: Verlet previous positions still span the preceding
60-Hz step. This diagnostic does not yet support a valid crash-rate transition; ordinary fixed-rate
crash tests remain separate. Airborne ages are initialized from the 60-Hz integer state on switch.

Initial implementation validation: production clock/helper checks pass at 60/61/75/90/100/120/144,
existing equal-time tests pass, and new synthetic window sampling checks verify zero linear-motion
drift, drift sign and rejection of missing coverage. Live replay-window validation and source
investigation follow. No correction or acceptance claim is made by this instrumentation commit.
Morning launchers still reference clean gated `167fa3e2`.

The first live 60 diagnostic arm covers 183 integer ticks x ten riders. Its xyz, speed, checkpoint,
crash, gauge and boost values are identical to retained clean `07ea5e18` at every recorded tick
(14,640 value comparisons). Thus the common animation storage has not changed this measured riding
prefix; longer prefixes and full snapshot equality still need their own checks.

The long-race tool now accepts `--seed` (default unchanged) so distribution experiments can use
independent seeds. Both FPS experiment tools use a verified PID cleanup helper: on Windows it
verifies the frozen executable path of the root and every launcher descendant individually before
stopping that PID; it never invokes tree-wide taskkill.

Candidate under test: `00134688` resolves rider-pair overlap by moving EACH rider 10% of penetration
on every update. For two stationary equal spheres, relative penetration therefore decays by 0.8 per
call, i.e. 0.8^(rate/60) per legacy field. The candidate uses each-side fraction `(1-0.8^h)/2`,
where h=60/rate; it does not scale the instantaneous pair velocity impulse. This is a
solver-relaxation hypothesis, not yet an established live root cause. [Box2D's simulation
documentation](https://box2d.org/documentation/md_simulation.html) was consulted again for its
separation of fixed outer steps and solver substeps; no external code is copied. It cannot establish
this game's collision parity. The candidate requires the same replay-window comparisons before
retention.

Prefix audit found one raw byte difference in slot 3 at rider +0x5CBE. This lies in the unused w
lane of the ragdoll Euler offset (+0x4EF0+0xDC0+12). `001A6D00` and `001A7880` produce xyz with
masked VU operations from locals, leaving w as host-stack residue. Its only consumer, `00222040`,
constructs its matrix exclusively from x/y/z sine/cosine lanes. The analyzer now canonicalizes ONLY
these ten four-byte unused lanes in its comparison copy, retaining every raw snapshot/hash and
reporting raw differences. Unknown padding and all live state still compare exactly. This does not
change any game bytes, gate masks, retail checks or reference files. A regression changes this lane
then changes velocity to verify that only the former can pass the snapshot check. The 60 diagnostic
through field 542 matches retained ordinary 60 in all 43,440 recorded
position/speed/checkpoint/crash/gauge/boost values (5,430 rider rows).

A separate force-window boundary defect is now isolated. The clock stamps the END of each
integration interval. `port_fps_force_window` used floor(end), so an interval ending exactly on a
legacy-field boundary incorrectly started a new force-clear window. At 120 this occurs every second
update; at 144 every twelfth. The interval's field is ceil(end)-1, computed without floating
rounding as `D_0029E3D8 - (phase == 0)`. The helper test defines the expected grouping independently
as `(step*60-1)/rate`, clears forces in the first interval of each field, and checks whether later
intervals preserve that clear. It FAILS on the old production helper (first at rate 61); the
corrected helper passes all rates 61/75/90/100/120/144. The original 60 path returns before this
arithmetic and its NULL-body inertness regression still passes. Live drift/finish results are
pending; this is evidence of an interval-bookkeeping error, not yet evidence that it accounts for
the long 120 race-time gap.

The pair-relaxation trial remains frozen as clean `15e50241` in its own private window directory.
Its candidate patch is removed from the next build so the force-boundary change can be measured
independently. No trial has replaced the morning launchers. A read-only external pilot recorder is
also under test: it steers toward the game's route graph through an ordinary future-dated pad file,
then emits a fixed race-clock script for replay; it never writes EE memory.

The shared comparison process stopper also used unchecked `taskkill /T`, including the otherwise
unchanged native-renderer gate. It now calls the same per-PID image-verification helper on Windows.
A live safety regression launches an owned sleeping Python child, deliberately supplies the wrong
executable, verifies that the process remains running, and only then stops it with its correct
verified path. The regression passes. No comparison function, exclusion, reference or timeout has
changed. Normal 60 path source checks remain unchanged; `check_image.py build/retail-r52/dhd.elf`
again reports all eight sections MATCH.

The open-loop steering fixture exposed a tool rounding defect, separate from the game: decimal
adjacent intervals can overlap because `0.2+0.1 > 0.3` in binary double. The live pilot's
two-interval pad file uses an absolute end, whereas the first exported fixed script used
start+duration. Its 60 replay did not finish by 500 s (checkpoint 22), despite the pilot finishing
at official 19858 fields / 330.966667 s. Therefore it is NOT validated as a full-track following
fixture and supplies no accepted finish distribution. Its next arm was stopped only after verifying
its owned executable processes in the private `races-boundary-follow-81257/120/` directory; process
IDs stay in the private operations log, and no other image was addressed.

The exporter now places each interval boundary one microsecond before the intended integer race
field, so double summation cannot add an extra sampled field. Every one of the full script's 30,001
field masks was checked against its original integer intervals, and an adjacent/opposite-input
regression passes. Both original and corrected scripts remain private (`fixed-pad.txt` and
`fixed-pad-fields.txt`); the latter's replay is in progress. This fixture correction does not affect
the original full-pedal input or game code.

The field-boundary fix alone does NOT reproduce the feedback pilot: corrected and original fixed
scripts first differ from its riding state at field 6001. The external controller can be descheduled
between its state read and atomic pad-file delivery, so its planned timestamp is not authoritative
evidence of what the game consumed. The previous DNF cannot be attributed solely to decimal
rounding. That claim remains rejected, not hidden by the script rewrite.

A diagnostic `DHD_RACE_PAD_RECORD` now logs the actual sampled buttons at each integer race field.
During this explicit recording mode it holds the first sample for that whole field, ensuring that a
static race-clock script can represent the recording exactly. Normal play/gates do not enable it.
The pilot exporter now requires this capability, validates uninterrupted consumed-field coverage,
and exports those observed masks rather than its proposed events. A fresh clean build and 60
record/replay proof are required before calling this an accepted track-following fixture. Physics is
unchanged from `cda63726`.

_Cleanup 0.2.5: `DHD_RACE_PAD_RECORD` and `record_track_script.py` were removed; still in git
history at `99d04692`._

The full-race comparison gains `--verify-60-trace`: before any high-rate arm, it must reproduce
every player/AI riding row of the feedback recording at 60 (xyz, speed, checkpoint, race time,
finish, crash and attack state). Missing coverage or any unequal value aborts the fixture; proof
metadata includes both raw trace hashes. A regression verifies equal recordings pass while changed
speed or missing rows fail. This protects the distribution study against a plausible-looking
steering script which never reproduced its 60 reference.

Conservative window-coverage refinement: a window starting with ANY active ragdoll is now excluded
from every rider's aggregate, not just that crashed slot. Its previous displacement still represents
1/60 s at the diagnostic switch; without explicit Verlet-history retiming it could affect other
riders through contacts or RNGs. Raw measurements remain exploratory and visible. This excludes
starts +6 s (slot 3) and +30 s (slot 4) from the initial seed's aggregate; GO and +15 s remain
supported. The earlier four-window sign counts are observations, not four valid independent bias
measurements. Expanded seeds/scripts add supported starts; the tool reports every excluded start. No
actual fixed-rate gameplay or compatibility check is altered by this analysis restriction. Full
fixed-rate crash timing is tested separately.

Revisit of the rejected partial-grip term: `00130D80`'s sub-cap tangential constraint cancels a
fixed fraction per invocation. The previous finding's full-race rejection cannot distinguish
per-step bias from route/crash chaos. The same fractional blend is restored ONLY behind
`DHD_FPS_GRIP_TRIAL=1`, exposed by `compare_fps_windows.py --grip-trial`. It is off by default and
cannot change 60 arithmetic. New and old policies will use ONE frozen clean executable and supported
common-prefix windows (GO / +15 s); no full-race or acceptance benefit is assumed from the isolated
semigroup calculation. The original full-pedal distributions remain on measured `cda63726`.

_Cleanup 0.2.5: the rejected grip trial was removed; still in git history at `99d04692`._

### Completed method and first corrected-policy results

Clean `cda63726`: all 3,797 units compile, link succeeds (23 existing SDK stubs, no undefined
symbols), executable SHA256 begins `0213a22a425fb1c9`. Default r46-plain gate MATCH: 14 EE
comparisons and 1,159 readback/Z records through field 9080 (`gate-boundary-cda63726/`); no
references or comparisons were changed. The shared stopper now verifies every process it terminates.

Original-seed full-pedal finish: 60 **392.516667 s**, 120 **399.150000 s**, 144 **426.650000 s**.
Before the interval fix, 120 was 432.733333 s and 144 402.400000 s. The fix improves this 120 race
and worsens this 144 race; neither one establishes a general statistical benefit. Dense full-race
first-five-second player speed ratios are **0.996218 / 1.031576**; AI **0.993327 / 0.957415**. Human
watchdog entries are 2 / 3 at 120/144 and zero at 60; it is still needed by this policy and has not
been replaced with contact-equivalent physics.

For the supported GO window's first quarter-second, mean AI relative speed drift changes
**-1.595487% -> -0.691348%** at 120, and **-5.388205% -> -4.5516%** at 144. Its three-second AI
drift instead changes **+0.099618% -> +1.1394%** and **-2.174424% -> -1.3477%**. These mixed
timescale results explicitly fail a blanket zero-bias claim. Every rider's numbers remain in each
window JSON. The paired +6/+30 starts contain an already-active ragdoll and are exploratory,
excluded from the conservative aggregate. Expanded seed/script coverage is still in progress. A
pair-relaxation trial leaves the first-quarter AI drift unchanged and does not remove the later
pattern, so it remains excluded.

The consumed-input track fixture (`pilot-consumed-3b3abdfd/`) finishes at **19134 fields / 318.900
s**. The mandatory 60 replay proof in `races-consumed-follow-3b3abdfd/60-replay-proof.json` matches
**191,350 rows** x **12 recorded riding fields = 2,296,200 values**, across every rider and natural
crash/finish. Higher-rate arms start only after that check passes. It is equality of the recorded
riding fields, not a full-EE or retail-byte claim. Earlier planned-timestamp exports remain invalid
as full-track proofs.

### Per-update AI floating-RNG candidate (separate opt-in trial)

A remaining non-timer roll is now identified in the passing controller: `0010F710` detects a slower
nearby rider in the same corridor; `0010F0D8` then calls `0010B998` every update, whose
ordinary-node branch samples a fresh floating lateral target with `00222B68`. The older audit's
claim that AI rolls were all timer-gated missed this floating-RNG path. Path-node initialization
also calls `0010B998`, so blindly gating the whole function would be wrong.

The opt-in `DHD_FPS_AI_JITTER_TRIAL=1` wraps only the passing-avoidance caller. It holds one sample
per controller/node/physical interval, while changed nodes, changed ranges and calls outside the
avoidance context still draw immediately. Steering geometry and its rate-aware smoothing continue
every real update. Per-slot draw/reuse counts at test exit provide live coverage evidence. The trial
is OFF by default and 60 keeps the direct original calls. A production- helper test uses an
independent `(step*60-1)/rate` interval definition: exactly 60 draws over one second at
61/75/90/100/120/144, stable values within each field, and distinct one-shot/node-change draws. It
passes. This is not yet an established cause of the measured AI drift; matched-window measurements
follow.

_Cleanup 0.2.5: the rejected AI jitter trial was removed; still in git history at `99d04692`._

### Round-52 continuation: completed expanded measurements

The current production policy still FAILS equivalence. Twelve three-second windows were run over
three seeds (81257/81258/81259) and pedal/steering-prefix scripts. Ten starts have no already-active
ragdoll and enter the conservative aggregate; two starts remain visible but excluded. These are ten
observations, not ten independent experiments: scripts can share the same GO prefix and riders share
collisions/RNG state. Later crashes within a window are retained; large resulting outliers must not
be read as smooth controller bias.

Method: replay each prefix at 60 on one frozen executable, save all ten rider records/controllers
and both RNGs immediately before switching, compare those snapshots, then sample each real update
and interpolate to equal elapsed times for 3 s. Only the independently established unused Euler w
lanes are canonicalized in comparison copies; raw bytes/hashes remain private. This is not a full-EE
save-state assertion. Per-slot first-quarter, first-second, three-second speed and endpoint drift
are retained in windows.json. No extrapolation or finish-based trajectory alignment is used. The
steering-prefix script is supported by the earlier pilot through these short starts; its failed
full-route export is not used for a finish distribution. The subsequently validated consumed-input
script supplies the separate full-route experiment.

Combined corrected-force-policy results below are mean relative speed drift in percent and
positive/negative window counts. 60 is the zero reference for every slot. Mixed signs do not
establish a zero mean; no confidence/equivalence claim is made from this small correlated sample.

| Rider slot | 120 mean % | 120 + / - | 144 mean % | 144 + / - |
|---|---:|---:|---:|---:|
| 0 (player) | -1.5757 | 4 / 6 | +1.6895 | 3 / 7 |
| 1 (AI) | -0.1023 | 5 / 5 | -0.8716 | 3 / 7 |
| 2 (AI) | -0.5200 | 5 / 5 | -0.7301 | 3 / 7 |
| 3 (AI) | -1.0727 | 3 / 7 | +1.0853 | 3 / 7 |
| 4 (AI) | -2.1620 | 4 / 6 | -1.1199 | 4 / 6 |
| 5 (AI) | -1.1316 | 5 / 5 | -1.3948 | 4 / 6 |
| 6 (AI) | -0.7500 | 4 / 6 | -0.9461 | 3 / 7 |
| 7 (AI) | +2.3091 | 8 / 2 | +7.1383 | 5 / 5 |
| 8 (AI) | +1.3265 | 8 / 2 | +0.4581 | 5 / 5 |
| 9 (AI) | +0.9225 | 3 / 7 | -0.4711 | 1 / 9 |

The GO quarter-second AI deficits remain -0.691348% / -4.551575% at 120/144, and the independent
fixed-rate cold-race first-five-second AI ratios remain 0.993327 / 0.957415. Aggregate cancellation
cannot erase those startup results. For the field-ownership fix, GO player three-second drift
changes -0.015080% -> +0.469863% at 120 and -0.966999% -> -1.330295% at 144; AI changes are recorded
above. The interval regression establishes the bookkeeping defect, not that it solves either
high-rate race-time gap.

Partial-grip trial (same b6264b92 binary, flag off/on): GO AI three-second drift 120 +1.139434% ->
+1.345200%, 144 -1.347692% -> -1.435153%; at +15 s, 120 +1.275980% -> +0.765048%, 144 +7.810028% ->
+7.570323%. Player results are unchanged in both windows. The first-quarter AI result is unchanged.
This term does not explain the cold-start bias and stays OFF. The rider-pair relaxation remains
removed.

Controller audit: 0010BE38 timers and 00112360's timer/rotating slot already use whole legacy
fields; 001109B0's x428 is a selection cursor, not elapsed time. 0010BD10/0010CB70/0010F0D8 use the
previously corrected shared smoothing helper. 0010E270 rubber-band targets are instantaneous, with
rate-aware x4214 smoothing. 0010FE50's speed*0.3/0.4 lookahead expresses a physical time horizon.
00111B18 derives catch-up terms from the shared race clock rather than integrating an additional
step. The newly found passing floating-RNG path is separately instrumented; no claim that this audit
exhausts finding 049 A/B terms or explains the remaining early deficit is made.

### Finish distributions (cda63726 default policy)

Three full-pedal seeds were run at all three rates to 500 s or player finish. DNF is right-censored
at 500 s, not removed from a mean or counted as 500 s. All nine AI riders finish in each of these
nine races. Seconds below use the game's official finish counter, not host wall time.

| Seed | 60 player | 120 player | 144 player |
|---|---:|---:|---:|
| 81257 | 392.517 | 399.150 | 426.650 |
| 81258 | DNF >500 | 411.617 | 380.267 |
| 81259 | DNF >500 | 389.000 | 410.000 |

| Rate | Player finished / trials | Finite player mean (s) | AI mean, 27 finishes (s) |
|---|---:|---:|---:|
| 60 | 1 / 3 | 392.517 | 313.347 |
| 120 | 3 / 3 | 399.922 | 322.671 |
| 144 | 3 / 3 | 405.639 | 328.027 |

The high-rate finite ranges contain the one finite 60 finish, but the censoring mass differs (2/3 at
60, 0/3 at 120/144). These distributions are NOT established equivalent. Per-slot AI finishes are
retained in pedal-three-seed-summary.json; the 27-finish mean is descriptive, not 27 independent
races. Original-seed watchdog uses are zero / two / three at 60/120/144. The human watchdog remains
an explicit high-rate policy; its original 60 early return passes NULL-body inertness tests, but no
underlying contact-equivalent replacement was found.

Private roots: windows-baseline-49d97095, windows-pair-15e50241, windows-boundary-cda63726,
windows-expanded-boundary-cda63726, windows-grip-control-b6264b92, windows-grip-trial-b6264b92,
races-boundary-pedal-81257/81258/81259. All JSON/CSV/snapshots remain outside this repository. No
generated assembly, retail data or captures are committed.

### Final clean executable and fixture provenance

Clean source bba533d2 compiled all 3,797 units and linked successfully. Executable:
build/port/clean/bba533d2/dhd.exe; SHA256
f00ce10386c33de9f6348ea0856a899f3b6e6d9ac28c2b1047b4a72f7a7b5a14. The default r46-plain
native-renderer gate PASSES/MATCH: 14 EE comparisons, 1,127 readback/Z records through virtual GS
field 9048; both final capture fields 7715. References, comparisons and their exclusions are
unchanged. Private gate-bba533d2/result.json records the exact binary hash.

With both experimental flags absent, this final binary reproduces cda63726's GO/+15 s controls at
all three rates: 37,980 rows x ten riding fields (tick/slot/xyz/speed/checkpoint/crash/gauge/boost)
= 379,800 equal values, zero mismatches. Thus later recorder/disabled-trial additions have direct
short-window controls. The three-seed full-pedal table above was measured on cda63726, not rerun in
full on bba533d2. Full-route consumed-input runs use 3b3abdfd (the same default physics plus the
explicit recording diagnostic). That valid steering fixture finishes 318.900 s at 60 and DNFs beyond
500 s at BOTH 120 and 144, at checkpoint 27. The high arms were allowed only after the
2,296,200-value 60 replay proof passed. Steering has one full-route seed; there is no claim of a
multi-seed steering distribution.

Fresh production-helper tests pass original 60 arithmetic/NULL-body inertness, rational force-window
ownership, AI random sample holding at every tested rate, and forced online-60 configuration
(60/61/75/90/100/120/144). Comparison, window interpolation/snapshot checks, consumed-input interval
checks and the live wrong-image PID refusal test pass. Sound HLE compiled at -O2 and reports
`test_sound: ok`; no PCM capture, live listening or new peer test is claimed. The original retail
image tool again reports all eight sections MATCH.

Both the grounded watchdog and 60 early return remain: no underlying contact-equivalent human fix
has been demonstrated. Online is still forced to 60, and replay/ghost remain disabled above 60. No
uncontended paced-144 benchmark or multi-track equivalence has been obtained. Their referenced
executable still exists. No merge, push or publication occurred.

### AI passing trial: live coverage and measured negative result

Both bba533d2 suites have equal start snapshots and supported GO/+15 s starts. At GO, the opt-in
cache has 3 fresh draws / 0 reuses at 120 (AI slots 1/5), 1 fresh draw / 0 reuses at 144 (AI 1).
Player and every AI window metric are unchanged: mean AI three-second drift stays +1.139434% /
-1.347692%, and quarter-second drift stays -0.691348% / -4.551575%. Thus this path cannot explain
the measured GO deficit in these fixtures.

At +15 s there IS live repeated-draw coverage in AI slot 7: 17 draws + 15 reuses at 120, 17 draws +
19 reuses at 144 (other slots zero). Mean AI drift changes +1.275980% -> +1.142004% / +7.810028% ->
+7.861624%. AI 7 itself changes +8.884869% -> +8.876991% / +65.912136% -> +65.419446%; its large
late-window divergence is not evidence of a smooth steering error. The player is unchanged. Early
quarter-second results remain unchanged at both starts. The per-update float RNG source exists and
the sample-hold helper has both unit and live coverage, but benefit is mixed and it stays OFF. It is
NOT promoted as a demonstrated solution to remaining systematic AI bias. Private
windows-ai-control-bba533d2/windows.json and windows-ai-jitter-bba533d2/windows.json retain every
slot/metric/raw trace.

Final bba533d2 canonical particle probe PASSES at 60/120/144: all 128 initial records equal; last
death times 1.966667 / 1.975000 / 1.972222 s, maximum death cohort differences 0 / 1/120 / 1/144 s.
This is a canonical simulation-only burst of the actual pickup routines, not proof that
route-dependent rendered particles remain identical. Result: particle-final-bba533d2/result.json.

Final bba533d2 forced-crash sweep is complete through field 1860 at all rates. The injected real
controller's interval is exactly fields 601..630 (30 fields) at 60, 120 and 144. 144 additionally
crashes naturally at 1657..1728 (72 fields), exactly as the b6264b92 control; 60/120 do not. Thus
the injected duration is preserved, while natural crash trajectories remain unequal. The general
race screen correctly says NEEDS_REVIEW (short fixture has no finish/route/lifetime coverage); that
verdict is not suppressed or relabeled as full acceptance. Private
crash-final-bba533d2/comparison.json retains both intervals and coverage.

This continuation's measurement/validation step is complete. Acceptance remains FAILED: systematic
early AI drift is unresolved, small correlated windows do not establish zero bias, full-race
censoring and steering outcomes differ, active-ragdoll switch history is unsupported, and the human
watchdog is still a policy. Next bounded experiment: isolate the GO first-quarter-second force,
steering and contact terms from the same prefix before interpreting another chaotic full race; add
explicit ragdoll-history retiming before admitting those window starts. No diagnostic candidate is
enabled on the basis of mixed means.

### Round-52 launch isolation: instrumentation

The x34 crank ramp and crank phase already have elapsed-time conversions; x7A74 boost hold already
uses whole legacy fields. Added an explicitly enabled, read-only DHD_FPS_LAUNCH_TRACE observer for
AI slot 1, from countdown through GO+125 fields, at controller entry/output, pedal request
resolution, pedal force application, low-speed velocity floor and crank update. Rows record physical
clock phase, input requests, launch hold, crank cadence/phase/ramp, velocity, force accumulator,
steering and wheel contacts. Both comparison harnesses accept --launch-trace and place raw CSVs only
in their private output root. No physics fix is inferred from the source audit alone. Existing
measured deficits and finish distributions remain the before control. No online research is needed
for these game-specific state observations; no external code used.

_Cleanup 0.2.5: DHD_FPS_LAUNCH_TRACE was removed; still in git history at `99d04692`._

### Launch controls and isolated candidates

Clean 434be413 (SHA256 96ce7e1c962953ea...) reproduces the previous GO quarter-second AI deficits
exactly: -0.691348026% / -4.551575308%. All start snapshots match. Private launch-clean-434be413/
has per-update AI slot-1 traces and launch-analysis.json. In that common-prefix experiment, first
pedal request interval begins +16.666667 ms at 60, +25 ms at 120, +27.777778 ms at 144 after the
switch. The controller increments whole-field x430 before 00111368 tests its reaction delay x384;
high rates apply the request only over the last fraction of the same legacy interval. Crank x34 and
phase already scale with time, x7A74 remains zero in this launch, and crank phase is not an input to
the pedal-force producer 00136790.

The low-speed assist in 00136790 separately re-tests its speed cap every real update, then adds
scaled velocity directly. Slot 1 gets 10 / 9.5 / 9.583333 equivalent kick fields at 60/120/144 in
the supported window; independent cold 144 gets 9.166667 versus 10 at 60. This is a thresholded
finite-step rule, so scaling each kick alone does not preserve its original cap overshoot.
Cold-start traces are in launch-cold-434be413/.

Experiments replace only enumerated freshly compiled objects against the 434be413 clean object list
(private manifests hash the replacements); a full clean build remains mandatory for any final
candidate. Reaction-age rounding to the containing legacy interval flips quarter-second AI drift to
+3.690622% / +3.906538% (3-second +1.324266% / +0.609270%). It is not accepted as equivalence.
Scheduling whole velocity kicks at field boundaries is also rejected: it changes the initial
fractional input/position feedback and gives large first-quarter relative outliers in initially
almost stationary slots 4 and 7. Raw per-slot values remain visible; no aggregate is clipped.
Neither experiment is evidence that the AI finish-time gap is solved.

Two earlier incremental-root runs (launch-control-434be413 and launch-observer-434be413) are
explicitly REJECTED in their private directories: stale root clock objects left the requested window
at 60. Their game processes were stopped only by PID after exact owned executable-path verification,
and their harness PIDs after exact task command-line verification. No unrelated process was stopped.
All accepted controls above use the committed clean build.

The retained candidate holds only the low-speed cap decision for a legacy field and distributes that
finite kick over the actual elapsed fractions. Straddling updates are split at the field boundary;
the next speed test uses the velocity after the old field's partial kick. Contact support is still
checked every update. The 60 caller keeps its original condition/arithmetic. A production-helper
regression independently checks the discrete rule (cap 10, kick 3 => final speed 12), at
61/75/90/100/120/144; all pass. The earlier continuously re-tested scaled kick instead stops near
10. The NULL-body 60 early return also passes. This fixes a specific finite-step threshold error,
not all contact/integration differences.

With equal snapshots, held-cap GO quarter AI drift is -0.495837% / -4.255546%, and 3-second drift
+0.371380% / +0.246080%. It improves the longer window but DOES NOT solve the 144 first-quarter
deficit. The reaction rounding and whole-impulse candidates are not retained in the default policy.
The complete held-cap 60 dense trace equals the clean control. Private launch-held-floor-trial/ and
launch-held-floor-cold/ retain raw evidence. Three-seed AI finishes and the final clean gate remain
required.

Launch trace schema correction: x7A69 is the tuck state (previous CSV header called it boost); x7A74
is the launch/pedal boost hold. New traces label those explicitly, resolve the actual tuck request
at x1188/x1250, and include AI x430/x384/x3B4. The correction is read-only. Optional
--stop-ai-finish exits only after every AI's official finish flag has been recorded, allowing the AI
distribution to avoid the weak no-steer player's long censored tail. Any player observation from
that mode is censored at its actual stop time.

### Solver-stage trace: a straddling force-window defect

The solver observer now includes AI slot 1's real solver operations, dt / remaining / friction
values, normals and grip, not only generated pedal force. Stage 10 is solver entry, 12 integrator
entry (including tentative work which may be restored), 13 force clear, 11 solver exit, 14/15
friction before/after cap conversion. Tentative integrations must NOT be naively summed as applied
impulses. Private launch-solver-fae69b12/ reproduces the held-cap metrics.

The first identified force-window policy error precedes the pedal request: after the zero-time
collision clears field zero, high update 264 spans 13.888889..20.833333 ms after the GO switch (144
Hz, phase 36). Its fresh Fz=-7888.91552 is allowed for the entire 6.944444 ms, although the fresh
legacy field begins at 16.666667 ms: only 4.166667 ms belongs to it. The old end-field ownership
correction assigns the entire straddling interval to the new field. 120's intervals align and do not
have this overlap error.

DHD_FPS_FORCE_OVERLAP_TRIAL=1 scales only that newly allowed force fraction by phase/60; ordinary
same-field suppression and exact-boundary behavior stay unchanged. It is OFF by default pending
measurements. --force-overlap-trial is available in both harnesses. An independent production-helper
regression integrates the overlap with [one legacy field, infinity), after field zero has been
discarded, at 61/75/90/100/120/144; all pass. The original default force-window regression still
passes separately. The 60 path returns before body access. This establishes the interval defect, not
its finish-time benefit.

_Cleanup 0.2.5: the rejected force-overlap trial was removed; still in git history at `99d04692`._

### First full-distribution result: cap candidate quarantined

Clean fae69b12 built all 3,797 units and linked; SHA256 begins 0f0b0710ce734eca. Default r46-plain
gate MATCH: 14 EE comparisons and 1,159 readback/Z records through GS field 9080. No reference,
exclusions, comparison, or timeout changed. gate-held-fae69b12/ records the frozen hash. Its
validated consumed-input steering 60 replay matches all 191,350 rows / 2,296,200 riding values, and
finishes at the established 318.900 s. Higher rate steering arms start only after that proof.

All three 60 full-pedal AI controls reproduce the original distribution: 27 finishes, mean
313.346914 s. Held-cap 120 now also has 27 finishes, mean 325.804321 s, versus the original policy's
322.671 s. Per-seed held-cap means: 330.300000 / 321.955556 / 325.157407 s. This is +3.9756% against
60 and does not meet the approximately 1% target. 144 is still running. Therefore the cap candidate
is quarantined behind DHD_FPS_PEDAL_CAP_TRIAL=1 (and --pedal-cap-trial), OFF by default. The default
caller retains the previous speed condition and scaled kick arithmetic. fae69b12 distributions are
explicitly the candidate experiment, not the later default policy.

_Cleanup 0.2.5: the rejected pedal-cap trial was removed; still in git history at `99d04692`._

The force-overlap trial also fails the short-window benefit screen: with equal start snapshots, 144
quarter AI drift -4.255546% -> -6.137533%, 3-second +0.246080% -> -1.982689%. It stays OFF. The
reaction-plus-held-cap combination gives quarter +4.137532% / +4.017784%, 3-second +0.355581% /
+1.773922%; it does not establish equivalence and is not promoted. Private launch-held-combo-valid/
retains the supported run; the earlier launch-held-combo-trial attempt failed to link while a clean
rebuild replaced its object basis and produced no game measurement. No stale output is used.

The candidate timing/cap defects are real finite-interval differences, but none explains away the
full remaining GO bias or establishes the requested finish equivalence. No production tuning is
chosen to cancel one aggregate.

### Completed three-seed held-cap finish experiment

All 81 AI finishes were observed, with nine AI riders in each rate/seed race. The diagnostic exits
after recording all nine AI flags, so the no-steer player is not used to choose the AI observation
horizon. Official finish counters / 60 give seconds; wall throughput under shared machine load is
not an acceptance measurement. These are three seeds, not 27 independent races per rate.

| Rate | Previous default AI mean (s) | Held-cap fae69b12 AI mean (s) | Candidate finishes |
|---|---:|---:|---:|
| 60 | 313.346914 | 313.346914 | 27 / 27 |
| 120 | 322.671 | 325.804321 | 27 / 27 |
| 144 | 328.027 | 325.333951 | 27 / 27 |

Candidate 144 per-seed means are 328.257407 / 326.737037 / 321.007407 s. The candidate remains
approximately 4% slower than 60 at BOTH high rates. It improves 144 and worsens 120 against the
previous policy; it fails the approximately 1% target and stays OFF. Full per-slot finishes and run
hashes are in finish-held-fae69b12-summary.json and finish-held-fae69b12-81257/81258/81259.

The validated consumed-input steering 60 replay passes all 2,296,200 values. After the primary 120
AI distribution failed, the optional full-route 120 steering arm was deliberately censored at field
9976 (166.266667 s), checkpoint 9, unfinished and not crashed. This is NOT a DNF / finish-time
observation. Its exact last row and stop reason are in steering-held-fae69b12/120/CENSORED.json.
Harnesses and game images were stopped only after command-line / exact image-path PID ownership
checks; private round52-owned-process-ops.log retains their IDs.

Supported three-second windows using the validated consumed-input steering script have equal start
snapshots. Player speed drift is -4.981091% / +0.287210% at 120/144; AI quarter-second drift remains
-0.495837% / -4.255546%, while AI three-second drift is +3.503271% / +2.250806%. These are secondary
short-route observations, not complete-route equivalence. Private
steering-windows-held-fae69b12-valid/ retains the raw states. The earlier directory without -valid
failed before game launch because that shell lacked DHD_ISO; it is marked REJECTED and supplies no
measurement.

No candidate is promoted on these mixed results. The first divergent policy quantity identified is
the straddling force interval before the first pedal request; reaction timing and cap re-sampling
are additional discrete-step differences. Correcting an isolated interval rule did not remove the
measured bias. A complete causal explanation of the remaining 144 launch deficit is still open. The
next investigation must track actual collision-time force clears (remaining / rollback), rather than
assigning every clear to the real update's end field, and distinguish committed from tentative
integration.

Final review corrected a diagnostic coverage gap: the initial launch CSV cutoff applied tick>125
during countdown as well as after GO, omitting late countdown updates. All GO+0..+125 measurements
above remain covered and valid; the missing countdown is not claimed as measured by those earlier
CSVs. The cutoff now applies only in phase >=2. Fresh clean cold 60/144 traces must cover every real
update from the full countdown through GO+125 before handoff. This changes observation only, not
controller/physics/default policy.

### Final launch-isolation handoff

Final clean source revision b8a1641bef672bb77580e6de570636d7fd4a5704 compiled all 3,797 units and
linked with the existing 23 SDK stubs, no undefined symbols. Executable:
build/port/clean/b8a1641b/dhd.exe. SHA256:
23ac42d9d9fdd40e64130207ff0840625522db90cb38f696eccf88d1a0ca232d. The default r46-plain gate
PASSES/MATCH: 14 EE comparisons and 1,152 readback/Z records through GS field 9073. References,
comparison functions, exclusions and timeouts are unchanged. Private gate-final-b8a1641b/result.json
records that exact frozen executable hash.

Full-countdown cold trace coverage is verified independently: at 60 there are 260 countdown + 126 GO
updates (frames 1..386); at 144, 627 countdown + 301 GO updates (frames 1..928). Both cover
countdown clock 260 and GO clock 125, with no missing real updates. x7A74 is zero throughout both
traces, so a launch-boost hold is not active in these fixtures. All 59,360 compared riding values
match the earlier default-policy cold prefixes exactly. This checks the trace-only cutoff correction
and the unchanged default policy; it is not high-rate versus retail equality. Private
cold-launch-final-b8a1641b/ and final-b8a1641b-countdown-coverage-and-default-prefix.json retain the
proof.

The preceding quarantined-default build bbf766f9 also passed r46-plain (14 EE, 1,137 readback/Z
records through GS field 9058). Its GO controls at 60/120/144 reproduce 133,780 recorded values of
the original default exactly; enabling only --pedal-cap-trial reproduces the candidate's same
133,780 values exactly, zero differences in either comparison. That count includes dense eight-field
rows plus ten-field coarse riding rows, so overlapping observations are not independent samples.
Private final-bbf766f9-default-and-cap-equality.json records every count. The sole runtime change
from bbf766f9 to b8a1641b is the corrected diagnostic countdown cutoff; the final cold-prefix and
renderer checks above cover it directly. The cap and overlap flags are both absent by default;
reaction rounding and whole impulses are not retained as default gameplay changes.

Production-helper tests pass the original 60/NULL-body path, cap overshoot example and exact
force-overlap intervals across 61/75/90/100/120/144. The existing timer, animation, particles,
recovery and forced-online-60 checks pass. Input interval, envelope, replay comparison and window
tests pass. Superseded/failed experiments remain explicitly labeled in the private evidence tree.

This bounded launch-isolation step is complete, but the requested approximately 1% AI finish
agreement and removal of the consistent GO deficit are NOT achieved. The held-cap experiment's
120/144 mean finishes remain 325.804321 / 325.333951 s versus 313.346914 s at 60, and its GO quarter
deficits remain -0.495837% / -4.255546%. It is not promoted. The final default therefore retains the
earlier measured gameplay policy and its unresolved bias; no new full-race equivalence claim is made
for the final diagnostic build. No merge, push, publication or installation occurred.

### Input-edge continuation: traced popup duplication (2026-10-18)

The HLE already samples once per real high-rate VBLANK; pad_refresh still re-reads live
buttons/pressure/sticks after the low-latency wait. Sampling frequency was not the demonstrated
defect. The popup input manager 001909A8 caches its held/pressed masks using 00238F48 (whole 60-Hz
fields). At high rates that timestamp remains equal across two or three real updates, so the early
return preserves the previous update's pressed mask. Every 00190D60 consumer can then act on that
same edge again.

Read-only per-call DHD_INPUT_TRACE captures prove this before the fix: SDL virtual gamepad, PC
SETTINGS, 144: update 734/735/736, all field 217, Down query action 1 returns 1 three times; cursor
0->1->2->3. At 120, updates 694/695, field 255, cursor 0->1->2. At 60, update 473, field 326, only
0->1. Windows SendInput at 144 independently produces two Down consumptions at updates 845/846,
field 256, cursor 0->1->2. Main menu and OPTIONS each move once in these same before runs; those
front-end screens use the separate 001F4A98 path. We do not extend the popup diagnosis to
unreproduced screens or claim that every action had this cause.

_Cleanup 0.2.5: DHD_INPUT_TRACE was removed; still in git history at `99d04692`._

The port-only patch gives this cache the real game-frame stamp above 60, keeping the original
whole-field stamp at 60. Multiple consumers in one update still share the masks; the following
update polls and rebuilds them. Held buttons, pressure and analog values are never pulsed or
blanked. The existing JIT pad_refresh and scripted/online delivery paths stay intact. Input latch
and vibration durations in 0021F8E0, plus action-lock and last-attack-side timers in
0017EB00/0017E440, now advance by whole legacy fields at high rates. These timers retain their
real-time units when polling runs every update.

The first fixed 144 SDL run has exactly one move for each PC SETTINGS Down/Up/800-ms-held-Down
input, and one confirm consumption. The production manager/button-reader regression tests two
independent press intervals, continuous held masks/sticks, and a repeated call within one update:
all pass at 60/61/75/90/100/120/144. Extended live checks and clean gates follow.

Reproducible tool: port/tools/probe_input.py. Keyboard mode uses SendInput and checks focus on its
owned small game window, restoring the previous foreground window after each press. SDL mode
attaches an explicitly enabled virtual gamepad to player 1, and goes through SDL event pumping,
mapping, latest-state copying, pad HLE, and the game. This is SDL integration coverage, not hands-on
physical-controller testing. SDL's pinned build enables its virtual-joystick driver for this probe;
no upstream source is modified. DHD_TEST_SDL_PAD and DHD_INPUT_TRACE are opt-in and inactive
normally.

_Cleanup 0.2.5: `probe_input.py` and the DHD_TEST_SDL_PAD virtual pad were removed; still in git
history at `99d04692`._

Sources used: SDL's official SDL_AttachVirtualJoystick, SDL_VirtualJoystickDesc and
SDL_SetJoystickVirtualButton documentation: https://wiki.libsdl.org/SDL3/SDL_AttachVirtualJoystick ,
https://wiki.libsdl.org/SDL3/SDL_VirtualJoystickDesc ,
https://wiki.libsdl.org/SDL3/SDL_SetJoystickVirtualButton . No external code was copied. Initial
attempts without a visible window and with the virtual pad in player 2 are rejected, not used as
input measurements. All raw trace CSVs, executables and game logs stay in private ignored artifacts.

Input-stamp review refinement: use D_0029E10C, which both the main loop and 001B5130's modal loop
increment per real update, rather than the kernel's VBLANK count. A kernel/RPC wait can deliver a
VBLANK inside a game update; that must not expire an edge while another consumer in that same update
is still reading it. The regression now injects that extra VBLANK and requires unchanged masks and
no extra poll. This is a stronger cache-ownership rule; the observed duplicate-edge cause is
unchanged. A new final clean build is required for the refined helper; earlier a89b9c38 checks are
intermediate.

#### Final input matrix on clean 43b5c068

Plain build_clean.py compiled all 3,797 units and linked with the usual 23 SDK stubs and no
remaining undefined symbols. Executable SHA256:
ad3aa0c99d9729296fb857129bc68e7ab6ade40844909438e040c436bbf1f7b1. The later probe-tool refinements
do not change runtime code.

Six sequential paced live-input suites cover SDL and Windows SendInput at 60/120/144. They contain
60 settled menu cursor presses (main, OPTIONS, PC SETTINGS): every press performs exactly one cursor
action. Every popup confirm is consumed once. Eighteen isolated punch taps start eighteen actual
attack animations, observed at 001525E8 rather than inferred from input bits. The three before
controls at 60/120/144 also start one attack per punch tap; the tested race fixture did not
reproduce a double punch. The new action-lock regression independently proves its 20-field duration
is 20/40/48 updates at 60/120/144, preserving time rather than per-update cooldown speed.

SDL held-steering intervals have 30/53/53 consecutive active samples at
60/120/144; keyboard intervals have 30/56/61, with the expected ramp up to
127. All have zero neutral holes between first and last active sample.
Held pedal inputs likewise have zero neutral holes. Sample counts describe
these input intervals, not FPS throughput or benchmark results.

The DHD_NAV route reaches PC SETTINGS and performs one Down and one Up at each rate: exactly two
direction-edge consumptions, with no duplicate update. Baseline SDL PC SETTINGS press counts are 1
at 60, 2 at 120 and 2-3 at 144; the independent before SendInput 144 test gives 2. After counts are
1 at all rates through both paths. Main/OPTIONS and the tested confirms remain 1 before and after.
The earlier unpedaled race probe is insufficient attack-start coverage and is not used to claim a
passed action test.

Private final-input-analysis.json retains per-case counts, analog raw values, held-input checks and
exact executable hashes. All nine cases pass semantic checks, not just process exit codes. Private
input CSVs and per-case logs retain the underlying per-update data. Probe navigation retries only
boot/ load/fade transitions; the settled counted presses are isolated inputs.

#### Input acceptance complete; unchanged default gate MATCH

The ten-second SDL held-Down check gives one cursor move in both the before 60 control and the final
144 build, spanning 600 and 400 whole game fields respectively. It crosses the original 300-field
input timer without adding repeats. Thus the tested hold behavior is preserved rather than
accelerated.

Clean runtime revision 43b5c068 passes the plain default r46-plain gate: all 14 EE comparisons
MATCH, with 1,143 readback records / 1,143 Z reads matching through virtual GS field 9064. The
frozen hash is the clean hash above. The checker, reference, exclusions and deadlines are unchanged.
Only physical window size and verified-PID shutdown were supplied by the local wrapper, equally to
both arms. No FPS benchmark was run.

The final tip's later changes are the input proof tool and this validation record; its
runtime/build-input files equal 43b5c068. The FPS-only morning launchers now use
build/port/clean/43b5c068/dhd.exe. Combined CPU-plus-FPS refresh is a separate merge-only local test
integration; its executable, gate and launcher results are retained with that build's private
README. Rejected and intermediate attempts are labeled in the private evidence tree. No merge to
main, push or publication occurred.

### Adaptive wall-time continuation (2026-10-18; validation in progress)

The selected high rate previously advanced a fixed step even after a missed picture deadline. Paced
offline high-rate updates now measure elapsed QPC time at the game's clock wait, after the JIT input
wait. An eight-update error filter spreads the difference between elapsed and nominal time across
updates. The unconsumed error stays in the accumulator, so smoothing does not discard time. Steps
stay between 1/selected-rate and 1/60 second. Late-frame debt is retained even when a step reaches
that cap. Sustained throughput below 60 updates/s cannot be made real-time under the required
maximum step, and is outside this bound.

This fits the existing per-time conversions better than changing between rate presets: field timers,
sub-field animation, integration, decays and blends all consume the same measured interval. Audio
follows that interval too. The Verlet ragdoll displacement converts its previous span to the current
span; the grounded human recovery watchdog now counts elapsed fields. Disabled cap/force-overlap
trials accept fractional phase but remain OFF. Input edges still expire on the real game-update
stamp, independent of field crossings. The picture pacer continues to use the selected nominal
interval.

DHD_FPS=60 and online use the original retail clock paths. Unpaced diagnostic runs keep the original
rational high-rate steps for repeatable comparisons. Replay and ghost policy is unchanged. Online
VBLANKs invalidate the offline wall anchor: returning offline cannot count time already advanced by
the original online 60 clock a second time. No matching/checking code is modified. Production-helper
tests inject sustained late frames, alternating noise and a 50-ms hitch; they verify game/wall time,
step bounds, whole-field and animation totals, free Verlet velocity and forced-online 60 at
61/75/90/100/120/144.

Research: Glenn Fiedler, Fix Your Timestep!, author-maintained source:
https://github.com/mas-bandwidth/gafferongames/blob/main/content/post/fix_your_timestep.md . Used
for bounded timesteps and retention of accumulator remainder; no code copied. Fresh paced
before/after races, forced-late-frame runs, clean builds and unchanged r46-plain gates are pending
below; helper checks alone are not race acceptance.

Clean adaptive runtime 9ded5172 compiles all 3,797 game units from scratch (71 s), links in 203 s
with the existing 23 SDK stubs and no undefined symbols. Executable:
build/port/clean/9ded5172/dhd.exe. SHA256:
f39dc8db44b20fa3daa6341d804f53c3a77f5c774425c73811754a4b7ad1ff4e. No recomp basis or build-input
overlay was used. New runtime validation is still pending; this build record is not a gate/AI
acceptance claim.

The clean 43b5c068 before controls completed all nine AI finishes at both rates: 120 mean 323.598148
s; 144 mean 330.625926 s (seed 81257, full pedal). GO..+10-s game/wall ratios are 0.794817 /
0.605804; whole all-AI-finish race ratios are 0.916123 / 0.772891. The comparison is about game time
keeping pace when frames are late, not a CPU optimization.

GO..+30 game seconds took 46.984089 wall seconds (ratio 0.638514), 92.206577 pictures/s, p99 25.037
ms, worst-1%-mean 26.206093 ms. Accepted after runs and all clean gates remain to be added.

GO analysis reconstructs actual fractional step widths from consecutive per-update clock stamps,
including the countdown before the first controller call. A cold fixed-144 capture independently
reproduces the rational-time result to below 1e-5 speed units. The new after runs enable this launch
trace through GO+125; its additional early logging cost must not be mistaken for a renderer
throughput change. The timing policy must hold despite that cost.

Final runtime superseding 9ded5172: 04d7bc06 invalidates cached Verlet spans on clock/scene reset.
The fresh-throw regression fails without that reset and passes across 61/75/90/100/120/144. Clean
04d7bc06 compiles all 3,797 units in 48 s and links in 157 s, with 23 existing stubs and no
undefined symbols. Executable SHA256:
be3573cc5b46f8cc98159532297aa30fb7ec177ee8ddffd18446b3e0fd8737d0. Path:
build/port/clean/04d7bc06/dhd.exe. The queued after campaign, 60 control, r46-plain gate and live
input suites use this executable. Gates/acceptance remain pending until their actual results below.

#### Final 120 paced all-AI race (04d7bc06)

GO..+10 game seconds: game/wall 0.794817 -> 1.000476; pictures/s 95.374496 -> 103.948084; p99 15.669
-> 14.338 ms; worst-1%-mean 17.313333 -> 16.472727 ms. Whole all-AI-finish race: game/wall 0.916123
-> 1.000027; pictures/s 109.935815 -> 113.693971; p99 13.227 -> 12.566 ms; worst-1%-mean 15.738018
-> 15.575647 ms.

Equal-time GO+0.25-s mean AI speed is 15.244382 -> 15.323516; mean over 25..250 ms is 7.067940 ->
7.276511. The unweighted mean of per-slot after/before ratios is 1.011306 at 0.25 s and 1.001103 for
the per-slot window means; no rider is excluded or clipped. Individual riders can still diverge.
Mean official AI finish is 323.598148 -> 321.374074 seconds, nine finishes per arm, one fixed seed.
The unsteered player is not claimed to have finished: this timing window ends when all nine AI
finish. These results prove the 120 clock target in this fixture, not high-rate physics equivalence.
Final 144, forced-after, 60 gate and integration are still pending. Private
adaptive-final-summary.json collects accepted fixtures; rejected/incomplete directories are excluded
explicitly.

#### Final busy-PC clock results and remaining AI failure

No further long benchmark campaign is required. All performance and AI timing below is noisy
shared-PC evidence, not a controlled throughput comparison. Already completed all-AI races are
retained.

At paced 144, accepted adaptive-final-after/144-attempt-1 gives opening game/wall 0.605804 ->
0.999873, picture rate 87.235 -> 106.204/s, p99 17.388 -> 14.162 ms, worst-1%-mean 19.380 -> 16.270
ms. Through all nine AI finishes, game/wall is 0.772891 -> 1.000047, picture rate 111.319 ->
131.643/s, p99 14.494 -> 11.819 ms and worst-1%-mean 16.158 -> 13.887 ms. This is not a claim that
the unsteered player finished the course.

With repeated 12-ms suspensions of verified owned game PIDs, the 30-game-second 144 window improves
from 46.984089 to 29.996133 wall seconds: game/wall 0.638514 -> 1.000129, pictures/s 92.207 ->
113.515, p99 25.037 -> 21.262 ms, worst-1%-mean 26.206 -> 23.062 ms. The three-second selected-60
control reports 1.000450 game/wall and 59.939 pictures/s; bit-exact acceptance still requires the
unchanged gate.

AI NONREGRESSION IS NOT MET. At 120, equal-time GO+0.25-s speed is 15.244382 -> 15.323516 and mean
official finish is 323.598148 -> 321.374074 s. At 144, GO+0.25-s speed is 15.517042 -> 15.114113 and
finish mean is 330.625926 -> 325.777778 s (nine AI per arm, seed 81257). The original
fifteen-sample, equal-time quarter-second per-slot mean after/before ratio is 1.005662 at 120 but
0.936563 at 144: the measured start deficit worsens 6.34% by that metric. No slot is omitted.
Busy-PC noise and different actual step sequences limit causal inference, but do not turn this into
an AI acceptance pass. No AI parameter tuning or weakened assertion was used. This remains an
experimental local clock fix with an explicit unmet AI requirement; the pre-existing high-rate
physics-equivalence failure remains open too. 