# 122: Watch the rest of a room race after finishing

A room finisher stays on the race screen. The retail eight-second results countdown stays
held. Three seconds after the finish, view 0 watches the best-placed human still racing.
Left and Right, or the left stick, cycle through unfinished humans and computer riders in
place order. A press switches once; holding it does not repeat. The race loop's pause flag
(`D_00398C40 +0x5BE4`) blocks switching while a menu is open.

A watched finisher stays on screen for three seconds. A departed rider is skipped at once.
Automatic selection picks the best remaining human. Without one, the view returns to this
PC's rider. A room closes when no human is racing, even if computer riders remain.
Two seconds after the server closes the race, this PC returns to the room and its results.
BACK TO THE ROOM and LEAVE THE ROOM keep their existing pause-menu actions.
Races started by DHD_NET_CREATE / DHD_NET_JOIN keep their existing finish flow.

## Camera

View 0 is `D_0037EA60`, a 0x380-byte object. Its rider is at +0x2B4 (EE 0x37ED14),
its mode at +0x2B8. `func_001487D8(view, rider)` assigns a rider and updates that rider's
view ownership flags (+0x7984/+0x7988), but can refuse a switch during a crash or respawn.
Spectating changes only the view pointer. It preserves those ownership flags so the local
rider's simulation and HUD routing stay local.

`func_00148560(view, 1)` selects normal follow. `func_00148860` places the view from the
new rider's body transform, copies the position into its scene node and resets the follow
history. During a cut, +0x358 is temporarily zero: this chooses the rider-relative
branch instead of reusing the old view node. Its normal value is then restored. This makes a cut. The override runs before `func_00148BE8`, so later gameplay
camera state changes cannot leave the spectator in another camera mode. Pause mode 5 is
left alone. The same hook now implements DHD_PUPPET_CAMERA; puppet physics no longer has
its own pointer write.

The +0xC0 and +0xA10 rider links in include/camera.h belong to a different object:
RiderCamera, at rider +0x3530. View 0's +0xC0 is a direction vector. Writing those offsets
as rider pointers into view 0 would corrupt it. These links are left alone.

The countdown enters mode 8 and the normal update changes it to mode 1. The finish handler
`func_0019F9B0` starts the results timer and a HUD notification; it does not reassign view 0.
Spectating explicitly restores mode 1 before each view update if another mode was selected.

## HUD and host

After the local finish, the normal view-0 HUD is hidden as a whole. Its speed, place,
boost, power-ups, trick text and finish notification describe the local rider. Showing them
beside another rider would mislead. The online room banner stays, including the last ten
seconds and TIME UP. WATCHING name and LEFT / RIGHT - CHANGE RIDER identify the view.
The text uses ASCII letters and punctuation supported by the font (finding 050); no arrow
glyphs. The watched human's overhead name is omitted. Other human name labels remain.

The finish does not stop the race clock or change race phase 3. Holding the results timer
keeps the game loop, AI, snapshots, hit events and finish_step running. Successful sends
count the owned AI snapshots separately and log the count while finished. check_left now
checks every quarter-second of wall time rather than every fifteen render frames.

room.ts needs no change. inRace excludes only departed race slots; finishing does not
remove a member. anyRacing ignores finished and DNF results. The relay still reaches a
finished member until back or race close. AUTO intermission starts at closeRace.
No online research or external code was needed: these facts come from the matching game
sources, the existing findings and the room server.

## Checks

Server tests: 3 files, 51 tests pass. Hidden-desktop room races against two nettool bots
(ANNA finishes at 35 s, BRAD at 45 s; this PC hosts with computer riders on and finishes at 15 s):

- The finish shows the game's own 1ST, CONGRATULATIONS and the time, with this PC's HUD.
- Three seconds later: `online: watching BRAD (slot 2)`, `camera cut slot 2, mode 1 -> 1`.
  The view is behind BRAD at once, with WATCHING BRAD and the Left / Right line; the
  speedometer and place are gone, the room banner still counts down.
- A scripted Right: `online: watching Woody (AI) (slot 9)`, again a cut.
- While watching, the host's computer-rider snapshots keep going out, 140 every 5 s
  (`online: watching: 2135 ... 3255 owned AI snapshots sent, phase 3`). The phase stays 3.
- The race closes after the bots finish: `online: race closed: back to the room`, then the room
  with the results.
- BACK TO THE ROOM from the pause menu while watching: `online: pause menu: back to the room`,
  back at once; the race goes on for the others.

An earlier build hid the HUD from the finish itself, so the finish place never showed. The HUD now
hides only once the camera has left this PC's rider (`on.watched`).
