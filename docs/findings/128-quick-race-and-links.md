# 128: Quick Race; joining a room from a link

## Quick Race

ONLINE asked a player to pick between Public Races, Create and Join before racing anyone. Quick
Race is now ONLINE's second row (after Name): one choice that finds a race.

1. It reads the public list (`host_service_net_list_lobbies_start/poll`, as Public Races does):
   the server lists only public rooms that have not started and have a free slot.
2. Of those with this build's version (`NetLobbyInfo.version`, the same test Public Races uses
   to grey out other builds) and `players < max_players`, it joins the one with the most
   players, which is the one that starts soonest.
3. None (or no answer from the list): it creates a public room, and the player waits there as
   after Create A Public Race; the room is on the list for the next Quick Race.
4. If the chosen room filled up, started or closed before the join arrived (`room_full`,
   `room_started`, `room_not_found`), it creates a public room instead of stopping on the error.

Triangle gives up while it looks or connects. The footer says "Looking for a race...", then
"Connecting to the server...". Every step goes in dhd.log (`online menu: quick race: ...`). Only
the client changed: no new message, no server change.

ONLINE's rows have names now (`HOME_NAME` ... `HOME_UPDATE`) in place of their numbers, since
the new row moves the ones below it.

## Joining a room from a link: DHD_NET_ROOM

`DHD_NET_ROOM=K7M2QX` in the game's environment, read once at the start (online_menu.c
`join_read`; lower case is taken, anything but a join code is logged and ignored):

- The main menu opens ONLINE by itself, through the same hook that reopens it after a room
  closes (`port_online_resume`, once the title's camera has settled), or ONLINE is chosen by hand
  first; either way ONLINE then joins that room.
- The join waits for the disc's online check (the footer: "Joining room K7M2QX: checking the
  disc first..."), then goes as Join With A Code does, with the same refusals on ONLINE (a full
  or started room, a different build). Triangle gives it up. It is tried once.

The name `DHD_NET_JOIN` was already taken: it is the tools' scripted flow in online.c
(`online_race.py`, nettool tests), which skips the menus and makes the race by itself.

### In the browser

`index.html?room=K7M2QX` puts `DHD_NET_ROOM` into the game's environment (`main.js`, next to
the `?env.NAME` variables, which win if both name it): the player picks their ISO, presses
Start, and the game goes to ONLINE and into the room. A `?room=` that is not a join code is said
in the log and ignored.

The page's menu (`) has **Copy invite link** while the player is in a room: the page's address
with `?room=CODE`. The code reaches the page two ways, neither new to the game:

- a public room's code comes with the game's presence (`host_service_presence`, finding 106),
  which the worker now reads instead of ignoring it, and passes on as a `room` message only when
  it changes;
- a private room's code never leaves the core by itself (presence.c's rule), so the page only
  learns it when the player copies it in the room (Circle, the existing `clipboard` message).

Leaving the room's presence (menus, a race offline) hides the button again.

## Checks

- `clang -fsyntax-only` (wasm32-wasip1, the module's flags) on online_menu.c: no new warnings.
- `node --check` on main.js and worker.js.
- Not built or run in the game or the browser page (`build_wasm.py`, `web/build.py` are the main
  session's). To try: two games on a local server, the first Quick Race creates a public room,
  the second's joins it; then `index.html?room=<code>` in a second browser.
