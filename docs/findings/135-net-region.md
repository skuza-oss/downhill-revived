# 135: where the player and the room are, in dhd.log and in the room list, and rooms made near their creator

Status: current
Checked: 2026-11-06

Since 0.7.12 (finding 158) race frames go over Cloudflare Realtime SFU from each player's nearest
Cloudflare location, not through the room, so where the room runs matters for the lobby, pings and
race setup, not for the riders' snapshots. The rest below still holds.

Why: a European player reported about 300 ms ping. Their 0.7.6 dhd.log had no network data at
all: it showed a join into a public room that was already racing, then two private rooms made and
left. A room is a Cloudflare Durable Object (`online/server/src/index.ts`:
`env.ROOM.get(env.ROOM.idFromName(code))`). Cloudflare places it near the location the first
request came through. A room made by a player on another continent, or through an ISP that routes
to a far Cloudflare location, gives Europeans a long ping, and nothing in the log could show it.

Branch `feat/net-region` (from next/0.7.7). Three parts: the server places new rooms by their
creator's region and says where everyone is (works for 0.7.6 players as soon as it is deployed);
the game writes `net:` lines in dhd.log; the game shows each room's region.

## The server

`online/server/src/region.ts` (pure functions, unit-tested) and small hooks in `index.ts`,
`room.ts` and `lobbies.ts`.

- **Location hint for new rooms.** `/v1/rooms/new` passes `locationHint` to the room's first
  `get()`, chosen from the creator's `request.cf.continent` and `country`, so Cloudflare's
  geolocation decides, not the location their ISP reached. Joining by code keeps
  `idFromName(code)` with no hint, because the object already exists and only the first `get()`
  reads a hint.
  Cloudflare's list (developers.cloudflare.com/durable-objects/reference/data-location/, read
  2026-11-02) is `wnam enam sam weur eeur apac apac-ne apac-se oc afr me`. `apac-ne` and `apac-se`
  were added in June 2026. `sam`, `afr` and `me` host no Durable Objects yet: Cloudflare puts those
  rooms in the nearest region that does (South America's go to eastern North America).
  The mapping:
  - Europe: `weur` for the Atlantic side (GB IE FR ES PT NL BE LU IS AD MC GI IM JE GG FO), `eeur`
    for the rest. This split follows where the Durable Objects actually run
    (where.durableobjects.live, 2026-11-02): weur is AMS LHR CDG MAD MRS LIS; eeur is FRA MXP WAW
    ARN OTP VIE PRG. So Germany, Italy, the Nordics and everything east of them are nearer eeur.
  - North America: `wnam` west of longitude -100 (`request.cf.longitude`). Without a longitude,
    the US state or Canadian province (`regionCode`) decides, and Mexico counts as west.
  - Asia: `apac-ne` for JP KR KP TW HK MO CN MN; `apac-se` for SG MY ID TH VN PH KH LA MM BN TL;
    `me` for the Gulf and Levant countries, which Cloudflare puts in continent AS; `apac` for the
    rest.
  - South America `sam`, Oceania `oc`, Africa `afr`. Unknown or Tor (`T1`) gets no hint (the
    default placement).
- **Where the room runs.** Inside the object, `request.cf.colo` is the client's edge, not the
  object's location. So the room fetches `https://cloudflare.com/cdn-cgi/trace` once, in the
  background, and keeps the `colo=` it answers with (the object's own egress). This is the usual
  way to find a Durable Object's location; where.durableobjects.live is the reference the
  Cloudflare docs link. Rooms made before this code learn their colo on their next hello.
  `COLO_PROBE` overrides the URL (`off` disables it; the tests set `off`). The room keeps
  `origin = {hint, createdIn, createdFrom}` (the hint, the creator's region and their edge colo)
  and `colo`.
- **Region**, the short name players see: the room's colo's region when the colo is in the table
  of Durable Object locations (EU: the 13 above; NA: DFW SJC LAX SEA DEN ATL EWR MIA IAD ORD; ASIA:
  SIN HKG NRT KIX ICN; OC: SYD BNE MEL). Otherwise the hint's region (`EU NA SA ASIA OC AF ME`),
  and `""` when neither is known.
- **New wire data** (docs/online-protocol.md, "Region"). Everything is additive, so `MIN_VERSION`
  stays 0.7.6:
  - `welcome` gets `edgeColo`, the player's own `country` and `continent`, `roomColo` and
    `roomRegion`.
  - A new message `where` carries `{region, colo, members: [{id, region}]}`. It is sent after
    every join and when the room learns its colo. It is a separate message because adding a field
    per member to `room` snapshots made the ten-player worst case 4223 bytes, over the protocol's
    4096 (session test "snapshot stays below 4KB"). Room snapshots are unchanged.
  - `/v1/lobbies` gets the asker's `region`, and each room gets `region`, `colo` and `createdIn`.
  - **Nobody learns another player's country** (the lead's review): rooms are public and players
    post their logs on GitHub. Other members are shown only by their region (`regionOfPlace`, the
    same `EU NA SA ASIA OC AF ME` as the list). A player's own country goes only into their own
    welcome and their own log. No address is ever sent. The usage stats keep the country as before,
    server side only.
- **The list's order.** Rooms made in the asker's region come first, then rooms that run in it,
  then the rest, each part busiest first. "Made in" comes first because the hint puts the asker's
  rooms where it puts everyone else's from that region: a Brazilian's rooms run in North America
  but are still the near ones for another Brazilian.

**Quick Race: server side** (decided). 0.7.6 reads only the first 8 listed rooms
(`LIST_MAX`), and its Quick Race joins the first one with space, so ordering on the server makes
0.7.6 players prefer near rooms the day the server is deployed. The new game keeps "first joinable"
and logs the chosen room's region.

Tests (`npm test`): 73, the 64 old ones unchanged plus `test/region.test.ts` (the list's `now` too):
- the hint mapping (24 places);
- regions and the colo table;
- reading a trace;
- the list order;
- welcome and `where` fields with a fake `request.cf` (Poland host, Ohio guest);
- no message either one receives (other than their own welcome), and not the public list, carries
  a `country` key or the other's country code;
- a room whose colo is set to FRA shows EU even with a `wnam`-style creator;
- the list served to Poland and to Ohio in opposite orders.

The 4 KB session test now runs with every member's region, a colo and a hint stored, and still
passes, because they are not in `room`.

## dhd.log: the `net:` lines

`port/net/client.c` (host side, the network thread, so the same code serves every native platform
and nettool). Nothing is written per frame. A line is formatted under the client's lock and written
after it is released, because dhd.log is a pipe.

- **On joining** (each welcome; `back in` after a lobby reconnect): the Cloudflare location this
  PC reached, its own country and continent, and where the room runs. An older server's welcome has no
  `edgeColo`, and the line says so.
- **When the room's colo becomes known** (a `where` with a new colo): one line.
- **Every 30 s in a room or race** (`NET_LOG_MS`; the network thread's tick): the median and worst
  of this PC's own pings since the last line, and how many. These are all its pings: 1 s apart in
  the room, 0.5 s in a race. Then every other player's ping as the server measures it, with host
  and region (never their country).
- **On leaving** (`net_leave`): how long this PC was in the room, and the last window. **On a
  drop**: the close code, whether it rejoins, and the last window.

Examples from the check below (a local server: `wrangler dev` gives a real Cloudflare `cf`, and
this PC's came out as MRS / DZ; the room's trace answered ORN):

    net: in room G5GWM3 (host): this PC reached Cloudflare MRS from DZ (AF); the room runs in AF (location not known yet)
    net: room G5GWM3 runs in ORN (AF)
    net: room G5GWM3, last 30 s: ping 1 ms median, 1 worst (30 pings); players: GUESTB (AF) 9 ms
    net: left room G5GWM3 after 1:51; last 21 s: ping 1 ms median, 1 worst (21 pings); players: GUESTB (AF) 8 ms

    online menu: quick race: room G5GWM3 (1/10, open, region AF)
    net: in room G5GWM3: this PC reached Cloudflare MRS from DZ (AF); the room runs in ORN (AF)
    net: room G5GWM3, last 30 s: ping 1 ms median, 3 worst (29 pings); players: HOSTA (host, AF) 9 ms
    net: left room G5GWM3 after 1:21; last 21 s: ping 1 ms median, 1 worst (21 pings); no other players

The country in "from DZ" is this PC's own and appears only in its own log; the other player shows
as their region (`AF`). An earlier run of this branch, before the review, logged `GUESTB (DZ)`
there.

From the loopback test, a drop and an older server:

    net: room TEST22: connection closed (1006); last 2 s: ping 141 ms median, 141 worst (2 pings); players: P0 (host) 20 ms, P1 50 ms, P2 90 ms
    net: in room TEST22: the server sent no locations (an older server)

On the deployed server the European player's report would read something like
`this PC reached Cloudflare FRA from DE (EU); the room runs in IAD (NA)`, and the 30 s lines would
show the 300 ms and who else had it.

The structs shared by the core and the host get fields appended:
- `NetMember.region`;
- `NetLobby.edge_colo`, `country`, `continent`, `room_colo` and `room_region`;
- `NetLobbyInfo.region` and `colo`.

`port/web/www/net_layout.js` is regenerated (`gen_net_layout.py --check`: four targets pass). The
browser's `net.js` fills the new fields from `welcome` and `where` (`check_net.mjs`), but the
browser writes no dhd.log.

## In the game

- **Public Races**: each row shows the room's region after its name, e.g.
  `ANNA'S ROOM  EU  2/10  ALPINE  OPEN`.
- **The room**: the card header reads `PLAYERS 2/10 - EU SERVER` (or `LAST RACE - ALPINE - EU
  SERVER`). The live results header in a race is unchanged, because it is full.
- **nettool list** prints each room's region and colo.

Seen in the checks: the guest's room capture shows `PLAYERS 2/10 - AF SERVER` with `1/2 READY`
beside it, with room to spare. A third short run, with the guest opening Public Races instead of
Quick Race, shows the row `HOSTA  AF  1/10  ALPINE  OPEN`. The page widens to fit its rows, so the
longest rows were not tried: a 14-letter name, `ASIA`, a long track and `ENDS IN 1:00`.

## The public list's times in no room

A player's 0.7.6 screenshot of PUBLIC RACES showed `HEINRICHPY 6/10 CITY ENDS IN 29893031:32`.
The row counts `finishEndsAt` (server epoch ms) down from `net_server_ms()`, which is
`net_now_ms()` plus the client's clock offset. On the list page there is no room yet, so no ping
had set the offset: the difference was about the Unix epoch in minutes. `NEXT IN` (`nextRaceAt`)
and the `RACING m:ss` time so far had the same fault.

- **Server**: `GET /v1/lobbies` adds `now`, the server's clock in ms (test: within the request's
  time).
- **Client**: `client_read_lobbies` (`port/net/client.c`; it replaces the parsing in `net.c` and
  runs under the API lock) sets the clock offset to `now - (sent + got) / 2` when no room's pong has
  given a better one. Each row gets `timed` (`NetLobbyInfo`, appended) when the clock is known: the
  list's `now`, or a room's welcome or pong. The browser does the same in `net.js`.
- **The row text**: moved to `port/net/lobby_row.h` (`net_lobby_now`), shared by the menu and the
  net tests. A time is shown only on a `timed` row and only when it is under an hour. Otherwise the
  row says `RACING` or `NEXT RACE` with no time, so an older server's list (no `now`) or a bad
  clock can't show a wild number.
- **Net test** (`test_session`): a list with `now`, read with no room joined, gives
  `HEINRICHPY ENDS IN 0:42`, `NEXT IN 0:20` and `RACING 2:05`. Without `now` it gives `RACING` and
  `NEXT RACE`. An epoch-sized difference never shows, even marked timed.
- **In the game** (one capture, taken before the lead's new process stopped game runs): a game in
  no room opened Public Races while a public room raced, its finish window running (the host
  riding, a nettool bot finished at 5 s). The row read `HEINRICHPY  AF  2/10  ALPINE  ENDS IN 0:31`.

**0.7.6 clients keep the wrong number until they update.** They ignore `now`, and the server can't
correct the times for them without changing what `finishEndsAt` means, which the room page and the
race also read.

## Checks

- `online/server`: `npm test` 73/73.
- `port/net/build_tests.py`: test_hash, test_json, test_loopback and test_session pass, and
  nettool builds. test_session adds the welcome's fields, the `where` regions kept across `room`
  snapshots, the leave line and an older server's welcome.
- `node --test port/web/check_net.mjs`: passes.
- `gen_net_layout.py --check`: four targets pass.
- The game (`build_wasm.py`, `build.py --no-core`; compiled after every change). Two games on hidden desktops
  (`hidden_game.Game`, a scratch driver: `local_server.py`, `DHD_NET_SCRIPTED`, field-timed pads as
  in `cross_race.py`, `DHD_LAUNCHER=1`, `DHD_MEMCARD=1` with empty folders). The host made a public
  room, the guest took Quick Race into it, both stayed about 100 s without readying, then each left
  by Triangle > Leave. The lines above are from that run (the last one after the region change).
- 0.7.6 against the new server: see "0.7.6 clients" below.

## 0.7.6 clients

Checked: two 0.7.6 release games (`DownhillRevived-0.7.6\dhd.exe`), same driver, against this
branch's server. The host made a public room, the guest's Quick Race found it and joined, both sat
in it about 50 s, both left. There were no errors in either log and none in the server's log:

    online menu: creating a room (public)
    online menu: room BU54C9 (host), version dhd-port 0.7.6 82bc8f51d9bc22db
    online menu: quick race: room BU54C9 (1/10, open)
    online menu: room BU54C9, version dhd-port 0.7.6 82bc8f51d9bc22db
    online menu: leave asked (Triangle)

What 0.7.6 players get as soon as the server is deployed:
- rooms made near their creator (the hint);
- a public list in their region's order, so their Quick Race and the first 8 rows they see favour
  near rooms;
- the usage stats unchanged.

They ignore the new welcome fields and the `where` message: the native client reads by key and
drops unknown types, and the browser copies known keys only. What needs the new game:
- the `net:` lines;
- the region in the list rows and the room header;
- the region in the quick race log line.

## Checks the lead still has to run (merged build)

From 2026-11-02 03:40 lanes run no game checks; these are for the merged build. A local server
(`port/tools/local_server.py`) is enough for 1-4.

1. **Log lines, two games** (any track; nobody needs to race). Game A: ONLINE > Create A Public
   Race, pick a rider, stay in the room. Game B: ONLINE > Quick Race. Stay about 70 s, then press
   Triangle in the room, Down to LEAVE, Cross, on both. In each dhd.log look for:
   - `net: in room XXXXXX ...: this PC reached Cloudflare <colo> from <own country> (<continent>); the room runs in ...`
   - `net: room XXXXXX runs in <colo> (<region>)` (a moment after; once per room)
   - `net: room XXXXXX, last 30 s: ping N ms median, M worst (K pings); players: <name> (host, <REGION>) N ms`.
     The other player shows as a region (EU, NA, AF...), **never a two-letter country**.
   - `net: left room XXXXXX after m:ss; last N s: ...`
   - B's `online menu: quick race: room XXXXXX (1/10, open, region <REGION>)`.
2. **Room header**: in A's room the card header above the players reads `PLAYERS 2/10 - <REGION>
   SERVER`, with `1/2 READY` on the right and not overlapping.
3. **Public Races row and its clock**: race in A's public room (A readies; a second player or a
   nettool bot with `--pick 3 --players 2 --ready-after 3 --finish-after 5` readies and finishes
   early, so the finish window runs while A still rides). On a third game in no room, ONLINE >
   Public Races. The row reads `<name>  <REGION>  2/10  <track>  ENDS IN 0:ss`, counting down
   sanely, never a 7-digit number. Before anyone finishes it reads `RACING m:ss`; between races,
   `NEXT IN m:ss`.
4. **0.7.6 on the new server**: two 0.7.6 games (Create A Public Race / Quick Race) still create,
   join and leave with no errors. Their list rows still show the old wrong times (expected, see
   above).
5. **After deploying** (only the real server can show these): from a European connection, Create A
   Private Race, then check the `net:` lines say `the room runs in <European colo> (EU)`, and that
   `/v1/lobbies` shows the room with `region` EU. If possible, repeat over a VPN in another
   continent and check its room lands there. `MIN_VERSION` stays 0.7.6.

## What only the deployed server can show

- Whether `locationHint` moves rooms as intended. Hints are best effort, and local `wrangler dev`
  ignores them. After deploying, create a room from Europe (and over a VPN from elsewhere), then
  read the `net:` lines or `/v1/lobbies`' `colo`.
- Real edge and room colos. Local dev's `cf` is the developer machine's (here MRS / DZ, probably
  through a VPN), and the trace answers from the same machine.
- Whether the colo table still matches where Durable Objects run. A colo not in it falls back to
  the hint's region, so the label stays right as long as the hint was honoured.

## Not done

- Rows at their longest in Public Races were not photographed (see above).
- No in-game choice of region and no automatic move of a far room; the player sees the region and
  chooses.
- The race's live results header has no region.
- A cross-platform race (`cross_race.py`) was not run. The client changes are shared C with no
  platform part.
