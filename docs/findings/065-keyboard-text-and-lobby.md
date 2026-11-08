# 065: Keyboard text entry and online selection flow

Status: partly superseded by 099 (the WM_CHAR text path)
Checked: 2026-11-04

Text entry now goes through SDL3's text input on the SDL thread (`port/hle/text_input.c`, a
locked queue the game thread reads; finding 099), not the Win32 window's WM_CHAR. The rules
(capitals, field alphabet and lengths, Backspace/Enter/Esc, Ctrl+V) are the same. The room flow
here was reworked later (findings 092, 115, 118, 128).

2026-10-18. Implementation and final clean-build/default-gate acceptance complete.
All game-derived captures stay local in ignored `build/port/r65/`.

## Player-visible changes and choices

Online NAME and CODE accept physical keyboard typing (then through the
window thread's TranslateMessage/WM_CHAR path; now SDL3 text input). The OS translates the keyboard
layout. Lowercase maps to the game's capitals; the existing field alphabet and
length limits remain (12-character name, six-character code). Backspace deletes,
Enter confirms, Esc discards the edit, Ctrl+V appends allowed clipboard text.
The original controller picker, sounds and SharedMenu widgets still work.

During text entry arrows retain picker navigation; other keyboard pad mappings
are suppressed. Held typing keys must be released before acting as pad buttons
after closing the field. Focus loss clears queued input. A bounded, locked queue
passes characters from window thread to game thread, which alone edits fields.
Outside text fields, keyboard controls are unchanged.

Original-game audit: finding 050 maps the shell/SharedMenu systems but identifies
no original editable name field. func_001F3B98 selects new/continue career by
rider. The save/load flow in 00187438.c / func_001895A0 constructs slot names from
rider/mode/level and numeric IDs. These are selection dialogs, not text editors.
Original menu strcpy/strlen call sites revealed no career/profile/save-name
editor. The two online fields are the editable player text found in this build.

Creating/joining opens the original rider carousel, bike/jersey shop and colour
chooser, with animated 3D previews and original legend bars. The host uses the
original track selector. Guests pass through the level chooser automatically
and load the host's track at start. Career/mode selection is skipped because
online supports single races. Unlocks remain the player's own.

GET READY returns to a SharedMenu RACE ROOM: code, occupancy, track, names,
readiness/ping, waiting count, explicit READY, Change Selection, host Start and
Leave. READY initially has focus. The host starts when all present players are
ready, with at least two players. Empty capacity does not stop two friends from
racing. Manual start lets the host review the room before launching.

Earlier members reserve their rider; a later duplicate cannot READY and gets a
change-selection prompt. This preserves the existing unique-rider setup without
silently replacing the previewed choice. Change Selection clears readiness and
waits for its server acknowledgement before reopening the retail screens. If a
concurrent start was already accepted it takes precedence. Host track changes
clear guest readiness.

The room waits for the ordinary rider transition before opening its modal. It
restores the projection saved on entering ONLINE and disables stock shop camera
mode with func_001F67F0(0), as title setup does. SharedMenu freezes animation, so
changing only a camera target inside the popup left the view in the helmet. The
final repeat confirms the normal shell backdrop and READY focus.

## Synchronisation, protocol and release

hle_online_selected_ride retains the exact rider and three-word outfit/bike
records. Online setup reapplies these after constructing setup, then sends them
through existing EVENT_SETUP. Selected riders bypass the old reroll. The grid
and race-over hooks from finding 054 remain intact. Offline the new shell hook
returns before touching EE state. These are host/build patches, not new retail
function decompilation claims.

No wire-format/server changes: protocol v1 / EVENT v2 remain. Existing executable
hash identity in hello.version refuses mixed builds cleanly. **Raise MIN_VERSION
at release to the release containing this work.** Nothing was deployed/published.

## Tests and live evidence

- `python port/tools/test_text_input.py`: PASS (filter/caps/bounds/Backspace,
  confirm/cancel and queue lifetime).
- `python port/net/build_tests.py`: PASS (lockstep, JSON, loopback);
  `build/port/r65/net-tests.log`.
- `online/server` `npm test`: PASS, 34 tests in two files, actual subprocess exit
  0; `build/port/r65/server-tests-final.log`.
- Two real instances on local wrangler dev, 127.0.0.1:8791. Focused SendInput
  physical keys supplied all interactive input; no injected WM_CHAR or menu RAM
  writes. X/S/A/Q/Z/digits typed without pad actions. Backspace/Enter/Esc worked.
  Ctrl+V pasted a code; lowercase typing plus Enter joined a real room.
- French layout 0000040C loaded on the actual game window (GetKeyboardLayout
  0x040C040C). Physical US Q scan 0x10 typed A. US layout restored. Final name
  XSAQZ1A includes that test character.
- Retail rider, Entry Outfit 2 and bike-colour previews visibly changed. Waiting
  counts 2 -> 1 -> 0, both READY, host Start enabled. Change Selection cleared a
  ready player; duplicate Cosmo blocked READY, Mai allowed it. Guest leave
  changed occupancy 2/4 -> 1/4; typed rejoin worked.
- Both PCs raced with Cosmo outfit [0,0,1], bike [8,3,0], and Mai outfit [1,0,0],
  bike [8,3,0]. Read-only setup captures agree, with local/remote slots reversed;
  all eight AI triples agree. Final repaired-camera repeat FPMTD2 at clocks
  854/833: camera4-rides.json / guest4-rides.json. Both retain the finding 054
  `online grid kept (no random start swap)` log.
- After-race test used existing DHD_NET_TEST_FINISH=10. Both actual results show
  HOST2 10.03 then GUEST2 10.06. Real Down/Cross selected NEW LEVEL, Right/Cross
  selected the next track, both logged `the race is over; back to offline play`
  and `puppet: offline`. Enter skipped the next offline flyby. Finding 054
  section 7 control restoration passed.

These are two local game instances, not a four-PC latency benchmark. First room
GO skew was 147.4 ms on this busy shared PC. Initial harness Alt focusing dropped
a character, and concurrent focused scripts fought focus; both harness mistakes
were removed and affected checks repeated serially. PrintWindow captures avoid
foreign windows occluding the game. Live checks used strict incremental links;
plain-clean acceptance is recorded separately below.

## Local screenshots

Paths relative to `build/port/r65/`; no screenshots are tracked.

| Screen | File |
| --- | --- |
| Name with AZERTY character | camera4-name-keyboard-azerty.png |
| Esc cancels name | camera4-name-cancelled.png |
| Pasted code, final clean executable | clean-final-code-final.png |
| Saved name, final clean executable | clean-final-name-saved.png |
| Typed code | guest2-code-typed.png |
| Rider preview | guest4-rider-preview.png |
| Jersey preview | camera4-outfit-preview.png |
| Bike colour | guest-bike-colour.png, guest-bike-colour-alt.png |
| Host track selector | camera4-host-track-select.png |
| Repaired room backdrop | camera4-room-camera-disabled.png |
| Waiting for guest | camera4-waiting-guest.png |
| All ready, host can start | camera4-all-ready-final.png |
| Guest READY | guest4-ready-final.png |
| Duplicate rider blocked | guest2-duplicate-rider.png |
| Guest left | host2-guest-left.png |
| Results | host2-results.png, guest2-results.png |
| NEW LEVEL selected | host2-results-new-level-selected.png |
| After-race track chooser | host2-after-race-levels.png, guest2-after-race-levels.png |
| Next offline race | host2-offline-next-race.png, guest2-offline-next-race.png |

The final clean code-entry capture shows the shortened
`TYPE/CTRL+V - ENTER JOIN - ESC BACK` footer fully inside the legend bar.

## Clean acceptance and commits

Part 1 plain clean c22335b2 compiled all 3,797 game units and linked:
`build/port/clean/c22335b2/dhd.exe`, SHA-256 prefix a904d9cc3702d29c.
Final plain clean pinned to 719ea8a2 passed: all 3,797 game units compiled with
zero errors (212 seconds), host link exit 0 (476 seconds), no recomp-basis option.
Executable: `build/port/clean/719ea8a2/dhd.exe`.
SHA-256: `3fe65eb385707ef757adec864c757c093292ddef0c174271de551e469aeb6abb`.
Recipe log: `build/port/r65/clean-acceptance.log`; manifest beside the executable.
Unchanged default r46-plain gate **MATCH**: all 14 EE comparisons, all six paired
moments through GO+4,500, and 1,094 readback records / Z reads through virtual GS
field 9,015. Both late capture fields are 7,715. No reference, exclusions, gate
options or checker changes. Proof: `build/port/r65/gate-719ea8a2/result.json` and
`build/port/r65/gate-719ea8a2.log`. Its result is PASS, failures is empty, native
renderer use is verified. Only owned test game/server PID families were stopped.

Final clean executable smoke: real SendInput name typing/Backspace, loaded
French AZERTY physical-Q-to-A, Enter save, Ctrl+V code and Esc cancel all passed
again on this exact executable. `clean-final-name-saved.png` and
`clean-final-code-final.png` show the saved XSAQZ1A and FPMTD2 code respectively.
No task-owned dhd.exe processes remain.

Commits, all with the requested co-author trailer:
cbc8324e keyboard; c22335b2 restore host line endings after editing;
86304d61 retail selection/readiness; a040909f handoff polish/live notes;
6dcf4f71 camera transition and acknowledgement; 2d405bb6 stock camera mode;
719ea8a2 footer width.

Those changes were preserved, not authored for this task. Subsequent commits use explicit
`git commit --only` paths to avoid other staged work.

## Sources

- No third-party implementation copied.

## RACE ROOM wording polish (2026-10-18)

Commit 87755924 changes only online_menu.c. The ready-count row now says
`Waiting for 1 more player` / `Waiting for N more players`; at zero it says
`Everyone is ready` for the host and `Waiting for the host to start` for guests.
The count still requires at least two present players, so a lone ready host
correctly waits for one player. Empty member rows are blank. Ready players get
a waiting footer, and the all-ready footer reflects the host/guest role.
The existing guest-only `The host starts the race` row remains disabled;
physical Down skips it from the readiness action directly to Leave Room.
Status rows are at most 28 characters for the four-player room, and revised
footers at most 37, below the approximately 45-character limit.

Two real incremental clients (same executable hash, local Wrangler 4.151.0 dev
server at 127.0.0.1:8791) created/joined a private room using physical SendInput,
selected different retail riders, and readied one at a time. PrintWindow captures
were visually checked at 640x480: plural waiting before READY, singular waiting,
all-ready host and guest, disabled guest start skipped by Down, and guest leave
returning the host to 1/4 with no dash in any empty slot. Rows and footers fit.
The computer-use native pipe was unavailable; this repeats the existing local
finding-065 harness. Only this task's recorded game process families were stopped.

Local captures in `build/port/r65/`:
- `polish-host-confirm.png`: two unready players, plural waiting.
- `polish-host-waiting-one.png`: one ready player, singular waiting.
- `polish-host-all-ready.png`: host everyone-ready row and start footer.
- `polish-guest-all-ready.png`: guest waiting-for-host row and footer.
- `polish-guest-start-skipped.png`: guest focus skips disabled start row.
- `polish-host-empty-slot.png`: guest left, three blank slots, 1/4 occupancy.

No protocol/server changes or third-party code. Nothing deployed or published.

Plain clean acceptance: `python port/tools/build_clean.py --rev 87755924
--tree build/port/r65/polish-clean-tree` compiled all 3,797 game units with
zero errors (121 seconds), then linked successfully (651 seconds). No recomp
basis. Executable: `build/port/clean/87755924/dhd.exe`. SHA-256:
`ea825b98672babaa5c53cb4d2e57ff1d2feb5f084058e61478ec6cfe5ef52d04`.
Recipe log: `build/port/r65/polish-clean.log`; manifest beside the executable.

Unchanged default r46-plain renderer gate **MATCH** on that clean executable: 14 EE
comparisons, all six paired checkpoints through GO+4,500, and 1,096 readback records / Z
reads through virtual GS field 9,017. Both final capture fields are 7,715. Result PASS,
failures empty, native renderer use verified. Proof:
`build/port/r65/gate-87755924/result.json` and `build/port/r65/polish-gate.log`. No
reference/exclusion/checker changes or nondefault gate options. All task-owned lobby
game/server processes stopped; the gate completed and cleaned up its own runs. No
screenshots or game data were staged.
## Online back/cancel lifecycle repair (2026-10-18)

The popup manager restores the view descriptor only when it began with multiple views
(func_001B4AF8/001B4CD0); the shell has one. Online pages now save and restore view 0's camera
binding, render mode and projection around every modal run, including public-list/code/name
cancellation and repeated entry.

The selection hook previously returned to menu 15 by writing its ID alone. That bypassed the rider
Back cleanup: shop camera/object visibility, camera animation, rider labels, fades and sound ducking
remained live. Completing or cancelling a selection now runs the stock rider Back cleanup, disables
the shop camera, restores the shell projection, and opens the room on the title scene. It no longer
starts another rider transition just to open a freezing popup. Stock Back within the
outfit/bike/colour screens remains available; cancelling all the way out returns to the connected
room, unready. Connection failure while a picker is active follows the same cleanup and displays the
failure.

Leaving or failing a room clears selection/readiness requests; a new room starts fresh. Exiting
online after selection restores the pre-online mode/submode and player count and reinitialises the
main menu with ONLINE focused. No protocol, server, matching-source or renderer changes; no
third-party code copied. Sources: original shell func_001F24B8/001F67F0, popup manager
func_001B4AF8/001B4CD0, and the existing finding-065 hooks.

Validation: the transformed shell translation unit compiled with zero errors; the modified host menu
compiled for i686 Windows. The repository's local SendInput/PrintWindow harness launched the
previous clean executable and captured its memory-card screen, but injected Back/Start/Cross did not
advance it, so live navigation is not claimed. No full suites, renderer gates, PCSX2 comparisons or
benchmarks were run. Clean committed build is recorded by the manifest beside
build/port/clean/<commit>/dhd.exe; final handoff gives its path.

## Lobby flow rework (2026-10-18)

The room is now a page over the rider screen (menu 3), never over the sky:
- the shop's exit (menu 18) is caught once its camera take D_00275CB0[rider] ends, before
  retail's level chooser (func_00139BF8) takes the camera; the choice is kept, the rider
  screen's Back cleanup runs, the black exit fade is lifted (func_001F79D8(0, 0x3C)) and the
  flight to the rider screen (take 2, rewound to 0 first) plays; the room opens 45 frames
  after menu 3 so the rider model is back;
- Triangle on the rider screen opens the room (held buttons, not func_001F4A98, whose latch
  would eat the screen's own presses); Change Rider closes it on the rider screen and blocks
  that screen until the press is released;
- leaving or losing the room goes to the title and the main menu reopens ONLINE
  (port_online_resume, patched into func_001EF0E0);
- popup pages act only after every button was released since they opened (no leaked X);
- the host changes the track in the room (row 1, left/right or X); Ready is a plain toggle;
  a guest who becomes host keeps the room's settings; without a connection the room says so.
Live-tested with two local instances against `wrangler dev`: create, join by code, rider
conflict, change rider, ready, start, host leave (guest becomes host), server loss.
A test pad (DHD_TEST_PAD names a shared-memory word merged into pad 1) drives the harness.

## Computer riders off, and the host's next race for everyone (2026-10-18)

- The room's options travel in its "laps" byte (bit 0: computer riders off). func_001B89C8's
  AI count is patched through hle_online_ai_count: the other players' puppets only, or at least
  one AI slot per other player. The snapshot, finish and results loops cover 1 + the AI count.
- EVENT_REMATCH (8): the host's next race, [changed track, track]. The host sends it when its
  race phase drops back (to the countdown or the flight) after GO or after the results.
  - After the results a guest's controller waits; on the event its menu presses TRY AGAIN, or
    NEW LEVEL and X on the level list, while the host's track is written to the setup block.
  - During the race a guest's pause menu does the same: D_0029E290 (1 riding, 0 menus),
    D_003F52D0.x150 the open menu (pause 0x38BC00, RE-START question 0x388990, level list
    0x386680), each menu's first word its row; pause rows 3 NEW LEVEL, 4 RE-START.
  - Guests with others in the room can't pick NEW LEVEL or RE-START themselves.
  - Each PC then resets the race's online state and agrees a new start (start_together).
Live-tested with two instances: TRY AGAIN and NEW LEVEL from the results and from the pause
menu; both PCs start within 0.1 ms.

## Everything unlocked online (2026-10-18)

From the room's first rider choice until the player is back offline, every rider, bike and track is
open, as retail's unlock-all code (func_001F0780) sets them: D_004325A0.x2180 riders, x2188 bikes,
x2114 tracks (all but 21 and 23), D_004225A0.x12190 23 flags. The rider screen's pros need one more:
their statues come alive from the save's rider bits D_004225A0.x12110 (bits 0-13, func_00193FE8),
which func_001BB670 copies into the screen's table D_004B25A0.x3C; the port sets the bits and calls
it. Leaving (the room's Leave, a lost room, or the end of the online race) restores the saved words
and calls func_001BB670 again. Meanwhile the memory card's writes are held (mc.c opens NUL for
writing), so the card keeps the player's own progress. Tested: all 16 riders selectable online, a
pro rider shown on the other PC, the player's own unlocks back after leaving.

## Names above online riders (2026-10-18)

`hle_online_names`, called after the local rider's cheat-message text in `func_0012C9E0`, draws the
other connected humans' lobby names. AI, the local rider and departed players have no tag. Text is
larger, white with a black outline; the entire 32-byte pen 0 is restored and its saved colour
re-emitted in all four text contexts, preserving subsequent HUD text.

The axis evidence is `func_001797A0`: retail's rider markers copy the scene node at rider +0x792C,
position +0x10, and add five on Z. The live orientation rows confirm Z is up; negative X moves
toward the camera, not above the head. `func_0012DDC0` and puppet.c confirm puppets update that same
scene node. Names use a lower 3.25-unit Z offset and the projected base's X to avoid the raised
point drifting sideways in a pitched camera. The centred font's widescreen X mapping is inverted for
the anchor, preserving glyph proportions. Behind-camera points, depth beyond 150, off-screen
bases/raised points and labels crossing the view edges are rejected before drawing. No debug log
remains.

Verification: harness build with `UNITS="0012C9E0.c"`, then targeted O2 host rebuilds/relinks of
online.c; no compile/link errors (23 existing SDK stubs). A temporary standalone helper check passes
human/local/offline/gone filters, projection bounds including an off-screen base with an on-screen
raised point, far/behind-camera rejection, widescreen/narrow aspect anchor mapping, and exact
pen/colour restoration. Two live clients on the local relay used Rider Alpha and Rider Bravo. Tags
follow the moving remote riders; the race HUD and pause menu retain their text styling. Only the
test instances' PIDs were stopped. Matching sources, game data and screenshots are not added to this
change. 

## Issue 17: shared online riders (2026-10-29)

The room checked every other member for a matching rider, so both the earlier
member (including the host) and the later member lost Ready. This supersedes
section 2's earlier-member restriction: online players may choose the same rider.
`online_menu.c` removes the conflict, forced unready, cursor diversion and footer;
track/options changes still invalidate readiness and host-only controls retain
their existing host checks.

Source trace: `src/unsorted/001B7198.cpp`'s `func_001B74E8` reads the
setup +0x2A8 table to avoid occupied riders when choosing opponents;
`func_001B7A40` writes an opponent's id to +0x32C and marks its rider type 4.
The picker in `001EB1D8.cpp` writes that table by rider index as well.
This is one classification per rider, not storage for each racing instance.
`func_001B9220` requests NGP assets separately per human/opponent slot;
`func_001B8D10` does the RTX/TEX passes per slot, and `func_001B94B8`
looks up the slot's outfit record. These paths suggest duplicate assets can be
instantiated separately without expanding the per-rider table.

`hle_online_riders` now keeps the local human's entry when a puppet shares
that rider (otherwise 4 would overwrite it; readers only test it for -1); each slot still receives its
own id, outfit and bike. AI choices continue to exclude human rider identities.
`choose_riders` honors valid explicit lobby picks in the scripted path too, so
the harness really exercises duplicates rather than silently choosing a different
rider (launcher picks keep the ten-rider rule, duplicates allowed). Unspecified picks
retain the fallback. Outfits remain the players' picks:
changing them automatically would hide the identical-asset case being tested.
No matching source or retail data was changed. Research used the local matching
source and port implementation; no external code was adapted.

Validation: `python port/tools/build_wasm.py` passed (3,815 game units, 35 port
units, strict link, clean offset scan); `python port/tools/build_host64.py
build/wasm/dhd_core.wasm` passed (132 imports, 37 Windows host objects).
`online_race.py --minimized --a DHD_NET_RIDER=4 --b DHD_NET_RIDER=4`
ran once for its default 420-second budget against localhost:8791. Both logs
confirm Host and Guest ride Kalolo, including each remote slot; identical outfits
and bikes loaded. Both reached GO and sustained snapshot exchange through race
clock ~25,230; the host received two hits and AI finish events arrived on both
clients. Inspected the host g+300 and guest g+900 harness captures: normal rider
and bike rendering, no evident texture corruption. The harness exited 0, but
reported `not every game got the results`: the scripted humans did not finish
within the budget. This validates loading and sustained racing with duplicates,
not human finish/results handling or every rider/outfit combination. Duplicates
remain enabled because this run exposed no duplicate-related engine failure;
there is no evidence here requiring deep engine changes. The room change was
compiled and reviewed logically, not driven through a new menu harness.

Later check: two games through the room flow, both on the same rider; the host could still
READY and START, and both loaded the race.
