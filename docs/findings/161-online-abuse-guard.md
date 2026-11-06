# 161: online abuse guard: kill switch, rate limits, bans and a short moderation log

Status: current
Checked: 2026-11-06

Branch `feat/online-guard` from `main` 9ecb8e1, started 2026-11-06. Merged into `main` at c230599 on
2026-11-06; released in 0.7.12.

## Goal and acceptance

The online server (`online/server/`) checks protocol, release (`MIN_VERSION`), build id and disc
hash in `hello`, and caps frame sizes and room size. It has no limit per connection source, no
message-rate cap, no way to ban anyone and no way to turn online off. The game is open source, so
the server can't prove a client is an official build: protection has to limit what any client can
do, and let the operator stop a bad actor.

The lane adds, server side (on its own branch it deployed without a game update and 0.7.11
clients kept working; since finding 158 merged, `main`'s server speaks protocol 3 and refuses
0.7.11, so it deploys only with the release that ships protocol 3):

1. **Kill switch.** The operator makes the server refuse new rooms, joins and race starts with a
   clear message, and `/v1/health` says so; no game update. (Round 1 made it a Worker variable,
   which needs a deploy that restarts every room; the lead's round-2 review moved it into the D1
   store, toggled by the local tool, no deploy: rooms already open keep their members and races
   in progress finish.)
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
- [x] Kill switch (round 1: the `ONLINE` variable; round 2: in D1, no deploy): table `settings`
  (`migrations/0002_settings.sql`, keys `online`, `online_message`), `tools/moderate.mjs online
  [on | off [--message]]`, `src/guard.ts` `onlineState` (kept 30 s per isolate). While off the
  Worker refuses `/v1/rooms/new` with `online_off` (4010) at once; a room refuses at hello anyone
  but its own members' rejoins; `/v1/health` and `/v1/lobbies` say `online:false`; rooms start no
  race, a running race finishes. Tests `test/guard.test.ts` (refusals, message, an open room's
  members staying and rejoining, a running race, the cache and its failure path). Checked against
  a local `wrangler dev`: `online off --message` showed in `/v1/health` at once, `online on`
  after the 30 s.
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
- [x] Round 2: ban lookups kept per isolate (`src/guard.ts` `findBan`, `BAN_CACHE_MS` 60 s,
  failures 10 s, at most `BAN_CACHE_MAX` 10,000 lookups); tests in `test/guard.test.ts` (a new
  ban and a lifted one bite after the minute, a kept ban that ends, failures, the size cap).
- [x] Install id in the game and `hello`: `port/render/settings/src/install.rs` (`install_id` in
  dhd.ini, 32 lowercase hex, made once; `dhd_settings_install_id`), `port/hle/host.c` puts it in
  `DHD_NET_INSTALL_ID` after the settings load, `port/net/net.c` takes it for each connection and
  `client.c` sends `"installId"` in every hello. Tests: `settings/tests/corpus.rs` `install_id`,
  `port/net/test_session.c`.
- [x] Privacy note, protocol docs: `docs/PLAYING.md` "What the official server keeps" and
  `port/release/README.txt` (what, why, 14 days, bans by hash or install id, nobody sees another's
  address, hash or install id); `docs/online-protocol.md` has every new code, the kill switch's
  message, the limits, caps, bans, the log and `hello.installId` (written with each item).
- [x] 2026-11-06 checks on the lane: `check.py server` OK (114 tests, typecheck), `check.py port`
  OK (quick included: core, runtime tests, host build, net tests, renderer tests), the settings
  crate's tests OK, acceptance 3 against `wrangler dev` OK, the load test with the guard on OK.
  Left for the lead: the one-time setup in `online/server/README.md` ("Abuse guard: one-time
  setup", round 2: commit the `d1_databases` binding with its id, apply both migrations), the
  deploy, and a real race once the game sends the install id.
- [x] 2026-11-06 round 2 (server only, nothing outside `online/server` and docs): `check.py
  server` OK (120 tests, typecheck), `check.py docs` OK; the tool's `online` against a local
  store and `wrangler dev` as above.

## Decisions

- 2026-11-06 (the user): bans and the moderation log, after players reported a cheater by name
  and there was no way to act on it. Names are not identity (anyone can pick any name), so bans
  target an IP hash or an install id chosen from the log, never a name.
- Keyed hash, 14 days: IP addresses are personal data under the GDPR; a keyed hash with short
  retention keeps what abuse handling needs and nothing more. Rotating the key makes old hashes
  useless.
- Server only first: the lane deploys without a game update; only the install id needs the next
  release.

- Refusals before any room come from the Worker itself (a WebSocket it accepts, one error frame,
  close): it answers at once, before `hello`; every release reads an error frame then a
  4000-4099 close as a refusal (no rejoin, `port/net/client.c` `client_on_closed`) and shows
  `message`, so no client change is needed.
- 2026-11-06, round 2 (the lead): the kill switch is a row in the D1 store, not a Worker
  variable. A variable needs a deploy, and a deploy restarts every room and drops every lobby.
  The `ONLINE` variable is gone, not kept as a second switch (one code path): its only remaining
  use would be "D1 down and online must go off at once", and then a deploy is as disruptive as
  the outage; a self-hosted server without the database has online always on.
- Each isolate keeps the switch 30 s (`SWITCH_CACHE_MS`): a connection costs no D1 read in the
  common case, and 30 s is short enough for an emergency. Concurrent lookups share one read. A
  failed read keeps the last state the isolate read (on, if none) and retries after 10 s
  (`FAILED_CACHE_MS`): a broken store turns online neither off nor on.
- While off, new rooms are refused by the Worker at once, but a join by code goes to its room,
  which refuses at hello anyone but its own members' rejoins (the token is only known there). Now
  that switching drops nobody, a racer whose connection blips mid-race must get back to the race
  that goes on; the cost is that an off server still wakes a room per join attempt (behind the
  limits per address). Members in a lobby are not kicked; their room starts no race.
- The tests turn the cache off (`GUARD_CACHE = "off"`, vitest.config.ts only) so a test's change
  bites at once; the cache's own tests pass envs without it, with their own clock.
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
  `wrangler.guard-local.jsonc`.
- 2026-11-06, round 2 (the lead): the official database's id is committed in `wrangler.jsonc`
  at deploy time, by the lead. It is not a secret (useless without the account's login), and
  self-hosters already replace `routes`; the README tells them to replace `database_id` too. The
  lane commits no id.
- Fail open: a store that can't be read lets players in and logs `guard:` lines (`wrangler
  tail`). A broken store must not stop online for everyone; a ban that fails to hold for a few
  minutes costs less.
- Retention by a daily cron trigger (`17 4 * * *`), not cleanup on write: the rows must go after
  14 days even when nobody joins. Ended bans are deleted too; a ban without expiry stays until
  lifted.
- 2026-11-06, round 2 (the lead): every room connection read D1 for a ban. Each isolate now
  keeps a lookup 60 s, "not banned" and "banned" alike (one rule; a lifted ban takes as long
  as a new one), and drops a kept ban once its expiry passes. A ban takes up to a minute to
  bite: a banned player who reconnects within it gets in once more, which costs less than a D1
  read per connection. A failed read is kept 10 s (players let in, logged once per 10 s, not per
  connection). At most 10,000 lookups per isolate, the oldest dropped first: memory stays small
  even under a flood of new addresses (which the limits per address slow down first).
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
- The install id is not a settings-table row: a row would reach the launcher's settings snapshot
  and, as a variable, the `settings:` line of dhd.log, which players post (whoever copies an id
  can get its player banned). It is its own key the crate knows (`install.rs`; not "unknown" in
  the log either), put into `DHD_NET_INSTALL_ID` by `host_putenv_s` directly, never through
  `settings_set`.
- Made at the settings load, saved only when dhd.ini exists: creating a dhd.ini for it would give
  the tools' runs the player's defaults (the crate applies them only when the file exists). The
  tools' runs and DHD_SETTINGS=none get one for the run. `DHD_NET_INSTALL_ID` set beforehand
  overrides (tests).
- The randomness: the crate takes no dependencies, so the id is SipHash under std's
  `RandomState` (keys from the operating system's random source) over time and process id;
  unguessable without the key, which is all an id that names nothing needs.
- net.c reads the variable on the game's thread (`connect_to`), not in `client_connected` on the
  socket's thread: the game changes its environment while it runs, and getenv beside putenv on
  another thread is a race.
- The browser's client (`port/web/www/net.js`) sends no install id: the browser offers no online
  play (finding 143), and the field is optional.

## Surprises

- `tools/loadtest.mjs` still said protocol 1 in its hello, so the server (protocol 2) refused it:
  fixed to take the protocol from `/v1/health`. With the guard on (`wrangler dev`): 4 players at
  600 bytes and 10 players at 3000 bytes for 8 s, every frame relayed in order, no cap or limit
  hit (relay median 4.2 and 7.3 ms locally).

- The test runtime relays a binary frame in about 2 ms (300 frames took 687 ms), so a test can't
  send 2000 of them in a window: one test sets a room's own cap (`CAP_BINARY`, `CAP_WINDOW_MS`
  through the room's env), the tally itself is tested directly.

## Result

Merged into `main` on 2026-11-06. The server refuses abuse without a game update; only the
install id waits for the next release.

- Kill switch in the D1 `settings` table, toggled with `node tools/moderate.mjs online off|on`
  (`online/server/src/guard.ts`, cached per isolate); refusal `online_off` (4010).
- Limits per connection source with the Workers rate-limiting binding, keyed by the IP hash
  (`rate_limited`, 4011); per-member message caps in the room (`too_fast`, 4012).
- Moderation log and bans in D1 (`migrations/`): joins with a keyed IP hash and the install id,
  kept 14 days; bans by IP hash and/or install id with an expiry (`banned`, 4013), cached per
  isolate for up to a minute. Operated only from `online/server/tools/moderate.mjs` with the
  operator's own wrangler login; no admin endpoint.
- Install id in dhd.ini (`port/render/settings/src/install.rs`), sent as `hello.installId`;
  never written to dhd.log.
- Privacy note in `docs/PLAYING.md` and `port/release/README.txt`.

Checked: `python tools/check.py server` and `python tools/check.py port` on the merged tree;
a test asserts no client ever receives another player's IP hash or install id; against a local
`wrangler dev`, a player listed by name was banned, refused, unbanned and let back in.
Not yet: the deploy and its one-time setup (`online/server/README.md`).
