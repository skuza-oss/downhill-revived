# 157: Keyboard and controller rebinding, in the launcher and in the game

Status: current
Checked: 2026-11-06

Branch `feat/rebinding` from `main` 113f76f, started 2026-11-05, merged 2026-11-06 (released in
0.7.12). The game's PC SETTINGS is called REVIVED SETTINGS since finding 163, which also put the
Controls page on a tab of its own; the release review's changes are the last section of Decisions.

## Goal and acceptance

Players choose which keys and controller buttons do what. The owner's choices (2026-11-05):

- **What is bound: the PS2's buttons.** Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3,
  START, SELECT, the d-pad and the sticks' directions. Not game actions: the game's own meaning of
  each button stays (Cross pedals in a race and confirms in menus). The pages may show what a
  button does in a race next to its name (docs/PLAYING.md's Controls table), as help only.
- **Where: the launcher and the game.** A Controls page in the launcher (beside Button Icons and
  Dead Zone, Settings > Controls) and a page in the game's PC SETTINGS where the player presses
  the key or button to assign. Both read and write the same `dhd.ini` keys through the one settings
  table (`port/render/settings`, AGENTS.md: one code path). The game applies a change made in the
  game at once; the launcher's apply at the next start, as its other settings do.
- **Two per PS2 button**: a main and an alternate, for keys and for pad buttons (today's
  "Space or X" and "Esc, Backspace" are such pairs). Assigning a key or button that another PS2
  button already uses moves it (no duplicates), and the page says so.
- **Controllers: a layout per player** (player 1 and player 2; every pad of a player uses that
  player's layout), with **swap sticks** and **invert X / invert Y per stick**. The keyboard is
  player 1's.
- **Defaults are today's mappings exactly**, so a player who changes nothing sees no difference:
  the keyboard table in docs/PLAYING.md (letters by place on the keyboard, scancodes, so AZERTY
  and QWERTZ keep working), gradual steering on the arrow keys, pads by position (bottom = Cross).
  A Reset (per page and per player) brings them back.
- Things that must keep working: the Esc pause and exit menu (finding on the exit menu), Alt+Enter
  or whatever the host reserves, the online menus' text entry, the touch controls (Android: out of
  scope, unchanged), SDL's `gamecontrollerdb.txt`, Button Icons (prompts follow the bound pad).
  Binding must never leave a player unable to reach a menu: Cross, Triangle, START and the d-pad
  cannot end up with nothing on both keyboard and pad (refuse the change and say why).

Out of scope: game-action bindings, mouse, touch, analog trigger behaviour, macros, Android UI
beyond what the shared settings give for free.

Acceptance:
- dhd.ini keys documented in docs/PLAYING.md (Controls) with the defaults; a missing or broken
  line falls back to the default with a dhd.log line.
- Unit tests for parsing, defaults, moving a key between buttons, the "never unreachable" rule.
- The launcher page and the in-game page both change the same dhd.ini lines; a binding changed in
  the game works without a restart.
- `python tools/check.py port` passes; boot and race state traces equal to the base build's with
  the defaults (no input changes).
- Checked by hand-equivalent harness runs on Windows (hidden desktop): a rebound key drives the
  game (DHD_TEST_INPUT or the harness's key route), and a pad layout per player.

## Progress

- [x] 2026-11-05: finding reserved (8bf0940)
- [x] 2026-11-05: the model and its rules in dhd_settings (`port/render/settings/src/controls.rs`,
  tests `tests/controls.rs`), the generated `port/hle/controls_table.h`, the C interface (e247813)
- [x] 2026-11-06: the host (keyboard.c, sdl_host.c's keys, gamepad.c's raw state and layout,
  host.c's layouts and services, the button icons) and the core (PC SETTINGS > Controls,
  `port/hle/controls_settings.c`; input held back while a binding waits; DHD_KEYS through the
  layout; DHD_TEST_PAD / DHD_TEST_PAD2) (e5199f3), Square on the page fixed (507d757)
- [x] 2026-11-06: the launcher: Settings > Controls > Bindings (73645c4; read again after the game
  exits, 7ab407d)
- [x] 2026-11-06: docs (PLAYING.md Controls and Settings, TESTING.md, STATE.md) and the checks below

## Decisions

- **dhd.ini: one line per layout**, `keyboard`, `pad1`, `pad2` (variables `DHD_KEYBOARD`,
  `DHD_PAD1`, `DHD_PAD2`): `button:input[,input]` entries apart by spaces, main then alternate
  (`none` for nothing), only the buttons that differ from the defaults; a controller's line may add
  `sticks:swap` and `invert:lx,ly,rx,ry`. An empty or missing line is the defaults, so the game
  without dhd.ini or with an untouched one runs exactly as before (no variable is even set). A
  button the line doesn't name keeps its defaults less any input the line gives another button, so
  a hand-written `keyboard = square:x` does what it says. Why not a line per button: 64 lines and
  64 variables for one feature, a log line full of them, and a change touching two buttons (a move)
  would rewrite two lines; one line per layout keeps a change one line, as every other setting.
- **Names by place**: keys by their US place (`a`, `space`, `lshift`, `kp1`, ...; scan codes, so
  AZERTY and QWERTZ keep working), controller inputs by their place on the pad (`south`, `east`,
  `west`, `north`, `lb`, `lt`, `ls`, `dpad_up`, `back`, `start`, `misc`, paddles, `touchpad`), as the
  pads were always read (bottom = Cross). The pages show each pad's own words (A / Cross / B).
- **Esc is never bound**: it stays Triangle in the menus and START (pause) while riding, so the
  pause and the exit menu always work whatever the layout. Alt, Ctrl and the Windows keys stay the
  system's (keys aren't read while they are held, as before); Caps Lock and the Guide/PS/Home
  button are not offered.
- **The keyboard's d-pad keys still steer gradually while riding** (finding 059's ramp): it is a
  property of keys on the d-pad, whichever keys they are. The keyboard can also bind the sticks'
  directions (ramped the same way, menus too). A controller's sticks are its own: Swap Sticks and
  an Invert per axis of the controller's own sticks (before the swap); its buttons bind the PS2's 16
  buttons.
- **The rules in one place** (the move and the player rule of this bullet and the next were
  replaced on 2026-11-06: each device on its own, a taken input swaps; see the dated section at the
  end of Decisions): dhd_settings (`controls.rs`) parses, writes, moves an input (the
  button it leaves keeps its other slot, else has none; an alternate without a main becomes the
  main; the same input on the button's other slot swaps the two) and refuses a change that leaves
  Cross, Triangle, START or a d-pad direction with nothing on the keyboard and Controller 1
  together (player 1) or on Controller 2 (player 2). The game calls it through the C interface (the
  host, which owns the layouts), the launcher as a crate, and the messages ("X moved from Cross to
  Square", "Not changed: Cross would have no key and no button") are its words on both.
- **A hand-made line that leaves a player stuck** is not refused (the game can't ask): that
  player's keyboard (player 1) or Controller 2 goes back to its defaults, with a dhd.log line; any
  entry that can't be read is skipped with a dhd.log line and its button keeps its default.
- **Players 3 and 4** (a multitap) keep the default controller layout: the owner asked for two.
- **The browser** keeps its own fixed layout (its keys and pads are JS, port/web/www) and shows no
  Controls row (its DHD_PC_SETTINGS_ROWS); **Android** shows none either (its SETTINGS has no room
  and phones play by touch); both are open below.

- **The pages**: in the game, CONTROLS is a screen of PC SETTINGS (its Controls row, after Unlock
  All), built like TOUCH CONTROLS: Device (Keyboard, Controller 1, Controller 2) and Show (Buttons;
  D-pad and Sticks) on top so no screen has more than 15 rows, then a row per PS2 button
  ("Cross:  < Space >  X", the arrows round the slot left/right chose), then Reset. Cross waits for
  the press (5 s; Esc gives up), Square clears; the hint line says what the button does in a race,
  and after a change what moved or why not (4 s). In the launcher, the Controls tab's Bindings: the
  same three layouts as a segmented choice, a row per button with two slot buttons (click waits,
  right-click clears, a Clear link while waiting), the stick options for a controller, Reset per
  layout; the tab's Reset takes all three back.
- **While the game page waits, the game sees no input** (pads_read, pad_refresh) until every key and
  button is let go after the press, so the press that binds never also acts in the menu.
- **The arrows keep the text fields' pad picker** (online names, room codes) whatever keys the d-pad
  has: while a text field is open they still move it, as before.
- **The button icons follow player 1's controller layout** while a controller is there or icons are
  chosen: each PS2 button's cell gets the icon of its main input's place (Xbox, Nintendo: the
  place's letter; PlayStation: the game's own icon of that place); an input with no icon (d-pad,
  Start, a stick) keeps the PS2 one. The key is one int (style + 3 x base-9 places), so font_sharp
  and native_core.inc are unchanged.

### 2026-11-06: each device on its own, a taken input swaps

The owner (Windows, a DualSense as Controller 1, the in-game Controls page) moved Square's button
onto Cross, then cleared Cross: dhd.log's `controls:` lines show `pad1 = cross:west square:none`,
then `cross:none square:south`, then `cross:none square:none`, and no way to confirm in any menu
with the controller. The old rule let it through because it counted the keyboard and Controller 1
together for player 1, and the keyboard still had Cross. The owner approved these rules
(`controls.rs`, which the game and the launcher share):

1. **Each device on its own** keeps Cross, Triangle, START and the four d-pad directions: the
   keyboard alone, Controller 1 alone, Controller 2 alone (`Controls::missing(device)`). The
   keyboard no longer counts for a pad, nor a pad for the keyboard.
2. **Clear is refused** on the last input of one of those buttons; the pages show "Cross needs a
   button" (a controller) or "Cross needs a key" (the keyboard).
3. **An input another PS2 button has swaps**: the target takes it, the other button gets the
   target's old input in that slot ("Swapped Cross and Square"). When the target had nothing in
   that slot (an empty alternate) there is nothing to swap: the input moves, the other button keeps
   its other slot ("X moved from Cross to Square"), and if that leaves an essential button with
   nothing the change is refused with the same "needs a button" message. The same input on the
   button's other slot still makes the two slots change places.
4. **A hand-written dhd.ini line that breaks rule 1** resets only that device's layout to its
   defaults, with a dhd.log line per device: `controls: pad1: Cross has no button: Controller 1 is
   back to its defaults`. The file keeps the line until the next change on that device, which
   writes the device's whole line again (the owner's `pad1 = cross:none square:none` becomes the
   defaults at the next start; one change there writes a good line).

The owner's sequence under the new rules: Square's button onto Cross swaps (`cross:west
square:south`), clearing Cross is refused every time, clearing Square is allowed; Controller 1
keeps a button for Cross throughout. It is the unit test `the_owners_sequence`.

### 2026-11-06: the release review

- **R1 is kept too** (60b4e29): it is the only input that switches REVIVED SETTINGS' tabs (they
  wrap, so R1 alone reaches each), so each device keeps an input on Cross, Triangle, START, R1 and
  the d-pad (`controls.rs`, `essential`). A value dhd.ini can't read keeps the button's default;
  only `none` (or nothing) unbinds it, as docs/PLAYING.md says.
- **A click on a binding row starts its change** (f98ce93): value rows say what they are
  (`MENU_VALUE_FIXED`, `_ARROWS`, `_ACTION`, `port/hle/menu_page.h`); a click on an ACTION row is
  Cross.
- **The launcher's wait** (7d6f880, 2420caa, `port/launcher/ui/controls.js`): it counts only on the
  Controls tab, for the device it began on, for five seconds; the button that answered it does
  nothing else until let go (Start no longer launches the game, B no longer goes back).

## Surprises

- `func_00220230(1)` (the game's held buttons for the second port, which menu_page.c ORs in) isn't
  a pad's buttons while no pad is open there: reading Square from it cleared slots on other presses
  (seen in the hidden-desktop run: three clears nobody asked for). The page now reads players 1 and
  2 from the host (507d757).
- `python port/tools/rustgs.py test` always adds `--features native`, which dhd_settings doesn't
  have: its tests run with plain `cargo test -p dhd_settings` in `port/render` (as AGENTS.md says).
- A hidden-desktop run with a dhd.ini of its own gets that file's defaults, Discord presence on
  among them: test dhd.ini files want `discord = off`.
- 2026-11-06: `rustgs.py test --target x86_64-pc-windows-msvc -- -p dhd_render -p dhd_settings`
  runs dhd_settings' tests through the wrapper too (`--features native` then has a package that
  owns it).
- 2026-11-06: the Controller 1 page can't be driven through a swap on the hidden desktop:
  DHD_TEST_PAD only runs from a race's GO, and the page's wait reads a real controller
  (`host_service_controls_capture`). Square (clear) comes from the keyboard through the layouts,
  so the refused clear on Controller 1 can be shown; the pad swap is the unit test's.
- 2026-11-06: dhd.log's `controls: keyboard ..., controller 1 ...` line prints the dhd.ini values
  as written, so after a per-device reset it still shows the broken line; the `back to its
  defaults` line before it says what the game plays with.

## Result

What changed: see Decisions; dhd.ini `keyboard`, `pad1`, `pad2` documented in docs/PLAYING.md
(Controls, Settings). Checks, 2026-11-06, Windows:

- Unit tests: `cargo test -p dhd_settings` (7 bindings tests: defaults are the old layouts, lines
  read and written, broken lines, moves, the reachability rule, stick options; the generated
  header), the launcher's `cargo test` (its edit and schema), `node port/launcher/test_controls.js`
  (with the launcher's schema too), `test_runtime.py`'s keyboard (a player's layout: J for Cross,
  WASD as the ramped d-pad, stick keys, Esc fixed) and gamepad mapping (every place through the
  default layout, triggers and pressures, another layout with an alternate, swap and invert).
- `python tools/check.py port`: OK (docs, tree, core, test_runtime, game, net tests, test_gs_wgpu,
  wasm2c chunks).
- State traces with the defaults against the base build (113f76f + the finding): boot 600/600 and
  race 5000/5000 rows equal (the lane's last game build, 507d757).
- Equivalence traces (the bindings drive the game as the buttons they stand for): race with
  DHD_TEST_PAD `east`, `dpad_left`, `south` and `DHD_PAD1=cross:east circle:south right:dpad_left
  left:dpad_right` = the race with DHD_RACE_PAD `cross`, `right`, `circle`, 5000/5000 rows; the same
  pad inputs without the layout differ from field 3334 (when they start); DHD_KEYS `j`, `kp5` with
  `DHD_KEYBOARD=cross:j circle:kp5` = the plain race, 5000/5000; a split-screen race with
  DHD_TEST_PAD2 `west` and `DHD_PAD2=cross:west square:south` = the plain one, 6000/6000.
- Hidden desktop, posted keys (live_game.py): with `keyboard = cross:j` in dhd.ini, X and Space
  leave the title as it is and J opens the main menu. In the game: OPTIONS > PC SETTINGS >
  Controls, Cross waits ("Press a key for Cross (Esc: cancel)"), J takes the main slot (dhd.ini
  `keyboard = cross:j,x`), on Square's row X moves there ("X moved from Cross to Square", dhd.ini
  `cross:j square:x`), Esc goes back, Space then does nothing and J opens CONTROLS: the change works
  without a restart. Button icons with `DHD_PAD1=cross:east circle:south triangle:west square:north`:
  Xbox icons show B for RETRY/SELECT and X for CANCEL/BACK, PlayStation icons ○ and □.
- The launcher builds (`port/tools/build_launcher.py`); its Controls tab drawn headless (Edge, a
  mocked Tauri over the real ui files): keyboard, Controller 1, a slot waiting.

Checks of the 2026-11-06 rules (each device on its own, swaps), Windows:

- Unit tests: dhd_settings' 8 bindings tests (new: `a_key_swaps`, `each_device_keeps_the_menus`,
  `the_owners_sequence`, the per-device reset in `broken_lines_fall_back`), the launcher's `cargo
  test controls` (its edit test now swaps and has Cross's last key refused), `node
  port/launcher/test_controls.js` with and without the launcher's schema (the page logic encodes
  no rule, unchanged). `test_runtime.py`'s keyboard and gamepad tests build layouts directly and
  encode no rule: unchanged, and they pass.
- `python tools/check.py port`: OK.
- State traces with the defaults against the base build (fa9de1a's sources built in the lane):
  boot 600/600 and race 5000/5000 rows equal.
- Hidden desktop (live_game.py, a dhd.ini holding the owner's `pad1 = cross:none square:none`):
  at boot `controls: pad1: Cross has no button: Controller 1 is back to its defaults` and the
  keyboard untouched. REVIVED SETTINGS > Controls > Controls, keyboard: Cross's row, Cross, then Z
  (Square's key): "Swapped Cross and Square" (dhd.ini `keyboard = cross:z,x square:space`); Square
  (now Space) clears Z (`cross:x`), Square again: "Cross needs a key", nothing changed. Controller
  1: its rows show the defaults; Square on Cross's row: "Cross needs a button", though the keyboard
  has Cross; Square on Square's row clears it and writes `pad1 = square:none` (the broken line
  gone). Captures kept outside the repository.

Left open:
- No real controller was pressed in these checks (the PC's pad is the owner's, and SDL here has no
  virtual pads): pads were checked through DHD_TEST_PAD, which goes through the same gamepad_apply as
  SDL's states, and the unit tests; a hand check with a pad (capture of a button, Swap Sticks, Invert)
  belongs to the lead's merged-build check.
- The browser's own keyboard and pad layout (port/web/www main.js, worker.js) don't take the
  bindings; Android's SETTINGS shows no Controls row (its controllers still use dhd.ini's pad1,
  pad2 if written by hand).
- Players 3 and 4 have no layout of their own.
- 2026-11-06: the swap on a controller's page was checked by the unit tests only (no virtual pad
  reaches the page's wait); the owner's DualSense repeat of their sequence is the hand check.
