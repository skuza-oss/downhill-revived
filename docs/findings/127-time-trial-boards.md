# 127: Time-trial leaderboards and ghosts on the online server

Branch `server-rejoin`, 2026-10-31. Server only: `online/server/src/times.ts` (new),
`src/index.ts`, `wrangler.jsonc`, `test/times.test.ts` (new). Protocol: `docs/online-protocol.md`,
"Time-trial boards". No game or client change yet.

## What it is

An HTTP API on the same Worker as the rooms: a player posts a finished time trial (and, if the
game has it, a ghost: the run's replay data, opaque to the server), and anyone reads the best
times of a track and fetches a ghost to race against.

| Method | Path | What |
|---|---|---|
| POST | `/v1/times` | `{version, clientVersion, isoSha256, track, laps?, name, timeMs, ghost?}` |
| GET | `/v1/times?track=N&laps=L&limit=20` | the best time per name, fastest first |
| GET | `/v1/ghost?track=N&laps=L&rank=1` | the ghost of that rank, base64 |
| OPTIONS | both | CORS preflight (`*`, GET/POST, `Content-Type`) |

`laps` defaults to 0 (the track's default, as in a room's settings). `track` and `laps` are bytes,
the game's own numbers (track table index; finding 115's settings use the same).

## Storage

One `TrackTimes` Durable Object per track and lap count (`idFromName("track-N-laps-L")`), with
SQLite storage like `Room` and `LobbyIndex`; `wrangler.jsonc` binds it as `TIMES` and adds the
migration `v2` (`new_sqlite_classes: ["TrackTimes"]`; `v1` stays as it was, as the README asks).
One object per board keeps every board's writes serial and its queries local; the boards are
small.

- `times`: one row per name (`name_key`, the name lower-cased, so "Ann" and "ann" are one
  entry; the name as last posted is shown), the time, when, the build, the release, the ISO hash
  and the ghost (BLOB). A post replaces the row only when it is faster. Ties go to the earlier post.
- At most 1000 rows per board (`MAX_ROWS`); only the best 100 keep their ghost (`GHOSTS_KEPT`), so
  a board holds at most about 12.5 MB of ghosts.
- `posts`: (address hash, time) for the rate limit, pruned on every post.

## Validation

- **Version gate** as `hello`'s: `clientVersion` must be a release and at least `MIN_VERSION`
  (403 `client_outdated` with both versions, as the room's refusal carries). Unlike hello there is
  no 0.1.0 fallback: a post without `clientVersion` is a bad request, since every release that
  can post will send one. `version` (the build string) 1-64 characters, as hello.
- `isoSha256`: 64 lowercase hex, as hello. Stored, not compared: the server does not know the
  USA disc's hash (as for rooms, `README.md`).
- **Name**: hello's rule (1-24 UTF-16 units after trimming) and no control characters, since a
  board shows it to everyone (the chat's character class, finding 139).
- **Time**: a whole number of ms from 10 s to 30 min (`MIN_TIME_MS`, `MAX_TIME_MS`): no run is
  that short, nobody posts one that long.
- **Ghost**: standard base64, at most 128 KiB decoded (`MAX_GHOST_BYTES`, 413 `too_large`); the
  body at most 180 KiB (`MAX_BODY_BYTES`, checked from `Content-Length` and again on the text).
  The real size of a ghost is not known yet (the game side has to pick a replay format); the cap
  is one constant.
- **Rate limit**: 6 posts per address per board per 10 minutes (429 `slow_down`). The address is
  `cf-connecting-ip`, kept only as a truncated SHA-256 (`addressHash`), never as the address.
  Per board rather than global keeps it in the one object the post goes to anyway; a global
  limit would need its own Durable Object (or Cloudflare's rate-limiting binding).

Errors are JSON `{"error","message"}` with the HTTP status; every answer has
`Access-Control-Allow-Origin: *`, as `/v1/lobbies`, so the browser build can use it.

## Times are not verified

Anyone who can send an HTTP request can post any time between 10 s and 30 min under any name,
with any ghost. The version gate, the ISO hash and the name rules only keep honest clients
consistent; they are no proof. Names are not owned either: anyone may post as "Ann" (a faster
fake replaces her row). The rate limit only slows a flood from one address.

What verification would take (out of scope here):

1. The post carries the run's full input replay (every field's pad state from the start, the
   seed, the rider and bike, the game build), not just a ghost to look at.
2. A headless build of the game (the port runs without a window: `bench_race.py`,
   `race_compare.py`) replays it on the server side, deterministically, and reads the finish
   time from the race state. Only a time it reproduces is listed. The replay must be
   deterministic across machines and frame rates (the port races at 60-240 fps, finding 114),
   which nobody has shown yet.
3. That means a job queue and machines able to run the game (it needs the player's own disc
   files, which the server must not hold: a verifier would have to run where the files are
   legally present), so it does not fit in a Worker.

Lighter steps that help without proving anything: listing times "unverified" in the game, an
owner-run delete endpoint (needs a secret, which must not be committed), or names tied to a
key the client keeps.

## Tests

`npx vitest run` in `online/server`: 55 of 55 (the 50 room tests and 5 new in
`test/times.test.ts`):

1. `keeps the best time per name, fastest first, with ranks`: slower posts don't replace,
   faster ones do, case-insensitive names, `limit`, another lap count is another board.
2. `stores a ghost and hands it back by rank`: 5000 bytes round-trip; 404 without a ghost or a rank.
3. `refuses outdated releases, bad fields, implausible times and big ghosts`: 403
   `client_outdated` for 0.1.0; 400 for a missing release, bad ISO, track 256, laps -1, empty,
   25-character and tab names, 9.999 s, 30 min + 1 ms, a fractional time, bad base64; 413 for a
   ghost one byte over; a ghost of exactly the cap is taken.
4. `limits posts per address`: the 7th post in the window gets 429; another address is fine.
5. `checks the query and answers CORS preflights`.

`npm run typecheck` passes. Not deployed: the `v2` migration runs on the next `wrangler deploy`.

## For the game side

- Record a ghost from the game's replay data, keep it under 128 KiB (or raise the cap), base64 it.
- Post after a finished time trial with the same `version`, `clientVersion`, `isoSha256` and name
  the online menu uses; show `rank` and `best` from the answer.
- Read `/v1/times` for a board screen and `/v1/ghost` to race against one. `net_list_lobbies`'s
  `http_get` is GET only: posting needs a POST in `ws.h`'s Rust crate (`dhd_net`) and in the
  browser's `net.js` (fetch).
- Tracks are the game's indices; a board per lap count, as rooms use `laps`.
