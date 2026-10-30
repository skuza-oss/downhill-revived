// Time-trial leaderboards and ghosts (finding 127): one TrackTimes Durable Object per track and lap
// count, SQLite-backed. The Worker (index.ts) validates requests with checkPost and routes them here.
// Times are NOT verified: anyone who can send an HTTP request can post any plausible time.
import { DurableObject } from "cloudflare:workers";
import { isByte, isOutdated, parseVersion } from "./protocol";

/** Fastest and slowest time a board takes, ms. A full downhill run takes well over 10 s; nobody
    posts a 30-minute one on purpose. */
export const MIN_TIME_MS = 10_000;
export const MAX_TIME_MS = 30 * 60_000;
/** A ghost (the game's own replay data, opaque here) after base64 decoding. */
export const MAX_GHOST_BYTES = 128 * 1024;
/** The whole POST body: the ghost as base64 (4/3 of it) and the JSON around it. */
export const MAX_BODY_BYTES = 180 * 1024;
/** Rows a board keeps (best per name), and how many of the best keep their ghost. */
export const MAX_ROWS = 1000;
export const GHOSTS_KEPT = 100;
/** Posts one address may make to one board in RATE_WINDOW_MS. */
export const RATE_POSTS = 6;
export const RATE_WINDOW_MS = 10 * 60_000;
export const DEFAULT_LIMIT = 20;
export const MAX_LIMIT = 100;

export interface TimePost {
  version: string;
  clientVersion: string;
  isoSha256: string;
  track: number;
  laps: number;
  name: string;
  timeMs: number;
  ghost: Uint8Array | null;
}

export type Checked = { ok: true; post: TimePost } | { ok: false; status: number; body: Record<string, unknown> };

const bad = (error: string, message: string, status = 400, extra: object = {}): Checked => ({
  ok: false,
  status,
  body: { error, message, ...extra },
});

/** A board's name: 1-24 UTF-16 units after trimming, as hello's, and no control characters
    (it is shown to everyone). */
export function boardName(v: unknown): string | null {
  if (typeof v !== "string") return null;
  const t = v.trim();
  if (t.length < 1 || t.length > 24) return null;
  if (/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/.test(t)) return null;
  return t;
}

/** Decodes base64 (standard alphabet, padding optional); null when it isn't base64. */
export function fromBase64(s: string): Uint8Array | null {
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(s) || s.length % 4 === 1) return null;
  try {
    const bin = atob(s);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  } catch {
    return null;
  }
}

export function toBase64(b: Uint8Array): string {
  let bin = "";
  for (let i = 0; i < b.length; i += 0x8000) bin += String.fromCharCode(...b.subarray(i, i + 0x8000));
  return btoa(bin);
}

/** Validates a POST /v1/times body (already parsed JSON) against the version gate and the limits. */
export function checkPost(m: unknown, minVersion: string): Checked {
  if (typeof m !== "object" || m === null) return bad("bad_request", "Expected a JSON object.");
  const o = m as Record<string, unknown>;
  // The same gate as hello: the release in clientVersion, at least MIN_VERSION. A post has no
  // 0.1.0 fallback: every release that can post sends it.
  const clientVersion = typeof o.clientVersion === "string" ? o.clientVersion.slice(0, 32) : "";
  if (!parseVersion(clientVersion)) return bad("bad_request", "clientVersion must be a release (X.Y.Z).");
  if (isOutdated(clientVersion, minVersion))
    return bad("client_outdated", "A new version is out. Update to post times.", 403, { minVersion, clientVersion });
  if (typeof o.version !== "string" || o.version.length < 1 || o.version.length > 64)
    return bad("bad_request", "version must be 1-64 characters.");
  if (typeof o.isoSha256 !== "string" || !/^[0-9a-f]{64}$/.test(o.isoSha256))
    return bad("bad_request", "isoSha256 must be 64 lowercase hex digits.");
  if (!isByte(o.track)) return bad("bad_request", "track must be 0-255.");
  if (o.laps !== undefined && !isByte(o.laps)) return bad("bad_request", "laps must be 0-255.");
  const name = boardName(o.name);
  if (name === null) return bad("bad_request", "name must be 1-24 characters, no control characters.");
  const t = o.timeMs;
  if (!Number.isInteger(t) || (t as number) < MIN_TIME_MS || (t as number) > MAX_TIME_MS)
    return bad("bad_time", `timeMs must be a whole number from ${MIN_TIME_MS} to ${MAX_TIME_MS}.`);
  let ghost: Uint8Array | null = null;
  if (o.ghost !== undefined && o.ghost !== null) {
    if (typeof o.ghost !== "string") return bad("bad_request", "ghost must be base64.");
    if (o.ghost.length > Math.ceil(MAX_GHOST_BYTES / 3) * 4)
      return bad("too_large", `A ghost is at most ${MAX_GHOST_BYTES} bytes.`, 413);
    ghost = fromBase64(o.ghost);
    if (ghost === null) return bad("bad_request", "ghost must be base64.");
    if (ghost.length > MAX_GHOST_BYTES) return bad("too_large", `A ghost is at most ${MAX_GHOST_BYTES} bytes.`, 413);
    if (ghost.length === 0) ghost = null;
  }
  return {
    ok: true,
    post: {
      version: o.version,
      clientVersion,
      isoSha256: o.isoSha256,
      track: o.track as number,
      laps: (o.laps as number | undefined) ?? 0,
      name,
      timeMs: t as number,
      ghost,
    },
  };
}

/** The board a track and lap count use (one Durable Object each). */
export function boardKey(track: number, laps: number): string {
  return `track-${track}-laps-${laps}`;
}

export interface Entry {
  rank: number;
  name: string;
  timeMs: number;
  at: number;
  ghost: boolean;
}

export type PostResult =
  | { ok: true; rank: number; best: boolean; timeMs: number }
  | { ok: false; status: number; body: Record<string, unknown> };

/** One board: the best time per name (names compared case-insensitively), the best GHOSTS_KEPT
    with their ghost, and the recent posts per address for the rate limit. */
export class TrackTimes extends DurableObject<Env> {
  private sql: SqlStorage;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.sql.exec(`CREATE TABLE IF NOT EXISTS times (
      name_key TEXT PRIMARY KEY, name TEXT NOT NULL, time_ms INTEGER NOT NULL, at INTEGER NOT NULL,
      version TEXT NOT NULL, client_version TEXT NOT NULL, iso TEXT NOT NULL, ghost BLOB)`);
    this.sql.exec("CREATE INDEX IF NOT EXISTS times_by_time ON times (time_ms, at)");
    this.sql.exec("CREATE TABLE IF NOT EXISTS posts (who TEXT NOT NULL, at INTEGER NOT NULL)");
    this.sql.exec("CREATE INDEX IF NOT EXISTS posts_by_who ON posts (who, at)");
  }

  /** `who`: a hash of the poster's address (never the address itself). */
  async post(p: TimePost, who: string, now = Date.now()): Promise<PostResult> {
    this.sql.exec("DELETE FROM posts WHERE at <= ?", now - RATE_WINDOW_MS);
    const recent = this.sql.exec<{ n: number }>("SELECT COUNT(*) AS n FROM posts WHERE who = ?", who).one().n;
    if (recent >= RATE_POSTS)
      return { ok: false, status: 429, body: { error: "slow_down", message: "Too many times posted: wait a few minutes." } };
    this.sql.exec("INSERT INTO posts (who, at) VALUES (?, ?)", who, now);

    const key = p.name.toLowerCase();
    const old = this.sql.exec<{ time_ms: number }>("SELECT time_ms FROM times WHERE name_key = ?", key).toArray()[0];
    const best = !old || p.timeMs < old.time_ms;
    if (best) {
      this.sql.exec(
        `INSERT INTO times (name_key, name, time_ms, at, version, client_version, iso, ghost)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?)
         ON CONFLICT (name_key) DO UPDATE SET name = excluded.name, time_ms = excluded.time_ms, at = excluded.at,
           version = excluded.version, client_version = excluded.client_version, iso = excluded.iso, ghost = excluded.ghost`,
        key, p.name, p.timeMs, now, p.version, p.clientVersion, p.isoSha256, p.ghost ? p.ghost.slice().buffer : null,
      );
      this.trim();
    }
    const row = this.sql.exec<{ time_ms: number; at: number }>("SELECT time_ms, at FROM times WHERE name_key = ?", key).toArray()[0];
    if (!row) return { ok: true, rank: 0, best, timeMs: p.timeMs };  // trimmed: not in the kept rows
    return { ok: true, rank: this.rankOf(row.time_ms, row.at), best, timeMs: row.time_ms };
  }

  /** The best `limit` times, fastest first (the earlier post first on a tie). */
  async top(limit: number): Promise<Entry[]> {
    return this.sql
      .exec<{ name: string; time_ms: number; at: number; has: number }>(
        "SELECT name, time_ms, at, ghost IS NOT NULL AS has FROM times ORDER BY time_ms, at LIMIT ?",
        limit,
      )
      .toArray()
      .map((r, i) => ({ rank: i + 1, name: r.name, timeMs: r.time_ms, at: r.at, ghost: r.has === 1 }));
  }

  /** The ghost of the time at `rank` (1 = the fastest), or null if there is none. */
  async ghost(rank: number): Promise<{ rank: number; name: string; timeMs: number; ghost: string } | null> {
    const r = this.sql
      .exec<{ name: string; time_ms: number; ghost: ArrayBuffer | null }>(
        "SELECT name, time_ms, ghost FROM times ORDER BY time_ms, at LIMIT 1 OFFSET ?",
        rank - 1,
      )
      .toArray()[0];
    if (!r || r.ghost === null) return null;
    return { rank, name: r.name, timeMs: r.time_ms, ghost: toBase64(new Uint8Array(r.ghost)) };
  }

  private rankOf(timeMs: number, at: number): number {
    return (
      this.sql
        .exec<{ n: number }>("SELECT COUNT(*) AS n FROM times WHERE time_ms < ? OR (time_ms = ? AND at < ?)", timeMs, timeMs, at)
        .one().n + 1
    );
  }

  /** Keeps MAX_ROWS rows, and ghosts only for the best GHOSTS_KEPT. */
  private trim(): void {
    this.sql.exec(
      `DELETE FROM times WHERE name_key IN (SELECT name_key FROM times ORDER BY time_ms, at LIMIT -1 OFFSET ?)`,
      MAX_ROWS,
    );
    this.sql.exec(
      `UPDATE times SET ghost = NULL WHERE ghost IS NOT NULL AND name_key IN
         (SELECT name_key FROM times ORDER BY time_ms, at LIMIT -1 OFFSET ?)`,
      GHOSTS_KEPT,
    );
  }
}
