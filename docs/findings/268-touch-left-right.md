# 268: touch players cannot change what the game's menus change with left/right (issue #108)

Status: current

Branch `fix/touch-left-right-268` from `main` 8d956bff, started 2026-11-17. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #108: on a phone without a controller, Career's shop (the Bike / Jersey screen) shows the
heading `◄ INVENTORY ►` over BIKES, JERSEYS, BIKE COLOR. Left/right changes the heading (to the shop
pages, where songs are bought) and nothing on the screen takes a tap for it: the player stays in
INVENTORY.

Done when: every menu of the game where left/right does something can be done by touch, through
one rule in the shared touch/menu code where the game's code allows it; pad and keyboard play and
game state are unchanged (boot and race state traces equal to the base); one tap is one step; a
hidden touch run moves the shop's heading INVENTORY → the next page → back, reaches an item's buy
prompt by taps only, and works two other menus of the list. Out of scope: gameplay, the race HUD's
touch controls, our own pages (their value rows take taps already, finding 092).

## Progress

- [x] 2026-11-17: the cause, on the base build in a hidden run (`DHD_MOUSE_TRACE=1`)
- [x] 2026-11-17: the fix, its unit test, `port/tools/touch_left_right_check.py`, the checks (Result)

## The cause

The report is right, and nothing in the original game locks the page. The shop is five pages
(`func_00139BF8`: INVENTORY, BIKE UPGRADES, BONUS PURCHASES and two more), each a popup of vtable
`D_00284190` with its own list at +0x500. They are linked in a ring: each page has two links
(+0x424, count at +0x434, added by `func_0018E4F8`; items of vtable `D_00284170` whose test
`func_0013DC20` takes the popup manager's direction 3 (next) or 2 (previous)), followed by
`func_0018E518`. While the page's list is on its heading (cursor -1, page +0x528 = 0) a pad's
left/right goes to the next or previous page; in the list, they change the bike, jersey or colour,
and Triangle takes the page back to its heading (`func_0013C970`).

The heading is drawn by the page itself (`func_0013BA60`: its title at +0x504 as `"\x8D%s\x8F"`,
the arrow glyphs around it, from x 13, y 12 in the pulsing style `D_00362468`), not as a row. The
shop's mouse code (finding 092) knew its list rows and its legend only, so a tap on the heading did
nothing. Base build 8d956bff, the hidden run:

```
shell mouse: shop at 170,18 row -1 cursor -1 click
shell mouse: shop at 21,18 row -1 cursor -1 click
```

## The fix

`port/hle/menu_page.c`, the code every tap on a game menu goes through, and `menu_side_button`
in `menu_pads.c` (a tap left of a line's words is Left, right of them Right, on them the line's own
button; a heading passes its middle as both ends, so its halves are its two buttons):

- **The shop's heading**: a page with links (+0x434 > 0) takes a tap on its heading's line (above
  y 24, up to 16 pixels right of the heading) as Left on its left half and Right on its right half,
  measured from the drawn text at the style's resting size (the style pulses, `func_0013C838`, and
  the middle must not move under the finger). From inside the list the tap presses Triangle first,
  so the page is on its heading when Left/Right arrive: one tap, one page.
- **Any game popup with a value on a row**: a tap on a row's line beside the panel behind the rows
  puts the cursor on that row and presses Left (left of it) or Right (right of it), what a pad's
  left/right does there. A tap on the row itself is still Cross, a hover still only moves the cursor.
  Popups that ignore left/right ignore these presses too.
- **The controls screen** (shell screen 13, `func_001F0A28`): each pad's row has its value (CONFIG
  n, VIBRATION) from x 180; a tap there is Left left of x 375 and Right right of it (`shell_lists`'
  `value_from`, `value_middle`). A tap on the label is Cross, as before (it saves and leaves).
- **BONUS PURCHASES' items** (list vtable `D_00283F50`) draw their rows themselves (the item text is
  only the price), so a row is its whole line and a tap on the cursor's row is Cross (the buy); their
  own legend "◂ PREV ▸ NEXT ✕ SELECT △ BACK", shown while the cursor is in them, has its own tap
  zones (measured in a picture). A tap on the "MORE..." line under the last row shown (or above the
  first, scrolled) walks the cursor on, so items 13 to 22 can be reached.

How a touch player does each left/right of the game now:

| Menu | The pad | By touch |
|---|---|---|
| The shop's five pages, heading `◄ INVENTORY ►` | ◄ ► on the heading | a tap on the heading's left or right half (**new**; run) |
| The pages' lists BIKES / JERSEYS / BIKE COLOR, WHEELS / FRAMES / FORKS | ◄ ► on a row: the previous / next bike, jersey, colour, part | a tap left or right of the cursor's row (finding 092), now also beside the panel |
| BONUS PURCHASES' items (bikes, tricks, riders, music, levels, videos) | ◄ ► PREV / NEXT, ✕ buy | the legend's PREV / NEXT / SELECT / BACK, a row by a tap, a second tap on it buys, MORE... scrolls (**new**; run: Music 1 to its "INSUFFICIENT FUNDS" with $0) |
| OPTIONS > AUDIO / OTHER: Vibration, Stereo Sound, Music / Speech / SFX Volume (`D_00284FB8`) | ◄ ► on the row | a tap left or right of the row (**new**; run: Music Volume 50 → 51 → 50 → 48) |
| The pause menu's audio page (`D_002857C0`, update `func_00166F68`) and the popup of vtable `D_00287D98` (a value 1-5) | ◄ ► on the row | the same rule (**new**; not run) |
| OPTIONS > CONTROLS (screen 13): CONFIG and VIBRATION per pad | ◄ ► on the row | a tap on the row's value part, left or right half (**new**; not run) |
| Rider select (screens 3-6), the track chooser | ◄ ► | their side zones and legends (finding 092, unchanged) |
| Our own pages (PC SETTINGS, the online room, Touch Controls) | ◄ ► on a value row | the row's "<" and the rest of it, the legend's PREV / NEXT (findings 092, 227, unchanged) |

Pads and keys: unchanged. All of it acts on a mouse click (a tap) only, through queued pad presses
(`press`): without one `menu_mouse_field` returns before any of it.

One tap is one step: each tap queues one press (held three fields, let go for three), finding 262's
latch is for keys and pads only. A slider moves while its button is held, so a tap moves AUDIO's
volumes by one to three of their 129 steps (about 1 to 2 %): 40 taps or more from silent to full.
A mouse click on the PC takes the same path, so desktop clicks beside a row are Left/Right too.

## Decisions

- **The heading belongs to the shop's code** (`game_popup_mouse`, already the shop's): its place is
  the shop page's draw (x 13, y 12, title +0x504), not the generic popup's. The page-link count
  (+0x434) is the game's own marker of a page with neighbours; only the shop uses links
  (`func_0018E4F8` has one caller).
- **Beside the row, not on it**: a tap on a row's words stays Cross everywhere, so no menu's
  choosing changes; the sides were dead before.
- **Triangle before Left/Right from the list**: a phone player sees the arrows on the heading in
  both states; one tap should change the page without first going back.
- **BONUS PURCHASES' legend is a measured table**: it is drawn by the list itself, not as a footer
  item, so finding 266's measured legend ranges cannot read it.
- **AUDIO's sliders take one press per tap**, not a value from the tap's place on the bar: the same
  presses a pad gives, no writes to the game's values.

## Surprises

- The heading's width changes every field (INVENTORY measured 135 to 187 pixels): the style's
  glyph size pulses while the heading is active. Measured at rest (0x15A by 0xC0).
- AUDIO / OTHER is drawn at the shell's scale, the shop's message popups wider (finding 266's
  "widescreen" note): a test aiming at a popup must use the scale of that popup.
- The grid popup of vtable `D_00287C88` (update `func_0019DE80`, a 3 by 3 choice with up/down and
  left/right) and the jukebox popup (`D_00287988`: Square plays a song's preview) were not seen in
  the runs; a tap does not reach the grid's left/right nor the preview's Square. Left open.

## Result

Commits on `fix/touch-left-right-268`: the lane plan; the fix with its unit test and check tool;
this finding's result.

- Unit test: `port/tools/test_menu_pads.py` (in `check.py port` through `test_runtime.py`):
  `menu_side_button` left of, on and right of a line's words, and a heading's halves at every x.
- Hidden runs (`touch_left_right_check.py`, 1600x720 at 20:9, `DHD_TOUCH_MOUSE=1`, no controller):
  `shop` PASS: from inside BIKES, a tap on the heading's right half pressed Triangle, then Right
  (BIKE UPGRADES), then Right (BONUS PURCHASES), Left (BIKE UPGRADES), Right (BONUS PURCHASES),
  one page per tap; SELECT on the legend entered the items, a tap on Music 1 took the cursor
  there (row 4) and a second tap opened the game's answer (popup `08286F30`, "INSUFFICIENT FUNDS /
  PRESS ✕ TO CONTINUE": a fresh Career has $0; with money it is the purchase). `audio` PASS: Music
  Volume 50 → 51 by a tap right of its row, → 50 and → 48 by two taps left of it. The base build
  (8d956bff) does nothing on the heading (the cause, above).
- State traces: boot (600 fields) and race (5000) of this branch equal to the base's (8d956bff).
- `check.py quick`, `check.py port`, `check.py docs`.
- Not run: a phone (none connected: the phone proof is owed), the controls screen, the pause
  menu's audio page, iOS.
