# Online protocol (version 3)

The wire protocol between the PC port (`port/net/`) and the online server (`online/server/`,
Cloudflare Workers + Durable Objects). The plan behind it is `docs/ONLINE-PLAN.md`.

- One WebSocket per player. Lobby control travels as JSON **text** frames. Race STATE/EVENT use compact
  little-endian binary messages on SFU DataChannels; WebSocket binary PING/PONG remains.
- The server runs no game logic. Race frames use the SFU (state sync: every
  PC runs its own race and shows the others' riders from their snapshots, findings 041 and 044)
  and reports players who leave.
- `PROTOCOL = 3`. It is in `hello`; the endpoint URL remains `/v1/`. Any incompatible change raises
  it; the server refuses clients with another number.

## Endpoints

`BASE` is `wss://<worker host>` (the official server, the client's default:
`wss://dhd.skuza.app`; your own deployment:
`wss://dhd-online.<account>.workers.dev`; local `wrangler dev`: `ws://127.0.0.1:8787`). HTTP endpoints use `https://` / `http://` on the same
host.

| Method | Path | What |
|---|---|---|
| WebSocket | `/v1/rooms/new` | Create a room; this socket becomes its host. |
| WebSocket | `/v1/rooms/<CODE>` | Join the room with that join code (case-insensitive). |
| GET | `/v1/lobbies` | Public lobby list (JSON, below). |
| GET | `/v1/health` | `{"ok":true,"protocol":3,"minVersion":"0.1.0","online":true}` (`online` and `message`: the kill switch, below) |
| GET | `/v1/latest` | The newest release's `latest.json` (the launcher's backup update check; below). |

Join codes are 6 characters from `23456789ABCDEFGHJKMNPQRSTUVWXYZ` (no `0 O 1 I L`). Clients
upper-case what the user types and reject characters outside that alphabet.

The client sends `hello` as its first message, within 10 seconds of connecting. Before `hello`
the server sends nothing.

## Refusals and errors

A refusal is one text frame followed by a close:

```json
{"type":"error","code":"iso_mismatch","message":"Your ISO does not match the host's (US SCUS-97177 expected)."}
```

| `code` | Close code | When |
|---|---|---|
| `bad_message` | 4000 | Unparseable JSON, unknown type, invalid `hello`, anything before `hello`. |
| `protocol_mismatch` | 4001 | `hello.protocol` is not the server's `PROTOCOL` (checked after `client_outdated`, so an old release gets Update Now). |
| `version_mismatch` | 4002 | `hello.version` differs from the host's. |
| `iso_mismatch` | 4003 | `hello.isoSha256` (the disc image's game files hash, finding 098) differs from the host's. |
| `room_full` | 4004 | No free slot. |
| `room_started` | 4005 | The race is running; late joins are not supported. |
| `room_not_found` | 4006 | No room has this code (or it was closed). |
| `kicked` | 4007 | The host removed this player. |
| `hello_timeout` | 4008 | No `hello` within 10 seconds. |
| `client_outdated` | 4009 | `hello.clientVersion` is below the server's `MIN_VERSION` (below). |
| `rate_limited` | 4011 | Too many connections or new rooms from one address in a minute (below). |
| `too_fast` | 4012 | A member sent far more than the game does, a second time (caps, under Limits). The first time it is a non-fatal `too_fast` error. |
| `banned` | 4013 | The operator banned this player's address (refused before any room) or install id (refused at `hello`). |
| `online_off` | 4010 | The operator turned online off (the kill switch, below): a new room, or a `hello` that is not a member's rejoin. Also a non-fatal error to a `start`, and to the room when a countdown runs out. |

### Minimum client version

The server refuses releases older than its `MIN_VERSION` (a Worker variable in
`online/server/wrangler.jsonc`, `MAJOR.MINOR.PATCH`; default `0.1.0`, every release). Since
0.2.0 the client sends its release in `hello.clientVersion` (`port/include/version.h`). A
`hello` without it is 0.1.0, which did not send one; an unparseable one counts as outdated. The
refusal carries both versions:

```json
{"type":"error","code":"client_outdated","message":"A new version is out. Update to play online.",
 "minVersion":"0.2.0","clientVersion":"0.1.0"}
```

0.1.0 knows no `client_outdated`: like any refusal it shows `message` on the ONLINE screen.
0.2.0 and later show the same text and add an **Update Now** row (finding 054).

### The kill switch (finding 161)

The operator turns online off and on without a deploy or a game update (`node tools/moderate.mjs
online off`, a row in the server's moderation database; `online/server/README.md`). The server
reads it at most every 30 s, so a change takes up to that long. While off:

- `/v1/rooms/new` is refused at once, before `hello` and before any room (the one exception to
  "before `hello` the server sends nothing");
- a join by code is refused at `hello`, unless the `hello` is a rejoin of one of the room's own
  members (its `rejoinToken`): rooms that were open stay, their members are not kicked and come
  back after a drop.

The refusal is:

```json
{"type":"error","code":"online_off","message":"Online races are paused. Try again later."}
```

then close 4010. The operator may give another text (`--message`, within about 45 characters, the
ONLINE screen's footer). Every release shows a refusal's `message`, so 0.7.11 and older need
nothing new. `/v1/health` adds `"online":false` and the `message`; `/v1/lobbies` lists no rooms
and adds the same two fields. An open room starts no race: its countdown is dropped and `start`
answers the non-fatal `online_off` error (so does a countdown that runs out, to the whole room). A
race already running goes on to its finish.

### Limits per address (finding 161)

Before a WebSocket reaches a room, the Worker counts it against the connecting address (by its
keyed hash, never the address itself): at most 30 room connections a minute (new rooms, joins and
rejoins together) and 10 new rooms a minute (`ratelimits` in `online/server/wrangler.jsonc`;
Cloudflare's rate limiter, per Cloudflare location and approximate). Over either, the Worker
refuses at once, like the kill switch:

```json
{"type":"error","code":"rate_limited","message":"Too many tries. Wait a minute and retry."}
```

then close 4011. The limits are generous because many players can share one address (a
household, a LAN party, carrier-grade NAT). `/v1/lobbies` and the other HTTP endpoints have no
limit of their own.

### Bans and the moderation log (finding 161)

When a player enters a room (a new member or a rejoin) the server writes a row to its moderation
log: the time, the room code, the member id, the name, `hello.clientVersion`, the country
(Cloudflare's), a keyed hash of the address (HMAC-SHA-256 with a Worker secret; the address
itself is never stored or logged) and `hello.installId`. Rows older than 14 days are deleted
every day. The operator bans a row's address hash and/or install id (with an expiry, or until
lifted) with a local tool; names are never a ban's key. A banned address is refused before any
room, a banned install id at `hello`:

```json
{"type":"error","code":"banned","message":"You can't play online until 2026-11-20."}
```

(or "You can't play online on this server." for a ban without an end), then close 4013. The
server keeps each lookup up to a minute, so a new ban (or a lifted one) takes up to that long to
bite. If the ban list can't be read, players are let in.

No message to any client carries an address, an address hash or an install id: not `welcome`,
`room`, `where`, `start`, the lobby list, errors or refusals (`online/server/test/moderation.test.ts`).

Non-fatal errors (a non-host sending a host command, a `start` while someone is not ready)
use the same `error` frame **without** a close; the client just shows `message`.

## Lobby (JSON text frames)

### Client → server

```jsonc
// First message, required. name: 1-24 characters.
{"type":"hello","protocol":3,"version":"dhd-port 0.2.0 1a2b3c4d5e6f7a8b","clientVersion":"0.2.0",
 "isoSha256":"<64 lowercase hex>","name":"Rider","features":["countdown"]}  // features: optional
// A rejoin (Limits, below) adds the token from this room's welcome: "rejoinToken":"<32 hex>"
// From the release after 0.7.11 (finding 161): "installId":"<32 lowercase hex>", optional (below)

// Host only. Every field optional; omitted fields keep their value.
{"type":"settings","track":3,"laps":2,"seed":12345,"public":true,"maxPlayers":10,"roomName":"Friday race"}

// Rider and bike for a member. Anyone may set their own; the host may set anyone's.
{"type":"pick","member":2,"rider":5,"bike":1}     // "member" omitted = yourself
{"type":"pick","choosing":true}                     // on the rider screen (unreadies); false when back

{"type":"ready","ready":true}
{"type":"kick","member":3}                          // host only
{"type":"start"}                                    // host only; everyone (host included) must be ready
{"type":"pong","s":1734000000123}                   // echo of a server ping, see below
{"type":"ping","t":12345.6}                          // optional client-side ping; the server answers "pong"
{"type":"end"}                                       // host only: race over, back to the lobby
```

- `track`, `laps`, `rider`, `bike`: integers 0..255 whose meaning belongs to the game (track
  table index, rider and bike table indices). `laps` 0 = the track's default.
- `seed`: unsigned 32-bit integer. If the host never sets one, the server draws one at `start`.
- `maxPlayers`: 2..10, default 10. It can't go below the current member count.
- `public`: listed in `/v1/lobbies` while not started and not full. Default `false`
  (private, join code only).
- `installId`: a random id the game makes once and keeps in dhd.ini (`install_id`), the same in
  every hello of that copy of the game, for the server's moderation log and bans (Refusals,
  above). Optional: releases before it send none and join as before; one that is not 32 lowercase
  hex digits counts as none. The server sends it to nobody. The game never writes it to dhd.log.
- `features`: what the client understands beyond version 1. `"countdown"`: the room may start
  the race by itself (below).

### Server → client

```jsonc
// Answer to hello: who you are in this room, and where you and the room are (Region, below).
{"type":"welcome","protocol":3,"code":"K7QX2M","memberId":2,"host":false,"serverTime":1734000000000,
 "edgeColo":"WAW","country":"PL","continent":"EU","roomColo":"FRA","roomRegion":"EU",
 "rejoinToken":"<32 hex>"}  // this member's rejoin token: only this client gets it (Limits, below)

// Where the room runs and each member's region (never their country): after every join and when
// the room learns its colo (Region, below).
{"type":"where","region":"EU","colo":"FRA","members":[{"id":0,"region":"EU"},{"id":2,"region":"NA"}]}

// Full room snapshot. Sent after every change and every ~2 s with fresh ping numbers.
{"type":"room","code":"K7QX2M","hostId":0,"roomName":"Friday race","public":true,"maxPlayers":10,
 "started":false,"version":"dhd-port 0.4.0",
 "settings":{"track":3,"laps":2,"seed":null},
 "members":[{"id":0,"name":"Ann","ready":true,"rider":5,"bike":1,"rttMs":38,"choosing":false},
            {"id":2,"name":"Bo","ready":false,"rider":0,"bike":0,"rttMs":null,"choosing":true}],
 "countdownEndsAt":null,"serverTime":1734000000000}

// Server-timestamped ping, every 2 s while in the lobby. Echo "s" back unchanged in a "pong".
{"type":"ping","s":1734000000123}

// Answer to a client "ping": "t" echoed, serverTime added (use it to estimate the clock offset).
{"type":"pong","t":12345.6,"serverTime":1734000000150}

// Race start, sent to every member at once.
{"type":"start","yourSlot":1,"track":3,"laps":2,"seed":2864434397,
 "players":[{"slot":0,"memberId":0,"name":"Ann","rider":5,"bike":1},
            {"slot":1,"memberId":2,"name":"Bo","rider":0,"bike":0}],
 "serverTime":1734000005000,"startAt":1734000008000}

// A racer left or dropped. The host's game hands their rider to its AI.
{"type":"left","slot":1,"memberId":2,"reason":"disconnect"}   // or "leave"/"kicked"

// Host ended the race; the room is a lobby again (ready flags cleared).
{"type":"ended"}
```

- **Member ids** are fixed for the life of a connection and never reused in a room. **Slots**
  (`0..N-1`) exist only in a race: the server assigns them at `start` in join order (host
  first). All race frames use slots.
- Lobby members who leave just disappear from the next `room`. If the host leaves, the
  longest-present member becomes host.
- `startAt` is server time (ms since the Unix epoch), about 3 s after `start`. Clients map it
  to local time with the offset from `ping`/`pong` (`offset ≈ serverTime - (tSend + rtt/2)`).
  The game itself starts every PC's countdown together on the server's clock once all have
  loaded (port/hle/online.c).

## Race (binary frames, little-endian)

At race start each racing member establishes one native WebRTC association to Cloudflare Realtime
SFU. It is discarded at race end. Each player publishes `state` (unordered, maxRetransmits 0) and
`events` (reliable, ordered), subscribing to the same channels of all other members. Channel
allocations identify members; `start.players` maps members to race slots. The receiver corrects
byte 1 from that mapping. WebSocket binary frames only carry PING/PONG; all other types are dropped.

DataChannel messages add an 8-byte little-endian unsigned prefix containing that race's existing
`start.startAt` value in ms. The original type/slot/payload follows it unchanged. This is a race
identifier, not a game clock or tick agreement. Endpoints discard older races' messages. Early
reliable EVENTs wait in a bounded queue until their matching WebSocket start is delivered; future
STATEs can be dropped. WebSocket callbacks and SFU callbacks are delivered on one network thread,
so the game sees start before buffered events. Channels close at race end.

### SFU signaling (private JSON on the room WebSocket)

- Client sends `{"type":"sfu_offer","sdp":"<its own offer>"}` after start. The DO calls
  `sessions/new` with that offer as `sessionDescription`. The answer is
  `{"type":"sfu_answer","sdp":"<this session's SFU answer>"}`, sent only to its owner.
  The offer includes the SCTP transport; no separate `datachannels/establish` call is used.
- Once WebRTC connects, client sends `{"type":"sfu_connected"}`. The DO publishes the two
  channels, then subscribes each connected member to every other publisher.
- Each endpoint receives only its own allocations, in one or more messages:
  `{"type":"sfu_channels","channels":[{"memberId":2,"name":"state","channelId":10,"local":false}]}`.
  The endpoint creates negotiated channels with these IDs and the delivery policies above.
  `local:true` means its own publication. Session IDs and app credentials stay on the server.
- After all allocated channels open, each client sends `{"type":"sfu_ready"}`. The DO replies
  to all racing members only after every member acknowledges. Until then, race frames stay
  queued locally so reliable events cannot precede a subscriber.
- Departures and replacements send `{"type":"sfu_closed","channelIds":[10,12]}` only to
  affected endpoints, after closing those allocations through the API (a subscription that
  doesn't close is announced all the same and closes with its subscriber's session).
- Teardown goes on past a failed close or revoke. A session's channels or a TURN username that
  did not close is kept in the room's pending cleanup, stored apart from the members (it outlives
  the member and the room), and tried again by the room's alarm: after 5 s, then twice as long
  each time up to every 5 minutes, until it succeeds or its time is up (a credential's 1-hour
  TTL; an hour for a session, as Cloudflare documents no end for a connected DataChannel-only
  session).
- TURN over TLS, for networks that block UDP (finding 158, round 4). The client tries UDP to the
  SFU first. If its association has not connected 2 s after `sfu_answer`, it sends
  `{"type":"sfu_turn"}`. The DO accepts that once per member per race, only from a racer with an
  SFU session in the running race. It mints TURN credentials through Cloudflare's TURN API
  (`generate-ice-servers`, TTL 1 hour, `customIdentifier` `room-<CODE>-m<memberId>`, no personal
  data) and sends them to that member only:
  `{"type":"sfu_turn","urls":["turns:turn.cloudflare.com:5349?transport=tcp","turns:turn.cloudflare.com:443?transport=tcp"],"username":"<hex>","credential":"<hex>"}`
  (`urls`: the `turns:` entries only). The client connects with TLS to the port-443 entry (the
  others are fallbacks), takes a relayed address (TURN Allocate, ChannelBind to the SFU's
  candidates, ChannelData over the stream) and gives it to ICE as a relayed candidate. The SFU
  sees the TURN service's address, never the player's. The DO revokes the credentials when that
  member's SFU session is cleared (race end, retire, leaving); a second `sfu_turn` in the race
  is a `bad_value` error. Clients accept `turns:` servers on `turn.cloudflare.com` only.
- Setup has a deadline: 25 s after the race's `start` (the room's `SFU_SETUP_MS` sets another),
  every racer must have acknowledged `sfu_ready`. Measured setups take 0.2-0.5 s over UDP and
  2.4-3.1 s through TURN; the margin covers ten racers' 90 subscription calls and one API call
  that runs into its timeout, and ends before the game's 30 s wait for the others' rides.
- A racer whose setup fails (an API error on its session, an invalid signaling step) or isn't
  done by the deadline is out of the race, as a racer who leaves it (release review round 2,
  finding 158). It gets `{"type":"error","code":"bad_value","message":"Realtime connection
  failed: out of the race."}`; the other racers get `left` (`reason` `left`, the host's AI takes
  the rider), the subscriptions to it close with `sfu_closed`, and the release is checked again
  without it, so the others go on. It stays in the room: in a race room its member shows
  `inRace` false and `raceState` `dnf` (`dnfReason` `left`), which takes its game back to the
  room (port/net/client.c). No automatic API mutation retries and no second setup in that race:
  a partially successful operation must not allocate duplicate channels.

The Worker reads `REALTIME_APP_ID`, `REALTIME_APP_TOKEN`, `TURN_KEY_ID` and `TURN_KEY_API_TOKEN`
from env/secrets. For local tests they live only in gitignored `.dev.vars`. None is sent to
clients. Client SDP is never broadcast or stored in room messages; SFU answers and TURN
credentials are sent only to their owner. No message carries another player's SDP, candidates,
TURN credentials or address. The native client has no peer signaling or TURN mesh path: a TURN
relay only ever carries that player's own traffic to the SFU.

The API limit is 50 requests/session/second; mutations are serialized, and channels use one
publication batch and one subscription batch per publisher. API operations have a 15 s timeout.
A lost WebRTC connection has a 30 s session reuse window; the game reports failure and rejoins
with a fresh session rather than retaining a broken connection. See finding 158 for live checks.

### PING, type 3 — client → server; PONG, type 4 — server → client

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | `type` = 3 (4 in the answer) |
| 1 | u8 | `slot` |
| 2 | n | any payload, at most 32 bytes (e.g. a u32 sequence and an f64 send time) |

The server answers the sender only, with the same bytes and the type changed to 4. Use it to
watch the round-trip during a race; it is never forwarded.

### STATE, type 5, and EVENT, type 6 — client ↔ SFU ↔ clients

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | `type` = 5 or 6 |
| 1 | u8 | `slot` of the sender |
| 2 | n | payload, the client's format (finding 044) |

State sync: a STATE frame is one rider snapshot (payload version 5, 104-834
bytes). Its timestamp is sender race time in ms since GO. Version 5 repeats each changed pad word in six consecutive snapshots,
and refreshes all 146 words every 30 snapshots, as index/u32 pairs; previous-action words 25-49 are reconstructed locally. Receivers reject older
timestamps before changing the pad mirror. Every owned rider is sent about 30 times/second.
EVENT remains payload version 2. The SFU reads none of the gameplay payload.

`DHD_NET_DROP_STATE=N` deterministically drops N percent of outgoing STATE messages for testing;
EVENT is unaffected. `DHD_NET_SFU_UDP=0` drops every UDP datagram to and from the SFU (a network that
blocks UDP), so the TURN-over-TLS fallback must carry the race. `net:` reports per-sender p50/p95/p99 estimated snapshot age and
gaps longer than 100 ms. Age uses the existing per-sender EVENT_GO server-clock stamp plus the
snapshot race timestamp, so it includes transit and time awaiting the next snapshot. Peer clock
offset error and simulation drift affect the estimate; this is not a precise one-way latency probe.

### Leaving

A racer leaves by closing the socket (or sends `{"type":"leave"}` first, which only changes
`reason`). The server broadcasts `left`; SFU traffic and departure notification have independent ordering. The host's game hands the rider to its AI and sends it as its
own from then on.

### Race room rules (finding 115)

The server keeps what everyone has to agree on:
- **Ready is "count me in for the next race"** (finding 128). A changed `track` keeps it, and so
  do `laps` changes that leave its cheats bit (2, the game's ROOM_CHEATS) as it was (computer
  riders on/off is bit 1). Cheats on or off unreadies everyone outside the running race: it is
  another kind of race. `public` and `roomName` never unready. Before finding 128 any `track` or
  `laps` change unreadied everyone.
- **Riders can be shared.** Two or more members may pick the same rider and all be ready
  (issue #17).
- **Choosing** (`pick` with `"choosing":true`): the member is on the rider screen; it is
  unreadied and `ready` answers `not_ready` until a pick with `"choosing":false`.
- **Countdown.** When at least two members are in the room, all ready, none choosing,
  and every one said `"countdown"` in `hello`, the server sets `countdownEndsAt` (server ms,
  `COUNTDOWN_MS` = 5 s on) and starts the race itself then, as `start` would. Any change that
  breaks the rule clears it. `start` still starts at once. Clients show the time left with
  `serverTime` as the clock offset. Older clients never get a countdown: the host starts.

## Lobby list

`GET /v1/lobbies` → `200 application/json`:

```json
{"protocol":3,"minVersion":"0.1.0","region":"EU","now":1734000000000,"lobbies":[
  {"code":"K7QX2M","roomName":"Friday race","version":"dhd-port 0.2.0 1a2b3c4d5e6f7a8b","track":3,
   "players":2,"maxPlayers":10,"hostName":"Ann","region":"EU","colo":"FRA","createdIn":"EU"}]}
```

`region` at the top is the asker's own; the list comes in their region's order (Region, below).
`now` is the server's clock (ms since the Unix epoch): the list's `raceStartedAt`,
`finishEndsAt` and `nextRaceAt` are on that clock, and a client in no room has no offset to it
from a room's pings. It sets its offset from `now` (half the request's round-trip late at most) and
shows no count without one (finding 135).

Only public rooms that have not started and have a free slot are listed. Clients should
show only rooms whose `version` matches their own.

## Latest release (launcher backup)

`GET /v1/latest` → `200 application/json`: the newest GitHub release's `latest.json`, as
`port/tools/package_release.py` writes it, reduced to what the launcher reads (finding 138):

```json
{"name":"Downhill Revived","version":"0.7.7",
 "url":"https://github.com/skuza-oss/downhill-revived/releases/download/v0.7.7/DownhillRevived-0.7.7-win.zip",
 "sha256":"<64 hex>","size":31457280,"page":"https://github.com/skuza-oss/downhill-revived/releases/tag/v0.7.7",
 "platforms":{"win":{"url":"...","sha256":"<64 hex>","size":31457280},"mac":{...},"linux":{...},"linux-appimage":{...}}}
```

The launcher asks it only when GitHub's releases API fails. The worker fetches
`https://github.com/skuza-oss/downhill-revived/releases/latest/download/latest.json` itself, serves
that copy for 5 minutes, and keeps the last good copy (this isolate's, and 30 days in the edge cache of
each Cloudflare location) for when GitHub fails: such an answer has the header `X-DHD-Stale` (why the
newer copy couldn't be had); `X-DHD-Fetched-At` says when the copy was fetched. A copy is served only
when `version` is `MAJOR.MINOR.PATCH`, `platforms` names at least one platform, and every `url` (the top
level's too) is an `https://` release asset of this repository (`github.com/skuza-oss/downhill-revived/releases/download/...`
only) with a 64-hex `sha256`; `page` is kept only under `https://github.com/skuza-oss/downhill-revived/`.
GitHub's `latest.json` is read with a 64 KB cap (its `Content-Length`, else the stream). No release notes. With no usable answer from GitHub and no copy kept: `503` (GitHub
unreachable or HTTP error) or `502` (its `latest.json` refused),
`{"error":"The newest release's info can't be had right now: ...","detail":"<what GitHub did>"}`.

## Limits

- One room holds at most 10 members.
- Text frames: at most 4 KB. Binary frames on the WebSocket: at most 3072 bytes (only PING
  carries a payload, at most 32 bytes).
- A room with no members is deleted.
- Caps per connection (finding 161, `online/server/src/guard.ts`): at most 100 text frames and
  100 binary frames in each 5 s window (20 a second each; the game sends about 1.5 text and, in
  a race, 2 binary PINGs a second: protocol 3 race frames travel over the SFU, finding 158, and a
  race's Realtime setup is a few text messages). Over a cap the rest of that window's frames of
  that kind are dropped (binary ones not answered);
  the first time the sender also gets the non-fatal
  `{"type":"error","code":"too_fast","message":"Slow down: too many messages."}`, and the next
  window over a cap closes it: `too_fast` "Disconnected: too many messages.", close 4012, and the
  member leaves as a disconnect.
- No reconnect in version 1: a dropped racer is handed to AI for the rest of the race.
- Rejoin (finding 137). Each member has a random rejoin token, sent only in its own `welcome`
  (never in `room` or anywhere else). A client keeps it for that room, and when its connection
  drops it says hello again with `"rejoinToken"`: the member with that token is taken back at
  once (id, host, rider, Ready), whether its old socket has closed yet or not; the old socket is
  closed (1000, `replaced`). A name alone takes no member back: a hello without a token, or with
  one no member has, joins as a new member, as any player whose name is already in the room does
  (names are not unique). A socket that ends without a close frame (1006) outside its race keeps
  its member 20 s for the rejoin (it doesn't race meanwhile); a member whose socket sends nothing
  for 20 s leaves as a disconnect and the socket is closed (1001, `silent`). A clean close (1000)
  or a drop during the member's race leaves at once.
- A server restart (a deploy) drops every socket with no close event. A lobby client says hello
  again with its token and gets its member back; members not back within the hello timeout leave
  as disconnects.

## Retail preview flow (finding 065)

The UI uses retail selection screens before READY. Rider/bike indexes still use
`pick`; the complete outfit and bike records still travel in EVENT_SETUP v2 when
the race loads. There is no new wire message or protocol revision. The host starts
when all present members are ready, with at least two members. Choosing again
clears readiness; a changed track requires guests to ready again. Exact executable
identity in `hello.version` refuses mixed builds. Raise MIN_VERSION to the release
that includes this UI at release time; no deployment is part of this change.

## Session rooms (0.7.4)

A creator opts in with `"features":["countdown","session"]` in hello.
The creator's feature selects the rules for the room's whole lifetime.
Version and ISO matching still apply. Protocol remains 1.
A creator without session keeps all the old rules, including immediate host Start,
no racing joins, and hiding racing or full rooms from the public list.
Binary STATE and EVENT formats do not change. The server runs no game logic.

### Settings and phases

The host sends `settings` with any of the old fields and these optional fields:

| Field | Values | Default |
| --- | --- | --- |
| finishWindow | 0, 30, 60, 120 seconds | 60 |
| nextRace | "auto", "host" | auto if public, host if private |
| then | "same", "next", "random" | next |
| rotation | Array of 0–64 byte track ids | empty |
| trackLimits | Parallel array of 6, 8 or 10; sent with rotation; includes a ten-rider entry | unset |
| pickOnly | Track ids from this message's rotation that NEXT and RANDOM never choose (an arena: MOSH); sent with rotation | empty |

The nextRace default follows public until nextRace is explicitly set.
The client sends the game's track list and limits once. Until then the rotation
is empty and keeps the track. Next walks forward, wrapping; a current track absent from the list
selects its first entry. Random excludes the current track whenever another
track exists. Track ids may repeat; clients should send a unique list.

The snapshot's phase is open, intermission, starting, or racing.
Open means no race yet, host control, or an expired AUTO timer with nobody ready.
Intermission is a 30 second AUTO timer. Starting is the five second countdown.
Racing means a race is running, even for a member waiting in the room.

Ready means "put me in the next race." Two or more members all ready and none
choosing start a countdown. One member alone cannot do this.
Shared rider picks remain allowed (issue #17, finding 115).
The host sends `{"type":"start"}` to count down with the ready members, including
a solo ready member. At least one must be ready and not choosing.
Start during a countdown keeps its original grid and deadline.

The countdown locks the grid. Later Ready waits for the following race.
Unready or choosing drops a member from the grid. They can ready again for the
following race. If the grid empties, the countdown cancels. lastEvent becomes
start_cancelled. An AUTO room returns to a new 30 second intermission; a host
room returns to open. A new countdown clears lastEvent.
At the deadline, only entrants receive start and slots, with an entering host
first, then join order. The existing startAt lead for loading still follows the
countdown. Members outside the grid receive room snapshots and no race frames.
Joins are accepted in every phase up to maxPlayers, counting all members.

### Finishes and return

An entrant sends `{"type":"finish","ticks":N,"place":P}` when its own human rider
crosses the line. N is an unsigned 32-bit race clock at 60 ticks a second.
P is 1–255, the full-field place computed locally, including computer riders.
Only the first finish from that entrant counts. Computer finishes send nothing.

The first human finish sets finishEndsAt to now plus the running finishWindow.
With finishWindow zero the deadline remains null. Later finishes do not move it.
At the deadline all unfinished entrants become DNF time_up and leave the race's frames.
Finished entrants retain their finish and leave them too.

A client sends `{"type":"back"}` when leaving the race scene for the room.
An unfinished entrant becomes DNF gave_up. A finisher keeps its finish.
Its race frames stop. Remaining racers receive `left` with its slot, memberId and
reason finished or gave_up. Timeouts use time_up; disconnects use left.
The client retains the connection and can pick and ready while others race.
Disconnecting or leaving removes membership and records DNF left if unfinished.

The race closes once no unfinished active entrant remains: all entrants have
finished, returned, timed out, or disconnected.
The server sends `{"type":"ended","reason":"done"}`, then room.
A host sends `{"type":"end"}` to void the running race.
The server sends ended with reason void, keeps the previous lastRace and does
not rotate. These messages do not disconnect anyone.

Completed lastRace entrants sort by ticks, then DNFs in grid order.
Their places use reported places, raised to at least the preceding place plus
one. DNFs follow finishers and get successive places.
LastRace stays until another race completes. Its id increments on completion.
Every entrant becomes unready when the race closes. Waiting members keep ready.
A completed race applies the running then rule to the rotation and selects the
next track. A void keeps the selected track.
The running nextRace copy then selects AUTO intermission or host open.

At nextRaceAt, a ready member starts a locked five second countdown.
If nobody is ready the room goes open with waitingForRider true.
The next Ready starts a new 30 second intermission, even with only one ready.
All-ready cannot bypass this intermission.

### Changes and migration

A cheats change (laps bit 2) unreadies members outside the running race; track and other laps
changes keep Ready (finding 128).
A track change during intermission gives at least 15 seconds before nextRaceAt.
A track change during the start countdown locks the grid again: the countdown runs in full from
then, and the new track's limit is checked as at any grid lock.
The host can change settings during a race after finishing or returning.
FinishWindow, nextRace and then are copied when the grid locks. Changes during
countdown or racing alter the next race's settings. rulesPending compares these
settings with the running copy. The running finish deadline and rotation action
use that copy. Rotation itself is the room's current track list.
Host migration always selects the longest-present member. A race continues
after host migration. Only an empty room is destroyed.

### Room snapshot and public list

Session room snapshots keep all old fields and add:

| Field | Meaning |
| --- | --- |
| phase | open, intermission, starting, racing |
| raceStartedAt | startAt of running race, otherwise null |
| finishEndsAt | running finish deadline, otherwise null |
| nextRaceAt | intermission deadline, otherwise null |
| countdownEndsAt | existing countdown deadline, otherwise null |
| serverTime | server epoch milliseconds at snapshot |
| waitingForRider | expired AUTO timer waiting for first Ready |
| lastEvent | start_cancelled, or null |
| rules | finishWindow, nextRace, then for the next race |
| rotation | configured track array |
| rulesPending | current rules differ from the locked copy |
| lastRace | null, or {id, track, entrants} |

Each member adds inRace (still attached to this race scene/relay).
Members with a current race entry add raceState: racing, finished, or dnf;
place; finishTicks for finishers; dnfReason for DNFs.
Members waiting outside the race omit raceState and result fields.
An entrant may be finished while inRace is true until back or race closure.
LastRace entrants carry id (member id), name, rider, place and either ticks or
dnfReason. Disconnected entrants remain in lastRace.
All deadlines are server epoch milliseconds; absent deadlines are null.
Text frames fit 4096 UTF-8 bytes for ten members, including prior results.
Session member and room names replace C0 controls and unpaired UTF-16 surrogates with U+FFFD to
bound JSON escaping. Old rooms retain their existing name handling.

GET /v1/lobbies includes public session rooms while racing and when full.
Their entries add phase, raceStartedAt, finishEndsAt, nextRaceAt and full.
Players counts all members. MaxPlayers is the membership limit.
Entries sort by players descending. Old rooms retain their visibility rules.
Screens filter by their own version before showing entries.

### Game-facing C API

NetLobby, NetMember, NetResult and NetLobbyInfo in port/net/net.h form a shared
core/host ABI. Keep every layout in sync. The browser's generated net_layout.js
comes from `python port/tools/gen_net_layout.py`; `--check` checks the layouts
with clang for wasm32, Windows x64, Linux x64 and macOS arm64.

NetLobby adds phase[16], last_event[32], race_started_at, finish_ends_at,
next_race_at, countdown_ends_at, server_time (double); waiting_for_rider,
rules_pending, finish_window, next_race, then, rotation_count, rotation[64],
last_race_id, last_race_track, nresults and result[8].
The times are server epoch ms; zero means absent. Compare them directly with
host_service_net_server_ms(). The existing countdown_ms remains available.
Next_race is 0 auto or 1 host. Then is 0 same, 1 next or 2 random.

NetMember adds in_race, race_state[12], dnf_reason[16], finish_ticks (signed
64-bit; -1 absent), and place (zero absent).
NetResult has id, rider, place, name[100], dnf_reason[16] and ticks (-1 for DNF).
NetLobbyInfo adds phase[16], race_started_at, finish_ends_at, next_race_at and full.
NET_RACING means this client received start. Waiting and returned clients remain
NET_LOBBY even when the room's phase is racing.

The game calls these imports on every platform, including the browser:

- host_service_net_finish(uint32_t ticks, int place)
- host_service_net_back(void)
- host_service_net_rules(int finish_window, int next_race, int then)
- host_service_net_rotation(uint32_t guest_address, int count), at most 64 bytes
- host_service_net_end_race(void), host void
- host_service_net_server_ms(void), returns double
- host_service_net_get_lobby(uint32_t guest_address), copies NetLobby

Native net_finish, net_back, net_rules and net_rotation serialize the messages.
Rules and rotation use settings. Existing pick, ready and start imports still apply.
The browser implements the same messages with its WebSocket and shared ABI.

Nettool accepts --races N for N completed room races (including a race joined
while running), --ready-after S in every open/intermission (-1 never),
--finish-after S (finish then back), or --give-up-after S (back without finish).
Neither action means never finish. --seconds bounds the entire session.
It logs each phase transition as PHASE followed by the phase and local race count.
A late join waits for its first grid. Existing recording replay mode remains.


## Ten racers (protocol 2)

New public and private rooms default to ten members. The host can choose 2–10,
but cannot shrink below current membership. A race still uses one local human.
NetLobby member, racer and result arrays each have ten entries. Native and browser
hosts use the regenerated shared ABI. Rider STATE format remains version 3;
EVENT format remains version 3. The binary relay path is unchanged.

Rotation settings carry `rotation` and parallel `trackLimits` arrays in the same
message. Limits are 6, 8 or 10. At least one entry must take ten riders. Clients
derive these from the retail track classifier, not a server track table. The
configured rotation remains in room snapshots; limits are kept by the server.
The native rotation import still takes up to 64 bytes: each byte stores a track
ID in its low five bits, (10 - limit) / 2 in bits 5-6, and bit 7 set for a pick-only track
(sent in `pickOnly`). Native and browser
hosts decode this into the two JSON arrays. Snapshot rotation IDs are plain IDs.

NEXT walks forward through the rotation; RANDOM draws from fitting entries other
than the current track when possible. Neither chooses a `pickOnly` entry (the host can still pick
it; its limit counts as any other's). Both skip entries smaller than membership.
SAME preserves the selection. At grid lock, if the ready grid exceeds the selected
track's limit, the next fitting rotation entry is selected without dropping anyone.
`lastEvent` is `track_switch:<old id>:<limit>:<new id>`; the game translates IDs
into a notice such as PERU MX TAKES 6: RACING FUJI. The grid and rules still lock
through the countdown. Computer riders fill the retail track count when enabled.

Room snapshots omit `choosing` when false and `bike` when zero. Receivers use those
defaults. This saves repeated fields while retaining all ten members and last results
within the 4096-byte UTF-8 frame budget. The room draws two columns of five cards,
including ping, readiness, race state and results. The fourth room rule sets capacity.

## Region (finding 135)

A room is a Durable Object, and it runs where it was created. `/v1/rooms/new` creates it with a
location hint from the creator's continent and country as Cloudflare's geolocation places them
(`request.cf`; never the IP), not from the Cloudflare location their ISP happens to reach:
Europe `weur` (the Atlantic side: GB, IE, FR, ES, PT, the Benelux...) or `eeur` (the rest),
North America `wnam` or `enam` (longitude -100, else the state or province), South America `sam`,
Asia `apac-ne` (JP, KR, TW, HK, CN, MN), `apac-se` (SG, MY, ID, TH, VN, PH...), the Middle East
`me`, the rest `apac`, Oceania `oc`, Africa `afr`. Cloudflare places `sam`, `afr` and `me` rooms
in the nearest region that hosts Durable Objects. Joining by code passes no hint (the room exists).

The room learns the Cloudflare location it runs in (a `colo` such as `FRA`) by reading
`https://cloudflare.com/cdn-cgi/trace` once (`request.cf.colo` inside the object is the client's
edge, not the object's); rooms made before this learn it on their next hello. Its region, the
short name players see, is that colo's (`EU`, `NA`, `ASIA`, `OC`) when the colo is in the
server's table of Durable Object locations, else the hint's (`EU`, `NA`, `SA`, `ASIA`, `OC`, `AF`,
`ME`); `""` when neither is known (a local `wrangler dev` without Cloudflare data).

- `welcome` adds `edgeColo` (the Cloudflare location this connection reached), the player's own
  `country` and `continent` (Cloudflare's, `""` unknown; sent to that player only), `roomColo` and `roomRegion` (`""` until the room's
  trace has answered; a `where` follows then).
- `where` (new message type) carries the room's `region` and `colo` and every member's `region`
  (the same set as above, from their continent; `""` unknown). It is apart from `room` so room
  snapshots stay within their 4096 bytes for ten members. No message tells a player another
  player's country: rooms are public and players post their logs.
- `/v1/lobbies` adds the asker's `region`, and per room `region`, `colo` and `createdIn` (the
  creator's region). Rooms made in the asker's region come first, then rooms that run in it, then
  the rest, each part busiest first. Releases up to 0.7.6 read the first eight rooms, and their
  Quick Race joins the first one with room, so they prefer a near room too.

Every field and the message type are additions: clients that don't know them ignore them (the
native client reads by key, the browser copies known keys), so `MIN_VERSION` stays as it is.

SFU allocations are restricted to running race members, one setup attempt per member per race.
Allocations close at race end or departure. Receiver rate protection silences a member for the
remaining race if any rider exceeds 120 snapshots in a one-second window (normal: 30/s).
