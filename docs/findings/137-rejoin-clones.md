# 137: rejoining players no longer clone themselves in online rooms

Status: current
Checked: 2026-11-04

Why: GitHub issue #30, "Players are cloning in lobbies". In a room, players at random appear twice
and take up slots. The host can clone too and then loses the host rights. The reporter's 0.7.7
dhd.log is from players in Indonesia in a room that runs in HKG. Their ISP sends them to far
Cloudflare locations (GIG, SJC, WAW, ARN, a different one on each connection), so their pings are
400-5000 ms.

Branch `fix/rejoin-clones` (from main at 0.7.7). Server only; no client change.

## What the log shows

The `net:` lines (finding 135) show the whole story:

- `net: room VFEJ8W: connection closed (1006), rejoining`, then `net: back in room VFEJ8W: this PC
  reached Cloudflare SJC` (it was GIG before). This PC was the host (`net: in room VFEJ8W (host)`).
  27 s later its player list (which leaves out the player's own member) holds `BUSET (host)`.
  That is the log owner's own old member, still in the room and still host. The rejoined copy is a
  new member that is not host.
- In room 3PBT5W the list has `TUKANGAC` and `XANANDRA` twice. One copy of each shows the same
  ping (`367 ms`, `757 ms`) in every line, because nothing answers the room's pings on the dead
  copy and its last figure stays. In VFEJ8W, `TUKANGAC 1112 ms` stays the same through a whole
  race and the room after it. Dead copies stayed for minutes.
- After this PC's own drop in 3PBT5W, it shows up as `BUSET` in its own list: the clone again.

## Cause

The game's rejoin (`port/net/net.c` on_close, `client.c` client_on_closed: up to 5 tries after
1, 2, 4, 8 s) opens a new WebSocket to the same room and sends the same hello as a first join.
The hello has no token, only the name, version and ISO hash.

On the server (`online/server/src/room.ts` onHello), a hello got an existing member back only as a
restart's "ghost": a member with that name **and no socket**. That covered a deploy, which drops
every socket at once. It did not cover a dropped connection. When a player's path breaks (here the
ISP moved them to another Cloudflare location mid-connection), the game notices first: its socket
fails, or it hears nothing for 10 s (`SILENT_MS`) and shuts the TCP connection down. On the
server's side, the old Cloudflare location still holds a connection to the room. Its close
reaches the room only when that location gives up on the dead TCP connection, which takes minutes
or may never happen. Meanwhile the rejoin's hello arrives. The old member still has its socket, so
it is no ghost, and the hello adds a **new member**: the clone. It takes a slot, comes back
un-ready (the game re-sends its pick and Ready after the welcome) and is not host. The old member
keeps the host rights until its late close or the leave. Then `leave()` hands the host to
`members[0]`, the longest-present other player (BUSET in VFEJ8W), not to the rejoined player.

The room had no check for dead sockets. It pinged every 2 s but never looked at whether anything
came back, so a dead member stayed until Cloudflare closed its socket.

## Fix (server)

(Point 1's match by name was replaced on 2026-11-03 by a rejoin token: see "Rejoin token" below.)

1. **A rejoin takes the member back while the old socket is still open.** The room notes when each
   socket last sent anything (`heard`, in memory; a socket not in it counts from the instance's
   start). A live game always sends: in a room, a ping every second and a pong to each of the
   room's pings (every 2 s); in a race, 20 snapshots and 2 pings a second. The browser build does
   the same. A hello under a member's name takes that member back:
   - when the member has no socket (a restart, or the drop in point 2), as before;
   - when the member's socket has sent nothing for `REJOIN_QUIET_MS` (4 s);
   - when that socket spoke less than 4 s ago, the hello waits until it has been quiet 4 s, then
     decides again. If the socket spoke during the wait, it belongs to a live game: **another
     player with the same name**, who joins as a new member as before. Names stay non-unique.
     Messages on the waiting socket are ignored until it is decided, and the hello timeout does
     not count the wait.

   The member keeps its id, slot in the list, host rights, rider, bike and Ready. The old socket
   is forgotten (its attachment is cleared, so its late close changes nothing) and closed with
   1000 `replaced`. An ordinary close code means a game that wasn't gone after all rejoins
   instead of showing a refusal. That rejoin then finds the member speaking on the new socket and
   joins as a new member, so there is no loop. A game never rejoins during its race (it races on
   alone). So in a session room, a member taken back while still in the race is retired from it
   (`left`), and the race closes if nobody is left riding.
2. **A drop keeps the member for its rejoin.** A socket that ends without a close frame (1006, or
   `wasClean` false, or a socket error) outside its member's race keeps the member for
   `REJOIN_GRACE_MS` (20 s) without a socket (`awaySince`): same host, same place in the list. A
   game that leaves sends `leave` first, and a clean close (1000) still removes the member at
   once, as do a refusal and a drop during the member's race (the game won't be back for that
   race; `left` and the DNF as before). Away members are not put on a grid or into a race. The
   alarm removes them when the 20 s are up, and the host then passes on as before.
3. **Dead sockets are dropped.** The alarm (every 2 s) removes a member whose socket has sent
   nothing for 20 s (`disconnect`, as a close would) and closes the socket (1001 `silent`). Before,
   such a member stayed until Cloudflare closed the socket.

The constants: 4 s is four of the game's ping intervals, so a live game always speaks inside it.
The rejoin itself takes at least 1 s of backoff plus the TCP, TLS and WebSocket handshakes, three
or more round trips. At these pings that is several seconds, so the old socket is usually quiet
long enough already when the hello comes and nobody waits. When the old connection breaks
cleanly (a reset), its close usually reaches the room before the hello, and point 2 keeps the
member. The 20 s grace is twice the game's own silence limit (10 s), and long enough for a rejoin
over a 5000 ms path (backoff plus about four round trips). A dead copy now disappears within
20-22 s instead of minutes. `REJOIN_QUIET_MS` can be set in the environment (the tests use 300).

## Which version needs what

(As first shipped. Since the rejoin token below, a rejoin needs the release with the token, and the
name rule is gone.)

- **0.7.7 players are fixed by deploying the server alone.** They already rejoin with the same
  hello, and the server now recognises it. No game update is needed, and the protocol and its
  messages are unchanged (a new close reason, `replaced`, uses close code 1000).
- No client change. A rejoin token (sent in the welcome and returned in the rejoin's hello)
  would make the match exact, with no wait and no reliance on names. One case would then work:
  two players with the same name, where one game is silent for 4 s while still connected, which
  doesn't happen with live games. It isn't needed for this issue. If it is wanted later, the
  server can accept it next to the name rule, so 0.7.7 keeps working.

## Rejoin token (2026-11-03, review fix)

The name rule above let anyone take a member over by name, host rights included (review of
v0.7.7..main, Muse #1 and Opus #13). Names are shown to the whole room and in shared logs. A member
away after a 1006 drop went to the first hello under its name at once; a connected member whose
socket was quiet for 4 s (exactly the bufferbloated path this finding is about) went after the
wait. The real player's rejoin then came back as a new member without host rights, and the theft
looked like an ordinary reconnect.

Branch `fix/review-server`. The member is now matched by a token, not by name:

- The server gives every new member a random token (16 bytes, 32 hex digits;
  `crypto.getRandomValues`) and sends it in that member's `welcome` only. It is kept in the room's
  state (so it survives a restart) and never appears in `room`, `where`, the lobby list or a log.
- The game keeps it for that room (`port/net/client.c`: `NetClient.rejoin_token`, set from the
  welcome, cleared by `client_init` for each new room; only hex is kept, so nothing else goes back
  into JSON) and adds `"rejoinToken"` to the hello of a rejoin.
- A hello whose token belongs to a member takes that member back **at once**, whether its old
  socket has closed or still speaks: only the game holding the token can send it, and that game
  has given its old socket up. The rest is as before: the member keeps id, host, rider, bike and
  Ready, the old socket is forgotten and closed with 1000 `replaced`, a member still in its race
  is retired from it.
- A hello without a token, or with one no member has, is a new member: the existing rule for a
  name already in the room (names stay non-unique). The name no longer takes anything back.

Gone with the name rule: `REJOIN_QUIET_MS`, the hello's wait for a quiet socket (`helloWaiting`)
and its timeout exception. That wait also dropped any message the waiting socket sent while its
comment said they waited (Opus #12); the code and the comment are both gone. What stays: the 20 s
grace for a 1006 drop, the restart ghosts (now taken back by token too), and dropping a socket
silent 20 s, which still uses `heard`.

No fallback for releases without tokens: the release that carries this client raises
`MIN_VERSION` (its core id changes anyway), so a 0.7.7 game is refused before its hello is
matched. Members created by an older server have no token and can't be taken back; their games
are below `MIN_VERSION` after the deploy. The browser build has no online play (finding 143) and
never rejoins, so `port/web/www/net.js` is unchanged.

Tests: `online/server/test/rejoin.test.ts` now has 12 cases. The rejoins of the first cases send
the welcome's token. New: tokens are 32 hex digits, differ per member and are absent from the room
snapshot; a token takes the member back at once (under 250 ms) while the old socket still pings
every 50 ms, and that socket is closed (1000); a same-name hello without a token is a new member
(id 2) even when the member's socket has been quiet 10 s, and the first socket stays open; a wrong
token is a new member, and the real token still gets member 1 back after it; an away host can't
be taken over by name (the impostor is member 2, not host, and its `settings` gets `not_host`),
and the host's token gets the host back. The restart case in `session.test.ts` rejoins with the
tokens. The test `Client` (`test/helpers.ts`) keeps its last welcome. Server tests 92/92, `npm run typecheck`
passes. `port/net/test_session.c` checks the client: no token in a first hello, the welcome's
token in the rejoin's hello, none in the next room's hello, and a non-hex token never sent.

A live rejoin on one PC: this checkout's server (`port/tools/local_server.py`), two `nettool`
players (host ACE, guest BOB), the host's connection through a small TCP proxy (a scratch script,
not committed). At 5 s the proxy reset the host's connection on the game's side and kept its
server-side socket open and silent: the dead path of issue #30. The host's game logged `connection
closed (1006), rejoining`, then `back in room X3A2ET (host)` over a second connection. It was member
0 and host again, its list showed only BOB (no clone), and the race started with two slots (host
slot 0, guest slot 1). The game core is unchanged (the token lives in `port/net/client.c`, a host
file): `dhd_core.wasm` is byte-identical to 60ba1c2's, and the deterministic state trace at 60 of
this branch's `dhd64.exe` matches a 60ba1c2 build's row for row (boot 600, race 5000).

For the lead: the release that ships this must raise `MIN_VERSION` (a 0.7.7 game's hello has no
token, so after the deploy its rejoins come back as new members, the old clones), and the server
deploy goes with it. A real check: in an online room, pull the host PC's network cable for about
5 s and plug it back in. `dhd.log` should show `net: back in room CODE (host)`, and the other
players' lists should hold that player once, still host.

## Tests

`online/server/test/rejoin.test.ts` (new, 8 cases; see the rejoin token above for today's 12):

- a player back before their old socket closed gets member 1 back, with Ready, rider and bike. The
  old socket is closed (1000), and a late 1006 close of it afterwards changes nothing;
- the host back before its old socket closed stays host (welcome `host: true`, `hostId` 0) and can
  change the settings;
- a 1006 drop keeps the member (and the host) through an alarm, and the rejoin gets it back;
- a member not back within the grace leaves, and the host passes on;
- a socket silent for 20 s is dropped and closed (1001) with no close event;
- a player whose game keeps speaking keeps their member; a second player with the same name joins
  as a new member (id 2), and the first socket stays open;
- a rejoin when the old socket has been quiet a while takes the member back without waiting;
- a clean close (1000) still removes the member at once.

The existing size test joined nine silent test clients all named `"ࠀ".repeat(24)`. Silent
same-named clients are now each other's rejoins, so their names now differ in the last character
(same length and bytes). Server tests 81/81 (73 before, 8 new), three runs; `npm run typecheck`
passes. No game runs (server only).
