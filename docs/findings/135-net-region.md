# 135: where the player and the room are, in dhd.log and in the room list, and rooms made near their creator

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

Tests (`npm test`): 72, the 64 old ones unchanged plus `test/region.test.ts`:
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

## Checks

- `online/server`: `npm test` 71/71.
- `port/net/build_tests.py`: test_hash, test_json, test_loopback and test_session pass, and
  nettool builds. test_session adds the welcome's fields, the `where` regions kept across `room`
  snapshots, the leave line and an older server's welcome.
- `node --test port/web/check_net.mjs`: passes.
- The game (`build_wasm.py -j 4`, `build.py --no-core`). Two games on hidden desktops
  (`hidden_game.Game`, a scratch driver: `local_server.py`, `DHD_NET_SCRIPTED`, field-timed pads as
  in `cross_race.py`, `DHD_LAUNCHER=1`, `DHD_MEMCARD=1` with empty folders). The host made a public
  room, the guest took Quick Race into it, both stayed about 100 s without readying, then each left
  by Triangle > Leave. The lines above are from that run.
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
