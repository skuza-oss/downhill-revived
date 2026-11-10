# 193: helicopters, birds and other scenery at the right speed at every frame rate

Status: current
Checked: 2026-11-10

Branch `fix/fast-scenery-high-fps` from `main` 9609ea6f, started 2026-11-09. Merged into `main` at ecde8cb7
on 2026-11-10; not yet released. Open leftovers are tracked in issue #76.

## The report

Players on Discord, 0.7.14: at 120, 144 and 240 fps the helicopters on Mt. Midoule fly far too
fast, and the birds on other tracks too. On some tracks (Red Pass was named) the helicopter moves
so fast and so far off its course that it hits the rider and kills it: it changes the race, not
only the picture. At 60 everything is right. An earlier attempt did not fix it.

## Goal and acceptance

Find the root cause and fix it in one place, so every such object (helicopters, birds, planes,
any scripted or path-following scenery or actor, every track) moves at the same speed in game time
at every frame rate. Per mechanism, not per object, unless the evidence shows truly separate
mechanisms.

Done when:

- `python port/tools/test_scenery_rate.py --out build/scenery-rate/x` (Mt. Midoule FR at 60, 144
  and 240) prints PASS on this branch and FAIL on main; with `--right N` the same on every track
  that has such objects.
- `python tools/check.py port` passes.
- Boot and race state traces at 60 (`state_trace_run.py boot|race`) equal main's
  (`state_diff.py` PASS): a deterministic run at 60 keeps the original arithmetic.
- `port/tools/test_fps.py` passes (the clock helpers, two new ones), and `test_fps_race.py` runs.

Out of scope: anything that is right at 60; the physics equivalence limits of finding 139.

## Root cause

The track's flying and walking objects move themselves: their updates never read the game clock.
Retail ran the world's update once per 60 Hz field, so each object steps by a velocity, a speed
change, a turn or a counter given per field, once per update. On the delta-time clock (findings
067, 139) an update is 60/N of a field, and every such step ran N/60 times too often: at 240, four
times as fast in game time. Finding 067 audited the code next to the clock's readers and the rider;
these objects' own steps were missed, except a few it found by reading (the helicopter's climb and
descent, `001737F0`/`00173CB0` `pos.z += 0.5f`; the attacker's climb away, `001926E0`), which left
the forward motion at one step an update.

There is no shared integrator to fix once: each object type has its own few lines of motion code
(`pos += dir * speed`, `speed += accel`, `yaw += turn`, `count++` against a field budget, a random
roll). The mechanism is one, its code is per object, so the fix is one rule applied at each site,
as finding 067 did for the rider: the step covers this update's part of a field (`port_step()`), a
counter counts whole fields (`port_whole_step()`, `port_count_down`), a roll draws once a field, a
blend's rate is step-aware (`port_blend`, already in the shared helpers). All in
`port/patches_fps.py`, behind `port_delta_clock()`: a deterministic run at 60 keeps retail's
arithmetic exactly.

### The helicopter

`E_HELI_L` objects: manager `func_00172758` (`D_003AF940`, up to 7 elements of 0x130), element
update `func_00172BD0`, waypoints `D_003B01E0`. Types (`001728A8`): 1 starts in state 5 and 3 in
state 7 (flying a loop of waypoints, hovering at its stops), 2 starts in state 1 and chases the
riders down the mountain (`00172CB0`: a waypoint 1100-1400 units below the lowest rider; state 3
flies there; on arrival 40 % of the time state 4, a hover 70 units above the track's height until
every rider is past, `00173018`).

| Unit | Per update | Fixed |
|---|---|---|
| `001737F0` | position += direction `x50` * speed `xB8` | the step * `port_step()` |
| `00173560` | speed `xB8` += `x87C` toward its target, or -= `x880` | * `port_step()` |
| `001733C0`, `00173E58` (hover) | yaw `xA8` += turn | * `port_step()` |
| `00173688`, `00174098` (hover) | bank `xA0` += `x70` | * `port_step()` |
| `00173A18` | states 5 and 7 flip a coin each update until heads | one draw a field |

The blends beside them (`func_00235EA8`, `func_00235F38`: turn rates, direction) were already
step-aware, and the waypoint radius (`00173358`: speed * 61.36 + 20) is a distance.

**The Red Pass kill.** The type-2 helicopter's route is chosen from the riders' heights when a leg
starts and timed by retail's speed. At 240 it flew the leg four times as fast (Red Pass FR, base
build, the same race at 60 and 240: 626-699 units a second against 171-176), reached its stop 17
s into the race instead of after a minute, and hovered over the riders' line for 23 s (state 4),
descending to 70 units above the track's height while every rider rode under it (nearest rider
90-160 units). Its climb over the ground (+0.5 a field while below 70 units, already scaled by
067) kept 60's speed while its forward speed was four times 60's, so over rising ground it flew
lower too. The helicopter's body is a collision target: `001728A8` sets its node's bit 57, the
flag the riders' contact solver (`00131998`) clears from a rider's own node while it probes, as the
boulders' bodies do (`001460D0`); a contact with it plays `B_COL_HELI` (`001CB638`, materials
174/206/238/270), and a hard one crashes the rider as any obstacle does. At 60
the same helicopter flew the whole minute in state 3 and never hovered there. After the fix its
path at 240 is 60's to 1-3 units for the whole minute (table below).

### The other objects (every world object manager of `001638B0` and `0016C0C8`)

| Object (manager) | Sites | What ran per update |
|---|---|---|
| Birds and bats (`00143D20`, flocks on "Bird Point Tool" / "Bat Point Tool" paths) | `00142A28` velocity += drag, gravity, steering, lift and thrust; `00142D50` position += velocity, heading 0.0625 a step, bank from the turn since the last update | forces * step; position * step; heading `port_blend(0.0625)` with its pair `sqrt(1-b^2)`; bank / step |
| Swooping attacker (`001918C0`, Mt. Imasaki FR) | `00192268` position += speed; `00192460` pass length counted per update with a random draw each update; the knock-down (`001928A0` -> `0012F110`) every update within 10 units | speed * step; count and draw once a field; the knock once a field |
| Falling stalactites and icicles (`001DC548`) | `001DCB50` fall speed += 0.03, position += fall speed | both * step |
| Big hoofed animal (`00129330`), dogs, cats, horses (`0011D198`) | `0012A3E8`, `0011F900` speed -= 0.0073; `00129FC8` 1-in-13 mode rolls, `0011F4C0` 1-in-61 kick roll; `0012AD80` ray schedule (6 fields) | speed * step; rolls once a field; schedule by whole fields |
| Deer (`001596E0`) | `0015B538` 10-field footfall lock counted per update (its sibling `0012AD60` was fixed by 067) | `port_count_down` |
| Ground tilt of animals, deer, dogs, peds, traffic | `0012B448`, `0015BC20`, `00121530`, `00127458`, `001812F8`: at most 1 (or 0.5) degree an update | `port_turn_limit`: that angle a field |
| Footfall speed ramps | `0012ACC8`, `0015B460`: a step-aware blend helper called once per footfall, so 067's central blend made it 4x slower at 240 | `port_event_rate` / `port_event_least`: retail's step per call |
| Path walkers (`00144AC0`) | `00145998` falling to the ground (as `0016FC00`, fixed by 067) | * step |
| Peds and spectators (`00127738`) | `00123F60` settle by a per-field speed; `001246D0` push counter against 60 fields | * step; whole fields |
| Golf carts (`0016CC98`) | `0016F710` brake (cap 0.1 below the speed each update); `00170160` wheel spin; `0016D8C0`, `0016DAF0` golfer pause (16 fields) | * step; * step; whole fields |
| Traffic (`0017F8B8`) | `00181390` wheel spin | * step |
| Knock-down of an animal or ped (`00165560`'s module) | `00165850` settle timeout, `00165E68` contact age, `001657F8` end wait: counted updates | whole fields |
| Crowd (`00127738` -> `001BEE28`) | cheer rolls each update, and the random spectator drawn for them | once a field |
| The player's camera (`00148F60`) | height while held (`0014CC38`), the tilt's steps (`00149988`), stunt offsets (`0014A580`), the leash after a wall push (`0014A9A0`) | * step |

Clean (checked, nothing per update without the clock): `001441C0` (path followers through
`001DCE58`, dt-based), `001714C0`, the train `001EAD60` (animation-driven), `00196138` (pickup
respawn timers), `0016C1D0`/`0016C2A0`/`001675D8` (one-shot setup), `001580B0` (a depth sort).
The debug and free cameras of `00148F60` were left as they are.

## Measured

`port/tools/test_scenery_rate.py` (new): the same race (state trace recipe, seed 81257, Cross
held, 3600 ticks from GO, deterministic) at each rate; per kind of object the cruise speed (90th
percentile of the distance covered per second of race clock while moving), and the positions of
the first 20 s against the 60 run at the same tick. Base = main 9609ea6f with the trace only
(`before2`); after = this branch.

Mt. Midoule FR and Red Pass:

| Track | Kind | 60 | 144 before | 240 before | 144 after | 240 after | 240 after, first 20 s vs 60 (median / largest) |
|---|---|---|---|---|---|---|---|
| Mt. Midoule FR | heli | 175.8 | 411.5 (x2.34) | 671.1 (x3.82) | 175.8 (x1.000) | 175.8 (x1.000) | 0.8 / 44 (before 848 / 2933) |
| Mt. Midoule FR | bird | 114.7 | 262.9 (x2.29) | (not flushed) | 105.2 (x0.917) | 106.7 (x0.930) | |
| Red Pass FR | heli | 175.8 | 406.9 (x2.31) | 667.2 (x3.80) | | 175.6 (x0.999) | 1.5 / 46 (before 1564 / 3277) |
| Red Pass MX | heli (hover) | 27.7 | 49.4 (x1.78) | 74.8 (x2.70) | | 34.3 (x1.24) | 15.6 / 65 |

Every track, 240 after against 60 (every track unlocked, `--right 0..24`, 25 races):

| Track (id) | Objects | 240 / 60 cruise |
|---|---|---|
| Mt. Midoule FR (0) | 2 heli, 10 birds | heli 1.000, birds 0.930 |
| Mt. Midoule TD (24) | 10 birds | 0.991 |
| Red Pass FR (1) | 2 heli | 0.999 |
| Red Pass MX (7) | 1 heli (hover) | 1.237 |
| Red Pass TD (13) | 1 heli | 1.000 |
| Mt. Konawaki FR (2) | 2 heli, 10 birds | heli 0.996, birds 1.127 |
| Mt. Konawaki TD (10) | 10 birds | 0.753 |
| Mt. Quintanaroo FR (3) | 2 heli | 1.000 |
| Mt. Zorkovaska FR (5) | 2 heli | 1.001 |
| Mt. Liddellroch FR (11) | 2 heli, 10 birds | heli 1.000, birds 1.004 |
| Mt. Liddellroch TD (16) | 10 birds (only the 240 run flushed them) | - |
| Mt. Imasaki FR (12) | 3 attackers (not triggered in the minute) | - |
| Mt. McSchley FR (19) | 2 heli | 1.000 |
| Mt. McSchley MX (25) | 10 birds | 0.965 |
| Salt City FR (20) | 2 heli | 1.000 |
| the other 9 | none in the first minute | - |

(Mosh Pit, 23, was not raced.) The test passes at 30 % because of the two outliers, which are not
a step taken per update (before the fix the same objects ran x2.3 at 144 and x2.7-3.8 at 240):

- A bird's speed comes from its flight mode and from a steering loop: outside its path's corridor
  it is pushed back and along the path (`00142A28`), and the corridor's in-and-out depends on the
  step. On Mt. Konawaki TD the 60 run's birds stay outside 90-100 % of the time and reach 1.8 units
  a field; with finer steps they get back inside half the time and level off lower: 120 x0.849, 144
  x0.819, 240 x0.753, a trend with the step and not with the update count. `DHD_STEP_CLOCK=1`
  (the delta-time paths at one field a step) gives 109.0 against 60's 109.1, so the code paths
  themselves are retail's; finer steps converge to a different, slower loop. Other flocks land
  within 7 % (Konawaki FR 13 %).
- The Red Pass MX helicopter hovers in stops and starts between close waypoints (speed 0 to 0.9 a
  field); a stop ends when its turn brings the heading within 90 degrees, and that turn's
  feedback loop lands a little differently with finer steps.

## Decisions

- **Scale the steps, not the update.** Running the object managers once per whole field (as
  `00125E38`'s creature picks are) would give retail's motion exactly, but the objects would
  then move in 60 Hz jumps on a 240 Hz picture: a helicopter at its cruise speed jumps 3 units
  every fourth picture, and the birds' remaining difference would be traded for a stutter. The
  steps are scaled instead, as finding 067 did for every other per-field velocity; at a step of
  one field the arithmetic is retail's.
- **Counters and rolls on whole fields**, as finding 067's: a random roll each update would draw
  4x as often at 240 (more events, and the shared generator's sequence moves for everyone).
- **The bird's bank divided by the step.** It is the direction change since the last update times
  the speed, a rate measured over one update; without the division birds at 240 bank a quarter
  as much (measured: Mt. Konawaki TD x0.707 without, x0.753 with).
- **The camera's per-update steps are fixed here too** (the audit found them in the same pass and
  they are the same rule); its debug cameras are not.
- **`state_trace_run.track_nav` moves right.** It pressed down, which the track chooser ignores:
  every `raceN` scenario raced Mt. Midoule FR. Right walks `UNLOCKED_TRACKS`' order (checked: 3 is
  Red Pass FR).

## Surprises

- The rate audit's top ranks are the animation stamps (256 a field at 240, finding 090 FPS1) and
  VU1/GS packet buffers. The helicopter shows when the audit is read per field and per object
  region (`D_003AF940` +0x180..+0x1E8: travel x3.5-x6.1 at 240; its waypoint changed 14 times in
  the 240 run against 6 in the 60 run over the same fields).
- The first "Red Pass" audit and runs were Mt. Midoule FR again (the chooser route above).
- The camera's matrix (`D_005156C0` +0x40..+0x78) travels 3.5-5 times as far per field at 240
  with no net change (a high-frequency wobble). Not explained by the camera steps fixed here; open.
- Finding 067's central blend fix had made two once-per-footfall ramps 4x slower at 240 (found by
  the audit, fixed with `port_event_rate`).

## Progress

- [x] 2026-11-09: lane started, finding reserved (2bf130b9).
- [x] 2026-11-10: reproduced (`rate_audit.py`, then `test_scenery_rate.py`), the helicopter and
  the mechanism found, every world object manager audited, all sites fixed, every track raced
  (tooling 5f73c61b, fix 5f5f9193, trace and tolerance 2b942ace).
- [x] Red Pass long races, 12000 ticks (200 s) at 240, base and fixed, FR and TD: no rider crash
  with a helicopter within 80 units in the second before it, in either build (25/20 and 51/42
  crashes in all). The kill is not reproduced by the scripted race (Cross held, pinned seed); the
  mechanism above is what puts the helicopter there.
- [x] Checks (below).

## Checks

- `python tools/check.py port`: OK (docs, tree, build_wasm, test_runtime, build_game, net_tests,
  test_gs_wgpu, test_wasm2c_chunks).
- State traces at 60 on that build (`state_trace_run.py boot|race`) against main 9609ea6f's:
  boot 600/600 rows, race 5000/5000 rows, `state_diff.py` PASS.
- `python port/tools/test_fps.py`: passes at 30-240 on the fixed and paced clocks, with the new
  checks (a turn capped per field turns 60 fields' worth a second; `port_event_rate` gives
  retail's rate back through the step-aware blend).
- `python port/tools/test_scenery_rate.py` (Mt. Midoule FR, 60/144/240): PASS on the branch
  (heli x1.000 at both, birds x0.917/x0.930), FAIL on main (heli x2.34/x3.82, birds x2.29).
- `python port/tools/test_fps_race.py` at 144, 3600 ticks: the fixed build against itself MATCH
  (36010 + 8670 rows); against main it differs from tick 125 (rider 7, 1e-34 in its animation
  column, then 0.0005 in z at tick 130): expected, the world's random draws now come once a field
  (the crowd, the animals, the helicopters' coin flips), which moves the shared generator's
  sequence for the AI riders at high rates.
- A 240 frame series of the race start on Mt. Midoule FR, main and branch (`live_game.py` on the
  hidden desktop, `DHD_RACE_CAPTURE` every 40 ticks), was kept outside the tree; the helicopter is
  a few pixels there, so the trace is the measure.

## Result

What changed: `port/patches_fps.py` (the sites in the tables above), `port/hle/fps.c`
(`port_event_rate`, `port_event_least`, `port_turn_limit`), the race observer
`port/hle/scenery_trace.c` (`DHD_SCENERY_TRACE`) and its test `port/tools/test_scenery_rate.py`,
`rate_audit.py`'s options, and `state_trace_run.track_nav` (right, not down).

Left open:

- Birds on Mt. Konawaki TD fly 25 % slower at 240 than at 60, and the Red Pass MX hovering
  helicopter's stop-and-go 24 % quicker: their steering loops depend on the step size (above).
  Only running those objects on the field grid would make them exact, at the cost of 60 Hz
  motion on a faster picture.
- The camera's high-frequency wobble at 240 (`D_005156C0`, Surprises).
- The swooping attacker of Mt. Imasaki FR was never triggered by the scripted race: fixed by
  reading, not measured.
- Mosh Pit (23) was not raced.
