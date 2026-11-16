# 266: touch players stuck on "PRESS X TO CONTINUE" messages (issue #105)

Status: in progress

Branch `fix/touch-continue-266` from `main` 3746e414, started 2026-11-16. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #105: on a phone, CAREER > SPECIALIZED CAREER > MOUNTAIN CROSS shows the message
"MOUNTAIN CROSS ENTRY BIKES ACQUIRED / PRESS X TO CONTINUE" over the Bike / Jersey screen, and
nothing on screen takes a tap: the player is stuck.

Done when: every message of the game that waits for one button (X to continue, unlocks and prizes,
yes/no questions, the title, the demo) can be answered by touch through one mechanism in the shared
menu / touch code; pad and keyboard play and game state are unchanged (boot and race state traces
equal to the base); a hidden touch run passes the Mountain Cross message with one tap, and two other
messages too; one tap never answers two boxes in a row. Out of scope: gameplay, the race HUD's
touch controls.

## Progress

- [x] 2026-11-16: cause found (below), on the base build in a hidden run with `DHD_MOUSE_TRACE=1`
- [x] 2026-11-16: the fix in `menu_page.c` / `menu_pads.c`, the unit test, the hidden check
  `port/tools/touch_continue_check.py`, the checks (Result)

## The cause

A tap in a menu is a mouse click (finding 092): on a popup of the game (the popup manager's page) a
click on an enabled row moves the cursor there and presses ✕; a click anywhere else does nothing.
The message is such a popup (`D_003638A0`, vtable `08283C20`, built by the shop), but its four
rows are text, all disabled, and its cursor is -1:

```
mouse trace: popup 083638a0 vtable 08283c20 rows 4 cursor -1 at 320,70 step 10 align 0/0
  row 0 at 0,0 (off): Mountain Cross
  row 1 at 0,0 (off): Entry Bikes Acquired
  row 2 at 0,0 (off):
  row 3 at 0,0 (off): Press ✕ to Continue
menu mouse: click at 320,100: row 3 (off)
menu mouse: click at 320,201: row -1
```

So no tap could press ✕ there; a pad's ✕ (or the keyboard's) closes it. The memory card's notices
are the same kind of popup (no rows, or text rows only), and the touch overlay stands aside in
the menus (finding 141), so nothing else on the screen could be tapped either.

## The fix: one rule for every message

`popup_mouse` (`port/hle/menu_page.c`), the code every tap on a game popup goes through: a popup
of the game with no row to choose (`menu_is_message` in `menu_pads.c`: no rows, or none enabled)
is a message that waits for one button, so a tap anywhere on the screen presses ✕. When its legend
(the popup's footer: a button glyph before each word, "✕ RETRY  △ CANCEL") offers another button,
a tap on that word presses that button (`glyph_legend_button`, measured as drawn, like our pages'
legend in `page_legend_button`). The log says what a tap did (`menu mouse: click at X,Y: row -1:
a message`, `menu mouse: the message takes Triangle`); the mouse trace prints each popup's legend.

How a touch player answers each kind of prompt now:

| Prompt | The pad | By touch |
|---|---|---|
| Career messages in the shop's message popup (`08283C20`): "ENTRY BIKES ACQUIRED"; the other unlock and prize texts of the same text table (`D_0026E090`: "Pro Bikes Unlocked", "Unlocked Pro Rider", "Bonus Unlocked", ...) when shown in a text-only popup | ✕ | a tap anywhere (**new**; run for Entry Bikes only) |
| The memory card's notices at boot and around SAVE / LOAD (vtables `08286AB0`, `08286A28`, `08286560`, `082869A0`, `082864D8`, `082863C8`, `08286340`, `08286450`, `08286890`: no rows or text rows) | ✕, some △ | a tap anywhere is ✕; a tap on a legend word is its button (**new**; run: "not found" > CANCEL) |
| Any other game popup with no row to choose | ✕ | a tap anywhere (**new**, by the same rule) |
| Questions with YES / NO rows (the pause menu's QUIT and RE-START questions) | the cursor, then ✕ | a tap on the choice (finding 092, unchanged); a question drawn without rows is a message by the rule above, its legend words answer it |
| The title's PRESS ✕ TO START, the flight to the rider screen, RIDER SELECTED, the ride to the shop | ✕ | a tap anywhere (finding 092, unchanged; run) |
| The shop, the track chooser, the rider screen | their buttons | their zones and legends (finding 092, unchanged) |
| Startup movies, the attract demo, the flyover, a replay | START / any button | SKIP / MENU (findings 209, 214, 218, unchanged) |
| Results after a finish, trophies | ✕ / △ in popups | rows by tap; a text-only popup is now a message (not run: no finish on a hidden desktop, as in 214) |
| Our own pages' notices (graphics notice, disc notice) | ✕ | the OK row or the legend (findings 227, 262, unchanged) |
| Back where △ / ○ cancels | △ | Android Back is △ in the menus (finding 141, unchanged); iOS: the legend's △ word when it has one |

Pads and keys: unchanged. The rule acts only on a mouse click (a tap) on a popup; without one,
`menu_mouse_field` returns before any of it.

One tap answers one box: a tap is one queued press, held three fields and let go for three
(`PRESS_FIELDS`); finding 262's latch is for keys and pads only. Run: after the tap that closed
the card notice the title still waited for its own tap; after the tap that closed the shop's
message the shop's list stayed on its header.

## Decisions

- **Only the game's popups**, not our pages (`page_of`): ours have their rows, OK row and legends
  already (findings 227, 262).
- **Anywhere, not the box**: the message's box is not a row, and a phone player expects a tap on
  the screen to go on; the box's legend words are the exception, so a second button stays
  reachable without Back (iOS has none).
- **Shop and track chooser keep their own rules**: their roots have no rows either, but
  `game_popup_mouse` takes their taps first.
- `DHD_TOUCH_WITH_SCRIPT=1` (tests only) now also merges the menus' taps, so a `DHD_NAV` route can
  end at a screen and taps go on from there.

## Surprises

- The game's item text reader (`func_0018D860`) follows the item's table without a check: the
  first trace line for the legend read a footer without text and faulted at boot (a guest read of
  address 0). Both readers now go through `item_text`, which returns nothing for an item with no
  string and no table.
- In widescreen the popups are drawn wider than the 4:3 shell screens: on a 1600x720 window a tap
  at 0.6125 of the width is x 519 in a popup but about 379 in a shell list. The pointer mapping is
  right for each (a player taps what they see); a test that aims at a popup's word must use the
  popup's scale.
- A `DHD_NAV` route's last step is pressed again every 30 fields while the screen stays the same
  (a ↓ on OPTIONS walks the cursor on and on); end a route with a step that never matches.

## Result

Commits on `fix/touch-continue-266`: the lane plan, then the fix with its test and check tool.

- Unit test: `port/tools/test_menu_pads.py` (in `check.py port` through `test_runtime.py`):
  `menu_is_message` on four text rows (the Mountain Cross message), no rows (the card notices),
  a question with an enabled row, and counts out of range.
- Hidden runs (`touch_continue_check.py`, 1600x720 at 20:9, `DHD_TOUCH_MOUSE=1`, no controller):
  `shop` PASS (one tap closed the Mountain Cross message on a fresh Career; the shop behind it
  untouched); `card` PASS (a tap on CANCEL pressed △ and closed "memory card not found"; the
  title still waited; a second tap opened the main menu). The base build (3746e414) fails `shop`:
  `click at 320,100: row 3 (off)`, the message stays.
- State traces: boot (600 fields) and race (5000) of this branch equal to 3746e414's.
- `check.py port` (quick and docs in it).
- Not run: a phone (the Mi 11i was not connected: the phone proof is owed), results after a
  finish, iOS.
