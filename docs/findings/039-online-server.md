# 039: Online server (lobbies and the race relay)

Status: partly superseded by 044 (INPUT/HASH lockstep relay, input delay, iso SHA-256)
Checked: 2026-11-04

Do not add input delay, INPUT or HASH frames, or any tick agreement between PCs: races are state
sync (finding 044), and the relay forwards STATE (5) and EVENT (6) frames
(`docs/online-protocol.md`). Still true: the Worker + Durable Object rooms, the public list, the
hibernation and "no awaits on the relay path" rules. Changed since: rooms hold 10 (finding 123),
the disc check is a hash of the disc's game files (finding 098), rooms live across races
(finding 119), rejoins use a token (finding 137).

Input lockstep was dropped on 2026-10-16; races use state sync
(findings 041, 044). The lobby and relay here are still what runs.

2026-10-16. Code in `online/server/`, protocol in `docs/online-protocol.md`, plan in
`docs/ONLINE-PLAN.md`.

## What was built

- Cloudflare Worker with two Durable Object classes: `Room` (one per 6-character join code)
  and `LobbyIndex` (a single global object holding the public list). SQLite-backed, as
  current Cloudflare guidance recommends for new classes.
- Lobby over JSON text frames: hello with protocol number, build version and ISO SHA-256
  (joiners must match the host), host-only settings, rider/bike picks, ready flags, start
  with slots `0..N-1` (host first), seed and a suggested input delay, kick, end.
- Race over binary frames: INPUT is relayed to the other racers with the sender's slot
  forced into byte 1; HASH is compared on the server; PING is echoed as PONG.
- Leaving mid-race broadcasts `left` with `fromField`, one past the last field the server
  relayed for that slot. Because each client receives the same ordered relay stream, that
  field is consistent on every PC.

## Decisions and why

- **Input delay from the two worst RTTs.** A relayed input goes sender → server → receiver,
  so the worst one-way time between two players is half the sum of the two largest
  player-to-server round-trips, not half of the single worst one. RTT per member is the
  median of its last five server pings.
- **Hibernation API, but the room stays awake in a race.** The room's 2-second alarm (lobby
  pings, hello timeouts, lobby list refresh) keeps running during a race, so the object is
  not evicted while racers send 60 frames a second, and the relay never pays a wake-up.
  If it is evicted anyway (only after seconds without any traffic), sockets survive and the
  relay state is rebuilt from the socket attachments; only `fromField` is lost until the
  racer's next frame, which the protocol covers with `fromField: null`. The test
  "keeps relaying after the room is evicted" checks this with `evictDurableObject`.
- **No awaits on the relay path.** Binary frames are handled synchronously: the slot byte
  is fixed in place and the same `ArrayBuffer` is sent to each other racer.

## Sources

- Cloudflare docs (via the Cloudflare docs MCP search): Durable Objects WebSocket
  hibernation, testing Durable Objects, and the Vitest integration. The integration
  moved from `@cloudflare/vitest-pool-workers` (`defineWorkersConfig`) to
  `@cloudflare/vitest-plugin` (`cloudflareTest()` plugin, `exports` from
  `cloudflare:workers` instead of `SELF`); this project uses the new package.
- In the workerd test runtime, client-side WebSockets deliver binary messages as `Blob`
  (no `binaryType`); the test helper converts them.

## Results

`npm test`: 25 tests pass (protocol helpers, desync tracker, lobby rules and refusals, lobby
list, relay, desync event, leave/kick, eviction). Local `wrangler dev` load test with 4
simulated lockstep players and 600 fields: every input delivered, no stalls, relay one-way
median 2.5 ms and p99 9 ms; 8 players and 300 fields also clean; the `--desync` run gets
exactly one `desync` event at field 299.
