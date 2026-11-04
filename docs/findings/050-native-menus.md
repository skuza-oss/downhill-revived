# 050: PC settings and online screens in the game's own menus (research), first-start disc prompt

Status: partly superseded by 093 (the first-start disc prompt, Launcher.ps1)
Checked: 2026-11-04

The front-end research (one loop, screens, SharedMenu popups, text and font, the hooks) is
current, and the PC SETTINGS page and ONLINE screens were built from it (findings 064, 065, 092;
`port/hle/pc_settings.c`, `menu_page.c`, `online_menu.c`). The first-start disc prompt is gone:
the launcher picks the disc (Android: its document picker), and the game has no disc dialog
(findings 093, 099). `Launcher.ps1` is gone too (the launcher is `port/launcher`, finding 093).

Round 35. The user wants the PC settings (today in port/launcher/Launcher.ps1) and, later, online
lobbies to look native: part of the game's own front end, with its font, layout, sounds and pad
navigation. This finding records how the front end works in the decomp and what host-side hooks
would add a "PC SETTINGS" page and new online screens without changing `src/`. No menu code is
written yet. Line numbers are of the files at commit bf0072f7.

An ImGui overlay was planned first and dropped before any code (user decision); it stays the
fallback (last section).

## How the front end works

### One loop, one switch
- `func_001EB4D0` (src/unsorted/001EB1D8.cpp:486) is the front end: init, then one frame per
  iteration until `D_0029DFB4` is set (:582). It sets the screen id `D_0029DED8` to 0 (:496) and
  points ~40 `.sbss` string-table pointers (`D_0029E038..D_0029E0DC`) at tables in `.data`
  (:511-552).
- The screen is a plain `switch (D_0029DED8)` (:632-723), one function per screen, not a table.
  Each frame ends with `func_0020F2A8(0)` (:727), which links the frame's text packet.
- Only this unit (0x1EB1D8-0x1F8200) reads or writes `D_0029DED8`. `D_0029DEEC` holds the previous
  id. Before it, `func_001B6708` (asm) loads `SHELL\UI.NGP/.RTX/.TEX/.PTR` (`func_001B9E28`,
  `func_001B9BE0`): 3D scene, rider ring and art only.

| id | function (001EB1D8.cpp line) | screen |
|---|---|---|
| 0 | `func_001ED3E0` (:1175) | title, PRESS X TO START; attract mode after 3600 idle ticks |
| 15 | `func_001ED5B8` (:1232) | main: SINGLE PLAYER -> 32, MULTI-PLAYER -> 10, OPTIONS -> 2 |
| 2 | `func_001F0160` (:2538) | OPTIONS |
| 13 / 19 / 20 | `func_001F0A28` (:2794) / `func_001F1168` (:2921) / `func_001F14A0` (:2996) | controls / difficulty / pickup editor |
| 10 | `func_001EED78` (:1958) | number of players (2-4) |
| 24 | `func_001EDD28` (:1475) | multiplayer game mode |
| 16 | `func_001F3B98` (:3744) | new / continue game |
| 1, 11, 12, 17, 21, 30 | `func_001ED900` (:1316), `func_001EF0E0` (:2042), `func_001EFA78` (:2335), `func_001EFE28` (:2428), `func_001EF5D0` (:2189), `func_001F4508` (:3948) | single-player modes |
| 3-6, 28, 33 | `func_001F24B8` (:3379), `func_001F4960` (:4067), `func_001F1A90` (:3188) | rider select |
| 18, 32, 34 | `func_001F1500` (:3052), `func_001F1968` (:3120), `func_001F2098` (:3309) | bike shop / level select (modal popup `func_00139BF8`), camera moves |
| 9 | `func_001F43C0` (:3890) | GET READY countdown, leaves the front end |
| 22, 25 | `func_001EE2A0` (:1659), `func_001EE8A8` (:1837) | continue career / multiplayer game |

Ids 7, 14, 23, 26, 27, 29, 31 and 35+ have no case: free for new screens (8 is written only as a
"previous" id before a race).

### A screen
Screens are written by hand. There is no data-driven item list; each screen has a static menu
struct `D_004C6xxx`: start tick, frames shown, first/last item, cursor, back id, next id,
disabled flags. One frame:
1. `func_001F7420` (:5340) initialises it on the first frame. `func_001F74F0` (:5364) ticks it
   and skips disabled items.
2. Background and button legend: `func_001BAF10(&D_004B25A0, n)` (001BA3B8.c:388).
3. The items, through the shell text helpers
   `(style, group, colour, x, y, string, target alpha)`:
   - `func_001F5398` (:4387), left-aligned
   - `func_001F5D08` (:4540), centred
   - `func_001F5EE8` (:4567), right-aligned

   Styles come from table `D_004C6560` (filled by `func_001F6AB8`, :5094). Group 1 is the
   highlight. Alpha 192 means disabled. Coordinates are 640 x 224 (a field).
4. Input: set `D_0029DF7C` = pad, then call `func_001F4A98` (:4092). It latches one edge per
   press into `D_0029DF84..97` (up DF86, down DF87, left DF84, right DF85, cross DF8B,
   triangle DF89, square DF88, circle DF8A, L1/R1/L2/R2 DF94-97, select DF8C). The left stick
   counts as the d-pad. Pads come from `func_002205A0`/`func_00220640`, with the latch in
   `func_0021F8E0`.
5. Act:
   - Down/up: `func_001F7550` (:5394) / `func_001F75C0` (:5425) move the cursor and play
     S_NEXT / S_PREVIOUS.
   - Cross: `func_001F7640` (S_SELECT, :5442), then `D_0029DEEC = D_0029DED8; D_0029DED8 = next;`
     and `func_001F76B8` (:5466) re-arms the screen.
   - Triangle: `func_001F7680` (S_BACK).
   - Sounds play by name through `func_001C57A8` (001C5040.c:322; SHELL_S bank, finding 022).

### The second menu system: SharedMenu popups
The options sub-pages, the in-race pause menu and the bike shop use a generic popup object
(include/menu.h, `SharedMenu`, 0x4FC bytes):
- 64 `MenuItem`s of 16 bytes, count at +0x418, title at +0x448, vtable pointer at +0x4F8.
  Vtable entries are 8 bytes (delta, function); slot 5 is the per-frame update, 7 accept,
  8 cancel, 11/12 enter/exit.
- The constructor is `func_0018D9D0`.
- Items are added in two ways:
  - by text id: `func_0018E3A8(menu, id, &D_0029DE68, enabled)` (0018E3A8.c), with ids indexing
    the English table `D_0026F540`, set up at 0018F310.c:37;
  - by raw string: `func_0018E408(menu, string, enabled)` (0018E408.c, through `func_0018D8A8`).
- `func_00190108(&D_003F52D0, menu)` (00190108.c) runs the popup modally. It calls enter, loops
  in `func_001901C0`, then calls exit. `D_003F52D0+0x12C` says whether the player cancelled.
- AUDIO / OTHER (options item 2, 001EB1D8.cpp:2576-2582) is the model for a settings page:
  - object `D_00389D90`, vtable `D_00284FB8` (asm/data/data.data.s:28927);
  - build `func_0015F0C8` (0015F0C8.c:104) adds Vibration/Stereo/volumes;
  - update `func_0015E750` (0015E750.c) changes the item under the cursor with left/right and
    redraws its value;
  - accept `func_0015F410`; cancel `func_0015F038` restores the values `func_0015E6E0` saved.
- The in-race pause menu is the same kind:
  - builder `func_0015E238` (0015E238.c) adds RESUME (id 0x19), OPTIONS (0x48), ... QUIT (0x1A);
  - vtable `D_00285040`, set in `func_0015E130`.

### Text and font
All menu strings are ASCII in the executable. There is no language or text file on the disc.
- Shell tables live in `.data`, e.g. `D_002758D8` (main menu, asm/data/data.data.s:9316) and
  `D_00275970` (options, :9403). The strings themselves are in `.rodata` 0x2949E0-0x2968E8 and
  `.sdata`.
- The engine is 0x20F058-0x210250, with two fonts in `D_004F3570` and four view contexts:
  - `func_0020F058(vram, mode)` loads a font.
  - Mode 0 is the font built into the executable: a 128x128 4-bit image at 0x263C00 with its
    palette at 0x263BC0 (in `.vutext`). It has 16x16 cells from '!', so upper case, digits and
    punctuation; lower case is folded to upper case.
  - Mode 1 uses a font atlas from the scene (`func_00210250`, scene node type 0x12, 001589A8.c:134).
    Bytes >= 0x80 are button icons.
- Drawing: `func_0020F9A8` / `func_0020F9F0` / `func_0020FA98` (left / centre / right),
  `(font, view, x, y, string)`. Each glyph is one textured GS sprite (`func_0020FDB0`).
  `func_0020F860` measures a line in 1/16 px.
- Style: colour with `func_0020F518(font, r, g, b, a)` (0x80 = 1.0), size and spacing with
  `func_0020FB80`.
- Panels: the shell's panels and highlights are scene objects toggled by id
  (`func_001F57D0(9000+id, on)`, :4453). The popups draw their boxes immediately:
  - `func_00190E48`, translucent box;
  - `func_0018F9C0`, highlight bar;
  - `func_001913A8`, value bar.

## Can host code add a PC SETTINGS page and online screens? Yes

All of it is host-side code plus build-time patches; nothing changes in `src/`. The port already
has the three mechanisms needed:
1. **Build-time patches.** port/patches.py patches a unit's text or replaces a function body in
   the PC build only (the same as the widescreen field of view, the race seed and the
   fabsf fixes).
2. **Host functions called by game code.** Game code is native i686 code, so a patched
   `case` or vtable slot can call a C function in port/hle directly. The game's functions are
   plain C symbols (`func_XXXXXXXX`) host code can call: text helpers, input latch, sounds,
   SharedMenu builders.
3. **Whole-function replacement.** `RETAIL(func_...)` in port/hle replaces a game function by
   native host code (kernel, IOP and GS calls already work this way).

Game state stays exact as long as the additions write no EE memory while unused. The gates walk
0 -> 15 -> 33 -> 1 -> 11 -> 18 -> 9 (finding 024) and never open Options. Rules:
- Item labels are host strings, chosen in the patched code (`index == 9 ? port_label :
  table[index + 1]`), not written into `.data` tables (an EE dump would see the word).
- PC settings live in dhd.ini only (`settings_save`, port/hle/host.c), never in the memory-card
  save struct `D_004225A0+0x11A60`.
- Every patch is checked with gate_native_renderer.py and the race dumps before it is accepted.

### Pieces, hooks and estimates
Estimates are for one port worker who knows the codebase, including gates.

| piece | hooks | difficulty | estimate |
|---|---|---|---|
| A. "PC SETTINGS" item in OPTIONS (menu 2) | Patch `func_001F0160`: last item 8 -> 9, disabled[10], label for index 9 from the host, `case 9:` calls the host page. The rows (y = 90 + i*10) end at 180, above the legend at 199. Strings table entry [10] stays as it is (an unused "UNLOCK RIDERS/BIKES(test)"). | easy | 0.5 day |
| B. The PC SETTINGS page as a SharedMenu popup, cloned from AUDIO / OTHER: Resolution, Display, Window size, Picture shape, Scaling, Widescreen (after restart), Skip movies | Host object (0x4FC bytes, host memory), host vtable (copy of `D_00284FB8` with our build/update/accept/cancel). Items via `func_0018E408` with host strings ("RESOLUTION  2X"). Left/right in update as `func_0015E750` does; redraw the label (`func_0018D8A8`). Sounds `func_001C57A8`. Saved with `settings_save`; run modally with `func_00190108`. | medium | 2-3 days |
| C. Apply live from the page | Today the window mode, size, shape and filter are read once at start (port/gs/present.c init, gpu_native.inc native_init). Needed: window style/size/fullscreen switching on the EE thread (it owns the window, gs_vsync pumps its messages); shape and filter as live fields of the native renderer; scale: re-create the native surfaces, or label it "after restart" first. Widescreen patches EE data at boot (host_widescreen_init) and both online players need the same, so: after restart. | medium (scale: medium-large) | 2-3 days; live scale +2 |
| D. Same page from the in-race pause menu | Patch `func_0015E238` to add a raw-string item with a private id; patch its accept function (vtable `D_00285040` slot 7) to open B. Online races are lockstep: the pause menu must not stop the simulation there (the online branch decides how pause works). | medium | 1-2 days |
| E. New front-end screens (online): ONLINE in the main menu (15) or the players menu (10), screens on free ids 35-39 | Patch `func_001ED5B8` (or `func_001EED78`) for the item. Patch the switch in `func_001EB4D0` with `default: port_shell_screen(D_0029DED8)`. Host screens follow the same frame pattern: background `func_001BAF10`, rows with `func_001F5398/5D08/5EE8`, input `func_001F4A98` flags, sounds, `D_0029DED8` transitions. | medium | 2 days for the frame and navigation |
| F. Public lobby list, create private lobby | E plus the online branch's client (port/online, WebSocket): a list that refreshes while shown, paging with L1/R1, status lines. | medium | 2-3 days after the client API is settled |
| G. Join by code, name entry | A character picker in the arcade style: up/down cycles A-Z 0-9, left/right moves; the font has upper case and digits only. Also typing on the PC keyboard: WM_CHAR from present.c's window procedure into a host buffer. | medium | 1-2 days |
| H. Starting an online race from these screens | Do what menus 11/24 do before id 9: game mode `D_004B25A0+0x10C`, player count `+0x288`, level, riders, then `D_0029DED8 = 9` with the countdown timer. Must agree with the online branch's lockstep start (seed, both sides' settings). | large: depends on the online design | 3-5 days, with the online branch |

Risks:
- The SharedMenu vtable layout (8-byte delta/function slots, gcc 2.95 style) and the item fields
  (`MenuItem` x0/x4/x6/xC) are known only from these callers. A host subclass needs one small
  test first: open an empty host popup from OPTIONS and close it.
- Host code calling game functions must follow the port's scalar ABI. The text helpers and menu
  calls take only ints and pointers, so they are safe. Float arguments go through the
  ScalarBridge contracts (port/scalar_contracts.json).
- The modal popup loop runs vsyncs itself, so live display changes made there run on the EE
  thread, where they belong.

### Fallback if a piece is too hard: an overlay in the game's style
Instead of game menus, the native renderer draws the page over the picture in native_present
(gpu_native.inc), under the same input rules as the dropped ImGui plan:
- the pad and keys go to the menu while it is open;
- the game keeps running;
- with the menu closed, it costs one flag test.

It looks like the game:
- **Font:** the game's own mode-0 font, decoded at run time from the player's executable image in
  EE memory (0x263C00 with palette 0x263BC0, PSMT4 128x128). Nothing goes into the repository.
- **Colours:** the shell style table `D_004C6560` (grey rows, gold highlight).
- **Sizes:** the shell's row spacing at 640 x 224, scaled to the window.
- **Sounds:** host-side calls to `func_001C57A8` only when the shell is running.

Estimate: 2-3 days, with no game patches at all. It is the safe choice for anything shown during a
race (D), where opening a game popup would pause or change the game's own state.

### Recommended order
1. A + B + C without live scale.
2. Gate.
3. Then E/G, with F/H once the online client API is fixed.
4. D last, or as the overlay.

## First-start disc prompt (implemented this round; removed, see the note at the top)
`host_disc_ready` (port/hle/host.c), called by `main` (port/hle/boot.c) before the relaunch, so
only the first of the two dhd.exe processes asks.
- **When it asks:** dhd.ini has no `iso`, or the file is gone, or it isn't Downhill Domination
  (USA). The check reads the ISO 9660 primary volume descriptor (sector 16, "CD001"), the root
  directory record at offset 156 (extent and size), and SYSTEM.CNF, which must name SCUS_971.77.
  Layout per ECMA-119 (https://ecma-international.org/publications-and-standards/standards/ecma-119/)
  and the OSDev ISO 9660 page (https://wiki.osdev.org/ISO_9660). This is the same lookup the
  port's sceCdSearchFile does (port/hle/cdvd.c).
- **What it shows:** a plain message, then a file-open dialog (comdlg32 loaded when needed, so no
  new import; it starts in dhd.exe's folder).
  - A rejected file gets a message with the reason ("isn't a disc image", "no SYSTEM.CNF",
    "another PlayStation 2 disc (SLUS_...)") and Retry/Cancel.
  - Cancel exits with code 0.
- **What it saves:** the choice is written to dhd.ini with `settings_save`, which replaces the
  key's line or appends one and keeps every other line and comment. Values may be quoted, for
  paths with `;` or `#`.
  - On first start it also writes the launcher's player defaults when dhd.ini lacks them:
    `renderer = native` and `skip_movies = 1`. Without dhd.ini the exact renderer stays the
    default, for the tools.
- **When it doesn't ask:** DHD_ISO set in the environment overrides dhd.ini and is never
  questioned (tests, online launcher), and so does DHD_HEADLESS.
- **Tested** with the clean build bf0072f7 in build/port/r35/first-start:
  - no dhd.ini;
  - a random file (rejected, retry);
  - the real image (dhd.ini written, game relaunched);
  - a second start (no prompt, relaunched at once);
  - a missing image with comments and sections in dhd.ini (prompt; Cancel exits 0; choosing
    keeps every other line).

## PC SETTINGS page (round 36, plans A + B + C)
Built as planned above, entirely from port/ (no src/ change):
- **The OPTIONS row** (port/patches.py on func_001F0160, 001EB1D8.cpp): the last item 8 -> 9,
  `disabled[10]`, the label of row 9 from `port_options_label` (the data table keeps its unused
  "UNLOCK RIDERS/BIKES(test)"), and a `case 9:` that hides the screen's model (9001), runs the
  page and comes back to OPTIONS exactly like the AUDIO row (case 2). The left/right sounds come
  from `port_menu_sound`, patched into the same unit so the u128 sound names never cross the
  host boundary (S_NEXT / S_PREVIOUS on `D_004B25A0 + 0x4F0`, as the front end's value rows).
- **The page** (port/hle/pc_settings.c) is a SharedMenu in host memory, built once with the
  game's constructor func_0018D9D0. Its vtable is a copy of the base `D_00287218` (17 slots of
  {short delta, short pad, function}) with host functions in slot 1 (build, as func_0015F0C8),
  5 (footer for the row under the cursor, then the base func_0018DC20), 7 (Cross: keep, close
  with func_00190688), 8 (Triangle: restore the values the page opened with, close with
  func_0018E468), 9 and 10 (left / right). Run modally with func_00190108(&D_003F52D0, page).
  The title and footer use the item's string pointer (+0x8) with a nonzero text id, since an id
  of 0 is not drawn; the footer is the game's legend 0xA6 unless the row needs a note.
- **Rows:** Resolution (1x-4x, window; next start), Display (window / borderless / fullscreen;
  live, port/gs/present.c gs_window_apply), Window Size (5 sizes; live), Picture (4:3, 16:9,
  stretch, pixels; live, native renderer's present options), Scaling (smooth / sharp; live),
  Widescreen (next start; also sets Picture to 16:9 / 4:3 as the launcher's checkbox did), Skip
  Movies (next start), Vibration (live). No renderer row. A value waiting for the next start is
  marked "*" and the footer says "* Applies when the game starts again"; a value the environment
  sets (DHD_*, listed in DHD_ENV_OVERRIDES by the first process) can't be changed and the footer
  says so. Each change is written to dhd.ini at once with the round 35 writer (other lines kept).
- **Vibration** is new: pad.c passes scePadSetActDirect's motor bytes (actuator 0 on/off,
  actuator 1 speed, per the alignment set with scePadSetActAlign) to XInputSetState
  (https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputsetstate;
  libpad request layout as in ps2sdk ee/rpc/pad). Output only, no game state.
- **Lessons:**
  - the page object must be 16-byte aligned: func_0018E1C0 stores the select/back sound name
    (a u128 at +0x490) with an aligned SSE move (the first build crashed on Cross/Triangle);
  - while a popup runs from OPTIONS, `D_0029DED8` already holds OPTIONS' "next" value 13 (set
    before the switch); it is set back to 2 when the popup returns (DHD_NAV scripts use `13:`);
  - back on OPTIONS the cursor is on CONTROLS again (func_001F76B8 resets the menu), as after the
    AUDIO page.
- **Found on the way (not this round's):** OPTIONS > CONTROLS crashes the port, the same on the
  round 35 build bf0072f7: func_0020FBB0 (text drawing), called by func_001F0A28, reads a string
  at 0x002963C0, a PS2 address used as a host pointer (a data relocation the image misses).

## Round 37: the front end walked and fixed; ONLINE in the main menu
Every OPTIONS row and every row of the race's pause menu was opened, used and closed on clean
builds with DHD_NAV scripts and screenshots (build/port/r37). What broke and how it was fixed:
- **OPTIONS > CONTROLS crashed** (above): six `.word`s splat left as raw PS2 addresses because
  they point inside a labelled block of strings, not at a label (`D_0029D8B0` -> 0x002963C0
  "SELECT CONTROL OPTIONS", four `D_0029D848` pairs -> 0x00295E90, `D_0029D904` -> 0x002968E8).
  port/tools/data.py now relocates them (RAW_STRING_POINTERS, slot -> target, checked against
  the word), as it already did for ASCIZ_POINTERS and FLOAT_POINTERS.
- **OPTIONS > VIDEO CLIPS crashed**: the popup's update (vtable `D_00288540` slot 5,
  func_001F94D0) calls the base update func_0018E830 with the menu still in `$a0`, but its
  source names no argument, so natively `this` was garbage. A scan of the data image's function
  pointers for the same pattern (a `(void)` function whose callee reads `$a0`/`$a1`) found four
  destructor wrappers (func_00201010/080/0F0/160 -> func_001FFD30(this, flags)). All five are
  patched in port/patches.py to pass their arguments on (the func_00202760 pattern).
- **Movies**: playing any movie crashes in the MPEG path (no IPU), so movies are always skipped
  (dhd.ini `skip_movies` is gone from the page and the launcher; DHD_MOVIES=1 still tries). In
  VIDEO CLIPS a chosen clip ends at once (func_001B53C0 is patched to finish its list when movies
  are skipped) and the game goes back to OPTIONS, on VIDEO CLIPS.
- **Cursor**: back from an OPTIONS sub-page the cursor is on the row that opened it (a port
  static fed to func_001F7420's cursor argument; retail puts it back on CONTROLS).
- **Pause menu** (PLAYER 1 PAUSED: RESUME, OPTIONS, TRICK LIST, NEW LEVEL, RE-START, MUSIC
  PLAYLIST, QUIT): see the round's results below.
- **Not fixed: memory cards.** LOAD / SAVE GAME DATA and LOAD REPLAY report no card: the IOP's
  mcserv RPC is not implemented (rpc_unknown). A card emulation (a file per card, ps2sdk's
  mcserv calls) is a round of its own and needs its own gate thought (the race gate assumes no
  card).

**Menu pages** (port/hle/menu_page.c, .h): PC SETTINGS' machinery as a helper. A `MenuPage` is
the SharedMenu (first, 16-byte aligned) plus the vtable copy and the page's functions: build
(rows), update (each frame, before the base update), accept (Cross), back (Triangle), left /
right. `menu_page_rebuild[_at]` builds the rows again at the next update (screens of one page);
`menu_page_enable` flips a row's enabled byte (item +6) without rebuilding. Rows are kept by
pointer, so a page rewrites their text in place every frame. PC SETTINGS now uses it.

**ONLINE** (port/hle/online_menu.c; patch on func_001ED5B8):
- the main menu has a fourth row, ONLINE (label from `port_main_label`; the game's table has
  three). Choosing it hides the screen's model (0x2329) and runs the page modally; back on the
  main menu the cursor stays on ONLINE. Leaving the main menu by any of the game's rows puts the
  menu's last row back to 2, so EE state on the gates' route is unchanged;
- HOME: Name, Public Races, Create A Public Race, Create A Private Race, Join With A Code;
- PUBLIC RACES: GET /v1/lobbies in a thread (net_list_lobbies), one row per race (name,
  players, track; other versions shown disabled), Cross joins, Look Again;
- NAME and JOIN WITH A CODE: text entry with the pad: left/right choose the letter shown in
  brackets, Cross adds it, Triangle deletes the last one (on an empty entry: back). Code letters
  are the server's alphabet (23456789ABCDEFGHJKMNPQRSTUVWXYZ); the code's sixth letter moves the
  cursor to Join. The name is saved to dhd.ini (`name`);
- RACE ROOM: the code, the track (host: left/right, sent with net_set_room), the rider
  (left/right, net_pick, saved to dhd.ini `rider`), up to four players with host mark, READY and
  ping, and the host's Start The Race, enabled when two or more are in and all are ready
  (everyone is ready on entering, as with DHD_NET_*). The footer says what is awaited ("Friends
  join with the code ...", "Waiting for the host to start"). Triangle leaves the room;
- what is needed is checked before connecting: a server (dhd.ini `server` / DHD_NET_SERVER; none
  is built in), the native renderer, and the USA ISO: its SHA-256 is worked out in a thread when
  the page opens and cached in iso-check.txt beside dhd.exe in Online.ps1's format
  ("size:ticks:hash", ticks .NET's), so the launcher and the game share it;
- when the room's race starts the page closes (`hle_online_from_menu`): online.c's
  online_mode() is on from then, the connection and the lobby steps (settings, pick, ready) are
  skipped, and the barrier, the menu script from the main menu's SINGLE PLAYER, the setup block,
  the riders and the start together run as with DHD_NET_*. pad_refresh (just-in-time frames)
  checks online mode every field, and a new DHD_NAV script starts from its first step.
The server needed nothing new: /v1/lobbies is in online/server and deployed.

**Pause menu results** (build/port/r37 pause-*, pausey-*; race on Alpine, START at GO+200):
RESUME goes on with the race; OPTIONS (Vibration, Stereo Sound, Music / Speech / SFX Volume)
opens and Triangle goes back to the pause menu; TRICK LIST shows "Tricks landed this race";
NEW LEVEL opens Select Next Level and Cross loads the chosen track (countdown, race runs);
RE-START and QUIT ask "Are you sure?": No goes back, Yes restarts the race (countdown 3-2-1) or
goes to the title screen (and on to the attract demo); MUSIC PLAYLIST opens the sets and shows
the chosen set's five tracks. No crash on any row.

**Gate status** (gate_native_renderer.py, reference build/port/compare/r27-defined-native):
from 19981960 on the gate fails, the same way in both arms: exact and native still match each
other at all six moments and on every readback (1,144-1,146), but at GO+0/+2/+12/+16 both differ
from the reference. On 66130152 (and cb8e185b) the 65 words are, all explained:
- 6 words 0x0029D854-0x0029D904: the CONTROLS fix above (the reference has the raw PS2
  addresses the crash came from);
- 38 words: the view descriptors D_0027ECC0 that the online merge stopped relocating (data.py
  NUMBERS, finding 041) and state the game derives from them (0x0027F4C0, 0x0029E288, 0x005287D0,
  0x0062FC74-0x0062FE78, 0x00660080/0190, 0x0075AE84, 0x0075C904-CAF4). Proof: build a99ce03b
  (723d8d1e with only NUMBERS removed, not for main) loses exactly these 38;
- 9 words rider +0x535C, slots 1-9: the w lane the online merge zeroed (patches.py,
  001A4F70.c: host stack bytes before, never read back); slot 0's word was already excluded
  as run-varying (0x002B461C);
- 9 words 0x00761A24-44: a log string buffer, whose last IRX line changed with the multitap
  RPCs;
- 3 words 0x004F4234/4254/4324 (new between 723d8d1e and 66130152, most likely ONLINE's
  fourth main menu row): the upper halves of three UV words in the text sprite buffer
  D_004F3AD0 (context 0), which the race's text never rewrites and the GS ignores; so "EE
  state unchanged" above holds for the menu's own state, not for this leftover.
The late moments (GO+3600, +4500) were never equal to this reference (58 / 1,744 words on the
passing 449ce49b) and grow by the same sets. The reference predates intended fixes; the
exclusions are unchanged. A new reference from a current build is the owner's decision.

## Round 38: the memory card, the movie crash, the gate reference

**Memory card** (port/hle/mc.c). The IOP's mcserv RPC server (0x80000400), answering the game's
own libmc (Sony libmc 2.x, recompiled: 0x00245E78 sceMcInit .. 0x00246AB0 sceMcFormat) at the
RPC level, so the library's own code (sceMcSync, the read-alignment end function) runs as on the
console. The function numbers are XMCSERV's (ps2sdk `ee/rpc/memorycard/src/libmc.c`, read only
for the numbers and semantics; no code taken); the send/receive layouts were read from the
library's asm and are listed at the top of mc.c: init (0xFE, versions 0x20A/0x20E), get info,
open (flags 0x40 = mkdir), close, read, write, get dir, format, slot max (0x15). Each card is a
folder beside dhd.exe: `saves\card1` is slot 1 (always in), `saves\card2` slot 2 (in once the
folder exists); a card file is a host file of the same path (the game writes
`BASCUS-97177\{icon.sys, listicon.ico, BASCUS-97177, pathfile.dat}`, 277 KB). get info says -1
(new card) the first time, then 0; free space is 8,000 KB less what the folder holds. dhd.ini
`memory_card = 0` / `DHD_MEMCARD=0` leaves the server unwritten as before (no card). The race,
gate, bench and online tools set `DHD_MEMCARD=0`, so their runs keep the no-card route.

Tested on clean build 1499a7c4 (build/port/r38ui: save1, load1, diff2, diff3): with an empty card
the boot's "MEMORY CARD not found" screen is gone; SAVE GAME DATA shows "Now saving game data to
MEMORY CARD slot 1" and writes the four files; a second save asks "Overwrite Game Data?". The game
loads its save by itself at boot. Round trip: DIFFICULTY set to HARDCORE and saved (one byte of
BASCUS-97177 changes, offset 72308: 1 -> 2), the game restarted, and the DIFFICULTY page opens on
HARDCORE. LOAD GAME DATA asks "Load Game Data?". Music volume is not in the save (the file is
unchanged after a volume change and a save). Not tested: the game's own save prompts after races
("Do you want to save?"), which use the same calls. Online: each PC now loads its player's save
(unlocks, difficulty); the AI riders still come from the host (finding 047).

**Movies: still skipped.** Played with DHD_MOVIES=1, the first movie crashed in
sceMpegAddStrCallback (func_0024C630, reading 0x43): its caller func_0023C1D0 passes five register
arguments that its source doesn't name (already noted in finding 021). patches.py now passes them
on. The crash stays: the MPEG object's work area pointer (`mp+0x40`, set by sceMpegCreate
func_0024CF48 from the work buffer D_0029DB1C = 0x01EBD6D0, a raw PS2 address) leads to a callback
table read at -1 + 0x44. Past that, nothing can show a picture: the port has no IPU (MPEG-2
macroblock decode, IDCT, colour conversion, its FIFO and DMA channels 3/4), the link still stubs
functions the movie path may reach (func_0024FFD8 in libmpeg, kCopy, kFindAddress, the `_ri*`
labels) and movie audio needs its own path. What playback needs: (1) the libmpeg create path checked
under DHD_MOVIES=1 until the stream callbacks run; (2) an IPU: PCSX2's IPU (pcsx2/IPU/, GPL-3, with
its mpeg2lib from libmpeg2) behind the port's DMA, or a native MPEG-2 decoder (libmpeg2, GPL-2)
replacing sceMpegGetPicture with the game's demux callbacks; (3) the decoded RGB32 frames uploaded
as the game expects, and the PSS audio stream to 989snd. Several days of work; `skip_movies` stays
gone until movies can play.

**Gate reference re-recorded.** gate_native_renderer.py's default reference is now
`build/port/compare/r38-reviewed`: the exact arm of the gate run on clean build 1499a7c4 (all
code of round 38's pacing commits c54bef4c, cb8e185b, b00c9174, the round 37 menu work and the
memory card; 638b1164 after it only adds documents), run build/port/r38ui/gate-1499a7c4. Why:
its 65 words against r27-defined-native at GO+0/+2/+12/+16 are exactly the 37 runs reviewed
above (round 37), each from an intended change; nothing else differs. The check and its
exclusions are unchanged; the manifest lists the run, the exe hash, the old reference and every
changed word. gate_hw_renderer.py and gate_gs_optimization.py also compare menu and race images
and keep r27-defined-native until their own reference is recorded.
