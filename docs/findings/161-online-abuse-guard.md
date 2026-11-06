# 161: online abuse guard: kill switch, rate limits, bans and a short moderation log

Status: in progress

Branch `feat/online-guard` from `main` 9ecb8e1, started 2026-11-06. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

The online server (`online/server/`) checks protocol, release (`MIN_VERSION`), build id and disc
hash in `hello`, and caps frame sizes and room size. It has no limit per connection source, no
message-rate cap, no way to ban anyone and no way to turn online off. The game is open source, so
the server can't prove a client is an official build: protection has to limit what any client can
do, and let the operator stop a bad actor.

The lane adds, server side, deployable without a game update (0.7.11 clients keep working):

1. **Kill switch.** A Worker variable (for example `ONLINE = "off"`) makes the server refuse new
   rooms, joins and race starts with a clear message, and lets `/v1/health` say so. Changing it
   needs only a variable change and deploy, no game update.
2. **Limits per connection source.** At most a few new rooms and a few connections per minute
   from one IP address, using Cloudflare's Workers rate-limiting binding (or the closest Cloudflare
   feature, with the reason in Decisions). Over the limit: a refusal with its own error code.
3. **Per-player caps in the room.** A member sending more text messages or binary frames per
   second than the game ever sends (with margin) is warned once, then closed with its own close
   code. The limits are named constants with the reasoning beside them.
4. **Bans and a moderation log.**
   - At each join the server records: time, room code, member id, name, client version, country,
     a keyed hash of the IP address (HMAC with a Worker secret, never the raw address), and the
     install id (5). Rows older than 14 days are deleted automatically.
   - A ban list keyed by IP hash or install id, each ban with an expiry (or none). It is checked
     when a player connects, before any room is reached. A banned player gets a plain refusal.
   - No public admin endpoint. The operator works from a local tool in `online/server/tools/`
     that reads and writes the store through wrangler with the operator's own login (D1 via
     `wrangler d1 execute` is the first candidate). It can list joins by name and time range
     (`--name alexander --since 2026-11-05`), show the matching rows (time, room, name, version,
     country; no hashes printed by default), ban one row's IP hash and/or install id with an
     expiry, list bans, and lift a ban.
5. **Install id.** The game creates a random id once, keeps it in dhd.ini, and sends it in
   `hello` as an optional field. Old clients without it still join.
6. **Privacy note.** A short section in the player docs (README or PLAYING): what the server keeps
   (the list above), why (abuse), for how long (14 days; bans until they expire), and that no
   player ever sees another player's address or install id.

Hard rules:
- No player ever receives another player's IP address, IP hash or install id: not in `welcome`,
  `room`, `where`, the lobby list, errors or logs sent to clients. A test asserts it.
- The raw IP address is never stored or logged by our code; only the keyed hash.
- The HMAC key and any tokens are Worker secrets, never in tracked files.

Acceptance:
1. `python tools/check.py server` passes, with new tests for each of 1-5, including the "nothing
   private reaches a client" test.
2. `python tools/check.py quick` passes (the install id in the game and `port/net` tests).
3. The moderation tool works against a local `wrangler dev` store: join two fake members, list
   them by name, ban one, and the banned one is refused on the next connect.
4. `docs/online-protocol.md` documents the new error codes, close codes, the kill switch's
   message, and the `hello` field.

Out of scope: cheat detection (impossible finish times, positions), report-a-player in the game,
anything in the SFU lane (branch `feat/online-sfu`) beyond what merging needs.

## Progress

- [x] 2026-11-06: lane plan (this file).
- [x] Kill switch: `ONLINE` var (`wrangler.jsonc`, `"on"`), `src/guard.ts` `onlineOff`; the Worker
  refuses every room WebSocket with `online_off` (4010) before any room, `/v1/health` and
  `/v1/lobbies` say `online:false`; rooms start no race while off. Tests `test/guard.test.ts`.
- [x] Rate limits per connection source: the Workers rate-limiting binding (`ratelimits` in
  `wrangler.jsonc`: `RL_CONNECT` 30 room connections a minute, `RL_CREATE` 10 new rooms a minute),
  keyed by the keyed IP hash (`src/guard.ts` `ipHash`, HMAC-SHA-256 with the `IP_HASH_KEY`
  secret); over: `rate_limited` (4011) from the Worker. Tests in `test/guard.test.ts`.
- [x] Per-member caps: `src/guard.ts` `tally` (per socket, 5 s windows: 100 text, 2000 binary),
  used first thing in `Room.webSocketMessage`; over: dropped, the first time a non-fatal `too_fast`
  warning, the next window over closes with `too_fast` (4012). Tests in `test/guard.test.ts`.
- [x] Moderation log, bans, local tool: D1 tables `joins` and `bans` (`online/server/migrations/`),
  `src/guard.ts` `findBan`, `logJoin`, `cleanup` (daily cron); address bans in the Worker before
  any room, install-id bans at `hello` in the room (`banned`, 4013); the log row written at every
  join and rejoin. `tools/moderate.mjs` (joins, ban, bans, unban, local-setup). The server reads
  `hello.installId` here (32 lowercase hex, optional; malformed = none). Tests
  `test/moderation.test.ts`, including "nothing private reaches a client". Acceptance 3 run
  against `wrangler dev` (two members, list by name, ban, refused, unban, let in): OK.
- [ ] Install id in the game and `hello`.
- [ ] Privacy note, protocol docs.

## Decisions

- 2026-11-06 (the user): bans and the moderation log, after players reported a cheater by name
  and there was no way to act on it. Names are not identity (anyone can pick any name), so bans
  target an IP hash or an install id chosen from the log, never a name.
- Keyed hash, 14 days: IP addresses are personal data under the GDPR; a keyed hash with short
  retention keeps what abuse handling needs and nothing more. Rotating the key makes old hashes
  useless.
- Server only first: the lane deploys without a game update; only the install id needs the next
  release.

- Kill switch refusals come from the Worker itself (a WebSocket it accepts, one error frame,
  close), not from a room: an off server reaches no Durable Object. It answers at once, before
  `hello`; every release reads an error frame then a 4000-4099 close as a refusal (no rejoin,
  `port/net/client.c` `client_on_closed`) and shows `message`, so no client change is needed.
- Turning online off refuses rejoins too: the variable needs a deploy, which restarts every room
  and drops every socket anyway, so "off" means off at once. Races running when a room sees the
  switch go on; only new starts are refused.
- Rate limits: Cloudflare's Workers rate-limiting binding (GA since 2025-09), as the plan said.
  It needs no resource made beforehand (`namespace_id` is a number of our choosing), so it is in
  `wrangler.jsonc` now and today's deploy keeps working. It counts per Cloudflare location and is
  approximate; a strict global limit would need a Durable Object per address, not worth it for
  keeping floods off. The key is the keyed IP hash, so not even Cloudflare's rate limiter sees the
  address from our code. The limits are generous (carrier-grade NAT puts many players behind one
  address); a missing binding or a failing limiter lets players in (logged).
- Without the `IP_HASH_KEY` secret each isolate draws a random HMAC key (warned once in the log):
  limits still hold within it, bans by address don't. Production needs the secret.
- Caps count over 5 s windows, not seconds, so a hitch's burst of queued frames (the game's
  network thread sends what piled up) doesn't count as a flood. Binary: twice what one STATE frame
  per rider would be for 10 riders (today one frame carries all of a PC's riders, so about 13 times
  what a host sends); text: ten times a fast-clicking player. One warning, then a close: a game
  that somehow goes over once (a bug) keeps playing and tells us in its log.
- The counts are memory only (a WeakMap per socket): a room waking from hibernation starts new
  windows. Nothing is stored for them.
- The store is D1, as the plan proposed: SQL makes "joins by name and time, rows with the same
  address" one query, and the operator's tool needs no endpoint at all: `wrangler d1 execute`
  with the operator's login. Its binding is not in `wrangler.jsonc`: a D1 binding needs the
  database's id, and wrangler's automatic provisioning would make a database on the next deploy
  without its tables. `README.md` gives the one-time steps (secret, `d1 create`, binding,
  `migrations apply`); tests bind a database through miniflare, local runs through the generated
  `wrangler.guard-local.jsonc`. Whether the database id is committed is the maintainer's choice
  (it is not a secret, but account-specific).
- Fail open: a store that can't be read lets players in and logs `guard:` lines (`wrangler
  tail`). A broken store must not stop online for everyone; a ban that fails to hold for a few
  minutes costs less.
- Retention by a daily cron trigger (`17 4 * * *`), not cleanup on write: the rows must go after
  14 days even when nobody joins. Ended bans are deleted too; a ban without expiry stays until
  lifted.
- Address bans in the Worker, before any room (the plan); install-id bans in the room at
  `hello`, the first moment the id is known. While that lookup runs, the socket's other messages
  are dropped, so nothing reaches a room as from a member before its hello is through.
- `ban` takes both the address hash and the install id by default: a cheater clears the install
  id by editing dhd.ini, so an install-only ban is easy to dodge. An address can be many players'
  (a household, carrier-grade NAT; the acceptance run showed it, both fake members came from one
  address), so `ban` names the other players the log has from that address and `--install-only`
  spares them. The lead may prefer install-only as the default.
- The log keeps the raw name (operators look for names players report) and the rejoins too (a
  rejoin may come from another address).

## Surprises

- The test runtime relays a binary frame in about 2 ms (300 frames took 687 ms), so a test can't
  send 2000 of them in a window: one test sets a room's own cap (`CAP_BINARY`, `CAP_WINDOW_MS`
  through the room's env), the tally itself is tested directly.

## Result
