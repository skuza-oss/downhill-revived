# Online protocol (version 2)

The wire protocol between the PC port (`port/net/`) and the online server (`online/server/`,
Cloudflare Workers + Durable Objects). The plan behind it is `docs/ONLINE-PLAN.md`.

- One WebSocket per player. Lobby control travels as JSON **text** frames, the race as compact
  little-endian **binary** frames.
- The server runs no game logic. In a race it relays STATE and EVENT frames (state sync: every
  PC runs its own race and shows the others' riders from their snapshots, findings 041 and 044)
  and reports players who leave.
- `PROTOCOL = 2`. It is in `hello`; the endpoint URL remains `/v1/`. Any incompatible change raises
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
| GET | `/v1/health` | `{"ok":true,"protocol":2,"minVersion":"0.1.0"}` |
| POST | `/v1/times` | Post a time-trial time and ghost (below, finding 127). |
| GET | `/v1/times?track=N&laps=L&limit=20` | A track's best times. |
| GET | `/v1/ghost?track=N&laps=L&rank=1` | The ghost of one of them. |

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
| `protocol_mismatch` | 4001 | `hello.protocol` is not the server's `PROTOCOL`. |
| `version_mismatch` | 4002 | `hello.version` differs from the host's. |
| `iso_mismatch` | 4003 | `hello.isoSha256` (the disc image's game files hash, finding 098) differs from the host's. |
| `room_full` | 4004 | No free slot. |
| `room_started` | 4005 | The race is running; late joins are not supported. |
| `room_not_found` | 4006 | No room has this code (or it was closed). |
| `kicked` | 4007 | The host removed this player. |
| `hello_timeout` | 4008 | No `hello` within 10 seconds. |
| `client_outdated` | 4009 | `hello.clientVersion` is below the server's `MIN_VERSION` (below). |

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

Non-fatal errors (a non-host sending a host command, a `start` while someone is not ready or
the host is alone: `not_ready`)
use the same `error` frame **without** a close; the client just shows `message`.

## Lobby (JSON text frames)

### Client → server

```jsonc
// First message, required. name: 1-24 characters.
{"type":"hello","protocol":2,"version":"dhd-port 0.2.0 1a2b3c4d5e6f7a8b","clientVersion":"0.2.0",
 "isoSha256":"<64 lowercase hex>","name":"Rider","features":["countdown","reconnect"]}  // features: optional

// Rejoining after a drop: the token the last welcome carried goes in the hello (finding 140).
{"type":"hello","protocol":2,"version":"dhd-port 0.7.5 1a2b3c4d5e6f7a8b","clientVersion":"0.7.5",
 "isoSha256":"<64 lowercase hex>","name":"Rider","features":["countdown","reconnect"],
 "reconnectToken":"<64 lowercase hex>"}

// Host only. Every field optional; omitted fields keep their value.
{"type":"settings","track":3,"laps":2,"seed":12345,"public":true,"maxPlayers":10,"roomName":"Friday race"}

// Rider and bike for a member. Anyone may set their own; the host may set anyone's.
{"type":"pick","member":2,"rider":5,"bike":1}     // "member" omitted = yourself
{"type":"pick","choosing":true}                     // on the rider screen (unreadies); false when back

{"type":"ready","ready":true}
{"type":"kick","member":3}                          // host only
{"type":"start"}                                    // host only; at least 2 members, all (host included) ready
{"type":"pong","s":1734000000123}                   // echo of a server ping, see below
{"type":"chat","text":"gg"}                         // anyone, in the room or a race (finding 139)
{"type":"ping","t":12345.6}                          // optional client-side ping; the server answers "pong"
{"type":"end"}                                       // host only: race over, back to the lobby
```

- `track`, `laps`, `rider`, `bike`: integers 0..255 whose meaning belongs to the game (track
  table index, rider and bike table indices). `laps` 0 = the track's default.
- `seed`: unsigned 32-bit integer. If the host never sets one, the server draws one at `start`.
- `maxPlayers`: 2..10, default 10. It can't go below the current member count.
- `public`: listed in `/v1/lobbies` while not started and not full. Default `false`
  (private, join code only).
- `features`: what the client understands beyond version 1. `"countdown"`: the room may start
  the race by itself (below). `"reconnect"`: the client can come back after a dropped socket, so
  the server gives it a token and holds its slot, in the lobby or a race, for 20 s (below). A
  hello without it (every release up to 0.7.3) gets no token and no held slot: its member is
  removed the moment the socket goes, exactly as before.
- `reconnectToken`: only in the hello that rejoins after a drop. The 64 hex digits the last
  `welcome` carried; the server re-attaches the sender to that member's held slot. Absent, the
  wrong shape, wrong or expired, it is ignored and the connection is a normal join.

### Server → client

```jsonc
// Answer to hello: who you are in this room. reconnectToken is there only when the hello listed
// the "reconnect" feature (finding 140): keep it and send it in a later hello to come back.
{"type":"welcome","protocol":2,"code":"K7QX2M","memberId":2,"host":false,"serverTime":1734000000000,
 "reconnectToken":"1f9c8a3d5b7e0246c9d1f3a4b5c6d7e8f9a0b1c2d3e4f5061728394a5b6c7d8e"}
// The welcome of a rejoin into a running race also names the race slot: "slot":1.

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

// A racer that can rejoin dropped: its slot is held, its rider is not being driven (finding 140).
{"type":"away","slot":1,"memberId":2}
// ...and it rejoined its slot: its frames come again. Older clients ignore both.
{"type":"back","slot":1,"memberId":2}

// A racer left or dropped. The host's game hands their rider to its AI.
{"type":"left","slot":1,"memberId":2,"reason":"disconnect"}   // or "leave"/"kicked"/"timeout"
{"type":"chat","memberId":2,"name":"Bo","text":"gg","at":1734000000123}  // to everyone, the sender too

// Host ended the race; the room is a lobby again (ready flags cleared).
{"type":"ended"}
```

- **Member ids** are fixed for the life of a connection and never reused in a room. **Slots**
  (`0..N-1`) exist only in a race: the server assigns them at `start` in join order (host
  first). All race frames use slots.
- Lobby members who leave just disappear from the next `room`. If the host leaves, the
  longest-present member becomes host. A member whose socket drops unexpectedly and whose hello
  listed `"reconnect"` stays in `members` with `"reconnecting":true` for ~20 s instead
  (`Rejoining after a drop`, below).
- `startAt` is server time (ms since the Unix epoch), about 3 s after `start`. Clients map it
  to local time with the offset from `ping`/`pong` (`offset ≈ serverTime - (tSend + rtt/2)`).
  The game itself starts every PC's countdown together on the server's clock once all have
  loaded (port/hle/online.c).

## Race (binary frames, little-endian)

After `start` each racer sends binary frames on the same socket. The server overwrites byte
1 (`slot`) with the sender's real slot, then forwards STATE and EVENT frames unchanged to every
other racer. Other types, frames shorter than 2 bytes, and binary frames from a socket that is
not racing (for example stragglers after `ended`), are dropped silently. Types 1 and 2 were the
first design's INPUT and HASH frames (input lockstep, finding 039); they are no longer used.

### PING, type 3 — client → server; PONG, type 4 — server → client

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | `type` = 3 (4 in the answer) |
| 1 | u8 | `slot` |
| 2 | n | any payload, at most 32 bytes (e.g. a u32 sequence and an f64 send time) |

The server answers the sender only, with the same bytes and the type changed to 4. Use it to
watch the round-trip during a race; it is never forwarded.

### STATE, type 5, and EVENT, type 6 — client ↔ server ↔ clients

| Offset | Size | Field |
|---|---|---|
| 0 | u8 | `type` = 5 or 6 |
| 1 | u8 | `slot` of the sender |
| 2 | n | payload, the client's format (finding 044) |

State sync: a STATE frame is a snapshot of riders the sender owns, an EVENT frame something
it decided for another rider (a hit). The server forwards both, slot corrected, to every other
racer and keeps nothing.

Each PC races at its player's frame rate (60-240 fps). A snapshot (payload version 3) is stamped
with the sender's race time in ms since GO, not its frame or tick count, about 20 a second at any
rate; the receiver puts it on its own race clock (finding 114). Finish times in EVENT frames stay
in race ticks (1/60 s of game time), the same at every rate. The server reads none of this.

EVENT payloads start `[version 2, kind]`. A client drops a kind it doesn't know (older clients
take kind 1, a hit, alone), so new kinds need no server change. Two carry what the room can't on a
server without the `chat` message (the official one), in the race only:

| Kind | Payload after `[2, kind]` | Use |
|---|---|---|
| 10 `EVENT_CHAT` | u8 length (1-120), the text | A race chat line (finding 139). The name shown is the stamped `slot`'s, never sent. At most 5 in 10 s a sender: more are dropped on both sides. The sender shows its own line (no echo). |
| 11 `EVENT_VOICE` | u16 sequence, an Opus packet (at most 400 bytes) | 20 ms of voice (finding 131): 48 kHz mono, VOIP, 8-64 kbps. The receiver orders by sequence, conceals a gap, drops late, repeated, oversized or flooding frames (over 60 a second), and decides by its own copy of the riders' positions who is heard (proximity, groups). The server relays it to the whole race. |

(Kind 9 is reserved for the taunts branch's `EVENT_TAUNT`.)

### Chat

Any member sends `chat` in the room or during a race; the server sends it to every member, the
sender included, with the member's id and name and the server time. The text is cleaned first
(control characters become spaces, runs of spaces one, trimmed) and must then be 1-120 UTF-16
units, else `bad_value`. A member may send 5 in 10 s, else `slow_down`. Older clients ignore
the message type. Nothing is kept: a player who joins later sees only what comes after.

### Leaving

A racer leaves by closing the socket (or sends `{"type":"leave"}` first, which only changes
`reason`). The server broadcasts `left`; every frame the racer sent before reached the others
first (same socket, ordered). The host's game hands the rider to its AI and sends it as its
own from then on. A racer whose hello listed `"reconnect"` and whose socket drops (no `leave`) is
`away` first and `left` only if it does not rejoin within 20 s (`Rejoining after a drop`, below).

A racer the server has not heard from for 10 s (`RACE_SILENT_MS`: no STATE, EVENT or PING frame;
clients ping every 500 ms while racing) leaves the same way with `reason` `"timeout"` (or is
`away`, as a drop, when it can rejoin), and the server closes its socket (1001). That covers a
lost connection that never closed, the case the client already covers the other way round (it
drops a server silent for 10 s). Finding 135.

### Race room rules (finding 115)

The server keeps what everyone has to agree on:
- **Ready is for what you saw.** A changed `track` or `laps` unreadies everyone (`public` and
  `roomName` don't).
- **Riders can be shared.** Two or more members may pick the same rider and all be ready
  (issue #17).
- **Choosing** (`pick` with `"choosing":true`): the member is on the rider screen; it is
  unreadied and `ready` answers `not_ready` until a pick with `"choosing":false`.
- **Countdown.** When at least two members are in the room, all ready, none choosing,
  and every one said `"countdown"` in `hello`, the server sets `countdownEndsAt` (server ms,
  `COUNTDOWN_MS` = 5 s on) and starts the race itself then, as `start` would. Any change that
  breaks the rule clears it. `start` still starts at once. Clients show the time left with
  `serverTime` as the clock offset. Older clients never get a countdown: the host starts.

## Rejoining after a drop (finding 140)

A socket that closes or errors without a `leave` (lost Wi-Fi, a NAT timeout), or a racer silent
for `RACE_SILENT_MS`, is a dropped connection. A client that put `"reconnect"` in its hello got a
secret `reconnectToken` in its own `welcome`; for such a member the room holds the slot for
`RECONNECT_WINDOW_MS` (20 s) instead of removing it:

- **In the lobby** the member stays in `room.members` with `"reconnecting":true`, keeping its id,
  name, rider and bike, ready flag and host role. No race starts while a slot is held (`start`
  answers `not_ready`; the countdown waits).
- **In a race** every member gets `away` `{"slot","memberId"}` at once: the rider is not being
  driven. Frames to the slot are dropped meanwhile (the relay keeps nothing, so hits meant for it
  are lost). The slot is not `left`.

The client comes back on `/v1/rooms/<CODE>` with `reconnectToken` in its hello:

- **The right token within the window** (and the host's `version` and ISO, as any join):
  the socket is re-attached to the same member. The `welcome` carries a **fresh** token (the one
  that came back is spent) and, in a race, `"slot"`. Everyone, the rejoiner too, gets `back`
  `{"slot","memberId"}`, and relaying resumes both ways. A racing client whose welcome has no
  `slot` is in the lobby: the race ended while it was away.
- **A missing, malformed, wrong, spent or expired token** is no token: an ordinary join (a new
  member in the lobby; `room_started` during a race). A token never matches a member that is still
  connected.
- **The window runs out:** the member is removed as by a leave (`reason` `"timeout"`): `left` in a
  race, so the host's AI takes the rider, and the host role moves on if it was the host's.

A `leave` message or a kick removes the member at once, token or not. A member whose hello did not
list `"reconnect"` (every release up to 0.7.3) gets no token and is removed the moment its socket
goes, exactly as before: holding its slot would only delay the AI. Older clients ignore `away`,
`back`, `welcome.reconnectToken`, `welcome.slot` and `room.members[].reconnecting`; while a racer
is away they show its puppet frozen, then moving again after `back`. `PROTOCOL` stays 1.

The token is 32 random bytes as 64 lowercase hex digits, sent only to its member, kept and checked
only in the room that issued it (compared in constant time). The client sends only the token,
never a member id.

## Lobby list

`GET /v1/lobbies` → `200 application/json`:

```json
{"protocol":2,"minVersion":"0.1.0","lobbies":[
  {"code":"K7QX2M","roomName":"Friday race","version":"dhd-port 0.2.0 1a2b3c4d5e6f7a8b","track":3,
   "players":2,"maxPlayers":10,"hostName":"Ann"}]}
```

Only public rooms that have not started and have a free slot are listed. Clients should
show only rooms whose `version` matches their own.

## Time-trial boards (finding 127)

HTTP on the same host, JSON in and out, `Access-Control-Allow-Origin: *` on every answer
(`OPTIONS` answers the preflight). One board per `track` and `laps` (bytes, the game's indices;
`laps` 0 or omitted = the track's default). **Times are not verified**: anyone can post any
plausible time under any name. Treat a board as a list of claims.

```jsonc
// POST /v1/times
{"version":"dhd-port 0.8.0 1a2b3c4d5e6f7a8b","clientVersion":"0.8.0","isoSha256":"<64 lowercase hex>",
 "track":3,"laps":0,"name":"Ann","timeMs":61234,"ghost":"<base64, optional>"}
// 200: rank of this name's best on the board (0: below the kept rows); best: this post improved it
{"ok":true,"track":3,"laps":0,"rank":4,"best":true,"timeMs":61234}

// GET /v1/times?track=3&limit=20   (limit 1-100, default 20)
{"track":3,"laps":0,"times":[{"rank":1,"name":"Bo","timeMs":59870,"at":1734000000000,"ghost":true}]}

// GET /v1/ghost?track=3&rank=1     (rank default 1; 404 when that rank has no ghost)
{"track":3,"laps":0,"rank":1,"name":"Bo","timeMs":59870,"ghost":"<base64>"}
```

- One entry per name (compared case-insensitively): a post replaces it only when faster. Ties go
  to the earlier post. A board keeps 1000 names; the best 100 keep their ghost.
- `clientVersion` is required and gated by `MIN_VERSION` as in `hello` (403
  `{"error":"client_outdated","minVersion","clientVersion"}`). `version` 1-64 characters,
  `isoSha256` 64 lowercase hex, `name` as in `hello` and without control characters, `timeMs` a
  whole number from 10 000 to 1 800 000, `ghost` standard base64 of at most 131 072 bytes (the
  body at most 180 KiB, else 413 `too_large`).
- Errors: `{"error","message"}` with 400 (`bad_request`, `bad_time`), 403, 404 (`not_found`), 405,
  413 or 429 (`slow_down`: more than 6 posts from one address to one board in 10 minutes).
- The server keeps no addresses: the rate limit counts a hash of `cf-connecting-ip`.

## Limits

- One room holds at most 10 members.
- Text frames: at most 4 KB. Binary frames: at most 3072 bytes (a STATE frame carries all of a
  player's riders).
- A room with no members is deleted.
- A dropped member that sent `"reconnect"` may come back into its slot, lobby or race, for 20 s
  (`Rejoining after a drop`, finding 140); after that, or for older clients at once, a dropped
  racer is handed to AI for the rest of the race.
- A server restart (a deploy) drops every socket with no close event. A lobby client says
  hello again under the same name and gets its member back (id, host, rider, Ready); members
  not back within the hello timeout leave as disconnects.

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
countdown. Members outside the grid receive room snapshots and no binary relay.
Joins are accepted in every phase up to maxPlayers, counting all members.

### Finishes and return

An entrant sends `{"type":"finish","ticks":N,"place":P}` when its own human rider
crosses the line. N is an unsigned 32-bit race clock at 60 ticks a second.
P is 1–255, the full-field place computed locally, including computer riders.
Only the first finish from that entrant counts. Computer finishes send nothing.

The first human finish sets finishEndsAt to now plus the running finishWindow.
With finishWindow zero the deadline remains null. Later finishes do not move it.
At the deadline all unfinished entrants become DNF time_up and leave the relay.
Finished entrants retain their finish and leave the relay too.

A client sends `{"type":"back"}` when leaving the race scene for the room.
An unfinished entrant becomes DNF gave_up. A finisher keeps its finish.
Its relay stops. Remaining racers receive `left` with its slot, memberId and
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

Track or laps changes unready members outside the running race.
A track change during intermission gives at least 15 seconds before nextRaceAt.
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
EVENT format remains version 2. The binary relay path is unchanged.

Rotation settings carry `rotation` and parallel `trackLimits` arrays in the same
message. Limits are 6, 8 or 10. At least one entry must take ten riders. Clients
derive these from the retail track classifier, not a server track table. The
configured rotation remains in room snapshots; limits are kept by the server.
The native rotation import still takes up to 64 bytes: each byte stores a track
ID in its low five bits and (10 - limit) / 2 in its high bits. Native and browser
hosts decode this into the two JSON arrays. Snapshot rotation IDs are plain IDs.

NEXT walks forward through the rotation; RANDOM draws from fitting entries other
than the current track when possible. Both skip entries smaller than membership.
SAME preserves the selection. At grid lock, if the ready grid exceeds the selected
track's limit, the next fitting rotation entry is selected without dropping anyone.
`lastEvent` is `track_switch:<old id>:<limit>:<new id>`; the game translates IDs
into a notice such as PERU MX TAKES 6: RACING FUJI. The grid and rules still lock
through the countdown. Computer riders fill the retail track count when enabled.

Room snapshots omit `choosing` when false and `bike` when zero. Receivers use those
defaults. This saves repeated fields while retaining all ten members and last results
within the 4096-byte UTF-8 frame budget. The room draws two columns of five cards,
including ping, readiness, race state and results. The fourth room rule sets capacity.
