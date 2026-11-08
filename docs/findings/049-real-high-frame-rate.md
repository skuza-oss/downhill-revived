# Real frames above 60: the game's frame clock (research)

Status: partly superseded by 139 (the verdict and plan for real 120; the 60-only launcher)
Checked: 2026-11-04

Still true, and the base of findings 067 and 139: the survey of the frame clock and its users
(the "frame clock", "who reads it", "verdict classes" and "work that ignores the step"
sections). Superseded: the verdict and the plan. Real high rates were built: the frame-rate
unlock (finding 067, 60-240 fps) and one delta-time clock for every rate, 60 included (finding
139, `port/hle/fps.c`, `port/hle/kernel.c`); replays and ghosts work on it (finding 142). The
launcher offers 60-240 and Monitor (`fps` in `port/render/settings/src/table.rs`).

2026-10-16: full source survey of the clock's users plus a hunt for per-frame work that
ignores the clock. No experiment build was run (reason under "Experiment"). Verdict: real 120
steps a second is possible but needs a set of known game-code fixes (about 25 hand edits plus 4
shared helpers that cover about 190 call sites), a fractional animation clock, and an audit of
code nobody has read yet for more per-frame constants. The port also has to reach 120 frames a
second first. Not a small step.

Context: the user rejected interpolated or generated in-between pictures (finding 038's
`DHD_INTERP` made ground, billboards and trees shimmer while the camera moved: pieces held at the
newer field next to pieces moved halfway, and the in-between pictures were drawn into the frame
buffer the display circuit wasn't showing). The launcher then offered only the game's own 60
pictures a second (today: 60-240, finding 067). The question here: can the game itself simulate more than 60 steps a second?

## The frame clock (`src/unsorted/00238*.c`)

- `func_00238A68(rate)` (called with 60 from `00158680`, `001831A0`, `0019EA38`, `0019EDE8`,
  `001B70F0`, `001B7198`, `001B9B70`, `001EB1D8`) resets it: `D_0029E3F0` (the most steps a
  second) = 60, `D_0029E3D0` (the step in fields) = 1, `D_0029E3D4` (steps a second, int) and
  `D_0029E3FC` (float) = rate, `D_0029E400` = 1/rate, and starts EE timer 1 (mode 0x82:
  BUSCLK/256).
- `func_00238C00`, called once a frame (`001B5130`, `00182D78`, `001EB1D8`), turns timer 1 counts
  into whole fields (9619 counts each, at most 3) on the field counter `D_0029E3D8`.
- `func_00238D98`, right after it: the step `D_0029E3D0` = fields since the last frame, clamped to
  1..2; steps a second = 60 / step; while that exceeds the target `D_0029E3F4` it waits another
  field and lengthens the step. Then `D_0029E3FC` = steps a second and `D_0029E400` = seconds a
  step (1/60 or 1/30).
- `func_00238F68(value)` sets the target to 60/1..60/6 (the largest not above `value`). Its only
  other caller is the crash ragdoll (`001A4F70.c`: `func_001A5460` every frame, `func_001A5218`
  on exit).

So the game was written for a variable step of one or two fields (60 or 30 frames a second), not
a fixed 1/60. Only `00238*.c` touch the clock variables; everything else goes through the
accessors.

### `D_0029D36C` (when the game asks for 30)

`int D_0029D36C = 0` in `.sdata` (`001A4F70.c`), read only by `func_001A5460` (30 if set, else
60) and `func_001A5218` (back to 60 if set). Nothing in `src/` or `asm/` writes it, so it is a
developer switch that is always 0 in retail: the shipped game never asks for 30. It runs at 30
only when a frame overruns a field (the step becomes 2). The switch sits in the crash ragdoll,
whose Verlet solver is locked to 1/60 (below), which fits a debug aid for that solver.

## Who reads it

| Accessor | Value | Users |
| --- | --- | ---: |
| `func_00238F60` | step in fields (int, 1-2) | 69 calls in 58 files (+ the particle unit's `__tail` alias) |
| `func_00238F40` | seconds a step (float) | 33 calls in 24 files |
| `func_00238F38` / `func_00238F30` | steps a second (float / int) | 12 calls in 12 files / 23 in 19 |
| `func_00238F48` | field counter (timestamps) | 157 uses in 61 files |
| `func_00238F50` | field counter at the last frame | `001A9640`, `001BA3B8` |

The rider stores the step each frame (`0012C8B0`: `x7980`, cleared in `0012BC58`); no C reader
of `x7980` was found (`00136B40`/`00136C20` declare it in their structs but read `x797C`/`x7984`
and the float steps a second).

## How the step is used: verdict classes

At 120 the step would be half a field. Each user falls in one of these classes:

- **A, counter**: adds or subtracts whole fields on a timer and compares it with a field count.
  Works unchanged if the integer accessor returns a per-frame whole-field accumulator (0 and 1 on
  alternate frames at 120, computed once per frame in `func_00238D98`; the accessor is called
  several times a frame so it must not tick itself). Timing error at most one half field.
- **B, int times float**: multiplies the step into a float amount. With the accumulator it is right
  on average but moves in 60 Hz jumps (every other frame nothing happens), which is the judder the
  user rejected. Needs a float step (`seconds a step * 60`).
- **C, needs code**: divides by the step, uses it as a loop or sample count, or the function also
  does fixed per-frame work.
- **F, float**: scales with seconds a step or steps a second; correct at 120 as is unless noted.

### Integer step users (`func_00238F60`)

| File (function) | Use | Drives | Verdict |
| --- | --- | --- | --- |
| `0010BE38` | `x3B4`, `x414 += step` | AI racer controller (rider `+0x78D4`, `0012C8B0` case 3) | A |
| `0010BF50` | `x430 +=` | AI racer | A |
| `0010C108` | `x420 -=` (floor 0) | AI racer countdown | A |
| `0010C4C0` | `x430 +=` | AI racer | A |
| `0010C940` | `x438 -=` | AI button delay | A |
| `0010CB70` | `x418 -=` | AI racer (same function smooths `x4214` per frame, class R below) | A |
| `0010D0C0` | `x400 +=` | AI rival-distance timer | A |
| `001105F8` | `x42C -=`, `x434 +=` (and `x2C4 += 1/steps a second`) | AI trick timers | A |
| `00112360` | `timer -=`, decision when 0 | AI attack/avoid decision (random roll gated by the timer) | A |
| `00113650` | `x404 +=`, release at 16 | AI button hold | A |
| `00113958`, `001139F0` | `x410`, `x41C +=` | AI states | A |
| `00117218`, `00117278` | `xB4`, `xB8`, `xAC -=` | AI cooldowns | A |
| `0011E9F0`, `00129C68` | `x25C`, `x26A`, `x174`, `x194 +=` | ambient NPC actors (spring walkers) | A |
| `00121348`, `00124960`, `0015B910` | ground-contact timers `+=` | NPC / object ground follow | A |
| `00122AA0` | `x18 +=`, `x3C -=` | NPC (also per-frame blend, class R) | A |
| `00124300` | `x1C +=` on a ground hit | chaser object | A |
| `00128840` | `x20 -=` | rider timer (player context) | A |
| `0012C8B0` | `x7980 = step` | stored per rider, no reader found | A |
| `0012F5D0` | `x7994 +=` | rider race time (fields) | A; but `x7A3C += speed` per frame: C (below) |
| `0012F6A0` | `x7966`, `x796A +=` | pedal / coast durations | A; but `--x7A74` per frame: C |
| `0012F8D8` | `x7964`, `x7968 +=` | tuck state durations (`00136038` uses `x7964/60` as seconds) | A |
| `001357D8` | `x4A8 +=` | rider state 4 | A |
| `00136038` | `x4A8 +=` while airborne, boost ramp `x4A8/30` | rider physics | A; same function `x70 *= 0.9` per frame: C |
| `00138E08` | `x316EC +=`, sample every `D_0029C3C4` | rider path/ghost sampler | A |
| `00145368`, `0015A4F0` | `time +=` | item state machines | A |
| `00148E70`, `00148F60:2185` | `x368 +=`, compare 61 / 6 | object timers | A |
| `00148F60:1126` | `x364 +=`, angle = `x364` degrees | 1 degree a field rotation | B |
| `00155208`, `00156580` | `xC0 +=` (>60), `xB0 -=`, `xBC +=` | rider-attached logic, co-op respawn | A |
| `00157950` | `scale = 60/(float)step * 0.5` | velocity from a position delta | C: divides by the step (0 at 120) |
| `00161B10` | `count = max(1, step)`, trail delta / count | bike trail effect | B (needs float for even spacing) |
| `0016F420`, `00171DD0`, `00172118`, `00174968`, `00180000`, `00191FC0`, `001B0410` | state timers `+=` / `-=` | world objects (0x16-0x19 range) | A |
| `0017DEA0`, `0017EDE8` | 25 per-button timers, double-tap window 30 | pad button module | A |
| `0018D5A0` | `x8 -=`, then rider state change | rider state | A |
| `00196888` | `x10 -=`, reload from table | spawn timer | A |
| `0019EE98` | `race->timer +=` | race start / countdown clock (race-clock-keyed pad scripts read it) | A |
| `001A8EF0` | sample per frame, `x6 = step` (rewrites the previous sample when 2) | replay / ghost input recorder | C: one sample a frame, format assumes 1-2 fields |
| `001AD2B8` | crank phase `+= ... * step` | rider pedal animation | B; also `x34 += 0.05` per frame: C |
| `001DCB50` | `x48 +=` (>=60) | ambient sound emitter | A |
| `001FB160` | `time +=` (>20) | rider mode | A |
| `00202D78`, `002065C0` | `D_0029E160 += step` | animation and tween clock (ms = ticks * 16.667) | C (see "animation clock") |
| `0020CDF0` | `delay`, `hold -= dt`; `level += rise * dt` | light/flare fades | B |
| `00228BE8` (`func_0022DE40`) | step passed to every particle system | particles | C (see "particles") |
| `00239778`, `002398C0` | `-= step * 0.0333`, `time += step / 60` | 4-slot envelope (likely pad vibration) | B |

Tally of the 69 calls: 57 counters (A), 7 int-times-float (B), 5 that need code (C), plus the
particle unit's alias call (C) and the per-frame work found next to some counters.

### Float users

| Accessor / helper | Users | Verdict |
| --- | --- | --- |
| `func_00238F40` dt into the spring `func_00236558` | `0011F900`, `0011FB70`, `0011FC70`, `0011FF38`, `00120208`, `00123AD0`, `00124B40`, `001257B8`, `00126238`, `00126898`, `0012A3E8`, `0012A4D0`, `0012A570`, `0012A6D8`, `0012A790`, `0015AD60`, `0015AEE8`, `00180A10` | F: acceleration and turn limits scale by dt, the second `func_00236438` call is a speed clamp, position += velocity * dt |
| `func_00238F40` ground follow | `00120CE8`, `0012B188`, `0015B798`, `00180E78` (`max drop = dt * 32`, `z += v * dt`) | F |
| `func_00238F40` into `func_00209248` | `0017CD70`, `0017CE98` | F (stores dt with a node) |
| steps a second as `1/sps` dt | `00133DE0` (rider body), `00131998` (rider wheels, substeps of at most a quarter turn), `001460D0`, `00157BE8`, `00165E68`, `001AB418`, `001DCE58`, `001457A8`, `00180A10`, `00147F40`, `00148560`, `0016F198`, `0016FC00`, `00210868`, `00148F60:1825` | F: integrated by `func_002327B0` (position, momentum, angular momentum += rate * dt) |
| `1/sps` timers | `0010EC80`, `001105F8`, `001140A0` | F (seconds) |
| `60/sps` | `00137C78`, `00137E68` (rider turn rates), `00236088`, `00236130` | F, except the two helpers (next section) |
| `sps * vector` | `00145E80`, `00170278` | F (per-step delta to per-second velocity) |
| `speed / sps` | `00135A30:139`, `00154058` | F, except `00154058`'s per-frame drop (below) |
| `sps * 0.5 * k` as a force | `00136790`, `00136B40`, `00136C20`, `00136F80` | **wrong**: see "per-step impulses" |
| `sps == 60` | `0021F210` | **wrong**: half-line display offset only at exactly 60 |

## Work that ignores the step (the risky class)

Found by reading the rider physics chain (`0012D360` -> `00135690` -> `00135A30` -> `00136038`,
`00131998`, `00138658`), the AI controller, and greps for decay factors, per-frame `++`, smoothing
helpers and random rolls. Each runs per frame, so at 120 it happens twice as often per second.
(Most of these were already wrong at 30 on the PS2; the game was tuned at 60.)

1. **Rider forces that are per-step impulses.** `00136790` (pedal / tuck / boost push),
   `00136B40`, `00136C20`, `00136F80` build `force = mass * (steps a second * 0.5) * k`, add it
   to the force accumulator (`func_00231C20`, `+0xE0`) and `func_002327B0` integrates momentum
   `+= force * dt` with `dt = 1/sps`. The momentum change per step is `mass * 0.5 * k` whatever
   the rate, so at 120 the rider gets twice the push per second. Gravity and drag in `00136038`
   are proper forces (no rate factor). Fix: use 60 in place of steps a second in these four.
2. **Rider angular damping**: `00136038` ends with `SCALE(o->x70, o->x70, 0.9f)` on the angular
   momentum every frame: 0.9 per field at 60, 0.81 per field at 120. Fix: `0.9^(60/sps)`.
3. **Steering lean**: `00138658` moves `x488` by `rate * 0.03` per frame while left/right is held
   and multiplies it by 0.95 per frame when released; `rate = 1 - x4A4 / 600`. Twice as fast in
   and out at 120: the main handling feel. Fix: scale both by the step (`0.95^(60/sps)`).
4. **Airborne frame counter**: `00135A30` does `++o->x4A4` every frame (reset on contact). It is
   read as fields: boost window `x4A4 < 15` and ramp `x4A4 * 0.0667` (`00136038`), steering rate
   (`00138658`, 300), `x4A4 / 60` seconds for air rotation weight (`00137E68`), `x4A4 == 1` for
   the first air frame (basis setup and `func_001EA918`), `x4A4 < D_0029C2FC`. Fix: count fields
   (accumulator) with a separate first-frame flag.
5. **Boost hold**: `0012F6A0` `--x7A74` per frame (holds `x7A78 = 5.0`, a boosted push). Fix: step.
6. **Pedal animation**: `001AD2B8` `x34 += 0.05f` per frame (and class B crank phase).
7. **Smoothing helpers that ignore the rate**: `func_00235EA8` (linear) and `func_00235F38`
   (angle) move a value a fixed fraction toward a target per call. 84 calls in 42 files, all per
   frame with constant or per-frame-computed rates: AI racer (`0010BD10`, `0010CB70`,
   `0010F0D8`), rider (`0012F5D0`, `00135478` heading, `00136D68`), rider animation blends toward
   0.5 (`00122AA0`, `00123350`, `00125178`, `00125A28`, `00125E38`, `001264D8`, `001238B8`), NPCs
   (`001208D8`, `001217E8`, `0012ACC8`, `0012AA58`, `00123788`, `00123F60`, `0015B1A0`),
   objects in `00145BD0`, `001454D8`, `00148F60`, `001556D8`, `0015B460`, `00165B90`,
   `0016FF30`, `00171B28`-`00174098`, `00180000`, `001805B0`, `00192268`, `00192460`,
   `001926E0`. At 120 every one of them converges twice as fast. Fix in one place: make the two
   helpers rate-aware (as below), keeping the exact path at 60.
8. **The rate-aware helpers only correct downwards**: `func_00236088` / `func_00236130` compute
   `t = 60/sps` and apply `rate = 1 - (1-rate)^t`, `min *= t` only `if (t > 1.0f)`. At 120
   (`t = 0.5`) they do nothing. 105 calls in 35 files (rider body and camera angles `001AE358`,
   `001AF0F8`, `001AF438`, `001AF5E8`, `001AF790`, `00174968`, `00154058`, ...). Fix: drop the
   condition (2 functions).
9. **Crash ragdoll**: `00239A80.c` `func_00239CE0` is a Verlet step with `dt^2 = 0.0002777778`
   (1/60 fixed) and damping 0.99 per step; `func_00239DA0` runs 10 constraint passes per frame
   (`001A5460`). Locked to frames: at 120 crashes play at double speed. Fix: run it on whole
   fields only (moves at 60, a visible hold during crashes) or re-derive it for 1/120 (dt^2,
   0.99^0.5, constraint stiffness per pass).
10. **Particles** (`00228BE8.c`, `func_0022DE40` passes the int step): `func_002295D0` divides by
    it (`x2748 / count`), `func_0022AA28` takes `1/count` and adds `x272C / ...` to the emission
    accumulator per frame without the step (twice the particles a second at 120), and the
    asm-only `func_0022B708` raises the velocity decay to the integer step by repeated squaring.
    Needs a float step through the particle unit, including a C version of `0022B708`.
11. **Animation clock**: `D_0029E160` (int fields) is the time base of the animation engine
    (`00202E00.cpp`, 35 uses: `elapsed = (clock - start) * 16.667 ms`), tweens (`00208660`,
    `00208708`) and `001AC6F8`, `001ACAE0`, `00206440`-`00206878`. With whole-field steps every
    skeleton and tween moves every other frame at 120. Needs a sub-field clock (float, or
    half-field ticks with every `16.667` and every stored start converted).
12. **Random rolls per frame**: ambient NPCs roll `func_00222BD8(0,100) == 42` each frame
    (`0011EDA8`, `0011F1E0`, `0011F298`, `0011F3B8`, `0011F760`, `0011FB70`, `0011FF38`,
    `00129FC8`, `0012A4D0`, `0012AC28`, `0015A968`, `0015AC68`, `0015AD60`, ...): twice the
    events a second, and twice the draws from the random generator. The AI racer's rolls are
    gated by its timers (`00112360`, `001109B0`), so they keep their rate.
13. **Smaller ones**: `0012F5D0` sums speed per frame into `x7A3C`; `001A3220` shows
    `x7A3C / x7994` (fields) as average speed, so it doubles. `00154058` (homing projectile)
    subtracts `k * t^2` from a per-step velocity every frame. `0021F210` adds the half-line display
    offset only when steps a second is exactly 60. `0013C838` (`field counter % 40`) and the
    once-per-field guards `0018CFC8`, `001909A8` keep stepping at 60 (visual only).

The field counter (`func_00238F48`, 157 uses, mostly menus/HUD in `001C5040` and timed callbacks
in `00236B28`-`00236C60`) is fine if it keeps counting real fields.

Not audited: every per-frame function that reads no clock accessor and uses none of the patterns
above (the `0.9`, `0.95`, `++x4A4` cases were only found by reading the rider chain), the 67
functions still in assembly, and VU microcode. A grep for per-frame `++`/`--` on struct fields
alone hits 172 game files; most are loop indices, but some will be frame counters like `x4A4`.

## What real 120 would need (the plan then; what was built is findings 067 and 139)

1. **Clock**: half-field steps (`D_0029E3F0` = 120 so steps a second = 120 and `D_0029E400` =
   1/120; `func_00238F68` targets doubled), the field counter `D_0029E3D8` kept in real fields,
   and the integer step turned into a per-frame whole-field accumulator. Exact 60 must stay
   bit-identical, so every change is behind the mode (for example `1-(1-r)^1` is not always `r`
   in floats; skip the formula at 60).
2. **Port scheduling**: the EE frame runs twice per field while VBLANK, pads, timer 1 and
   `sound_tick` stay on real fields (`port/hle/kernel.c` `vsync()`/`deliver()`: today one game
   frame per virtual field). And the port must actually produce 120 frames a second: 96 unpaced
   in a race today (2026-10-16), heavy scenes slower; the VU1 thread bounds the frame (findings
   046, 048). This is the largest dependency.
3. **Game fixes**, all behind the mode: the four impulses (1), angular damping (2), steering (3),
   air counter (4), boost hold (5), pedal (6), the two raw smoothing helpers (7) and the two
   rate-aware ones (8) changed centrally, ragdoll (9), particles (10), animation clock (11), NPC
   rolls (12), the small ones (13), class B users switched to a float step, `00157950` and
   `001A8EF0` (C). About 25 hand edits plus 4 shared helpers that fix about 190 call sites, and
   the animation clock (about 45 uses in 12 files).
4. **Audit and test**: read the remaining per-frame update code (rider, AI, camera, NPC, items,
   HUD) for fixed per-frame work; then compare 60 against 120 on the same race-clock-keyed inputs
   (finish time, speed curve, AI positions, countdown, ragdoll, animations by eye) until the
   differences are what finer integration explains.

Estimate (agent time, excluding the renderer speed work in item 2): clock and port scheduling 2-4
days; the known fixes about a week; audit of unread per-frame code 1-2 weeks; validation tooling
and tuning about a week. Roughly 3-6 weeks, and the result can't be proven complete by reading.

## Risks

- **Handling feel**: even with every fix, 120 is not the same game as 60: explicit Euler with half
  the step, collision time-of-impact, the substep split in `00131998`, smoothing at `(1-r)^0.5`
  and random draws per frame all change trajectories slightly. Any missed per-frame constant shows
  up as a handling difference (faster steering, stiffer rotation, stronger pedalling), which is
  what players notice first.
- **Exactness**: state at 120 can't match PCSX2 (finer steps, different floats, different random
  sequence). 60 stays the exact mode; 120 can only be checked statistically.
- **Online** (corrected 2026-10-28): this point assumed input lockstep, which was dropped on
  2026-10-16. Online races are **state sync** (findings 044, 041): each PC runs its own race and
  the others are puppet riders moved from ~20 snapshots a second, so peers need neither the same
  rate nor identical floats, and mixed 60/144 races are possible. What a higher rate changes there
  is only what counts in ticks: the snapshot clock, the puppet delay, the send rate and timeouts
  (finding 114).
- **Determinism** of replays and ghosts: `001A8EF0` records one sample per frame with the step in
  fields; recordings at 120 differ in format and content from 60.

## Experiment

Not run. The survey settles the verdict: it shows by code which systems run at the wrong speed
and why (per-step impulses, per-frame decays, frame counters, rate-blind smoothing, a fixed-dt
ragdoll). A build that only halves the step without those fixes would also divide by a zero step
(`00157950`, `func_002295D0`, `func_0022AA28`) on the frames where the accumulator gives 0, so it
would need part of the fix list before it could run a race. What a test can add is a measure of
what the survey missed, so it belongs at the end of the work as the acceptance test: a build
with `DHD_SIM_HZ=120` (virtual VBLANK at twice the rate, `sound_tick` every other VBLANK, the
clock and accessors patched as in item 1, timer 1 untouched) running the race with the
race-clock-keyed pad script (`race_compare.py`'s `RACE_PAD`, read through `0019EE98`'s
`race->timer`), compared with 60 on finish time, rider speed at fixed race-clock moments, AI
order, countdown length and the crash ragdoll.

## Other projects (research)

- Perfect Dark PC port (fgsfdsfgs/perfect_dark): the N64 engine already had a variable timestep;
  the port offers "60 FPS support, including fixes for some framerate-related issues" and
  "experimental high framerate support (up to 240 FPS)", noting "issues running faster than ~165
  FPS". The closest precedent: a variable-step engine still needed per-system fixes.
  https://github.com/fgsfdsfgs/perfect_dark
- Zelda64Recomp reaches high frame rates by RT64 interpolation; "Changing framerate has no effect
  on gameplay" (the logic stays at the game's rate). That is the route the user rejected.
  https://github.com/Zelda64Recomp/Zelda64Recomp
- PCSX2 "60 fps codes" patches change the game's frame divider and often its timestep constants;
  games that tie physics and logic to frames speed up timers, animations and collisions.
  https://forums.pcsx2.net/Thread-60-fps-codes
- Dolphin PR 11486 (Skylanders): the delta time is bounded by a 1/60 "target frame rate"
  constant that has to change with the rate, like `D_0029E3F0` here.
  https://github.com/dolphin-emu/dolphin/pull/11486
