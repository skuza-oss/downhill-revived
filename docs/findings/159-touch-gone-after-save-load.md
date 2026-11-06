# 159: Touch controls gone after loading a career save

Status: current
Checked: 2026-11-06

Branch `fix/touch-after-load` from `main` 113f76f, started 2026-11-06. Merged into `main` at 1ff885d
on 2026-11-06; released in 0.7.12.

## Goal and acceptance

Issue #54 (0.7.10, Android): a player saved their career to continue later; after loading that
save, the touch controls never show in races again, also after restarting the app. A brand-new
career shows them. The player's shared log (Share log, finding 152) is kept by the lead,
outside the repository; its settings line has `TOUCH_WITH_PAD=hide` and the classic pad.

What decides whether the overlay shows (finding 141; `port/hle/touch.c` `touch_visible`,
`race_on_screen`, `hidden_by_pad`, `touch_pad_used`; `port/hle/touch_field.c`: the race HUD hook
`touch_hud_seen` set by `widescreen.c`'s HUD calls, the race screen D_0029DED8 = 9 and phase
D_0043F348 1-4, and reads of the save at D_004225A0). Candidates to rule in or out, with
evidence: the race HUD not drawn (or drawn through calls the hook does not see) with that save's
options; the save's own settings (controller, vibration, HUD, a mode) changing what touch_field
reads; `hidden_by_pad` set by a pad the game believes in; a career-only race mode or screen value.

Acceptance: the cause named and shown (a reproduction: a career saved and loaded, on the phone or
through the Windows harness with the touch logic's test hooks), the fix in the common code path,
the overlay showing in races after such a load, `test_touch` extended with the case, `check.py
port` passing, and boot and race state traces equal to the base build (the fix must not touch
game state).

## Progress

- [x] 2026-11-06: finding reserved (f3a6b57)
- [x] 2026-11-06: cause found in the player's log and the code, reproduced on Windows (below)
- [x] 2026-11-06: fix: touch_field's race test reads the race phase and mode, not the screen id;
  `test_touch` case `test_race_running` (794d9d2)
- [x] 2026-11-06: checks on 794d9d2: `check.py port` OK (`test_touch: all passed`); the
  reproduction again (below); boot (600 rows) and race (5000 rows) state traces equal to the
  base build's (`state_diff.py`: PASS)
- [x] 2026-11-06: the owner's go for the other "screen 9" tests (lead): one shared race test,
  `port_race_running` / `port_in_race` (fps.h), used by the touch controls (64b224c), the
  grounded-stall recovery (ec952de, its test c77d784), the `clock:` lines and `DHD_FPS_TRACE`
  (4be2db1) and Discord presence (c256f2a); its race signal moved to the race world's update
  (ce25541, see Surprises); a dhd.log line for each stall recovery (039c248); a trace mask entry
  (4edfaaf)
- [x] 2026-11-06: checks on 039c248: `check.py port` OK (`test_touch: all passed`),
  `port/tools/test_fps.py` OK at every rate; state traces against the base build (113f76f): boot
  (600 rows) and race (5000) match; the race on the delta-time clock (`--env DHD_STEP_CLOCK=1`,
  where the stall recovery runs) matches with the mask entry of 4edfaaf (below); the same
  continued-career runs as before on the new build (Result)

## Cause

`touch_field.c` counted the race HUD hook only on "the race screen", the shell's screen id
`D_0029DED8` = 9 with the race phase `D_0043F348` 1-4. 9 is GET READY (finding 050), the screen
that starts a race on most routes (a new career, single events, multiplayer, online). A career
continued from a save does not pass it: SINGLE PLAYER > rider > CONTINUE SAVED GAME (screen 16,
row 1) opens screen 22 (`func_001EE2A0`), and its Cross sends S_ENTER_GAME without changing the
screen id (`001EB1D8.cpp`, `func_001EE2A0`, the `D_0029DF8B` branch; compare `func_001EDD28`, which
sets 9 first). The race runs with the id still 22, so `race_hud` was never 1: no overlay, the HUD
not moved. Every later session that continues the career takes the same route, which is why a
restart did not help and a new career did.

Evidence:
- The player's log (0.7.10, Samsung SM-A075M): the freeze line written while the app was in the
  background mid-race reads `menu 22 phase 3`.
- Windows reproduction (hidden desktop, base build of 113f76f, `DHD_TOUCH_MOUSE=1
  DHD_TOUCH_LOG=1`, a career profile with progress on the first rider on the card,
  `DHD_NAV=0:cross,15:cross,3:cross,16:down,16:cross,22:cross,22/5:cross`): the nav steps print
  `menu=22 phase=5` (loaded) and `menu=22 phase=1` (countdown); in 50 s of race the touch log has
  no line at all (its `air` line prints only while `race_hud` is 1), the picture shows the race HUD
  at the game's places and no overlay. The control, the same build and settings through a new
  game (`RACE_NAV` of `state_trace_run.py`, empty card): `menu=9 phase=1`, `touch log: air 0`
  at the countdown, the Classic pad drawn and the speedometer moved to the top left.

Ruled out: `hidden_by_pad` (a touch brings it back, and nothing sends pad events in the
reproduction), the save's options block (the reproduction loads them and the HUD is drawn as
always), a HUD drawn through calls the hook does not see (the HUD is the same; only the screen
test failed).

## Decisions

- First fix (794d9d2): the touch race test was the race phase 1-4 and not the attract demo (mode
  10, `D_004B25A0` +0x10C; the demo starts from the title, screen 0, and draws the HUD's timers
  through the hooked text calls). Replaced by the shared test below. Not a list of screen ids: besides 9 and 22, the bike shop (screen 18, modes 15 and 17) enters
  a race and then sets the id to `D_004B25A0` +0xA08 or 9, and the results' NEXT / RETRY flows
  (`0013BA60.cpp`, `001F8A08.c`, `001F8E90.c`, `00198AC0.c`) re-enter without touching it.
- The HUD hook stays the evidence that a race is on screen: the HUD's code runs only from the
  race's draw loop (`001B40A0.c`) and the race's own UI panels (`0014F300.c`, `001688C8.c`, both
  in the race UI's tables), so a race phase left over in the menus shows nothing.
- One race test for the port (64b224c, ce25541; `port/include/fps.h`, `port/hle/fps.c`): a flag
  that the race world's update (`func_0016C0C8`, patched to call `port_loop_frame(1)`) sets and the
  front end's frame (`port_shell_frame`, `port_loop_frame(0)`) clears. `port_in_race()`: the flag,
  not the attract demo (presence: the load and the flight over the track count, as screen 9 did);
  `port_race_running(first_phase)`: that and the race phase from 1 (touch, the countdown) or 2
  (the stall recovery, `clock:`, `DHD_FPS_TRACE`, as before) to 4. The phase alone is not enough
  for the users without a HUD hook (presence would show a race on the title after QUIT); the flag
  has no time window, so a slow menu frame cannot flip it. `port_race_rule` is the pure part that
  `test_touch` and `test_fps` check. dhd.log says `game: a race's loop runs` / `game: the front end
  runs` at each change.
- Online keeps its own test (`fps.c` `online_race`, `puppet.c`, `online.c`): online races always
  start through screen 9 (`online_menu.c`). The test harness's race pad (`autopad.c`
  `race_sync_menu`) keeps screen 9 too: it is the same rule as the PCSX2 comparison adapter's.

## Surprises

- Other port code tested "screen 9" for a race too, so a continued career missed it as well:
  `port/hle/fps.c` the human rider's grounded-stall recovery (`port_fps_contact_recovery`), the
  `clock:` lines (finding 139; the `perf:` lines use their own test and do appear in the
  player's log) and `DHD_FPS_TRACE`, and `port/hle/presence.c` (Discord showed menus during the
  race). All moved to the shared test on the owner's go.
- `func_001B40A0` (the race's loop) is NONMATCHING C: the core runs its recompiled asm
  (`rc_func_001B40A0` in core.c), so a C patch there never runs. The first shared test hooked it
  (64b224c) and the flag never rose: no `game:` line, and every user of it, touch included, would
  have been off (the unit tests pass; only a game run shows it). Moved to `func_0016C0C8` (ce25541).
  The older patch there, `hle_online_watch_resume()` (online spectating: the pause menu's builder
  marks the watch paused "until re-entry"), is dead the same way; not changed here, for the
  online lane to look at (finding 165 moved it).
- The stall recovery runs only on the delta-time clock (`port_delta_clock()`: paced play, a rate
  other than 60, or `DHD_STEP_CLOCK=1`). The deterministic state traces at 60 never reach it, so
  they cannot show it either way; a trace with `DHD_STEP_CLOCK=1` does.
- The test harness's `DHD_RACE_PAD` waits for screen 9 (above), so in a continued career it
  presses nothing: a first attempt to make a stall "pedalled" only coasted (the rider stopped at a
  wall at 0 mph with `pedal 0`).

- The race phase is not reset when a race is left: quitting the continued career's race (pause >
  QUIT > Yes) returns to the title, screen 0, with the phase still 3. Nothing shows there (no HUD
  is drawn), which is why the HUD hook stays part of the test; a check on the phase alone would
  be wrong.

- The first delta-clock race trace differed from field 3584 on (502 of 5000 rows), each build
  repeating itself exactly. Only the `ram` column: cpu, vu0, the GS stream and VRAM and the sound
  matched in every row, so every rider drew in the same place. EE dumps at 3584 and 4984 differ in
  one word, `0x0093C85C`, a dead EE stack local holding an address in the module's C stack
  (`0x0603FE68` against `0x0603FE58`): `port_loop_frame`'s call in `func_0016C0C8` deepens the
  module's frame by 16 bytes. The mask has two words of the same kind (`0x0093C3F0`,
  `0x0093C620`); it is the third (4edfaaf). In that race the new build logs three stall recoveries;
  since everything drawn is equal, the base build ran the same three (it had no log line).

## Result

The overlay shows in every race, also one started from CONTINUE SAVED GAME. Shown on Windows with
the fixed build (the reproduction's route, then pause > QUIT > Yes through
`DHD_NAV=...,22/5:cross,22/3+600:start,22/3:up,22/3:cross,22/3:down,22/3:cross`): the countdown
at `menu=22 phase=1`, `touch log: air` lines through the race (race_hud 1), the Classic pad drawn
and the speedometer moved to the top left; after QUIT the title with no overlay. The game's
state is unchanged: boot and race traces equal the base build's (the race route goes through
screen 9, where nothing changed; the fix only changes what touch_field passes to the host).

The follow-up (one race test for the port): in a career continued from a save the race is now a
race for everything that asks. On the final build (Windows, hidden desktop, that save, the
continued-career route): `game: a race's loop runs` at the load (phase 5), the touch overlay and
the moved HUD in the race (`touch log: air` lines), `clock: race 30 s ...` lines every 30 s of
race (none on the base build in 75 s), and after QUIT the front end again. A race through screen 9
is unchanged: boot, race and delta-clock race traces match the base build's.

What changes in a continued career's race, game state: the grounded-stall recovery
(`port_fps_contact_recovery`, delta-time clock only: paced play at any rate, all players' normal
case) now watches the human rider there as it always did after GET READY. Shown with a
diagnostic build (not committed) on the continued-career route, Cross held by key for 90 s: the
recovery's test passes all race (`race 1`) and its stall counter runs (`stalled` 1.0-1.3 steps,
then movement resets it); under the old screen test the counter was reset on every call. No 2 s
stall happened in that run, so no recovery fired there; where one does, the rider gets the same
recovery as in a new career's race (`fps: rider 0 stuck on the ground while pedalling for 2 s` in
dhd.log). A base-build run on the same route with the pedal not pressed (the harness's
`DHD_RACE_PAD` waits for screen 9) showed a rider stopped at 1 mph for 30 s, but not pedalling, so
not a case the recovery covers. Nothing else in the race changes: the clock lines, the trace and
presence only read.

Left for a phone (the lead): on the Android build, SINGLE PLAYER > a rider with a saved career >
CONTINUE SAVED GAME > an event: the overlay in the countdown and the race, the HUD moved; leaving
the race, none in the menus. On a PC with Discord: the same route shows the race (track, place)
in the profile, not menus.
