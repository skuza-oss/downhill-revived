# 255: menus, HUD and world at the same speed at 60 and 240 (issue #76)

Status: current
Checked: 2026-11-15

Branch `fix/high-fps-sweep-255` from `main` 34d6273d, started 2026-11-15. Open leftovers stay in
issue #76.

## Goal and acceptance

At 120/144/240 fps some things still moved at a different speed than at 60 (issue #76). The
known open case: the title screen's pulsing PRESS X TO START box ran faster at high rates, and
other menu and HUD pulses, blinks and fades of that kind were suspected. The birds of Mt.
Konawaki TD and the hovering Red Pass MX helicopter (finding 193's leftovers) were reported fixed
and were only to be confirmed; the camera wobble of finding 193 to be checked once.

Done when: `check.py quick`, `check.py port`, `check.py docs` pass; boot and race state traces
equal the base build's; the sweep in `port/tools/` with an entry in `docs/TESTING.md`; the table
below. Out of scope: frame times and fps (a check of speeds, not a benchmark); anything right at
60.

## The sweep: `port/tools/rate_sweep.py`

Each scene runs at 60 and at 240, headless on the deterministic clock (the state trace's recipe:
virtual clock, no movies, no sound, no card, default settings, pinned seed, field-keyed menu route,
race-clock pad), with the rate audit (`port/hle/rate_audit.c`). From the moment the scene shows,
over the same fields at both rates, every word of EE RAM sums how far it moved. Two things are
new against finding 090's audit:

- **Field-end sampling** (`DHD_RATE_AUDIT_FIELDS=1`): the 240 run is read at field ends only, the
  same instants as the 60 run. Read at every update, a curve sampled four times as finely gains
  3-7 % of travel by itself (seen on the title's 3D background), which buried real differences.
- **Oscillators kept.** `rate_audit.py compare` keeps only words that move one way (net / travel
  at least 0.5): a pulse, a blink, a bounce has travel and no net, and was filtered out. That is
  why the title's pulse was never found by the audit.

A word stepped once per update without the frame's time shows at x3.6-4 (less than 4 when it
turns inside a field). Words the game counts once per picture by design (the picture counter and
double-buffer flag, the texture cache's use stamps, the SIF RPC client, pad buffers, GS frame
descriptors and packet buffers, the original clock's timer counts, the animation clock in 1/1024
field) were each read in their code and are listed apart (`BOOKKEEPING` in the tool).

Scenes: the boot title (menu 0, behind the card dialog), the five menus on the route to a race
(15 main, 3, 16, 1, 11 rider and bike choice, 18 track chooser), every track's first minute (26
chooser positions, every track unlocked, Cross held), the countdown (on the field grid, finding
142), the results (`DHD_TEST_FINISH=20`, new: the player's rider finishes at race second 20) and a
replay (`DHD_REPLAY_TEST=20`; its 60 run takes the delta-time paths at one field a step,
`DHD_STEP_CLOCK=1`, as replays exist only there). Race scenes start once a race's loop runs
(`DHD_RATE_AUDIT_RACE`): the race phase word holds a stale 3 and the race clock a stale value
before the first race. A scene takes 5-130 s; all 36 about 70 minutes with two games at once.

Covered on the branch build: all 36 scenes ran. Chooser positions 24 and 25 repeat 0 and 1 (the
chooser wraps), so 24 distinct tracks. The effect matrix is x1.00 on every track (before x3.71
on every one); the indices of "Still open" are x3.3-4.5 on every track.

## Fixed

| Case | Where | 60 against 240, before -> after |
|---|---|---|
| Title PRESS X TO START pulse | `001ED3E0`: the box's edges `D_0029E008`/`D_0029E00C` += `D_0029E018` (2.4) per call | x3.62 -> x0.977 at field ends, x1.000 read every update (2159.9 / 2159.9 units over 900 fields) |
| Effects' rotating matrix | `0022E3D0`: `D_0027BF00` turned by the fixed rotation `D_0027BF40` per call (every menu and race) | x3.72 -> x1.002 |
| Every menu's age | `001F74F0`: `count++` up to 1000 per call | x4 until the cap (seen as x1.27 in a 10 s window) -> x1.00 (whole fields, never left at 0) |

- **The title pulse.** Retail moved the edges 2.4 a field and flipped the direction at >= 400 or
  <= 352: from its start at 401.6 it turns at 351.2 and 401.6, a 42-field pulse. On the
  delta-time clock the call runs once per update (four times as fast at 240). Scaling the step
  alone held the box still: from 401.6 a quarter step (0.6) stays above 400, so the flip ran on
  every update. The delta path moves the edges by `port_step()` and turns the direction only
  when it points out of the range, at retail's own turning points (351.2, 401.6): finer steps
  turning at 352/400 made a 40.5-field pulse (3.6 % quicker). A step that passes a turning point
  is reflected back inside it (the review's find: at 30 fps a 4.8-unit step went to 348.8, a
  44-field cycle; uneven slow frames changed range and cadence). One-field steps land on the
  turning points, so 60 takes no reflection: retail's arithmetic. The field-end reading's 2.3 %
  is the corner cut where a turn falls inside a field.
- **The effect matrix.** Finding 090 put the matrix's wobble phase (`D_0029DBAC`) on whole fields,
  but the turn itself (`func_00221A60` by `D_0027BF40`, then the axis reset and `func_00222800`)
  ran per call. It now turns once per whole field (0-2 an update), each turn retail's.
- **Menu age.** The counter every menu block keeps at +4 (`func_001F74F0`); it counts whole
  fields, as finding 139's counters, and is never left at 0: `func_001F7420` takes 0 as
  "initialize" (selection and fades reset, a press lost), and above 60 a menu's first update often
  has no whole field (the review's find). Retail's age counts calls (at boot the field counter
  stands still while it counts), and no code reads it but that `== 0` and the `< 1000` cap
  (assembly searched; `menu_page.c` names the field `frames` and never reads it), so steps of two
  fields below 60 skip no value anything tests.

All three keep retail's arithmetic on the original clock: boot and race state traces equal the
base build's.

### The pulse and the menu age at 30, 45, 60, 240 and uneven steps

`port/tools/test_menu_rate.py` (`docs/TESTING.md`): every update, the box's edge against retail's
42-field triangle in game time, and per screen opened the updates until its menu block is counted
(retail: 1). 20 fps plays the same steps as 30 (the clock caps a step at two fields). Uneven:
`DHD_STEP_PATTERN=0.25,0.5,0.25,1.75,0.25,0.5,2,0.25,0.75,0.25,1.25,0.5` at 240.

| Run | Before (3c6c6cae + the test): pulse | menus | After: pulse | menus |
|---|---|---|---|---|
| 60 (original clock) | 351.20..401.60, error 0.000 | 1, 1, 1, 1 | the same | the same |
| 60 delta paths | 351.20..401.60, 0.000 | 1, 1, 1, 1 | the same | the same |
| 30 | 348.80..401.60, 52.8 (44-field cycle) | 1, 1, 1, 1 | 353.60..401.60, 0.000 | 1, 1, 1, 1 |
| 45 | 350.38..404.80, 52.8 | 1, 1, 1, 1 | 352.00..401.60, 0.001 | 1, 1, 1, 1 |
| 240 | 351.11..402.20, 41.9 | 1, 1, 2, 3 | 351.20..401.60, 0.055 | 1, 1, 1, 1 |
| uneven | 347.00..404.60, 51.6 | 2, 2, 2, 1 | 351.20..401.60, 0.009 | 1, 1, 1, 1 |

The range is read at update ends (at 30 the turn at 351.2 falls inside an update). 240's 0.055 is
float rounding of 0.6-unit steps, growing about 0.004 units per 100 fields: a cycle of 42.0006
fields. The test's tolerance is 0.1 units.

## Confirmed, not changed

- **The old cases are not fixed on this base.** `test_scenery_rate.py` on the branch build, 60 and
  240: Mt. Konawaki TD (`--right 8`, track 10) birds cruise x0.753, the Red Pass MX hovering
  helicopter (`--right 4`, track 7) x1.237, the same as finding 193 measured. Mt. Konawaki FR
  (`--right 6`, track 2): heli x0.996, birds x1.127. As the owner asked, no time was spent on them;
  finding 193 says why they differ (steering loops that converge differently with finer steps).
- **The camera wobble** (finding 193, `D_005156C0` +0x40..+0x78, 3.5-5x travel at 240) does not
  reproduce: Mt. Midoule FR, 1800 fields read at every update, the matrix travels x0.92-1.04 of
  60's (position x0.96-1.17), with no net drift. Dropped.

## Still open (where each step lives)

| Word | Scenes | 60 against 240 | What it is, as far as read |
|---|---|---|---|
| `0x002B10EC` (rider block `D_002AF2C0` +0x1E2C) | every track, results, replay | x3.6-4.2 | a small int (0-8) that changes every field: a round-robin index advanced per update; whose turn comes up 4x as often (cost, maybe AI or sound work), no motion seen |
| `0x003B3650` and `D_00398EA0` +0x1050 | every track | x3.34 | the same kind of rotating index |
| `D_00398EA0` +0x5730..+0x577C (floats) | most tracks, results, replay | x3.2-13 | the rider sound update's per-rider fields (`001C5040`, finding 090 fixed its pitch/volume slopes); rider-dependent, so the size scatters; not read further |
| `0x003F53E4` | track chooser | x4 | an int counted per update; no named writer (written through a pointer) |
| `D_00723A80` +0x330/+0x334, `D_0037EA60` +0xD0, `D_00720290` | some tracks | x2.3-7 | particle and effect blocks (`002286F8`, `00228040`); not read further |
| `D_004C69A0`, `D_004C6440`-`D_004C6520` +0xC | menus | x0.92-1.07 | `001F7B80` re-rolls a random animation (1 or 2) every update for riders not on show; nothing plays; the shared generator is drawn 4x as often |

The race scenes are otherwise dominated by the riders' own races diverging between rates
(finding 139: one different contact moves everything after it), so heaps, rider and camera words
there differ in both directions without a common bias; a step bug shows as a consistent x3.3-4
across tracks, which is how the rows above were picked.

## Decisions

- **Turning points, not only the step.** A step-scaled bounce with retail's thresholds gives the
  right speed but a 3.6 % shorter pulse; retail's turning points give its 42 fields exactly.
- **Whole-field turns for the effect matrix** rather than a fractional rotation: its phase
  (`D_0029DBAC`) already moves on whole fields (finding 090), so the two stay together.
- **The old cases left alone**, as asked, though the sweep shows them unchanged.
- **Scenes measured by game state, not pictures**: the audit reads every word, so pulses, blinks,
  HUD timers and object tables are all covered without naming them first.

## Surprises

- The title is menu 0 (behind the card dialog), not 15 (the main menu): the first title run of the
  sweep measured the main menu.
- The race phase word holds 3 and the race clock a stale count from boot until the first race, so
  a phase gate opened during the race load (the results window started at field 1520).
- The deterministic 60 clock never starts a replay: `DHD_REPLAY_TEST` is a delta-time path.
- The first version of the title fix froze the box at 240 (the flip on every update above 400);
  the sweep showed it as travel x0.000.

## Progress

- [x] 2026-11-15: lane started, finding reserved (7d358d6c).
- [x] Sweep tool (6241cc77), gates and bookkeeping list (75445c86).
- [x] Title pulse (ab138042), effect matrix (ea88d52a), menu age (3f744af9).
- [x] Checks (below).
- [x] Review fixes: the test (aa03fc38, f64c0e04), menu age never 0 (ecd7fef1), pulse
  reflection (287adb8a).

## Checks

- Boot and race state traces (`state_trace_run.py boot|race`) of the branch build against the
  base build 34d6273d: boot 600/600, race 5000/5000 rows, `state_diff.py` PASS.
- `python tools/check.py docs`, `quick`, `port`: OK (on the branch head 3f744af9 plus these docs).
- `rate_sweep.py` on the base-with-tools build and the branch build, all scenes (numbers above).
- Review fixes: `test_menu_rate.py` fails on the build before them and passes after (table
  above); boot 600/600 and race 5000/5000 rows equal the base build's (`state_diff.py` PASS);
  `check.py quick`, `port`, `docs` OK.

## Result

What changed: `port/patches_fps.py` (the three fixes), `port/hle/rate_audit.c` (field-end
sampling, race and replay gates, `DHD_RATE_WATCH`), `port/hle/fps.c` (`DHD_TEST_FINISH`,
`port_clock_playing`, `DHD_STEP_PATTERN`), `port/tools/rate_sweep.py` and
`port/tools/test_menu_rate.py` (new), `docs/TESTING.md`.
