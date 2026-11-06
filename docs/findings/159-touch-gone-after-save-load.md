# 159: Touch controls gone after loading a career save

Status: in progress

Branch `fix/touch-after-load` from `main` 113f76f, started 2026-11-06. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Issue #54 (0.7.10, Android): a player saved their career to continue later; after loading that
save, the touch controls never show in races again, also after restarting the app. A brand-new
career shows them. The player's shared log (Share log, finding 152) is kept by the lead
(`F:\Temp\issue54\`); its settings line has `TOUCH_WITH_PAD=hide` and the classic pad.

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

- The race test is `touch_race_running(phase, mode)` in `touch.h` (a pure function, so
  `test_touch` covers it): phase 1-4 and not the attract demo (mode 10, `D_004B25A0` +0x10C; the
  demo starts from the title, screen 0, and draws the HUD's timers through the hooked text calls).
  Not a list of screen ids: besides 9 and 22, the bike shop (screen 18, modes 15 and 17) enters
  a race and then sets the id to `D_004B25A0` +0xA08 or 9, and the results' NEXT / RETRY flows
  (`0013BA60.cpp`, `001F8A08.c`, `001F8E90.c`, `00198AC0.c`) re-enter without touching it.
- The HUD hook stays the evidence that a race is on screen: the HUD's code runs only from the
  race's draw loop (`001B40A0.c`) and the race's own UI panels (`0014F300.c`, `001688C8.c`, both
  in the race UI's tables), so a race phase left over in the menus shows nothing.

## Surprises

- Other port code tests "screen 9" for a race too, so a continued career misses it as well
  (not changed here, outside this lane):
  - `port/hle/fps.c:437`, the human rider's grounded-stall recovery (`a->stalled_steps`): it never
    runs in a continued career's races. It changes game state, so it needs its own decision and
    state traces.
  - `port/hle/fps.c:538` (dhd.log's `clock:` lines, finding 139: none in such a race; the
    `perf:` lines use their own test and do appear in the player's log), `fps.c:602`
    (`DHD_FPS_TRACE`, a test trace), `port/hle/presence.c:35` (Discord shows menus during the
    race).
  - Online (`fps.c:25`, `puppet.c`, `online.c`) always starts through screen 9
    (`online_menu.c`), so it is not affected.

- The race phase is not reset when a race is left: quitting the continued career's race (pause >
  QUIT > Yes) returns to the title, screen 0, with the phase still 3. Nothing shows there (no HUD
  is drawn), which is why the HUD hook stays part of the test; a check on the phase alone would
  be wrong.

## Result

The overlay shows in every race, also one started from CONTINUE SAVED GAME. Shown on Windows with
the fixed build (the reproduction's route, then pause > QUIT > Yes through
`DHD_NAV=...,22/5:cross,22/3+600:start,22/3:up,22/3:cross,22/3:down,22/3:cross`): the countdown
at `menu=22 phase=1`, `touch log: air` lines through the race (race_hud 1), the Classic pad drawn
and the speedometer moved to the top left; after QUIT the title with no overlay. The game's
state is unchanged: boot and race traces equal the base build's (the race route goes through
screen 9, where nothing changed; the fix only changes what touch_field passes to the host).

Left for a phone (the lead): on the Android build, SINGLE PLAYER > a rider with a saved career >
CONTINUE SAVED GAME > an event: the overlay in the countdown and the race, the HUD moved; leaving
the race, none in the menus. The "screen 9" tests listed under Surprises are separate fixes.
