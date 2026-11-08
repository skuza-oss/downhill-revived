// Code updates at a safe moment (finding 184). A deploy gives every busy room new code within
// wrangler.jsonc's code_update_strategy max_delay (24 h): busy rooms never hibernate, so without
// this each one would be reset at that limit, at any moment. Instead a room that learns its code is
// not the one deployed restarts itself when nothing is under way (safeMoment): it saves, tells its
// players (UPDATE_NOTICE), closes their sockets with RESTART_CLOSE and resets (ctx.abort); its
// games rejoin by token (finding 137) and keep their seats and Realtime sessions.
//
// Which version is deployed, in order:
//  1. The rollout row (the moderation store's `settings` key `rollout`, written by
//     tools/deploy.mjs; read with the kill switch, kept 30 s per isolate): a version id means that
//     version runs at 100%: a room on another one restarts. An empty value means a gradual
//     deployment is under way: a room can't tell which version Cloudflare gave it, so none moves.
//  2. No row (or no store, a self-hosted server deploying with `wrangler deploy`): the Worker's
//     own version, which it passes on its requests to the room (WORKER_VERSION_HEADER): a newer
//     one than the room's means a new deployment.
// A restart that brings the room back on the same version (the deployment hadn't reached it, or
// Cloudflare keeps it on that one) is tried again after RETRY_MS, then given up for that target.

/** A Worker version as the version metadata binding has it (CF_VERSION_METADATA). */
export interface CodeVersion {
  id: string;
  /** When the version was uploaded: an ISO date (sometimes empty). */
  timestamp: string;
}

/** The Worker → room header with the Worker's version (`<id> <timestamp>`). The Worker drops one a
    client sent; a room on older code ignores it. */
export const WORKER_VERSION_HEADER = "x-dhd-worker-version";

/** The message a room sends its members before it restarts for new code: a footer line in the
    game during the reconnect. 0.7.13 and older ignore the type and rejoin as after any drop. */
export const UPDATE_NOTICE = { type: "notice", code: "server_update", message: "Server updated, reconnecting..." } as const;

/** The close a room gives its sockets before it restarts (1012, service restart): below 4000, so
    every release rejoins (port/net/client.c client_on_closed). */
export const RESTART_CLOSE = 1012;
export const RESTART_REASON = "server_update";

/** No restart this close to an automatic next race: members must be back before its grid locks. */
export const NEXT_RACE_MARGIN_MS = 10_000;

/** A restart that didn't change the room's version is tried again this long after each try; after
    the last, that target is given up (the room then adopts new code at Cloudflare's max_delay or
    when it next sleeps). */
export const RETRY_MS = [120_000, 600_000, 1_800_000];

/** The room's last restart for new code, stored under its own key ("update") so it outlives the
    restart (stored-state compatibility: an older room ignores the key). */
export interface RestartRecord {
  /** The version the room restarted to adopt ("" when it went by a newer Worker version). */
  target: string;
  /** The room's own version id when it restarted. */
  from: string;
  /** When (ms since the Unix epoch) and how many restarts for this target so far. */
  at: number;
  tries: number;
  /** Members then, to log how long their rejoins took. */
  members?: number;
}

/** Reads a version from the binding, a header or an RPC argument; null if it isn't one. */
export function versionOf(v: unknown): CodeVersion | null {
  if (typeof v === "string") {
    const [id, timestamp = ""] = v.trim().split(/\s+/, 2);
    return id && id.length <= 64 ? { id, timestamp } : null;
  }
  if (!v || typeof v !== "object") return null;
  const { id, timestamp } = v as Record<string, unknown>;
  if (typeof id !== "string" || !id || id.length > 64) return null;
  return { id, timestamp: typeof timestamp === "string" ? timestamp : "" };
}

/** The header value for `v`. */
export function versionHeader(v: CodeVersion): string {
  return `${v.id} ${v.timestamp}`.trim();
}

/** Whether `a` is a newer upload than `b`: by timestamp; an unreadable one is never newer. */
export function newerVersion(a: CodeVersion | null, b: CodeVersion | null): boolean {
  if (!a || !b || a.id === b.id) return false;
  const ta = Date.parse(a.timestamp), tb = Date.parse(b.timestamp);
  return Number.isFinite(ta) && Number.isFinite(tb) && ta > tb;
}

/** The newer of two heard versions. */
export function newest(a: CodeVersion | null, b: CodeVersion | null): CodeVersion | null {
  if (!a) return b;
  if (!b) return a;
  return newerVersion(b, a) ? b : a;
}

/** What may not be cut by a restart. */
export interface RoomMoment {
  /** A race runs. */
  race: boolean;
  /** A grid is locked or a start countdown runs. */
  grid: boolean;
  countdown: boolean;
  /** Realtime work in flight: a subscription pass, a member's signaling call, the cleanup. */
  setup: boolean;
  /** A hello waits for the store (its ban lookup). */
  hello: boolean;
  /** ms until an automatic next race locks its grid (null: none set). */
  nextRaceInMs: number | null;
  /** Members in the room. An empty room needs no restart: it adopts on its next start. */
  members: number;
}

/** No race, no locked grid, no countdown, no Realtime work, no hello in flight, and no automatic
    race due within NEXT_RACE_MARGIN_MS: the lobby or the results between races. */
export function safeMoment(m: RoomMoment): boolean {
  return m.members > 0 && !m.race && !m.grid && !m.countdown && !m.setup && !m.hello &&
    (m.nextRaceInMs === null || m.nextRaceInMs > NEXT_RACE_MARGIN_MS);
}

/**
 * The version the room should restart for, or null. `own`: the room's version (null unknown: never
 * restarts). `rollout`: the rollout row (undefined: no store or no row; "": a gradual deployment).
 * `heard`: the newest Worker version heard on requests. `last`: the last restart's record.
 */
export function restartTarget(
  own: CodeVersion | null, rollout: string | null | undefined, heard: CodeVersion | null,
  last: RestartRecord | null, now: number,
): string | null {
  if (!own) return null;
  let target: string | null;
  if (rollout !== undefined && rollout !== null) target = rollout && rollout !== own.id ? rollout : null;
  else target = newerVersion(heard, own) ? heard!.id : null;
  if (target === null) return null;
  // The last restart for this target left the room on the same version: wait, then give up.
  const key = rollout ? target : "";
  if (last && last.target === key && last.from === own.id) {
    if (last.tries > RETRY_MS.length) return null;
    if (now < last.at + RETRY_MS[last.tries - 1]) return null;
  }
  return target;
}

/** The record to store before restarting for `target` (from restartTarget). */
export function nextRecord(own: CodeVersion, target: string, byRollout: boolean, last: RestartRecord | null, now: number, members: number): RestartRecord {
  const key = byRollout ? target : "";
  const tries = last && last.target === key && last.from === own.id ? last.tries + 1 : 1;
  return { target: key, from: own.id, at: now, tries, members };
}

/** The rollout row's value as the room reads it: a version id, "" while a gradual deployment runs,
    undefined when there is none. Anything else is no row. */
export function rolloutOf(value: unknown): string | undefined {
  if (typeof value !== "string") return undefined;
  const v = value.trim();
  return v === "" || /^[0-9a-f-]{8,64}$/i.test(v) ? v : undefined;
}
