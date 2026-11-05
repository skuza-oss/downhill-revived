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

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
