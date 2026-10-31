# 118: The 0.7.4 online flow in the game: room rules, back to the room, quick race

0.7.3's rooms raced once and then offered a rematch from the results screen. In 0.7.4 a room
lives on between races (the server side is finding 119, the wire is "Session rooms (0.7.4)" in
`docs/online-protocol.md`). This finding is the game's side: the race room, the race's pause
menu, the banner, and how a player gets from the race back into the room without leaving it.
The flow was agreed on a mock-up first (`flow-074-v2`, kept locally); "Then" defaults to NEXT
TRACK in public and private rooms.

## The room (port/hle/online_menu.c)

- A ROOM RULES row sits under the host settings, with its summary under it ("60 S - AUTO 30 S -
  NEXT TRACK", " *" while a change waits for the next race). Cross opens the rules overlay:
  Finish Window 30 S / 60 S / 2 MIN / NO LIMIT (60), Next Race AUTO 30 S / HOST STARTS, Then
  SAME / NEXT / RANDOM TRACK, Mode RACE (greyed, the only mode). Guests can open it and read
  it; only the host's left/right change a rule (`host_service_net_rules`).
- The host sends the track list once per room (`host_service_net_rotation`, the track ids in
  the room's order), so NEXT and RANDOM pick from the same tracks the room shows.
- The room follows the server's phase: open, intermission (NEXT RACE IN), starting (RACE STARTS
  IN), racing (RACE LIVE, then RACE ENDS IN once someone finished). The top bar counts in green
  (countdown), lilac (intermission) or blue (finish window).
- Player cards carry a phase tag (RACING, READY NEXT, FINISHED, OUT, STARTING / NEXT RACE) and a
  result column: the live result while a race runs, else the last race's, else JOINED LATE. The
  list head reads RACE RESULTS - LIVE, LAST RACE - track, or PLAYERS. There is no results screen.
- Notes over the room: "X FINISHED 1ST", "X IS OUT (time up / gave up / left)", "X IS THE HOST
  NOW" (the server hands the room to the player there longest), "START CANCELLED: NOBODY WAS
  READY".
- READY is not offered to a player still racing. START NOW (host) needs a ready player and no
  countdown; the server starts with the ready players, and the others read "YOU WEREN'T READY: READY NOW TO
  RACE THE ONE AFTER". With nothing counting, the status line says what the room waits for
  ("WAITING FOR A RIDER", "WAITING FOR THE HOST TO START THE NEXT RACE").

## The race (port/hle/online.c)

- A finish is sent as soon as this PC's rider finishes (`host_service_net_finish(ticks,
  place)`, ticks in 1/60 s, place 1-based).
- The banner under the HUD (`race_banner`, from `hle_online_hud`): "ANNA FINISHED - RACE ENDS
  IN 0:52" (the first finisher by time, or "2 FINISHED"), "- NO TIME LIMIT" without a window,
  the last 10 s as a large number, then "TIME UP - DNF".
- Leaving the race keeps the room. When this PC is out of the race (finished, the window ran
  out, or the host ended it) `state_sync` waits 2 s with the reason on screen ("TIME UP - DNF",
  "THE HOST ENDED THE RACE"; a finisher goes after 3 s), then leaves the race screen
  (`leave_race`). `race_left` then calls `back_to_room`: `host_service_net_back()`, the
  puppets and the shared pacing off, the online race state cleared, but the connection kept;
  `port_online_back_to_room()` makes the online shell reopen the room over the rider screen.
  LEAVE THE ROOM still ends with `race_over()` (disconnect, as before).
- How the race screen is left without the player: the race-frame loop (`func_001B40A0`) runs
  until its end timer `D_004B1F40` (`func_001B45F8(timer, code, ticks)`, polled by
  `func_001B4688`) fires, and returns the code: 1 the results (the finish, `func_0019F9B0`, which
  online holds back), 2 the pause (START, `func_001B4900`, which also stores the pausing pad in
  `D_003F52D0.x130`), 0/4/5/6 others. Menus only open outside that loop, so after a finish
  neither `func_0015E0C0` (QUIT's Yes, the builder of `D_00388E90`) nor pushing a menu
  (`func_00190660`) does anything: the race sat on "1ST CONGRATULATIONS!". `leave_race` does what
  START does (pad 0, code 2), and the pause menu's builder, seeing `quit_on_pause`, calls
  `func_0015E0C0` at once instead of adding rows, as `D_00388E90`'s builder does. A player with
  the pause menu open leaves when they resume (the loop ends again at once).

## The pause menu online (port/patches_online_flow.py)

In an online room the race's pause menu has RESUME / BACK TO THE ROOM / END THE RACE (host only)
/ OPTIONS / LEAVE THE ROOM instead of NEW LEVEL / RE-START / QUIT.

- The builder `func_0015E238` asks `port_online_pause_rows(menu)` first. RESUME and OPTIONS
  are the game's own rows (string ids 0x19 and 0x48, `func_0018E3A8`); the room's rows are plain
  strings (`func_0018E408`). Offline it returns 0 and the menu is retail's.
- The accept `func_0015E448` lets `port_online_pause_accept(index)` take the room's rows; RESUME
  and OPTIONS fall through to the game's own handling.
- The room's rows confirm in the game's quit question (`D_00387F90`: builder `func_0015DE40`,
  accept `func_0015DED0`). `port_online_question` sets its title and footer (MenuItem +0x438 /
  +0x448): "BACK TO THE ROOM?" / "YOU WON'T FINISH THIS RACE (DNF)" (or "THE OTHERS ARE STILL
  RACING" after a finish), "END THE RACE FOR EVERYONE?" / "NO RESULTS FROM THIS RACE WILL COUNT",
  "LEAVE THE ROOM?" / "YOU LEAVE THE ROOM AND THIS RACE". Its Yes runs
  `port_online_question_yes` (END sends `host_service_net_end_race`, LEAVE marks the room to be
  left) before the game's own `D_00388E90`, which quits the race screen.

The 0.7.3 rematch path (the restart lifecycle patches in `port/patches.py`, the guest pad, the
`--next` option of `port/tools/online_race.py`, `hle_puppet_rematch`) is gone; the puppets'
reset at a race start is now `hle_puppet_start`.

## The online menu

- QUICK RACE (the first row after the name) fetches the public list, which the server sorts by
  players, and joins the first room of this version with a free place, racing or not: a player
  who joins mid-race waits in the room for the next one. With none, it says so and offers
  "Create A Public Race".
- PUBLIC RACES lists racing and full rooms too, with what each does now: "RACING 1:12", "ENDS IN
  0:43", "NEXT IN 0:18", "STARTING", "OPEN", "FULL", "OTHER VERSION". The row's text is written
  again every frame so the times count; full rooms and other versions are greyed.

## Checked

Windows x64 builds of this branch (with finding 119's server and finding 120's counter merged)
against the session server under local Wrangler, the game started without focus, nettool as
the other players. `nettool --races` now says its riders, that it loaded, and as host the
countdown's start, as a game does, so a game doesn't wait 30-75 s for it; `host --public` lists
its room.

- Private room, this PC hosting and finishing at 12 s (`DHD_NET_TEST_FINISH`): the banner read
  "ANNA FINISHED - RACE ENDS IN 0:18", the finish screen, back in the room 3 s later without
  leaving it. The room showed LAST RACE - ALPINE (1ST 0:12.03, 2ND 0:30.00), the track moved on
  to MOAB (NEXT TRACK) and HOST STARTS (private rooms' default).
- Public room: NEXT RACE IN 0:30 counting, STARTING with "YOU WEREN'T READY", RACE LIVE while
  the bot raced alone, "ANNA LEFT", NEXT RACE on JUNGLE, then "WAITING FOR A RIDER".
- QUICK RACE: skipped a room of another version, joined the room of this one as a guest,
  raced; the host finished, the window closed and the guest left with "TIME UP - DNF". With no
  room, it offered "Create A Public Race".
- FPS & Ping on, in the room: "60 FPS / 1 MS PING" at the top right of the presented picture.

The pause menu and the host's hand-over, by keys posted to a game on a desktop that is never
shown (`DHD_TEST_INPUT`), with the local server and nettool:
- The pause menu opens on RESUME with RESUME / BACK TO THE ROOM / END THE RACE / OPTIONS /
  LEAVE THE ROOM (the host's). Each room row asks first: "BACK TO THE ROOM?" / "YOU WON'T FINISH
  THIS RACE (DNF)", "END THE RACE FOR EVERYONE?" / "NO RESULTS FROM THIS RACE WILL COUNT",
  "LEAVE THE ROOM?" / "YOU LEAVE THE ROOM AND THIS RACE". No goes back to the pause menu.
- BACK TO THE ROOM: in the room as "DNF GAVE UP", "YOU'RE OUT OF THIS RACE: WAITING FOR BOB",
  the room's race still live. END THE RACE: "THE HOST ENDED THE RACE", both back in the room,
  the track kept (a void race doesn't move the rotation). LEAVE THE ROOM: out of the room, as
  QUIT was (the title screen).
- QUICK RACE into a bot's public room as a guest; the bot left: "HOSTBOT LEFT", "YOU ARE THE
  HOST NOW", the settings' arrows and START on.
- The server reloaded under the room (Wrangler's reload, as a deploy does): both players back
  as themselves, the game still the host (finding 119: a restart's ghosts).

Two things this found, fixed in the session server (finding 119): a player back in the room
during a race showed READY NEXT from the Ready that put them in it (Square then took it away),
and the race's close dropped a Ready pressed meanwhile; and a restart left every member as a
ghost, so the games reconnecting filled the room ("The room is full") under a ghost host.

Tests on this PC: a controller connected to it reaches a game on a hidden desktop too (SDL's
background joystick events, gamepad.c). The driver sets `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0`
(the environment beats the game's hint); `DHD_SDL=0` would also turn off `DHD_TEST_INPUT`'s keys.

Not run here: a PC<->Mac race, which a release with online changes needs.
