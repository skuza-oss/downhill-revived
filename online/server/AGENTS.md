# online/server/: the online server (Cloudflare Workers + Durable Objects)

The root `AGENTS.md` rules apply. Protocol: `docs/online-protocol.md`; steps and deployment:
`README.md` here.

- Races are state sync (finding 044). Since protocol 3 the STATE and EVENT frames travel over
  Cloudflare Realtime SFU (TURN over TLS as the fallback); the room only sets up each member's
  session (one per room stay, its subscriptions made at the grid's lock: finding 177) and
  credentials and never sends a player another's address, SDP, session id or credentials
  (finding 158). Do not add input lockstep, input delay or race hashes. Rooms, sessions and
  rules: findings 118, 119, 123, 128, 135, 137; the abuse guard: finding 161; the update backup
  `/v1/latest`: finding 138.
- The official server is `wss://dhd.skuza.app`, the game's default. No other server address in a
  tracked file: `wrangler.jsonc` names no account, and examples use placeholders
  (`dhd-online.<account>.workers.dev`).
- `MIN_VERSION` in `wrangler.jsonc` is the oldest game release the server lets in. Raise it only
  when a release is published and older ones should stop racing; read the value from the file,
  never copy it into docs.
- Deploy with `tools/deploy.mjs` (finding 184), only from the maintainer's account after review
  (deploy notes are kept outside the repository): a 10% canary watched in Workers Logs, then 100%
  or a rollback; busy rooms adopt the new code between races (`src/rollout.ts`). `--emergency`
  (immediate, every room restarts at once) is for urgent fixes only. Worker↔room messages and
  stored state stay readable by the previous version: a rollout runs both.
- Old clients keep connecting: a change to a message must stay readable by the oldest
  `MIN_VERSION` client, or come with a raised minimum.
- Checks: `npm test` (vitest in the Workers runtime) and `npm run typecheck`; `npm run
  loadtest` against `wrangler dev`. Local test servers: stop the `workerd` they leave behind.
