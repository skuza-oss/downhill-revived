// The online server's abuse guard (finding 161): the kill switch, limits per connection source,
// per-member caps, bans and the short moderation log. The game is open source, so the server can't
// tell an official build from another: what any client may do is limited here, and the operator
// can stop one.
import { CLOSE_CODES } from "./protocol";

/** The guard's settings and bindings. All optional: a server without them runs as before (online
    on, no limits per address, no bans, no log); wrangler.jsonc and README.md say which exist. */
export interface GuardEnv {
  /** The secret for the keyed IP hash (ipHash): a Worker secret, never in a tracked file. */
  IP_HASH_KEY?: string;
  /** Rate limiters per connection source (wrangler.jsonc `ratelimits`, keyed by ipHash): every
      room WebSocket, and new rooms. */
  RL_CONNECT?: RateLimit;
  RL_CREATE?: RateLimit;
  /** The kill switch, the moderation log and the ban list (migrations/; README.md sets it up). */
  MODERATION?: D1Database;
  /** Tests only (vitest.config.ts): "off" reads the store at every lookup, so a test sees its own
      change at once. */
  GUARD_CACHE?: string;
}

// ---- What an isolate keeps of the store ----

/**
 * Each isolate (the Worker's, each room's) keeps what it read of the store for a short time, so a
 * connection costs no D1 read in the common case: the kill switch SWITCH_CACHE_MS, a ban lookup
 * BAN_CACHE_MS (banned or not). So a change made with tools/moderate.mjs bites within that long:
 * `online off` up to 30 s late, a new ban (or a lifted one) up to a minute late. A read that
 * fails is kept only FAILED_CACHE_MS: a store back from an outage is read again soon, and a broken
 * one is not asked (and logged) at every connection.
 */
export const SWITCH_CACHE_MS = 30_000;
export const BAN_CACHE_MS = 60_000;
export const FAILED_CACHE_MS = 10_000;
/** At most this many ban lookups kept per isolate (one per address or install id seen lately). */
export const BAN_CACHE_MAX = 10_000;

interface Cached<T> {
  db: D1Database;
  /** ms since the Unix epoch; Infinity while the read runs (callers then share it). */
  until: number;
  value: Promise<T>;
}

const noCache = (env: GuardEnv) => env.GUARD_CACHE === "off";

/** Forgets everything this isolate kept (tests). */
export function resetGuardCaches(): void {
  switchCache = null;
  lastSwitch = ONLINE_ON;
  banCache.clear();
}

// ---- The kill switch ----

/**
 * The refusal while online is off. Every release shows a refusal's `message` on the ONLINE
 * screen's footer, which holds about 45 characters at 640 pixels (finding 054): keep it short.
 */
export const OFF_MESSAGE = "Online races are paused. Try again later.";
/** The longest operator's message the server shows (tools/moderate.mjs refuses longer ones). */
export const MESSAGE_MAX = 120;

/** The kill switch as the store has it (migrations/0002_settings.sql: keys `online` and
    `online_message`, written by tools/moderate.mjs online). `message`: what players read while off. */
export interface OnlineState {
  on: boolean;
  message: string;
}
const ONLINE_ON: OnlineState = { on: true, message: "" };

let switchCache: Cached<OnlineState> | null = null;
/** The last state this isolate read: what a failed read keeps. */
let lastSwitch: OnlineState = ONLINE_ON;

/** The operator's message, or OFF_MESSAGE when there is none. */
export function offMessage(message: unknown): string {
  return typeof message === "string" && message.trim() ? message.trim().slice(0, MESSAGE_MAX) : OFF_MESSAGE;
}

async function readSwitch(db: D1Database): Promise<OnlineState> {
  const { results } = await db
    .prepare("SELECT key, value FROM settings WHERE key IN ('online', 'online_message')")
    .all<{ key: string; value: string }>();
  const get = (key: string) => results.find((r) => r.key === key)?.value;
  return get("online")?.trim().toLowerCase() === "off" ? { on: false, message: offMessage(get("online_message")) } : ONLINE_ON;
}

/**
 * The kill switch (finding 161): off while the operator has turned online off with
 * `tools/moderate.mjs online off`; kept per isolate SWITCH_CACHE_MS (above). No store: on. A store
 * that can't be read keeps the last state this isolate read (on, if none) and logs it: a broken
 * store turns online neither off nor on.
 */
export function onlineState(env: GuardEnv, now = Date.now()): Promise<OnlineState> {
  const db = env.MODERATION;
  if (!db) return Promise.resolve(ONLINE_ON);
  const cached = switchCache;
  if (!noCache(env) && cached && cached.db === db && now < cached.until) return cached.value;
  const entry: Cached<OnlineState> = { db, until: Infinity, value: Promise.resolve(ONLINE_ON) };
  entry.value = readSwitch(db).then(
    (state) => {
      entry.until = now + SWITCH_CACHE_MS;
      return (lastSwitch = state);
    },
    (e) => {
      console.error("guard: the kill switch can't be read; keeping the last state", e);
      entry.until = now + FAILED_CACHE_MS;
      return lastSwitch;
    },
  );
  switchCache = entry;
  return entry.value;
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
 * Binary: since protocol 3 (finding 158) race frames travel over the Realtime SFU, not this
 * socket; what is left is the game's binary PING, two a second in a race (port/net/client.c
 * PING_RACE_MS), each answered with one PONG to the sender only. 20 a second, 100 a window, is ten
 * times that; nothing a member sends here is copied to other members any more.
 *
 * Text: in the lobby the game sends a ping a second and a pong every 2 s (PING_LOBBY_MS, the
 * server's PING_INTERVAL_MS), plus a message per click (pick, ready, settings; the host's room
 * setup sends three at once); a race adds its Realtime setup, a few messages per race (sfu_offer,
 * sfu_connected, sfu_ready, turn; docs/online-protocol.md). 20 a second, 100 a window, is ten
 * times what a player clicking fast sends.
 */
export const CAP_WINDOW_MS = 5_000;
export const BINARY_PER_WINDOW = 100;
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

// ---- Bans and the moderation log ----

/** How long the moderation log keeps a join: 14 days (finding 161, the privacy note). */
export const LOG_DAYS = 14;
const DAY_MS = 86_400_000;

/** A random install id from the game (port/render/settings install_id): 32 lowercase hex. */
export function installIdOf(v: unknown): string | null {
  return typeof v === "string" && /^[0-9a-f]{32}$/.test(v) ? v : null;
}

export interface Ban {
  kind: "ip" | "install";
  /** ms since the Unix epoch; null: until lifted. */
  expiresAt: number | null;
}

const banCache = new Map<string, Cached<Ban | null>>();

async function readBan(db: D1Database, by: { ip?: string; install?: string | null }, now: number): Promise<Ban | null> {
  const row = await db
    .prepare(
      "SELECT kind, expires_at FROM bans WHERE ((kind = 'ip' AND value = ?1) OR (kind = 'install' AND value = ?2))" +
        " AND (expires_at IS NULL OR expires_at > ?3) ORDER BY expires_at IS NULL DESC, expires_at DESC LIMIT 1",
    )
    .bind(by.ip || null, by.install || null, now)
    .first<{ kind: "ip" | "install"; expires_at: number | null }>();
  return row ? { kind: row.kind, expiresAt: row.expires_at } : null;
}

/** Makes room for one more ban lookup: drops the ones past their time, then the oldest. */
function trimBanCache(now: number): void {
  if (banCache.size < BAN_CACHE_MAX) return;
  for (const [key, c] of banCache) if (c.until <= now) banCache.delete(key);
  for (const key of banCache.keys()) {
    if (banCache.size < BAN_CACHE_MAX) break;
    banCache.delete(key);
  }
}

/**
 * The ban in force for this address hash or install id, or null. Kept per isolate BAN_CACHE_MS
 * (above): a ban the operator just added may take up to that long to bite, and a lifted one as
 * long to go; a kept ban that has ended meanwhile no longer counts. A server without the store, or
 * a store that can't be read, bans nobody: players get in (availability first) and the failure is
 * logged, so the operator sees a broken store in `wrangler tail`.
 */
export async function findBan(env: GuardEnv, by: { ip?: string; install?: string | null }, now = Date.now()): Promise<Ban | null> {
  const db = env.MODERATION;
  if (!db || (!by.ip && !by.install)) return null;
  const key = `${by.ip || ""}|${by.install || ""}`;
  let entry = noCache(env) ? undefined : banCache.get(key);
  if (!entry || entry.db !== db || now >= entry.until) {
    const fresh: Cached<Ban | null> = { db, until: Infinity, value: Promise.resolve(null) };
    fresh.value = readBan(db, by, now).then(
      (ban) => {
        fresh.until = now + BAN_CACHE_MS;
        return ban;
      },
      (e) => {
        console.error("guard: the ban list can't be read; letting the player in", e);
        fresh.until = now + FAILED_CACHE_MS;
        return null;
      },
    );
    banCache.delete(key);
    trimBanCache(now);
    banCache.set(key, (entry = fresh));
  }
  const ban = await entry.value;
  return ban && ban.expiresAt !== null && ban.expiresAt <= now ? null : ban;
}

/** The refusal for a banned player: when it ends (UTC day), or none (footer-sized). */
export function banMessage(ban: Ban): string {
  if (ban.expiresAt === null) return "You can't play online on this server.";
  return `You can't play online until ${new Date(ban.expiresAt).toISOString().slice(0, 10)}.`;
}

/** One moderation log row: a player who entered a room. */
export interface JoinRow {
  at: number;
  room: string;
  member: number;
  kind: "host" | "guest" | "rejoin";
  name: string;
  clientVersion: string;
  country: string;
  ipHash: string;
  installId: string | null;
}

/** Writes a join into the moderation log; a failure is logged and nothing else. */
export async function logJoin(env: GuardEnv, row: JoinRow): Promise<void> {
  const db = env.MODERATION;
  if (!db) return;
  try {
    await db
      .prepare(
        "INSERT INTO joins (at, room, member, kind, name, client_version, country, ip_hash, install_id)" +
          " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
      )
      .bind(row.at, row.room, row.member, row.kind, row.name, row.clientVersion, row.country, row.ipHash, row.installId)
      .run();
  } catch (e) {
    console.error("guard: the moderation log can't be written", e);
  }
}

/** The daily cleanup (src/index.ts scheduled): joins older than LOG_DAYS and bans that have ended
    go. Returns how many of each. */
export async function cleanup(env: GuardEnv, now = Date.now()): Promise<{ joins: number; bans: number }> {
  const db = env.MODERATION;
  if (!db) return { joins: 0, bans: 0 };
  const [joins, bans] = await db.batch([
    db.prepare("DELETE FROM joins WHERE at < ?1").bind(now - LOG_DAYS * DAY_MS),
    db.prepare("DELETE FROM bans WHERE expires_at IS NOT NULL AND expires_at <= ?1").bind(now),
  ]);
  return { joins: joins.meta.changes ?? 0, bans: bans.meta.changes ?? 0 };
}
