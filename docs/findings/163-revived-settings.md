# 163: REVIVED SETTINGS: tabs, a clearer Controls page, online name size

Status: current
Checked: 2026-11-06

Branch `feat/revived-settings` from `main` 9ecb8e1, started 2026-11-06. Merged into `main` at
4d419ed on 2026-11-06; released in 0.7.12.

## Goal and acceptance

The owner tried the in-game settings on Windows (2026-11-06) and asked for these changes:

1. **Name and place.** PC SETTINGS becomes **REVIVED SETTINGS** and moves to the **top** of the
   OPTIONS menu (the front end's OPTIONS screen, `port/patches.py`'s patch of `func_001F0160`, item
   9 today). On Android the page keeps its name SETTINGS. Every mention of "PC SETTINGS" in
   players' text (docs/PLAYING.md, port/release/README.txt, ANDROID-NOTES.md, the launcher's
   Settings page text, hints) follows.
2. **Tabs.** The page has too many rows. One screen with a tab bar at the top: **Display,
   Graphics, Controls, Online, Game** (the lane may adjust the split if a tab would be empty or
   absurd, and records why), switched with **L1/R1** (whatever keys and buttons are bound to them:
   finding 157), shown in the legend with the bound inputs' icons or keys. Up/down move within the
   tab; the existing rules stay (left/right change a row; Cross keeps and closes; Triangle undoes
   and closes; "*" marks next-start rows; hosts' DHD_PC_SETTINGS_ROWS hide rows, and a tab whose
   rows are all hidden is not shown). Touch Controls (Android) and the Controls screen go under
   Controls.
3. **The Controls screen (finding 157) made obvious.** What confused the owner: how to change a
   binding, how to save or leave, and the main vs alternate slots.
   - **The game edits one slot per PS2 button: the main.** The row shows the main binding; the
     alternate, if any, is shown dimmed as text ("also: X") and is changed in the launcher only.
     No left/right slot moving in the game.
   - **Changing:** select a row and press Cross: a clear prompt replaces the hint, "Press a key or
     button for CROSS (Esc cancels)", with the 5 s timeout counting down. The new binding shows at
     once with "Saved" (or why it was refused / what moved).
   - **Saving and leaving:** every change is saved at once (say so on the screen: "Changes are saved
     as you make them"); Triangle (or Esc) goes back. A legend line always shows what each button
     does on this screen: `✕ Change   □ Clear   △ Back` (plus the device switch's L1/R1 if it uses
     them), with the right icons for the pad in use.
   - Keep Swap Sticks, Invert and Reset, at the end of the list, each saying what it does.
4. **Online player names.** A new setting `names` (dhd.ini, shared settings table, the launcher's
   Settings too): **Off / Small / Medium / Large**, **default Small**, which is smaller than today's
   size; **Medium is today's size**, Large bigger. Off draws no names (`port/hle/online.c`,
   `hle_online_names`). It lives in the Online tab. Online state sync must not change: names are
   drawing only.

Unchanged: game state (boot and race state traces equal to the base build), the launcher's
existing Bindings block, the settings' dhd.ini keys apart from the new `names`.

Acceptance: the page and the Controls screen captured on the hidden desktop (Windows) in each
tab, with a rebinding done through the new flow, kept local (never committed); names at
each size in an online race against a local server and a bot (docs/TESTING.md, the online tests);
`check.py port` and `check.py server` pass; state traces equal; docs updated.

## Progress

- [x] 2026-11-06: finding reserved
- [x] 2026-11-06: tabs, main-slot Controls flow and shared names setting implemented
- [x] 2026-11-06: unit tests, full port/server checks, hidden-desktop captures and state traces

## Decisions

- Display holds window mode/size, picture, widescreen, rate, V-Sync and FPS & Ping;
  Graphics holds resolution, scaling, textures and font; Controls holds vibration, dead zone,
  icons, Controls and Touch Controls; Online holds player name size; Game holds movies and unlocks.
- Tabs use the mapped PS2 L1/R1 bits from the host, with bound input words in the legend.
  Tabs can also be tapped or clicked, keeping them usable on phones.
  Empty tabs follow the host row mask. Controls subpages keep their immediate-save behavior.
- Names use 75%, 100% and 125% of the old font metrics. Off returns before touching the pen.
  The setting affects drawing only, with no protocol or server changes.
- Research: SDL3's [button enum](https://wiki.libsdl.org/SDL3/SDL_GamepadButton) confirms the
  shoulder inputs by position. Existing host bindings already translate these to PS2 L1/R1;
  no external code was copied.

## Surprises

- The first core build exposed a missing portable string-compare include in online.c; added it
  before the final build. The baseline boot and race traces completed (600 and 5000 fields).

## Result

Implemented and checked on Windows, 2026-11-06. REVIVED SETTINGS is the first OPTIONS row;
Android keeps SETTINGS. The five tabs and the Controls flow are described above. The launcher
keeps its Bindings block and gets the shared `names` choice in Online. Player docs, new dhd.ini
headers and browser help use the new name.

Validation:

- `cargo test -p dhd_settings`: 20 tests passed, including the names choices/default, generated
  C table and browser corpus. `node port/launcher/test_controls.js` passed.
- The new guest `settings-tabs` unit test (in `test_runtime.py`) covers hidden rows, ordered tabs,
  the Controls subpages, no visible tabs, and switching past either end.
- `python tools/check.py port -j 6`: all steps passed on the final code (core, runtime, host,
  network tests, renderer tests and wasm2c chunks). `python tools/check.py server`: 94 tests
  and the type check passed. No server or protocol changes.
- State traces against the base code (9ecb8e1): boot 600/600 and race 5000/5000 fields equal.
  Final core id: `eab088198abdcebc`.
- Hidden desktop: OPTIONS, every tab, Controls, its countdown prompt, Saved and the return flow.
  Cross changed to J, saved as `keyboard = cross:j,x`; J opens Controls without a restart.
  The alternate stays dimmed and is not selected with left/right.
- Off, Small, Medium and Large captured in online races against a local server and a snapshot
  bot. The Large capture uses a bot following the local rider so its label stays in view;
  the other sizes use a bot echoing a host AI. Off has no label, and the three sizes are visible.
  Captures stay in `DHD_SCRATCH/revived-settings/menus` and `names-off`, `names-small`,
  `names-medium`, `names-large` there, never in git. PNGs were decoded completely for validation.

No acceptance work remains.
