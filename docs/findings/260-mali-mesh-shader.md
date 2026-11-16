# 260: the Redmi 9 dies at the title in 0.8.6: the Mali-G52 compiler and the mesh vertex stage

Status: current

Branch `fix/mali-mesh-shader-260` from `main` 2760f372 (0.8.6 plus findings 257-259), started
2026-11-16. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

0.8.6 with its default settings (Native VU on) dies at the title on the Redmi 9 (Mali-G52 MC2,
driver r26p0), at every start: the driver's shader compiler (`cmpbe_v2_compile_multiple_shaders`)
crashes while the first mesh pipeline, `E0180033E26BC000`, is built (finding 259). The next start
builds it again and dies again.

1. Find the shader construct the compiler dies on and write the same maths in a form it compiles,
   one code path for every device.
2. A start that died inside a pipeline build must not walk into the same death at the next start;
   the log and the trouble report still say what died.
3. On the phone: the fixed debuggable build with the default settings reaches the title, the menus
   and a race, 5 starts of 5; race screenshots with Native VU on; one timing run at 2x with Native
   VU on and one off (finding 259's route); a test build with the bad shader dies once and the
   next start gets through.
4. `check.py quick` and `port`; boot and race state traces equal to the base; the renderer's
   picture tests.

Out of scope: speed work, other phones (the Mi 11i and the Mac are not touched).

## Progress

- [x] 2026-11-16: finding reserved (f2a1cae4)
- [x] 2026-11-16: the construct found on the phone with a test build that reads native.wgsl from
  the app's files folder (13 variants, below)
- [x] 2026-11-16: the fix: the mesh vertex stage chooses its position with `select`, no branch;
  `native_spirv` checks the rule for every stage (b87cc9c6)
- [x] 2026-11-16: the guard: a draw pipeline the driver died building is not built again
  (78203e78)
- [x] 2026-11-16: the phone: the guard with the bad shader, 5 starts of 5 with the defaults, the
  race picture, Native VU on against off at 2x
- [x] 2026-11-16: `check.py quick`, `check.py port`, state traces, picture tests (Result)
- [x] 2026-11-16: review: a refusal for good needs the crash handler's note; a kill or a close
  makes a suspect, built alone, refused only when left again (unit tests; phone not attached)
- [ ] the lead: the merge, a release

## The construct

malioc does not reproduce it: every stage of 0.8.6 compiles for Mali-G52 (malioc 2026.5, r51p0
library), as in finding 153; malioc compiles one stage alone, the phone dies in the linked
compile of both stages. So the bisect ran on the phone: a test build (not merged) read
`native.wgsl` from `files/l260-native.wgsl` when present, and each start pushed a variant and
looked for the crash and for the mesh pipelines `E26BC000` / `E0180033E26BC000` built.

| variant of 0.8.6's native.wgsl | result |
|---|---|
| as it is | crash |
| mesh vertex divides by w itself and gives w = 1 (no-perspective varyings kept) | crash |
| no `@invariant` on the position | crash |
| `nat.mesh_clip` / `nat.mesh_fog` (the new uniforms) replaced by constants | crash |
| STQ not divided by w (fog still from 1/w) | crash |
| fog constant (STQ still divided by w, the position `if` kept) | crash |
| **perspective varyings** (no `@interpolate(linear)`), the mesh body unchanged | builds |
| a trivial mesh stage: inputs to outputs | builds |
| the trivial stage plus STQ divided by w | builds |
| the trivial stage plus the fog | builds |
| the trivial stage plus the position `if`/`else` (no output from 1/w) | builds |
| w = 1 with `select` in place of the `if` (no-perspective) | builds |
| **0.8.6's stage with only the `if`/`else` made a `select`** (same values) | builds |

**The construct: a vertex stage whose no-perspective outputs are computed (from 1/w) and whose
position is chosen by an `if`/`else` branch.** Either half alone builds; perspective varyings
with the same branch build. 0.8.5's mesh stage had the same branch with perspective varyings
(`MeshVaryings`); finding 249's first change (48962741, "a mesh's pixels run the GS draws'
fragment stages") made its outputs `native_vertex`'s no-perspective `Varyings`, which met the
branch. `native_vertex` has no-perspective outputs and had an `if` (the sprite snap), but its
outputs are its inputs, passed through: it built.

Releases hit: only 0.8.6 (`git tag --contains 48962741`: v0.8.6). 0.7.16 to 0.8.5 have the
perspective mesh varyings.

## The fix (b87cc9c6)

- `native_mesh_vertex` makes both positions (placed as the GS places the kicked vertex, and the
  plain projection) and `select` picks one: the same values as the branch. Its bool tests are `&`,
  not `&&`: naga writes the short-circuit `&&` as branches too (the scan below found three in the
  stage that built on the phone).
- `native_vertex`'s sprite snap is a `select` with `&` as well, so the rule is simple: **no vertex
  stage with a no-perspective output has a conditional branch**. `native_spirv`
  (`noperspective_branches`) checks it in every stage's SPIR-V for the three device kinds, with a
  small test of its own (`noperspective_branches_scan`: an `if` found, a `&&` found, a `select`
  with `&` clean). On 0.8.6's native.wgsl it fails (`native_vertex` 2, `native_mesh_vertex` 3).
- Cost (malioc, Mali-G52, longest path, arithmetic / load-store cycles): `native_mesh_vertex`
  2.08 / 11 -> 2.17 / 11 (both sides are computed now; the stage is load-store bound, so its bound
  does not move); `native_vertex` 1.08 / 11 unchanged. Mali-G57: mesh 1.03 / 4 -> 1.25 / 4
  (position) and 1.16 / 6 -> 1.38 / 6 (varyings), load-store bound too. Finding 249's speed gain
  does not depend on the branch (its gain was the fragment stages and the clip-space vertices,
  both kept).

## The guard (78203e78, crash evidence after review)

In `pipelines.rs`'s `list` module, on every native platform:

- While the driver builds a draw pipeline, its key is in `pipelines-building.txt` beside
  `pipelines.txt` (under the list's first line), and in one of eight lock-free slots with the
  building thread's id; both are cleared when the build returns, and the file is removed when
  nothing builds and nothing is suspected. Two small writes a build under the guard's mutex, no
  sync: a build is a driver compile of tens to hundreds of ms, the listed keys are built ahead by
  the worker thread, and a dying process leaves its written file to the system.
- **The crash note is the evidence.** The game's crash handler (`port/hle/host.c` `crashed`, the
  POSIX signal handler on Android, iOS, Linux and macOS and the unhandled-exception filter on
  Windows) first asks `dhdr_native_pipeline_crash` (atomics only: async-signal-safe) for the key
  its own thread is building and writes `pipelines-crash.txt`: `crash KEY`, or `crash` when that
  thread built nothing (open/write, a stack buffer).
- At the next native renderer's create (`list::recover`, once a process, before any draw
  pipeline) each key left in the marker is judged (`judge`):
  - named by the crash note: `!KEY` in `pipelines.txt`, refused from then on (`refused`): its
    draws are skipped, a specialised one falls back to the generic, and a refused mesh key turns
    Native VU off at the create (`refuse_meshes`: the VU runs every program);
  - no crash note (the player closed the game, Android killed it, or a death the handler did not
    see, such as an abort): not refused. It becomes a suspect, `?KEY` in the marker: built again,
    but alone (a suspect's build takes `TURNS` for writing, every other build for reading), and
    cleared when its build returns. A suspect left behind again with no build of it returning in
    between is refused (the "two runs in a row" rule, for deaths without a note);
  - another key in the build at the time of a named crash (two builds at once: render thread and
    pipeline worker): built again, not refused; a crash on a thread that built nothing makes every
    key left a suspect;
  - listed in `pipelines.txt` (built here before with this driver and shaders): logged, never
    refused.
- The list's first line carries an FNV-1a hash of `native.wgsl` and `native_dual.wgsl` (format 2).
  **The way back needs no file editing**: a driver update or a release whose native shaders differ
  is a new first line, and every refused key is tried again. Every player's list starts again once
  with this change (one start that builds at first use). There is no other reset: the launcher's
  Graphics reset writes dhd.ini only.
- Nothing is hidden: the crash stays in the run before's log (`dhd-previous.log`, with the
  startup record's `died at native pipeline ...`), and each start says which rule applied
  (`gs native: the run before crashed inside the driver's build of pipeline KEY; ...`,
  `... with no crash in its build on record (closed, killed, or not seen); it is built again,
  alone, ...`, `... not in its build (built before, or another thread's crash); ...`), then
  `gs native: pipelines refused on this GPU, driver and shaders (...): KEY ...` and
  `gs native: Native VU can't run on this GPU (the driver died building mesh pipeline KEY in an
  earlier run); the VU runs every program`. The phone launcher already shows "it crashed, share
  the log" after the dead start.
- Unit tests (`only_a_crash_note_refuses_at_once`, `a_run_that_died_building_is_judged_on_evidence`):
  a kill without a note (a suspect, no refusal, built alone, cleared after its build), a note
  (refusal), two keys in building on two threads with the crash in one (only that one refused),
  a crash on a thread that built nothing (suspects only), a suspect left again (refused), a listed
  key, another driver's marker, and a new header trying a refused key again.

On the phone, with the first form of the guard (78203e78, which refused every key left in the
marker; this branch with the test hook, 0.8.6's shader pushed through it): start g1 died (`SIGSEGV in thread
dhd-pipelines`, two mesh keys building at once, `E26BC000` on the render thread and
`E0180033E26BC000` on the worker); g2 logged both as refused, turned Native VU off and reached the
title; g3 the same from the list alone. The phone's `pipelines.txt` was put back afterwards. With
the crash note, the same death names `E0180033E26BC000` (the worker's): it is refused, which turns
Native VU off, so `E26BC000` is not built at the next start. The crash-note form was not run on the
phone: the Redmi 9 was not attached when it was done (unit tests only).

## The phone (the fixed build)

Build: this branch's head as a debuggable APK with the profile 0.8.6 shipped with
(`build_apk.py --wasm ... --pgo-use dhd-197.profdata`), installed with `adb install -r` over lane
259's build (same debug key). Runs through `run-as` and dhd.ini keys, lane 259's way; no screen
mirror; screenshots with `adb exec-out screencap` in untimed runs only.

- **5 starts of 5 with the defaults** (dhd.ini: only the route's test keys, `DHD_FRAME_CLOCK=1` and
  the bench window 300-9000; no scale, Native VU, aspect or frame-rate key): each went through the
  title, the menus and the race start to field 9000 at 2x (the phone's default), built 50 mesh
  pipelines, no crash, no refusal. 50.0 to 51.3 fields/s over the route to field 9000 with the
  default frame rate and vsync.
- Race picture with Native VU on (6 s into the race, a 2x run before those five): the rider, the
  ground near the camera, trees, banners, cable cars, the helicopter, the speedometer, the track map
  and place, the touch controls: right.

**Native VU on against off at 2x**, finding 259's route and keys (`DHD_FPS=240 DHD_VSYNC=0
DHD_FRAME_CLOCK=1`, bench 300-14000, every effect on), run on, off, on, off with 120 s between
runs, battery 33.6-34.1 C at the starts and 34.9 C at the ends, no mirror. Fields a second:

| | race | race start | mid-race | route | race p99 ms | VU thread work ms | GS thread work ms |
|---|---|---|---|---|---|---|---|
| on | **50.93** (50.85 / 51.01) | 40.41 | 53.48 | 55.10 | 33.0 / 33.7 | 9.7 / 9.9 | 6.8 / 6.8 |
| off | **50.15** (50.30 / 49.99) | 40.77 | 51.79 | 54.26 | 34.6 / 33.8 | 12.7 / 12.7 | 9.9 / 9.9 |

**On a Mali, on is now as fast or faster**: +1.6 % in the race, +3.3 % mid-race, +1.5 % over the
route; the race start is 0.9 % behind (the two runs of a setting differ by 0.3 to 0.6 %).
0.7.15+'s answer was on 5 % slower (finding 259's table); finding 249's two changes are what
turned it. Native VU still saves the VU thread 3 ms a field and the GS thread 3 ms. Off is
50.15 here against finding 259's 48.6 for the same route: other builds of the same release (259
built 0.8.6, this is the branch with findings 257-260), a list built ahead, and the phone's day;
compare only within one table.

## Decisions

- **`select`, not perspective varyings.** Perspective varyings also built, but every draw would
  then interpolate differently (exact only where w = 1); the `select` gives the very values of the
  branch, so the pictures do not change anywhere.
- **The rule covers every vertex stage, not only the mesh one**: `native_vertex` lost its branch
  too, so the check is one plain sentence and a future stage cannot meet the construct unseen.
- **Refuse the key, not Native VU for one start**: a start with Native VU off would be followed
  by one that builds the mesh pipeline again and dies (every other start lost). The refusal lasts
  as long as the driver and the shaders do.
- **Evidence, not a leftover key**: a key left in the marker only says the run ended during its
  build. A player closing the game or Android killing it while a new list is built (the first
  start after an update) must not turn Native VU off for good, so only the crash handler's note
  refuses at once. The note is written by the crashing thread about its own build, so two builds
  at once cannot condemn the innocent one.
- **Suspects are built alone** rather than refused: one more death names one key, and a healthy
  phone loses nothing (a build is already the slow part; the lock only orders builds).
- **A listed key is never refused**: it built here before with the same driver and shaders, so a
  death in its build is a kill (the game closed while it built ahead at the boot), and refusing a
  good key would hide draws or turn Native VU off for nothing.
- **No sync on the marker**: what a dying process wrote stays in the system's cache and reaches
  the file; only a power loss in that instant would lose it, and then nothing worse than today
  happens.

## Surprises

- **Three of the 0.8.6 stage's branches were `&&`**: naga writes WGSL's short-circuit `&&` as a
  branch (OpBranchConditional) even between plain comparisons.
- **After a crash the phone launcher shows its "it crashed" page once** (`LastExit.firstAsk`), even
  with `launcher = 0`: a script that starts the game after a crashed run must start it twice.
- Two mesh pipelines were in the compiler at once when the bad shader died (render thread and
  pipeline worker): the first guard refused both; the crash note names the worker's one.

## Result

- The construct: an `if`/`else` choosing the position in a vertex stage whose no-perspective
  outputs are computed from 1/w. Mali-G52 r26p0's compiler dies on it in the linked compile;
  malioc does not reproduce it. Only 0.8.6 shipped it (48962741).
- The fix (b87cc9c6): `select` in both vertex stages, `&` for their tests; the pictures are
  identical to 0.8.6's, and `native_spirv` refuses the form in any vertex stage.
- The guard (78203e78 and the crash-note commit): a draw pipeline the game's crash handler names
  as its thread's build is refused from the next start on that GPU, driver and shaders; a run that
  ended any other way during a build makes the key a suspect, built again alone and refused only
  if left again; a refused mesh pipeline turns Native VU off; every step logged.
- Checked: `check.py quick` and `check.py port` (with `test_gs_wgpu --skip-hardware` and the
  native replay); `native_spirv` with malioc for Mali-G52 (3192 of 3192 stages compile); the
  list's unit tests (`a_run_that_died_building_is_judged_on_evidence` and the judge's cases); boot (600
  fields) and race (5000) state traces of this branch's `dhd64.exe` equal to 2760f372's (the base
  built from the same tree with the four changed files at 2760f372); `native_vu_pictures.py` at 4x
  with Native VU on, base against this branch: identical in all 11 fields.
- On the Redmi 9: the first guard with the bad shader (one death, then the title with Native VU
  off; the crash-note form not run there, the phone was not attached);
  5 starts of 5 with the defaults through the race start; Native VU on 1.6 % faster than off in
  the race at 2x.
- Cost elsewhere: the mesh vertex stage computes both positions (malioc: +0.09 arithmetic cycles
  a vertex on the G52, +0.22 on the G57, both load-store bound); not measured on another phone.
  Every player's pipeline list starts again once (format 2): one start that builds at first use.
- Left: the release; reports from other Mali phones on older drivers (Bifrost on r2x) would show
  whether the guard ever fires in the field (its log lines).
- The phone was left with this branch's debuggable APK (the PGO build, sha256 f2bef578...), its
  dhd.ini as found (md5 29bc64ce...), `pipelines.txt` as found (lane 259's Native VU off list,
  md5 9d38c198...), no `l260-*` or `pipelines-building.txt` file, the lock removed.
