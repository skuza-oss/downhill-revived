# 165: no port patch in code the port does not compile

Status: current
Checked: 2026-11-06

Branch `fix/dead-patches` from `main` 1ff885d, started 2026-11-06. Merged into `main` at c11b489 on
2026-11-06; released in 0.7.12.

## Goal and acceptance

Finding 159 (Surprises) found that `func_001B40A0` (the race loop) is NONMATCHING C, so the core
runs its recompiled asm and the patch in `port/patches.py` that calls `hle_online_watch_resume()`
there never ran. The lane:

1. Makes the build refuse any patch whose text falls in source the port does not compile (an
   `#ifdef NONMATCHING` block, whose function the core runs from recompiled asm, or any other
   branch the port's preprocessor drops), naming the patch and the function; a unit test for the
   guard; every patch it finds fixed.
2. Moves the spectate resume (finding 122) to a hook that runs, with an online run against a
   local server and a bot that shows the bug and the fix.
3. Checks: `python tools/check.py port` and `check.py server` pass; boot and race state traces
   equal to the base build's.

Out of scope: any other change to online state sync.

## Progress

- [x] 2026-11-06: finding reserved (6cf0a94)
- [x] 2026-11-06: the guard's survey over every patch: two dead patches (below)
- [x] 2026-11-06: `patches_fps.py`: five exact edits instead of the 6-count regex (2521471)
- [x] 2026-11-06: the spectate resume moved to `func_0016C0C8` (591aade)
- [x] 2026-11-06: the guard in `port/tools/port_source.py`, `port/tools/test_patch_guard.py` run
  by `test_runtime.py` (734a4d3)
- [x] 2026-11-06: the lead's question (the Salt City lane (finding 166), merged into
  main after this branch started; its Surprises): a race that closes while the pause menu is up
  is not left; fixed in the same hook (3f019a4; "Leaving a race closed during a pause")
- [x] 2026-11-06: checks (Result)

## The guard

`apply_patches` (`port/tools/port_source.py`; every core build, `abi_check.py` and the test
programs go through it) finds the spans of a unit the port's preprocessor drops
(`dead_ranges`): the port never defines `NONMATCHING` (the core runs such a function's
recompiled asm, `build_wasm.py` `recompile_objects`) or `__mips__` (the core is wasm32), and
`#if 0` is never true. Nested conditionals and `#elif` / `#else` chains count; a condition it
cannot decide counts as compiled. A `text`, `re` or `body` patch whose search text overlaps such
a span raises `DeadPatch`:

    dead port patch 0 of unsorted/001B40A0.c (text 'int func_001B40A0(Obj *arg0)') in
    func_001B40A0: it changes text under `#ifdef NONMATCHING` (from line 5), and the core runs
    func_001B40A0 from its recompiled asm (INCLUDE_ASM), so this C never runs. Patch code the
    port compiles (finding 165).

`dead_patches(src_root)` lists every dead patch instead of stopping at the first;
`test_patch_guard.py` checks the rules on a synthetic unit (NONMATCHING, `__mips__` with an
unknown `#elif`, `!defined(__mips__)`, nesting, a directive in a comment) and that the real
patches have none.

What the survey found over all of `port/patches*.py` (3815 units):

- `unsorted/001B40A0.c`, the spectate resume: below.
- `unsorted/00202E00.cpp`, `patches_fps.py`'s `('re', (r'\* 16\.66667f', 6), ...)`: its sixth
  match is in `func_00204F38` (the pose blend), NONMATCHING. No bug: `recomp.py` already scales
  that function's recompiled asm (the `0x00204F84` case), so the C patch only counted a match
  that never ran. Now five exact `edit()`s on the compiled timers; the core's code is the same.

`port/abi_calls.json` (`fix_abi_calls`, the other source rewrite) has no sites, so nothing to
check there.

## The spectate bug

Room spectating (finding 122) holds the watching camera while the pause menu is up:
`port_online_pause_rows` (the pause menu's builder) sets `on.watch_paused`, and
`hle_online_watch_camera` (each view update) and the Left / Right switch in `pads_read_now` do
nothing while it is set. The hold was to end at the race loop's entry, a patch at the top of
`func_001B40A0`, which never ran. So after any pause the hold stayed until the race ended
(`back_to_room` / `race_over` clear `on`):

- Paused before the finish: after it, `online: watching BRAD (slot 1)` is logged and the HUD says
  WATCHING BRAD, but no `camera cut` follows: the camera stays on this PC's own rider, and
  Left / Right do nothing.
- Paused while watching: Left / Right do nothing for the rest of the race, and the automatic
  switch (a watched rider who finishes or leaves) never moves the camera (scenario A below, base
  build: at the race's close `watching PCHOST (slot 0)` is logged with no cut).

Where the pause runs: START ends the race loop (`func_001B4900`, the loop returns with 2; see
`online.c` `leave_race`), the race screen opens the pause menu in the front end, and RESUME
re-enters `func_001B40A0`. `func_0016C0C8` (the race world's update, already patched for
`port_loop_frame(1)`, finding 159) runs every frame of the loop, so its first call after RESUME is
the loop's re-entry: the call moved there, as `hle_online_loop_frame()` (`online.c`; it was
`hle_online_watch_resume`). The in-loop pause state
(`D_00398C40` +0x5BE4, set by `func_0015FC70` before it opens its popup) still blocks the
camera and the switch on its own, as before.

## Leaving a race closed during a pause

The Salt City lane (finding 166, its Surprises) saw a room race go on for
six minutes after `race closed: back to the room`, with TIME UP - DNF on screen, until the next START, which went straight to the room. Its
log (that lane's run17, the Salt City TD race) shows the host's START (the pause menu) about five
seconds before the server closed the race, and the menu still up when it closed; the player then
resumed. Not the dead spectate hook (nothing on the leave path reads `watch_paused`; the
spectate fix alone still stalls, below), and not "after one pause" in general: a race paused and
resumed earlier is left as always (scenario C below, base build). The close has to come while the
menu is up.

`leave_race` (`online.c`) sets `quit_on_pause` and asks the loop to stop as START does,
`func_001B45F8(D_004B1F40, 2, 0)`. `func_001B45F8` takes a request only while the loop object's
+4 is 0, and the START that opened the menu had set it, so the request was dropped; RESUME
re-enters the loop, which clears +4 (`func_001B40A0`'s start), and nothing asked again.
`quit_on_pause` stayed set, so the next START's pause builder (`port_online_pause_rows`) left at
once. Fix: `hle_online_loop_frame` asks again while `quit_on_pause` is set (a request while one
is pending does nothing), so the loop's first frame after RESUME stops it and the builder leaves
for the room. The player still sees the pause menu until they close it (RESUME, or BACK TO THE
ROOM as before); leaving straight from the open menu would mean closing it from outside the
menu code, which nothing here does yet.

## Decisions

- The guard lives in `apply_patches`, not in `transform.py`: that is where the patches are
  applied, for the core and every other user, so no path skips it.
- Only `NONMATCHING`, `__mips__` and `0` are decided; anything else counts as compiled, so the
  guard never refuses a live patch. Overlap is the rule (a search text partly in a dropped
  branch is refused too).
- The race loop's hook clears only `watch_paused`, not `watch_direction`. It runs every
  loop frame now (it ran once per loop entry before), and clearing the direction every frame would
  make a held Left / Right switch every frame. `pads_read_now` keeps `watch_direction` current in
  the menu too, so a direction held through RESUME is no new press, the same rule as finding 122's
  "a press switches once".

## Surprises

- Online room races ignore `DHD_AUTOPAD_FRAMES` from the race on: `online.c` sets `DHD_NAV` to
  its race script, and `host_pad_script` prefers `DHD_NAV`. Under `DHD_NET_SCRIPTED` only the
  race-clock scripts (`DHD_RACE_PAD` with `DHD_RACE_SYNC=1`) reach the race, and the race clock
  stops while the pause menu is up, so a script can open the menu but never press RESUME. The
  runs below are therefore unscripted (no `DHD_NET_SCRIPTED`): the field script reaches the room
  (START in the shop at field 2800, Square READY at 3100), then live_game's posted keys drive the
  race.
- nettool readies only when the room has `--players` members (default 4): pass `--players 2`
  with one bot.

## Result

Online evidence (hidden desktop, `port/tools/local_server.py` + `live_game.py`, this PC hosts a
public room with computer riders on, one nettool bot `BRAD --players 2 --pick 3 --ready-after 3
--races 1 --finish-after 100`, `DHD_NET_TEST_FINISH=20`, menus by
`DHD_AUTOPAD_FRAMES` as `cross_race.py`'s host plus `2800:8:start,3100:8:square`; keys: Esc is
START, x is Cross). Two scenarios, each on the base build (1ff885d) and the fixed one:

| Scenario | Base build | Fixed build |
|---|---|---|
| A: Esc + x (RESUME) 12 s after the countdown, finish at 20 s, Right | `watching BRAD (slot 1)`, no `camera cut`; the picture shows this PC's rider under WATCHING BRAD; Right: nothing | `watching BRAD (slot 1)`, `camera cut slot 1`; the picture is behind BRAD; Right: `watching Ajax (AI) (slot 3)`, `camera cut slot 3` |
| B: watching, Right, then Esc + x (RESUME), Right, Left | Right before the pause: `watching Jonnie (AI)`, cut; after RESUME, Right and Left: nothing | before: `watching Woody (AI)`, cut; after RESUME, Right: `watching Rakel (AI) (slot 7)`, cut; Left: `watching BRAD (slot 1)`, cut |

Leaving a race closed during a pause, scenario D: this PC never finishes (`DHD_NET_TEST_FINISH`
unset), the bot finishes at 25 s, Esc 45 s after the countdown, the server closes the race
about eight seconds later (`race closed`, TIME UP for this PC) with the menu still up, x (RESUME)
six seconds after that:

| Build | After RESUME |
|---|---|
| base (1ff885d) | the race runs on under TIME UP - DNF; 30 s later still no `this PC's race is over`; the next Esc returns at once |
| spectate fix only (734a4d3) | the same |
| fixed (3f019a4) | `this PC's race is over; back to the room` within a second (RESUME at field 8647, the room by 8707); the picture 5 s later is the room, `DNF TIME UP` on this PC's row |

Control C (base build): Esc + x 12 s after the countdown, never finishing; the race closes about
40 s later and is left by itself (`this PC's race is over` with no key), so a pause and resume
well before the close does not stall it.

In all runs the pause menu shows (PLAYER 1 PAUSED, RESUME ... LEAVE THE ROOM), the race
closes after the bot's finish (`race closed: back to the room`) and the PC is back in the room.

Scenarios A and B run again on the final build: the same as the fixed column above (A: `camera
cut slot 1` after the finish, Right to `Drud (AI)` with a cut; B: after RESUME Right and Left each
switch with a cut).

Checks on the final tree: `python tools/check.py port` OK (with `test_patch_guard: all passed`);
`python tools/check.py server` OK; state traces against the base build (1ff885d,
`state_trace_run.py`, `state_diff.py`): boot all 600 rows match, race all 5000 rows match (also on
734a4d3 before the leave fix).
