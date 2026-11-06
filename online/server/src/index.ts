// Worker entry: routes lobby-list requests to the LobbyIndex and WebSockets to a Room by code.
import { PROTOCOL, minVersionOf, randomCode } from "./protocol";
import { edgeStore, latestResponse, upstreamFetch } from "./latest";
import { locationHintFor, placeOf, regionOfPlace, sortForRegion } from "./region";
import {
  RATE_MESSAGE, banMessage, cleanup, findBan, ipHash, onlineState, overLimit, refuseSocket, type GuardEnv,
} from "./guard";

export { Room } from "./room";
export { LobbyIndex } from "./lobbies";

const CORS = { "Access-Control-Allow-Origin": "*" };

function json(body: unknown, status = 200): Response {
  return Response.json(body, { status, headers: CORS });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const path = new URL(request.url).pathname.replace(/\/+$/, "");

    // minVersion: the oldest client release the server lets play (MIN_VERSION, wrangler.jsonc).
    const minVersion = minVersionOf(env.MIN_VERSION);
    // The kill switch (finding 161: tools/moderate.mjs online off, kept 30 s per isolate): no new
    // rooms, joins or races, and the health check and the lobby list say so.
    const lookup = path === "/v1/health" || path === "/v1/lobbies" || path.startsWith("/v1/rooms/");
    const sw = lookup ? await onlineState(env as GuardEnv) : null;
    const off = sw !== null && !sw.on;
    const offState = off ? { online: false, message: sw.message } : { online: true };
    if (path === "/v1/health") return json({ ok: true, protocol: PROTOCOL, minVersion, ...offState });

    // The newest release's latest.json, the launcher's backup when GitHub's API can't be reached
    // (src/latest.ts, finding 138).
    if (path === "/v1/latest") {
      if (request.method !== "GET") return json({ error: "GET only" }, 405);
      const cacheKey = new URL("/v1/latest/kept", request.url).toString();
      return latestResponse({ fetchUpstream: upstreamFetch, store: edgeStore(cacheKey), now: Date.now });
    }

    if (path === "/v1/lobbies") {
      if (request.method !== "GET") return json({ error: "GET only" }, 405);
      // region: the asker's, as Cloudflare places them; their region's rooms come first (finding 135).
      const region = regionOfPlace(placeOf(request));
      if (off) return json({ protocol: PROTOCOL, minVersion, region, now: Date.now(), lobbies: [], ...offState });
      const lobbies = sortForRegion(await env.LOBBIES.get(env.LOBBIES.idFromName("global")).list(), region);
      // now: the server's clock (ms since the Unix epoch). A client in no room yet has no clock
      // offset from a room's pings; without this its list counted "ENDS IN" from the epoch (0.7.6
      // showed ENDS IN 29893031:32; finding 135).
      return json({ protocol: PROTOCOL, minVersion, region, now: Date.now(), lobbies });
    }

    const m = path.match(/^\/v1\/rooms\/([^/]+)$/);
    if (m) {
      if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
        return json({ error: "Expected a WebSocket upgrade" }, 426);
      // Off: no new room. A join by code still reaches its room, which lets only its own members
      // back in (a dropped racer rejoining a race that goes on) and refuses the rest at hello.
      if (off && m[1] === "new") return refuseSocket("online_off", sw.message);
      // Limits per connection source (finding 161), by the address's keyed hash, before any room.
      const ip = await ipHash(env as GuardEnv, request);
      if (await overLimit(env as GuardEnv, ip, m[1] === "new")) return refuseSocket("rate_limited", RATE_MESSAGE);
      // A ban on this address, before any room (an install id's is checked at hello, in the room).
      const ban = await findBan(env as GuardEnv, { ip });
      if (ban) return refuseSocket("banned", banMessage(ban));
      const headers = new Headers(request.headers);
      headers.delete("x-dhd-create");
      // The address's hash for the room's moderation log (src/room.ts); never one a client sent.
      headers.delete("x-dhd-ip-hash");
      if (ip) headers.set("x-dhd-ip-hash", ip);
      if (m[1] === "new") {
        headers.set("x-dhd-create", "1");
        // A new room runs near its creator: the hint from their continent and country, not from the
        // Cloudflare location their ISP happened to reach (finding 135). Only the first get() of an
        // object reads a hint; joining by code below needs none.
        const place = placeOf(request);
        const hint = locationHintFor(place);
        for (let attempt = 0; attempt < 8; attempt++) {
          const code = randomCode();
          const room = env.ROOM.get(env.ROOM.idFromName(code), hint ? { locationHint: hint } : undefined);
          if (await room.claim(code, { hint: hint ?? "", createdIn: regionOfPlace(place), createdFrom: place.colo ?? "" }))
            return room.fetch(new Request(request, { headers }));
        }
        return json({ error: "Could not allocate a room code" }, 503);
      }
      const code = m[1].toUpperCase();
      if (!/^[A-Z0-9]{1,12}$/.test(code)) return json({ error: "Bad room code" }, 404);
      return env.ROOM.get(env.ROOM.idFromName(code)).fetch(new Request(request, { headers }));
    }

    return json({ error: "Not found" }, 404);
  },

  // Daily (wrangler.jsonc triggers): the moderation log's rows older than 14 days and ended bans go
  // (finding 161).
  async scheduled(_controller: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    ctx.waitUntil(
      cleanup(env as GuardEnv).then(
        (n) => console.log(`guard: cleanup removed ${n.joins} joins and ${n.bans} ended bans`),
        (e) => console.error("guard: cleanup failed", e),
      ),
    );
  },
} satisfies ExportedHandler<Env>;
