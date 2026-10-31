// Worker entry: routes lobby-list requests to the LobbyIndex, WebSockets to a Room by code, the
// time-trial boards (finding 127) to a TrackTimes per track, and the leaderboards worktree's
// read/submit endpoints to a single global Leaderboard object.
import { PROTOCOL, isByte, isU32, minVersionOf, randomCode } from "./protocol";
import { DEFAULT_LIMIT, MAX_BODY_BYTES, MAX_LIMIT, type PostResult, boardKey, checkPost } from "./times";
import { LEADERBOARD_LIMIT_MAX, LEADERBOARD_MAX, SUBMIT_MAX, SUBMIT_WINDOW_MS, isMode } from "./leaderboard";
import { MAX_RACE_MS, MIN_RACE_MS, REPLAY_MAX_BYTES, validateGhost } from "./replay";

export { Room } from "./room";
export { LobbyIndex } from "./lobbies";
export { TrackTimes } from "./times";
export { Leaderboard } from "./leaderboard";

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

/** The most a submit request body may weigh: the replay limit as base64 plus a little JSON. */
const POST_MAX_BYTES = Math.ceil(REPLAY_MAX_BYTES / 3) * 4 + 4096;

/** Strict base64 (our own encoder pads): rejects anything atob would accept loosely. */
function decodeBase64(s: unknown): Uint8Array | null {
  if (typeof s !== "string" || s.length === 0) return null;
  if (s.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(s)) return null;
  try {
    const bin = atob(s);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  } catch {
    return null;
  }
}

/** The board's read side: `GET /v1/leaderboard?track=N[&laps=M][&mode=tt|race][&limit=K]`. */
async function getLeaderboard(url: URL, env: Env, minVersion: string): Promise<Response> {
  const q = url.searchParams;
  if (!q.has("track")) return json({ error: "track is required" }, 400);
  const track = Number(q.get("track"));
  if (!isByte(track)) return json({ error: "track must be 0-255" }, 400);
  const laps = q.has("laps") ? Number(q.get("laps")) : 0;
  if (!isByte(laps)) return json({ error: "laps must be 0-255" }, 400);
  const modeRaw = q.get("mode") ?? "tt";
  if (!isMode(modeRaw)) return json({ error: "mode must be tt or race" }, 400);
  const limitRaw = q.has("limit") ? Number(q.get("limit")) : LEADERBOARD_MAX;
  if (!Number.isInteger(limitRaw) || limitRaw < 1 || limitRaw > LEADERBOARD_LIMIT_MAX)
    return json({ error: `limit must be 1-${LEADERBOARD_LIMIT_MAX}` }, 400);

  const board = env.LEADERBOARDS.get(env.LEADERBOARDS.idFromName("global"));
  const entries = await board.top(track, laps, modeRaw, limitRaw);
  return json({ protocol: PROTOCOL, minVersion, track, laps, mode: modeRaw, entries });
}

/**
 * A submitted result: `POST /v1/leaderboard` with a JSON body
 * `{track, laps, mode, name, rider, seed, timeMs, replay}` (`replay` is the ghost file, base64).
 * The server bounds every field, checks the replay's structure and its agreement with the claim,
 * rate-limits the client and lets the Leaderboard keep the player's best.
 */
async function postLeaderboard(request: Request, env: Env): Promise<Response> {
  const declared = Number(request.headers.get("content-length") ?? "0");
  if (Number.isFinite(declared) && declared > POST_MAX_BYTES)
    return json({ ok: false, error: "too_large", message: "The replay is too large." }, 413);
  const text = await request.text();
  if (text.length > POST_MAX_BYTES)
    return json({ ok: false, error: "too_large", message: "The replay is too large." }, 413);

  let body: unknown;
  try {
    body = JSON.parse(text);
  } catch {
    return json({ ok: false, error: "bad_json", message: "The body is not JSON." }, 400);
  }
  if (typeof body !== "object" || body === null)
    return json({ ok: false, error: "bad_json", message: "The body is not an object." }, 400);
  const m = body as Record<string, unknown>;
  const reject = (error: string, message: string) => json({ ok: false, error, message }, 400);

  if (!isByte(m.track)) return reject("bad_value", "track must be 0-255.");
  if (!isByte(m.laps)) return reject("bad_value", "laps must be 0-255.");
  if (!isMode(m.mode)) return reject("bad_value", "mode must be tt or race.");
  if (!isByte(m.rider)) return reject("bad_value", "rider must be 0-255.");
  if (!isU32(m.seed)) return reject("bad_value", "seed must be a 32-bit integer.");
  const name = typeof m.name === "string" ? m.name.trim() : "";
  if (name.length < 1 || name.length > 24) return reject("bad_value", "name must be 1-24 characters.");
  if (!Number.isInteger(m.timeMs) || (m.timeMs as number) < MIN_RACE_MS || (m.timeMs as number) > MAX_RACE_MS)
    return reject("bad_time", "The finish time is not plausible.");

  const bytes = decodeBase64(m.replay);
  if (bytes === null) return reject("bad_replay", "The replay is not base64.");
  if (bytes.length > REPLAY_MAX_BYTES) return reject("too_large", "The replay is too large.");

  const timeMs = m.timeMs as number;
  const checked = validateGhost(bytes, {
    track: m.track,
    laps: m.laps,
    seed: m.seed,
    timeMs,
    checkSeed: true,
  });
  if (!checked.ok) return reject(checked.error, checked.message);

  const board = env.LEADERBOARDS.get(env.LEADERBOARDS.idFromName("global"));
  const max = Number(env.LEADERBOARD_SUBMIT_MAX) || SUBMIT_MAX;
  const clientKey = request.headers.get("cf-connecting-ip") ?? name;
  if (!(await board.allow(clientKey, max, SUBMIT_WINDOW_MS)))
    return json({ ok: false, error: "slow_down", message: "Too many submissions: wait a moment." }, 429);

  const result = await board.submit({
    track: m.track,
    laps: m.laps,
    mode: m.mode,
    name,
    timeMs,
    rider: m.rider,
    seed: m.seed,
    clientKey,
  });
  return json({
    ok: true,
    kept: result.kept,
    rank: result.rank,
    track: m.track,
    laps: m.laps,
    mode: m.mode,
    timeMs,
    entries: result.entries,
  });
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

    if (path === "/v1/leaderboard") {
      if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
      if (request.method === "GET") return getLeaderboard(url, env, minVersion);
      if (request.method === "POST") return postLeaderboard(request, env);
      return json({ error: "GET or POST only" }, 405);
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
