# Online server

Lobbies, join codes and the race relay for the PC port's online races. Cloudflare
Workers + Durable Objects, TypeScript. The wire protocol is `docs/online-protocol.md`; the plan
is `docs/ONLINE-PLAN.md`.

| File | What |
|---|---|
| `src/index.ts` | Worker entry: routes `/v1/lobbies`, `/v1/health` and room WebSockets. |
| `src/room.ts` | `Room` Durable Object, one per join code: lobby, start, race relay, leave handling. |
| `src/lobbies.ts` | `LobbyIndex` Durable Object (a single global one): the public lobby list. |
| `src/protocol.ts` | Constants, join codes, the version gate. |
| `src/times.ts` | `TrackTimes` Durable Object, one per track and lap count: time-trial boards and ghosts (`/v1/times`, `/v1/ghost`, finding 127). |
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

Live logs: `npx wrangler tail`. Redeploying disconnects every open room (Durable Objects
restart on a new version), so don't deploy during races.

The Durable Object classes are created by the `v1` migration in `wrangler.jsonc`, and
`TrackTimes` by `v2`. Renaming or
removing a class later needs a new migration entry, not an edit of `v1`.

## Limits and what is not there yet

- Rejoin: a client that sent the `reconnect` feature keeps its slot, in the lobby or a race, for
  20 s after a drop and comes back with the token its `welcome` carried; the others see `away`,
  then `back` or `left` (`docs/online-protocol.md`, finding 126). Older clients' racers become
  the host's AI at once, as before.
- No authentication or rate limiting beyond the protocol checks (version, ISO hash, sizes, the
  rejoin token, which is unguessable and room-bound).
- Time-trial times are not verified: anyone can post a plausible fake (finding 127 says what
  checking them would take).
- The server does not check the ISO hash against the known US ISO; the client must refuse
  other regions, and the server only makes everyone match the host.
