// Worker entry: routes lobby-list requests to the LobbyIndex, WebSockets to a Room by code, and the
// time-trial boards (finding 127) to a TrackTimes per track.
import { PROTOCOL, isByte, minVersionOf, randomCode } from "./protocol";
import { DEFAULT_LIMIT, MAX_BODY_BYTES, MAX_LIMIT, type PostResult, boardKey, checkPost } from "./times";

export { Room } from "./room";
export { LobbyIndex } from "./lobbies";
export { TrackTimes } from "./times";

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
};

/** A query parameter as a whole number in [min, max]; `fallback` when absent, null when bad. */
function intParam(url: URL, name: string, min: number, max: number, fallback?: number): number | null {
  const v = url.searchParams.get(name);
  if (v === null || v === "") return fallback ?? null;
  if (!/^\d{1,6}$/.test(v)) return null;
  const n = Number(v);
  return n >= min && n <= max ? n : null;
}

/** The poster's address as a hash: the rate limit counts posts per address, and no address is kept. */
async function addressHash(request: Request): Promise<string> {
  const ip = request.headers.get("cf-connecting-ip") ?? "unknown";
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`dhd-times ${ip}`));
  return [...new Uint8Array(digest).slice(0, 12)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function times(request: Request, env: Env, url: URL, path: string, minVersion: string): Promise<Response> {
  const board = (track: number, laps: number) => env.TIMES.get(env.TIMES.idFromName(boardKey(track, laps)));
  if (path === "/v1/times" && request.method === "POST") {
    const length = Number(request.headers.get("content-length") ?? "0");
    if (length > MAX_BODY_BYTES) return json({ error: "too_large", message: "Body too large." }, 413);
    const text = await request.text();
    if (text.length > MAX_BODY_BYTES) return json({ error: "too_large", message: "Body too large." }, 413);
    let body: unknown;
    try {
      body = JSON.parse(text);
    } catch {
      return json({ error: "bad_request", message: "Invalid JSON." }, 400);
    }
    const checked = checkPost(body, minVersion);
    if (!checked.ok) return json(checked.body, checked.status);
    const p = checked.post;
    const r = (await board(p.track, p.laps).post(p, await addressHash(request))) as PostResult;
    if (!r.ok) return json(r.body, r.status);
    return json({ ok: true, track: p.track, laps: p.laps, rank: r.rank, best: r.best, timeMs: r.timeMs });
  }
  if (request.method !== "GET") return json({ error: "GET or POST only" }, 405);
  const track = intParam(url, "track", 0, 255);
  const laps = intParam(url, "laps", 0, 255, 0);
  if (track === null || !isByte(track) || laps === null) return json({ error: "bad_request", message: "track (0-255) and laps (0-255) please." }, 400);
  if (path === "/v1/times") {
    const limit = intParam(url, "limit", 1, MAX_LIMIT, DEFAULT_LIMIT);
    if (limit === null) return json({ error: "bad_request", message: `limit is 1-${MAX_LIMIT}.` }, 400);
    return json({ track, laps, times: await board(track, laps).top(limit) });
  }
  const rank = intParam(url, "rank", 1, 1000, 1);
  if (rank === null) return json({ error: "bad_request", message: "rank is 1-1000." }, 400);
  const g = await board(track, laps).ghost(rank);
  if (!g) return json({ error: "not_found", message: "No ghost for that rank." }, 404);
  return json({ track, laps, ...g });
}

function json(body: unknown, status = 200): Response {
  return Response.json(body, { status, headers: CORS });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname.replace(/\/+$/, "");

    // minVersion: the oldest client release the server lets play (MIN_VERSION, wrangler.jsonc).
    const minVersion = minVersionOf(env.MIN_VERSION);
    if (path === "/v1/health") return json({ ok: true, protocol: PROTOCOL, minVersion });

    if (path === "/v1/times" || path === "/v1/ghost") {
      if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
      return times(request, env, url, path, minVersion);
    }

    if (path === "/v1/lobbies") {
      if (request.method !== "GET") return json({ error: "GET only" }, 405);
      const lobbies = await env.LOBBIES.get(env.LOBBIES.idFromName("global")).list();
      return json({ protocol: PROTOCOL, minVersion, lobbies });
    }

    const m = path.match(/^\/v1\/rooms\/([^/]+)$/);
    if (m) {
      if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
        return json({ error: "Expected a WebSocket upgrade" }, 426);
      const headers = new Headers(request.headers);
      headers.delete("x-dhd-create");
      if (m[1] === "new") {
        headers.set("x-dhd-create", "1");
        for (let attempt = 0; attempt < 8; attempt++) {
          const code = randomCode();
          const room = env.ROOM.get(env.ROOM.idFromName(code));
          if (await room.claim(code)) return room.fetch(new Request(request, { headers }));
        }
        return json({ error: "Could not allocate a room code" }, 503);
      }
      const code = m[1].toUpperCase();
      if (!/^[A-Z0-9]{1,12}$/.test(code)) return json({ error: "Bad room code" }, 404);
      return env.ROOM.get(env.ROOM.idFromName(code)).fetch(new Request(request, { headers }));
    }

    return json({ error: "Not found" }, 404);
  },
} satisfies ExportedHandler<Env>;
