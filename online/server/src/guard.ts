// The online server's abuse guard (finding 161): the kill switch, limits per connection source,
// per-member caps, bans and the short moderation log. The game is open source, so the server can't
// tell an official build from another: what any client may do is limited here, and the operator
// can stop one.
import { CLOSE_CODES } from "./protocol";

/** The guard's settings and bindings. All optional: a server without them runs as before (online
    on, no limits per address, no bans, no log); wrangler.jsonc and README.md say which exist. */
export interface GuardEnv {
  /** The kill switch: "off" refuses new rooms, joins and race starts. Anything else is on. */
  ONLINE?: string;
  /** The refusal text while online is off (OFF_MESSAGE when unset). */
  ONLINE_MESSAGE?: string;
  /** The secret for the keyed IP hash (ipHash): a Worker secret, never in a tracked file. */
  IP_HASH_KEY?: string;
  /** Rate limiters per connection source (wrangler.jsonc `ratelimits`, keyed by ipHash): every
      room WebSocket, and new rooms. */
  RL_CONNECT?: RateLimit;
  RL_CREATE?: RateLimit;
}

// ---- The kill switch ----

/**
 * The refusal while online is off. Every release shows a refusal's `message` on the ONLINE
 * screen's footer, which holds about 45 characters at 640 pixels (finding 054): keep it short.
 */
export const OFF_MESSAGE = "Online races are paused. Try again later.";

/** True while the operator has turned online off (ONLINE = "off"). */
export function onlineOff(env: GuardEnv): boolean {
  return typeof env.ONLINE === "string" && env.ONLINE.trim().toLowerCase() === "off";
}

/** What players read while online is off: ONLINE_MESSAGE, else OFF_MESSAGE. */
export function offMessage(env: GuardEnv): string {
  const m = env.ONLINE_MESSAGE;
  return typeof m === "string" && m.trim() ? m.trim().slice(0, 120) : OFF_MESSAGE;
}

// ---- Refusals before any room ----

/**
 * A refusal the Worker itself gives, before the connection reaches a room: the WebSocket is
 * accepted, says one error frame and closes with the code's close code. Every release reads it
 * as any room's refusal (an error frame, then a 4000-4099 close: no rejoin) and shows `message`.
 */
export function refuseSocket(code: keyof typeof CLOSE_CODES, message: string): Response {
  const { 0: client, 1: server } = new WebSocketPair();
  server.accept();
  server.send(JSON.stringify({ type: "error", code, message }));
  server.close(CLOSE_CODES[code], code);
  return new Response(null, { status: 101, webSocket: client });
}

// ---- The keyed IP hash ----

const encoder = new TextEncoder();
let keyFor: { secret: string; key: Promise<CryptoKey> } | null = null;
let warnedNoKey = false;

/** The HMAC key from IP_HASH_KEY. Without the secret (a server not set up for it, a local test)
    each isolate draws its own random key: rate limits still work within it, but hashes don't match
    across isolates or restarts, so bans by address don't hold. Said once in the log. */
function hashKey(env: GuardEnv): Promise<CryptoKey> {
  let secret = typeof env.IP_HASH_KEY === "string" ? env.IP_HASH_KEY : "";
  if (!secret) {
    if (!warnedNoKey) console.warn("guard: no IP_HASH_KEY secret; address hashes last only this isolate");
    warnedNoKey = true;
    secret = keyFor?.secret.startsWith("random:") ? keyFor.secret : `random:${crypto.randomUUID()}`;
  }
  if (keyFor?.secret !== secret)
    keyFor = {
      secret,
      key: crypto.subtle.importKey("raw", encoder.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]),
    };
  return keyFor.key;
}

/**
 * The connection's address as a keyed hash: HMAC-SHA-256 of Cloudflare's CF-Connecting-IP with
 * the IP_HASH_KEY secret, 64 hex digits. The address itself is read only here and never stored,
 * logged or passed on; without the key nobody can tell which address a hash stands for, and a new
 * key makes every old hash useless (finding 161). "" when the request has no address.
 */
export async function ipHash(env: GuardEnv, request: Request): Promise<string> {
  const ip = request.headers.get("CF-Connecting-IP") ?? "";
  if (!ip) return "";
  const mac = await crypto.subtle.sign("HMAC", await hashKey(env), encoder.encode(ip));
  return Array.from(new Uint8Array(mac), (b) => b.toString(16).padStart(2, "0")).join("");
}

// ---- Limits per connection source ----

/** The refusal over a limit per address (footer-sized, see OFF_MESSAGE). */
export const RATE_MESSAGE = "Too many tries. Wait a minute and retry.";

/**
 * True when this address has opened too many room connections (RL_CONNECT), or made too many
 * rooms (RL_CREATE, `create`), in the last minute; the limits and their reasons are in
 * wrangler.jsonc. Keyed by the address's hash, so the rate limiter never sees the address. A
 * missing binding, a request without an address or a limiter that fails lets the player in (a
 * failure is logged): the limits keep floods off, they are not worth refusing players for.
 */
export async function overLimit(env: GuardEnv, hash: string, create: boolean): Promise<boolean> {
  if (!hash) return false;
  try {
    if (env.RL_CONNECT && !(await env.RL_CONNECT.limit({ key: hash })).success) return true;
    if (create && env.RL_CREATE && !(await env.RL_CREATE.limit({ key: hash })).success) return true;
  } catch (e) {
    console.error("guard: rate limiter failed; letting the connection in", e);
  }
  return false;
}

// ---- Caps per member in a room ----

/**
 * What one socket may send, counted over windows of CAP_WINDOW_MS (a window, not a second, so a
 * hitch's burst of queued frames doesn't count as a flood).
 *
 * Binary: the game sends one STATE frame a snapshot, about 20 a second at any frame rate, with all
 * the riders it runs in it (port/hle/online.c SNAPSHOT_TICKS; the host's AI riders too), plus
 * EVENT frames (hits, pickups, the setup, the finish: a few a second in a crowd) and two binary
 * pings a second (port/net/client.c PING_RACE_MS). Even one STATE frame per rider, 10 riders at
 * 20 a second, would be 200 a second: the cap is twice that, 400 a second, 2000 a window. Each
 * frame goes out to up to 9 others, so this is also what one member can make the room send.
 *
 * Text: in the lobby the game sends a ping a second and a pong every 2 s (PING_LOBBY_MS, the
 * server's PING_INTERVAL_MS), plus a message per click (pick, ready, settings; the host's room
 * setup sends three at once). 20 a second, 100 a window, is ten times what a player clicking fast
 * sends.
 */
export const CAP_WINDOW_MS = 5_000;
export const BINARY_PER_WINDOW = 2_000;
export const TEXT_PER_WINDOW = 100;

/** The warning (a non-fatal error) the first time a socket goes over a cap, and the refusal the
    next time (footer-sized, see OFF_MESSAGE). */
export const TOO_FAST_WARNING = "Slow down: too many messages.";
export const TOO_FAST_MESSAGE = "Disconnected: too many messages.";

/** One socket's counts in its current window. */
export interface Tally {
  start: number;
  text: number;
  binary: number;
  /** Over a cap in this window already (its other messages are dropped). */
  over: boolean;
  /** Warned once already: the next window over a cap closes the socket. */
  warned: boolean;
}

export function newTally(now: number): Tally {
  return { start: now, text: 0, binary: 0, over: false, warned: false };
}

/**
 * Counts one message: "ok" to handle it; "drop" to drop it (over a cap in this window); "warn" the
 * first time ever over a cap (drop it and warn the sender); "close" the next window over a cap.
 */
export interface Caps {
  windowMs: number;
  text: number;
  binary: number;
}
export const CAPS: Caps = { windowMs: CAP_WINDOW_MS, text: TEXT_PER_WINDOW, binary: BINARY_PER_WINDOW };

export function tally(t: Tally, binary: boolean, now: number, caps: Caps = CAPS): "ok" | "drop" | "warn" | "close" {
  if (now - t.start >= caps.windowMs) {
    t.start = now;
    t.text = t.binary = 0;
    t.over = false;
  }
  const n = binary ? ++t.binary : ++t.text;
  if (n <= (binary ? caps.binary : caps.text)) return "ok";
  if (t.over) return "drop";
  t.over = true;
  if (t.warned) return "close";
  t.warned = true;
  return "warn";
}
