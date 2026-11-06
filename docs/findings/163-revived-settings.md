# 163: REVIVED SETTINGS: tabs, a clearer Controls page, online name size

Status: in progress

Branch `feat/revived-settings` from `main` 9ecb8e1, started 2026-11-06. This file is the lane's
plan while it runs and its finding when it merges.

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

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
