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
- [ ] Kill switch.
- [ ] Rate limits per connection source.
- [ ] Per-member caps.
- [ ] Moderation log, bans, local tool.
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

## Surprises

## Result
