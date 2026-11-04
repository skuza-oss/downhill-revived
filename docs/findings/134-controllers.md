# 134: controllers: stick dead zone, button icons, starting without the launcher

Status: current
Checked: 2026-11-04

Branch `feat/controllers` (from `next/0.7.7`), merged for 0.7.7. Issues #20 (a dead-zone setting with a
visual indicator) and #27 (a Steam Deck shortcut without the launcher; Xbox button prompts).
No change to `src/`. The in-game checks left for the merged build are listed at the end.

## 1. The sticks' dead zone (#20)

### What the game does
A stick axis reaches the game as a byte, 128 the centre (pad.c, finding 059). `func_00220AE0`
maps it to -127..127 with its own dead zone (raw 92-162 give 0; 163 and 91 give ±1, which it
also drops), and each riding action squares the result (`v * v / 127`: `func_0017E1C0` for
steering, the same for leaning and the right stick's pedal and brake). So the first byte that
moves anything is 174 one way and 80 the other: **about 36% of the travel does nothing**, which
is what players feel. The squaring has a quirk the PS2 had too: the squared value 1 (raw 174-177
and 77-80) is read by `func_0012DEB0` as a digital press, full lock, so a stick held just past
the dead zone gives a short burst of full steering.

### What the setting does
`stick_dead_zone` (dhd.ini, `DHD_STICK_DEAD_ZONE`; the one table in `port/render/settings`):
`game` (default, also without dhd.ini) keeps the byte exactly as before, `(v >> 8) + 128`. A
percentage (choices 5-50%; any number 0-95 in dhd.ini) sets where the stick starts to count:
`port/hle/stick.h`'s `stick_byte` makes the travel up to it the centre and spreads the travel
past it over the bytes that steer in proportion, 178..255 one way and 76..0 the other (the first
bytes whose squared value is 2, past the full-lock pair). Steering then starts at the player's
dead zone, grows smoothly and still reaches full lock at the end of the travel. Choices made:
- One setting for both sticks (the left steers and leans, the right pedals and brakes; one
  number is what players asked for and what the launcher can show).
- Each axis on its own (axial), as the game reads them: steering is the left X alone.
- No outer dead zone: full deflection already reaches 255/0, and a second number would need its
  own explanation; stick.h is where it would go.
- Applied where SDL's axis values are read (`port/hle/gamepad.c`, `axis_byte`), so the keyboard
  (its own ramp, finding 059), the scripts and the browser are untouched. gamepad.c keeps the
  value in an atomic; host.c's `host_pad_device` follows `DHD_STICK_DEAD_ZONE` each field on the
  game thread, where PC SETTINGS changes it, so the row applies at once.

PC SETTINGS > Dead Zone (applies at once) and the launcher's new **Controls** tab: Stick dead
zone, the stick picture, Button icons, and Vibration (moved there from Game).

### The launcher's picture
`port/launcher/ui/stick.js`: two boxes, left and right stick, redrawn every frame while the tab
is open, from the first controller the launcher's WebView sees (the browser Gamepad API, which
pad.js already uses: no new Rust or SDL in the launcher). The hollow ring is the stick as it is,
the filled dot (orange when it moves the rider) what the rider gets: stick.h's mapping,
`func_00220AE0`'s curve and the action's squaring, written again in JS (stick.js names where each
piece comes from); the shaded bands are the travel that does nothing with the chosen setting.
While the picture is on screen the stick no longer moves the launcher's focus (the d-pad still
does). Without a controller it says to connect one and move a stick (the Gamepad API shows a pad
only after an input). Chosen over a preview from the game: the game isn't running while the
launcher is open, and the Gamepad API needs nothing new.

### Tests
- `test_gamepad mapping` (`port/tools/test_runtime.py` runs it): each kind of pad's button
  symbols from what SDL reports, 18 stick values through the dead zone at `game`, 5%, 20% and
  40% (inside the zone the centre, just past it 178/76, half way, full travel 255/0), every 7th
  value of the travel at 0-95% (bytes only grow, none inside the game's band or the full-lock
  pair), and the setting's values. All pass. SDL's build has no virtual joysticks
  (`sdl3.py`: `SDL_VIRTUAL_JOYSTICK=OFF`), so a virtual pad through SDL was not used; the values
  are given as SDL hands them to gamepad.c.
- `DHD_STICKS` (new, autopad.c, like DHD_KEYS): moves the left stick from the race's GO,
  `start:duration:lx=P[/ly=P],...` with P the travel in percent, through stick.h with the
  current `DHD_STICK_DEAD_ZONE`, and logs `sticks race=... dead=... lx=..% byte=... steer
  right=... left=...` at each change: the in-game check below.

## 2. Starting without the launcher's window (#27)

Today `dhd.exe` started on its own opens the launcher and ends (`launcher_link.c`), so a Steam
shortcut to it shows the launcher. PLAYING.md still described a `launcher = off` setting that no
longer exists; that text is replaced.

`DownhillRevived --play` (launcher `main.rs`, `play_at_once`): the game starts at once with the
saved settings, through the same `Place::start` as PLAY (data folder, `DHD_SETTINGS`,
`DHD_MEMCARD_DIR`, `DHD_DATA_DIR`, `DHD_LAUNCHER=1`), with no window and no WebView (nothing of
Tauri starts), and the launcher ends when the game does; play time is counted as from PLAY. The
window opens instead when something needs the player: no game beside the launcher, no disc image
in dhd.ini or one the game would refuse (`disc::problem`), or the game ending with the online
menu's Update Now (exit code 85: the window then offers the update). Updates per `auto_update`:
the check runs on a thread while the game plays, so it never delays the start; `on` installs a
newer release after the game ends (no relaunch), `ask` leaves it to the next time the window
opens, `off` checks nothing. The same on Windows, the AppImage (its AppRun passes arguments on)
and the macOS app (`open -a "Downhill Revived" --args --play`). Documented in docs/PLAYING.md and
README.txt, with Steam's Add a Non-Steam Game and Launch Options `--play`.
`port/tools/hidden_game.py`'s `Game` takes `args` now, to start the launcher with `--play` on the
hidden desktop.

## 3. Button icons (#27)

### How the game draws them
The menus' font (the scene's 512x512 atlas, text engine mode 1, findings 050 and 058) holds the
button icons as cells; a string names one with a byte >= 0x80 ("\xAD SELECT  \xA7 BACK",
"PRESS \xAD TO START") and `func_0020FDB0` draws it as one sprite like any letter. Seen in the
atlas (dumped once from GS memory for this, not kept): 0xAD Cross, 0xAB Circle, 0xA0 Square,
0xA7 Triangle (grey discs with the symbol), 0xB2 L1, 0xB3 L2, 0xB9 R1, 0xBA R2 (the letters),
0x8D/0x8F/0x90/0x9D the d-pad's arrows (left, right, up, down; PREV/NEXT), which stay. The
strings use 0xA0 for INFO/CHANGE/PREVIEW, 0xAB for ADD/MODIFY/Switch Camera, 0xB9 for DELETE.

### The setting
`prompts` (`DHD_PROMPTS`): Auto (default) / PlayStation / Xbox / Nintendo; the tools' runs (no
dhd.ini) keep PlayStation. Auto follows player 1's controller: gamepad.c tells each pad's kind
from `SDL_GetGamepadType` (PS3/4/5, Xbox 360/One, Switch Pro and Joy-Cons), else from the label
SDL gives the bottom face button (A: Xbox, B: Nintendo, Cross: PlayStation), which covers a Steam
Deck and SDL's standard pads (`gamepad_kind_of`); host.c reads player 1's (`gamepad_kind`) each
field. A keyboard or no pad keeps the PS2's icons. PC SETTINGS > Button Icons (applies at
once) and the launcher's Controls tab. The browser hides both new PC SETTINGS rows (its
`DHD_PC_SETTINGS_ROWS` doesn't list them).

### Drawing
The icons are the project's own art: twelve 128x128 PNGs in `port/res/buttons/` (A, B, X, Y, LB,
RB, LT, RT, L, R, ZL, ZR; neutral grey on dark discs and rounded boxes, made for the project with an
image generator, no brand logos or colours; docs/CREDITS.md says so). `build_host64.py` embeds
them (`button_icons.inc`, as it does the window icon) and `port/gs/button_icons.c` decodes them
with the Rust library's PNG decoder the first time they are needed.

They reach the GS without VRAM of their own (so the free GS blocks finding 132 found in the shell
menus, 0x38E0-0x3F80, are not used, and races and the pause menu, which that census didn't cover,
need nothing either): the renderer already draws the menus' atlas from a
copy it makes in memory (`font_sharp.c`, finding 058: the sharp font, twice the atlas's size, with
mipmaps, drawn as a replacement texture). `button_icons_paint` paints the chosen icons into that
copy over the PS2 icon cells: each fitted, its own shape kept, into the box the game's icon fills
in its cell (an area average of the 128-pixel art, 4x4 taps a texel), so it has the game's size and
place in every string, and its brightest grey is the game's symbol colour, so the text's colour
modulates it as it does the game's icons. By position, as the pads are read: Cross = A (Xbox) / B
(Nintendo), Circle = B / A, Square = X / Y, Triangle = Y / X; L1, L2, R1, R2 = LB, LT, RB, RT / L, ZL,
R, ZR; the d-pad arrows stay. With `font = original` and icons chosen, a copy is made anyway (the
font as it is, each texel 2x2, plus the icons). The style is chosen on the game thread each field
(host.c: the setting and player 1's controller's kind from gamepad.c) and mixed into the menus'
font texture's key (`native_core.inc`), so a change (PC SETTINGS, plugging in another pad) makes a
new copy. Nothing is written to GS memory or game memory: the game and its state see the PS2's
icons; the exact renderer (the reference path, `renderer = exact`) and the browser draw the PS2's
icons. Sharpness: the copy has 64 texels a cell (the font copy's 2x, its memory cap); at 1x-4x the
icons are drawn from mipmaps, at 8x magnified about 2x with the copy's filtering, like the text.

The first version of this lane drew letter discs as geometry in the game's text packet (a patch of
`func_0020FDB0` calling `port/hle/prompts.c`); the drawn icons replace it, and the core is
unchanged again (HostPad, pad.c, the patches and the browser worker as in next/0.7.7).

Seen on this PC (hidden desktop, Xbox icons, scale 2, one run): the title's PRESS (A) TO START
and the main menu's (A) SELECT (Y) BACK with the project's icons at the game's icons' place and
size. They looked dimmer than the game's own icons there (the art's light grey was taken as 255);
the art's brightest grey is now the game's symbol colour (not seen after that change). Before the
art, the geometry version showed the same strings. The atlas settled which cell is which (Circle and Square are not in
the strings' order).

### Checks done
- Builds (core and dhd64.exe), `cargo test -p dhd_settings` (the new keys, settings_table.h and
  web_corpus.json regenerated), launcher `cargo test` (17 pass), `test_gamepad mapping`,
  `node --check` of the launcher's scripts and the browser worker.
- Boot state trace with default settings: 600 of 600 fields equal to next/0.7.7's (with the
  geometry version; the drawn icons change no core code at all).
- The core module cache cherry-picked from next/0.7.7 (b6ef0b0) hashed only tracked files, so a
  change to a new, not yet added source hit a stale module: fixed in this branch (its key takes
  untracked files git doesn't ignore).

## Checks for the merged build (the lead)
Use `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0`, `DHD_LAUNCHER=1`, `DHD_MEMCARD=1` with an empty
`DHD_MEMCARD_DIR`, scale 2.
1. **Icons per pad type** (the drawn icons, grey letters on dark discs, LB/RB etc. on dark rounded
   boxes; they take the text's tint, yellow on a highlighted row). `DHD_PROMPTS=xbox`, then `nintendo`, then `playstation`:
   - title: "PRESS (A) TO START" (Nintendo "(B)"; PlayStation the PS2's cross icon);
   - main menu footer: "(A) SELECT (Y) BACK" / "(B) SELECT (X) BACK" / cross and triangle; the
     left "PREV/NEXT" arrows unchanged in all three;
   - a screen with Square and Circle: the rider select's bio page ("\xA0 more", Square: Xbox
     "(X) more", Nintendo "(Y) more") and the replay or track editor footers with "\xAB"
     (Circle: Xbox "(B)", Nintendo "(A)"); a screen with the L1/R1 text icons, e.g. the
     "\xAB ADD \xA0 CHANGE \xB9 DELETE" footer: "(B) ADD (X) CHANGE RB DELETE" on Xbox;
   - widescreen on (`DHD_WIDESCREEN=1`): the discs stay round;
   - `DHD_PROMPTS=auto` with a DualSense: the PS2 icons; with an Xbox pad or a Steam Deck: A/B/X/Y
     (dhd.log's `controller: ... is player 1` line names the pad).
   - PC SETTINGS: the rows Dead Zone and Button Icons after Vibration; changing Button Icons
     changes the page's own footer icons at once.
2. **Dead zone.** A race with `DHD_RACE_SYNC=1`, the state trace's RACE_NAV and
   `DHD_STICKS=2:2:lx=15,5:2:lx=25,8:2:lx=60,11:2:lx=100`, run twice:
   - `DHD_STICK_DEAD_ZONE=game`: dhd.log `sticks race=...` lines: lx=15% byte=147 and 25%
     byte=159 give `steer right=0`; 60% byte=204 steers; 100% byte=255 full;
   - `DHD_STICK_DEAD_ZONE=10`: 15% gives byte 182 and `steer right` above 0, 25% more, 60%
     more, 100% byte=255 (127);
   - with no DHD_STICKS and the default setting, the race state trace equals
     the shared reference build's race trace.
   - Launcher (`DownhillRevived.exe`, Settings > Controls) with a pad: moving the left stick moves
     the ring; with Game the dot stays grey in the middle until about a third of the travel; with
     10% it turns orange soon after the centre; the stick doesn't move the focus there.
3. **--play.** In a folder with the launcher and `dhd.exe` beside it and a dhd.ini naming the
   disc (or `DHD_GAME_DIR` pointing at it): `DownhillRevived.exe --play` on the hidden desktop
   (hidden_game `Game(launcher, out, env={'DHD_SETTINGS': '<unset>', 'DHD_GAME_DIR': ...},
   args=['--play'])`): the game's SDL window appears with no launcher window on that desktop,
   dhd.log starts with the run's settings line; posting WM_CLOSE to the game's window ends the
   game and then the launcher, and `saves/playtime.txt` grows. With `iso` removed from dhd.ini,
   `--play` opens the launcher's window instead. My script for this is
   the lane's play_check.py script (not committed) (`LAUNCHER GAME_EXE OUT`); it was not run.
