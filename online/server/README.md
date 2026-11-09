# Online server

Lobbies, join codes and each race's Realtime setup for the PC port's online races. Cloudflare
Workers + Durable Objects, TypeScript. The wire protocol is `docs/online-protocol.md`; the plan
is `docs/ONLINE-PLAN.md`.

| File | What |
|---|---|
| `src/index.ts` | Worker entry: routes `/v1/lobbies`, `/v1/health`, `/v1/latest` and room WebSockets. |
| `src/latest.ts` | `/v1/latest`: the newest release's `latest.json`, fetched from GitHub, checked and kept; the launcher's backup update check (finding 138). |
| `src/room.ts` | `Room` Durable Object, one per join code: lobby, start, the race's Realtime setup, leave handling. |
| `src/sfu.ts` | The Realtime SFU control plane (sessions, DataChannels) the room drives (finding 158). |
| `src/lobbies.ts` | `LobbyIndex` Durable Object (a single global one): the public lobby list. |
| `src/protocol.ts` | Constants, join codes, the version gate. |
| `src/guard.ts` | The abuse guard (finding 161): kill switch, limits per address, caps per member, bans, the moderation log. |
| `migrations/` | The moderation store's tables (D1). |
| `tools/moderate.mjs` | The operator's moderation tool: the log, bans (through `wrangler d1 execute`). |
| `src/rollout.ts` | Code updates at a safe moment: when a room restarts itself for new code, and the notice it sends (finding 184). |
| `tools/deploy.mjs` | The operator's deploy: checks, a 10% canary watched in Workers Logs, then 100% or a rollback (finding 184). |
| `test/` | Vitest suite, run inside the Workers runtime (`@cloudflare/vitest-plugin`). |
| `tools/loadtest.mjs` | The WebSocket side of a race (binary PINGs, the caps) against a running server; reports the PING round trip. Race frames travel over the SFU (finding 158): `port/render/net/src/probe.rs` measures them. |

Rooms use the WebSocket hibernation API, so an idle lobby costs little. Race frames do not go
through the room: since protocol 3 they travel over Cloudflare Realtime SFU DataChannels, and the
room only creates each member's session (one per room stay, finding 177) and channels (finding 158).

## Develop and test

Node 22 or newer.

```sh
cd online/server
npm install
npm test                 # unit + integration tests (workerd, no network)
npm run typecheck        # generates src/worker-configuration.d.ts, then tsc
npm run dev              # local server on http://127.0.0.1:8787
npm run loadtest         # in a second terminal: 4 players, 10 s
node tools/loadtest.mjs --players 10 --seconds 30 --rate 2
node tools/loadtest.mjs --url wss://dhd-online.<account>.workers.dev   # against a deployment
```

The load test connects every player from one address: the limit per address (30 room
connections a minute, `wrangler.jsonc` `ratelimits`) refuses runs past that (`rate_limited`).

Race frames no longer pass through this server (protocol 3 and later: Cloudflare Realtime SFU,
below); the load test measures the room's messages. `port/render/net/src/probe.rs` measures the SFU
path (findings 158, 177).

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

`wrangler deploy` prints the URL, `https://dhd-online.<account-subdomain>.workers.dev`. (The
official server deploys with `tools/deploy.mjs`: [New versions](#new-versions-without-cutting-races),
below.) Players
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

   Add its binding to `wrangler.jsonc` with the id `d1 create` printed, and commit it: a
   database id is not a secret (it is useless without the account's login), like the `routes`
   entry. Once the official server's binding is in the file, a self-hosted server replaces
   `database_id` with its own database's id, as it replaces `routes`:

   ```jsonc
   "d1_databases": [
     { "binding": "MODERATION", "database_name": "dhd-moderation", "database_id": "<id from d1 create>", "migrations_dir": "migrations" }
   ],
   ```

   Then the tables (also after a pull that adds a file to `migrations/`):

   ```sh
   npx wrangler d1 migrations apply dhd-moderation --remote
   ```
3. Deploy (not during races). `npx wrangler tail` shows `guard:` lines when the store can't be
   read (players are let in then) and the daily cleanup's counts.
4. Check: `node tools/moderate.mjs online` says "Online is on." (and `bans` lists none).

Never deploy a binding without its `database_id`: wrangler would then make a new, empty database
without the tables.

A store that can't be read lets players in (availability first) and logs it; the log keeps 14
days (the daily cron in `wrangler.jsonc` deletes older rows and ended bans). Each server isolate
keeps a ban lookup (banned or not) for up to 60 s, so a connection costs no database read in the
common case: a ban you just added may take up to a minute to bite, and a lifted one as long to
go. Someone already in a room stays until they leave; the ban stops their next connection.

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

Live logs: `npx wrangler tail`. A new version reaches a busy room only between its races
(below).

The Durable Object classes are created by the `v1` migration in `wrangler.jsonc`. Renaming or
removing a class later needs a new migration entry, not an edit of `v1`.

### New versions without cutting races

A busy room never hibernates (an alarm every 2 s, a game message every second), so Cloudflare would
reset it at the end of `code_update_strategy`'s `max_delay` (`wrangler.jsonc`: 24 h), at any
moment. Instead the room restarts itself at a safe moment (`src/rollout.ts`, finding 184): no race,
no locked grid or countdown, no Realtime setup in flight, no automatic race due within 10 s. It
saves, sends its players `{"type":"notice","code":"server_update"}` (the game shows "Server
updated, reconnecting..." as the room page's footer; 0.7.13 and older show nothing), closes their
sockets with 1012 and resets; the games rejoin by token within about a second and keep their seats
and Realtime sessions. A room learns which version is deployed from the rollout row (the moderation
store's `settings` key `rollout`, which `tools/deploy.mjs` writes: the version at 100%, or empty
while a gradual deployment runs, when no room moves), or, without the row, from the Worker's own
version on its requests (newer than the room's: restart). Empty rooms simply start on the new code.

The official server deploys with one command, from `online/server`, under the operator's login:

```sh
export CLOUDFLARE_ACCOUNT_ID=<the account that runs the server>
export DHD_LOGS_TOKEN_FILE=<a file holding a read-only Workers Observability token>   # or .deploy.json
node tools/deploy.mjs                 # check.py server, upload, 10% for 10 min, then 100% or back
node tools/deploy.mjs --dry-run       # the plan only
node tools/deploy.mjs --emergency     # 100% now with immediate updates: every room restarts, races too
```

1. `python tools/check.py server` (nothing is uploaded if it fails), then `wrangler versions upload`.
2. The rollout row is set to "hold", then `wrangler versions deploy NEW@10% OLD@90%` with deferred
   updates: Cloudflare gives about 10% of the Durable Objects the new version (new rooms start on
   it; busy ones assigned to it keep the old code meanwhile, since no room moves while split).
3. For `--watch` minutes (10) it reads Workers Logs every `--poll` seconds (60) and compares the new
   version with the old over the same minutes: error lines, invocations ending in an exception,
   "out of the race" lines and objects (`tools/rollout-verdict.mjs`). The new version fails when
   one of these is at least 3 and above twice the old rate plus a margin.
4. Then `NEW@100%` (or `OLD@100%` on a failure, or Ctrl-C) and, 20 s later, the rollout row: from
   then on every busy room on the other version restarts at its next safe moment. A summary ends
   the run; the exit code is 1 after a rollback. With too little traffic on the canary it says so
   and goes on (`--require-traffic`: roll back instead).

`.deploy.json` (gitignored) can hold `{"logsTokenFile": "<path>"}` instead of the variable; the
tool reads the token and never prints it. A server without the MODERATION database has no rollout
row: deploy it with `npx wrangler deploy` (100%; rooms follow the newer Worker version) or
`--emergency`. Rooms that a deploy left on the old version (a restart that came back on it) try
again after 2, 10 and 30 minutes, then wait for Cloudflare's 24 h.

## Limits and what is not there yet

- A racer whose room connection drops races on over the SFU and has 20 s to rejoin (findings 137,
  175); one not back by then becomes the host's AI rider for the rest of the race.
- No authentication: the game is open source, so any client can connect. The abuse guard
  (finding 161) limits what one can do: limits per address (`ratelimits` in `wrangler.jsonc`),
  the kill switch, per-player caps in a room, and bans.
- The server does not check the ISO hash against the known US ISO; the client must refuse
  other regions, and the server only makes everyone match the host.

## Realtime SFU (since protocol 3)

The deploy needs four Worker secrets (values from the Cloudflare dashboard, Realtime; never in a
tracked file, a log or a client message):

| Secret | What |
|---|---|
| `REALTIME_APP_ID` | the Realtime SFU app's id |
| `REALTIME_APP_TOKEN` | that app's API token |
| `TURN_KEY_ID` | the TURN key's id (Realtime > TURN), for the fallback over TLS on 443 |
| `TURN_KEY_API_TOKEN` | that TURN key's API token |

```sh
npx wrangler secret put REALTIME_APP_ID      # paste each value when asked
npx wrangler secret put REALTIME_APP_TOKEN
npx wrangler secret put TURN_KEY_ID
npx wrangler secret put TURN_KEY_API_TOKEN
```

Without the TURN pair, races still run over UDP; a player whose network blocks UDP gets no relay
(the room logs "TURN credentials unavailable") and leaves each race 10 s after its start; the others
race on. Local development reads all four from the gitignored `.dev.vars`. Lobby tests use a fake
API and do not need credentials.
The DO owns session and channel creation; clients connect only to Cloudflare. See
[the protocol](../../docs/online-protocol.md) and finding 158 for setup, tests and live acceptance.

## P2P rooms (finding 180, test builds)

A room's creator can pick P2P instead of the SFU: the games connect to the host's game directly
(Cloudflare TURN over TLS on 443 when no direct path works), and the host's game runs the room.
The server only admits members, relays host-star signaling and gives each member its own TURN
credentials (`src/p2p.ts`). P2P rooms are private, joined by code and never listed: signaling
gives the host every member's network address and every member the host's.

A self-hosted server serves `/v1/p2p/*` from its one Worker. `wrangler.jsonc` is the official main
Worker's config and has no P2P rooms (the official server's are their own Worker, below, and
`tools/deploy.mjs` uploads versions, which Cloudflare refuses for a version that creates a Durable
Object class), so a self-hosted server deploys `wrangler.jsonc` plus the P2P binding and its
migration, which `tools/selfhost-config.mjs` writes as `wrangler.selfhost.jsonc` (gitignored; run it
again after editing `wrangler.jsonc`):

```sh
node tools/selfhost-config.mjs
npx wrangler deploy --config wrangler.selfhost.jsonc
```

Without it, `/v1/p2p/*` answers "This server has no P2P rooms". Local test servers
(`port/tools/local_server.py`, `npm run dev`, `npm run dev:guard`) use the same config.

The official server runs P2P rooms as a Worker of their own, so deploying them never restarts an
SFU room:

```sh
npx wrangler deploy --config wrangler.p2p.jsonc
npx wrangler secret put TURN_KEY_ID --config wrangler.p2p.jsonc         # the same values as above
npx wrangler secret put TURN_KEY_API_TOKEN --config wrangler.p2p.jsonc
npx wrangler secret put IP_HASH_KEY --config wrangler.p2p.jsonc
```

Its route (`dhd.skuza.app/v1/p2p/*`) takes those paths ahead of the main Worker's custom domain.
Relayed P2P races use the account's TURN quota: set a billing alert (finding 180).
