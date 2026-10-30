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

## Checks

- `clang -fsyntax-only` (wasm32-wasip1, the module's flags) on online_menu.c: no new warnings.
- Not built or run in the game (the core is the main session's build). To try: two games on a
  local server, the first Quick Race creates a public room, the second's joins it.
