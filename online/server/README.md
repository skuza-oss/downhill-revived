# Online server

Lobbies, join codes and the race relay for the PC port's online races. Cloudflare
Workers + Durable Objects, TypeScript. The wire protocol is `docs/online-protocol.md`; the plan
is `docs/ONLINE-PLAN.md`.

| File | What |
|---|---|
| `src/index.ts` | Worker entry: routes `/v1/lobbies`, `/v1/health`, `/v1/latest` and room WebSockets. |
| `src/latest.ts` | `/v1/latest`: the newest release's `latest.json`, fetched from GitHub, checked and kept; the launcher's backup update check (finding 138). |
| `src/room.ts` | `Room` Durable Object, one per join code: lobby, start, race relay, leave handling. |
| `src/lobbies.ts` | `LobbyIndex` Durable Object (a single global one): the public lobby list. |
| `src/protocol.ts` | Constants, join codes, the version gate. |
| `src/guard.ts` | The abuse guard (finding 161): kill switch, limits per address, caps per member, bans, the moderation log. |
| `migrations/` | The moderation store's tables (D1). |
| `tools/moderate.mjs` | The operator's moderation tool: the log, bans (through `wrangler d1 execute`). |
| `test/` | Vitest suite, run inside the Workers runtime (`@cloudflare/vitest-plugin`). |
| `tools/loadtest.mjs` | Simulated race (STATE frames, 20 a second per player) against a running server; reports relay latency. |

Rooms use the WebSocket hibernation API, so an idle lobby costs little. The relay path does
no awaits and no copies: it fixes the slot byte and sends the received buffer to the other
racers.

## Develop and test

Node 22 or newer.

```sh
cd online/server
npm install
npm test                 # unit + integration tests (workerd, no network)
npm run typecheck        # generates src/worker-configuration.d.ts, then tsc
npm run dev              # local server on http://127.0.0.1:8787
npm run loadtest         # in a second terminal: 4 players, 10 s
node tools/loadtest.mjs --players 8 --seconds 30 --bytes 3000
node tools/loadtest.mjs --url wss://dhd-online.<account>.workers.dev   # against a deployment
```

The load test connects every player from one address: the limit per address (30 room
connections a minute, `wrangler.jsonc` `ratelimits`) refuses runs past that (`rate_limited`).

Measured with `wrangler dev` on the dev PC (4 players, 4 s, 600-byte STATE frames): every
frame arrived whole and in order, relay one-way (sender → server → receiver) median 2.2 ms,
p99 4.7 ms; binary PING round-trip median 3.8 ms. These are local numbers: on the internet add
the players' own round-trips to the nearest Cloudflare location.

## Deploy

The official server is `wss://dhd.skuza.app`; the game uses it
when `dhd.ini` names no other. Running your own is optional. You need a Cloudflare account
(the free plan supports Durable Objects with SQLite storage):

```sh
cd online/server
npm install
npx wrangler login       # opens the browser once; stores the token in your user profile, not here
npx wrangler deploy
```

`wrangler deploy` prints the URL, `https://dhd-online.<account-subdomain>.workers.dev`. Players
put `server = wss://dhd-online.<account-subdomain>.workers.dev` in their `dhd.ini`. The client
then uses:

- create a room: `wss://dhd-online.<account-subdomain>.workers.dev/v1/rooms/new`
- join a room: `wss://dhd-online.<account-subdomain>.workers.dev/v1/rooms/<CODE>`
- lobby list: `https://dhd-online.<account-subdomain>.workers.dev/v1/lobbies`

Check it with `curl https://dhd-online.<account-subdomain>.workers.dev/v1/health`, then run
the load test with `--url wss://...`. A custom domain can be added later under the Worker's
*Settings → Domains & Routes* in the dashboard; the paths stay the same.

### Minimum client version

`MIN_VERSION` under `vars` in `wrangler.jsonc` (`MAJOR.MINOR.PATCH`) is the oldest game release
the server lets in; older ones get `client_outdated` (close 4009) and the message "A new version
is out. Update to play online." (`docs/online-protocol.md`). 0.1.0 sends no version and
counts as 0.1.0. Raise it after a release is published, so players update, and deploy again;
`/v1/health` shows the value in force. For a one-off change without editing the file:
`npx wrangler deploy --var MIN_VERSION:0.2.0`.

### Abuse guard: one-time setup (finding 161)

The limits per address (`ratelimits`) and the caps per member need nothing beyond
`wrangler.jsonc`. The kill switch, bans and the moderation log need a D1 database and a secret,
made once by the maintainer with their own login (`npx wrangler login`). Until they exist the
server runs with online always on, without bans and log; without the secret, address hashes
change with every isolate.

1. The secret for the keyed address hash (64 random hex digits; keep no copy in the repository):

   ```sh
   node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
   npx wrangler secret put IP_HASH_KEY       # paste the value
   ```

   A new value later makes every stored hash useless (address bans stop matching): rotate it
   only to forget them all.
2. The database and its tables:

   ```sh
   npx wrangler d1 create dhd-moderation
   ```

   Add its binding to `wrangler.jsonc` (the id is the one `d1 create` printed):

   ```jsonc
   "d1_databases": [
     { "binding": "MODERATION", "database_name": "dhd-moderation", "database_id": "<id>", "migrations_dir": "migrations" }
   ],
   ```

   ```sh
   npx wrangler d1 migrations apply dhd-moderation --remote
   ```
3. Deploy (not during races). `npx wrangler tail` shows `guard:` lines when the store can't be
   read (players are let in then) and the daily cleanup's counts.

A store that can't be read lets players in (availability first) and logs it; the log keeps 14
days (the daily cron in `wrangler.jsonc` deletes older rows and ended bans).

### Moderation

`tools/moderate.mjs` (also `npm run moderate -- ...`) reads the log, changes bans and turns online
off and on (below) in the deployed database through `wrangler d1 execute` under your login; there
is no admin endpoint.
Bans go by address hash or install id, never by name (anyone can take any name):

```sh
node tools/moderate.mjs joins --name alexander --since 2026-11-05   # who, when, which room
node tools/moderate.mjs joins --like 812          # every row with row 812's address or install id
node tools/moderate.mjs ban 812 --days 14 --reason "impossible times"
node tools/moderate.mjs bans                      # bans in force
node tools/moderate.mjs unban 3
```

`ban` takes a row's address hash and install id (`--ip-only`, `--install-only` for one),
30 days unless `--days`, `--hours`, `--until` or `--forever` says otherwise, and names the other
players the log has from that address (a household or carrier-grade NAT shares one): ban those
cases with `--install-only`. Hashes and install ids are printed only with `--hashes`. Run with no
arguments for the full help.

Locally: `npm run dev:guard` writes `wrangler.guard-local.jsonc` (gitignored: `wrangler.jsonc`
with a local moderation database), applies the migrations to the local store and starts
`wrangler dev` with it; `node tools/moderate.mjs ... --local` works on that store.

### Turning online off

The kill switch (finding 161) is a row in the moderation database, set with the moderation tool;
no deploy, so no room restarts:

```sh
node tools/moderate.mjs online off                          # the default message
node tools/moderate.mjs online off --message "Back at 18:00 UTC."
node tools/moderate.mjs online                              # show the state
node tools/moderate.mjs online on
```

While off, the server refuses new rooms, joins and race starts with a plain message on the game's
ONLINE screen ("Online races are paused. Try again later.", or `--message`, within about 45
characters), and `/v1/health` and `/v1/lobbies` show `"online":false`. Rooms already open stay:
their members are not kicked and may rejoin after a drop, a race already running finishes, but no
new race starts and nobody new gets in. Each server isolate keeps the state it read for up to
30 s, so a change bites within that long. Without the database (one-time setup above) there is no
kill switch: online is on.

Live logs: `npx wrangler tail`. Redeploying disconnects every open room (Durable Objects
restart on a new version), so don't deploy during races.

The Durable Object classes are created by the `v1` migration in `wrangler.jsonc`. Renaming or
removing a class later needs a new migration entry, not an edit of `v1`.

## Limits and what is not there yet

- No reconnect: a dropped racer becomes the host's AI rider for the rest of the race.
- No authentication: the game is open source, so any client can connect. The abuse guard
  (finding 161) limits what one can do: limits per address (`ratelimits` in `wrangler.jsonc`),
  the kill switch, per-player caps in a room, and bans.
- The server does not check the ISO hash against the known US ISO; the client must refuse
  other regions, and the server only makes everyone match the host.
