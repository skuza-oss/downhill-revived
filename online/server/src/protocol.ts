// Wire protocol constants and pure helpers. The specification is docs/online-protocol.md.

export const PROTOCOL = 3;

export const MAX_MEMBERS = 10;
export const DEFAULT_MAX_PLAYERS = 10;
export const MAX_TEXT_BYTES = 4096;
/** A STATE frame carries all of a player's riders (port/net/net.h NET_FRAME_MAX). */
export const MAX_BINARY_BYTES = 3072;

export const HELLO_TIMEOUT_MS = 10_000;
export const PING_INTERVAL_MS = 2_000;
export const START_LEAD_MS = 3_000;
/**
 * The race room's countdown: once everyone is ready the race starts by itself this long after
 * (any change that unreadies someone stops it). Only rooms whose every member's hello lists
 * the "countdown" feature run it; older clients start by the host's `start` alone.
 */
export const COUNTDOWN_MS = 5_000;
export const FEATURE_COUNTDOWN = "countdown";
export const RTT_SAMPLES = 5;

// Binary race frames. (Types 1 and 2 were the first design's INPUT and HASH, input lockstep, which
// the game no longer uses; the server drops them.)
export const FRAME_PING = 3;
export const FRAME_PONG = 4;
/** State sync (finding 044): a rider snapshot and a gameplay event, relayed as they are. */
export const FRAME_STATE = 5;
export const FRAME_EVENT = 6;
export const PING_MAX_PAYLOAD = 32;

/** Refusal reasons and the close code each one uses. */
export const CLOSE_CODES = {
  bad_message: 4000,
  protocol_mismatch: 4001,
  version_mismatch: 4002,
  iso_mismatch: 4003,
  room_full: 4004,
  room_started: 4005,
  room_not_found: 4006,
  kicked: 4007,
  hello_timeout: 4008,
  client_outdated: 4009,
  /** The operator turned online off (the kill switch in the D1 store; finding 161). */
  online_off: 4010,
  /** Too many connections or new rooms from one address in a minute (wrangler.jsonc ratelimits). */
  rate_limited: 4011,
  /** A member sent more messages or race frames than the game ever does, after a warning
      (src/guard.ts TEXT_PER_WINDOW, BINARY_PER_WINDOW). The warning is a non-fatal too_fast. */
  too_fast: 4012,
  /** The operator banned this address hash or install id (src/guard.ts findBan). */
  banned: 4013,
} as const;
export type ErrorCode = keyof typeof CLOSE_CODES | "not_host" | "not_ready" | "bad_value";

// ---- The client version gate ----

/**
 * The oldest release that may play, when wrangler.jsonc's MIN_VERSION is missing or not a
 * version. Keep it at the oldest release still allowed.
 */
export const DEFAULT_MIN_VERSION = "0.1.0";
/**
 * Release 0.1.0 sends no hello.clientVersion (its hello.version is "dhd-port <exe hash>"), so a
 * hello without one is taken as 0.1.0.
 */
export const LEGACY_CLIENT_VERSION = "0.1.0";
/**
 * The refusal text. 0.1.0 shows an error's `message` as it is, in the online menu's footer, so
 * it must fit there: 44 characters, the footer holds about 45 at 640 pixels (finding 054).
 * Newer clients recognise the code, show their own copy and offer to update.
 */
export const OUTDATED_MESSAGE = "A new version is out. Update to play online.";

/** "X.Y.Z" (a leading "v" and a "-suffix" allowed) as numbers, or null. */
export function parseVersion(v: unknown): number[] | null {
  if (typeof v !== "string") return null;
  const m = v.trim().match(/^v?(\d{1,6})\.(\d{1,6})(?:\.(\d{1,6}))?(?:[-+][0-9A-Za-z.-]*)?$/);
  return m ? [Number(m[1]), Number(m[2]), Number(m[3] ?? 0)] : null;
}

/** <0, 0, >0 as a is older than, the same as, newer than b (both parsed). */
export function compareVersions(a: number[], b: number[]): number {
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] - b[i];
  return 0;
}

/** The release a hello comes from: its clientVersion, else 0.1.0 (see LEGACY_CLIENT_VERSION). */
export function clientVersionOf(hello: Record<string, unknown>): string {
  return hello.clientVersion === undefined ? LEGACY_CLIENT_VERSION : String(hello.clientVersion).slice(0, 32);
}

/** The configured minimum (MIN_VERSION), or DEFAULT_MIN_VERSION when it isn't a version. */
export function minVersionOf(configured: unknown): string {
  return parseVersion(configured) ? String(configured).trim() : DEFAULT_MIN_VERSION;
}

/** True when a client of this release may not play: older than the minimum, or unreadable. */
export function isOutdated(clientVersion: string, minVersion: string): boolean {
  const c = parseVersion(clientVersion);
  const min = parseVersion(minVersion) ?? parseVersion(DEFAULT_MIN_VERSION)!;
  return c === null || compareVersions(c, min) < 0;
}

// Join codes: no 0/O, 1/I/L.
export const CODE_ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
export const CODE_LENGTH = 6;

export function randomCode(): string {
  const bytes = new Uint8Array(CODE_LENGTH);
  crypto.getRandomValues(bytes);
  let code = "";
  for (const b of bytes) code += CODE_ALPHABET[b % CODE_ALPHABET.length];
  return code;
}

export function isValidCode(code: string): boolean {
  if (code.length !== CODE_LENGTH) return false;
  for (const c of code) if (!CODE_ALPHABET.includes(c)) return false;
  return true;
}

export function randomSeed(): number {
  return crypto.getRandomValues(new Uint32Array(1))[0];
}

export function median(values: number[]): number {
  const s = [...values].sort((a, b) => a - b);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}

/**
 * A member's round-trip from our pings. The first ping goes out right after connect, while the
 * connection is still warming up, and reads high (53-60 ms links measured 100+), so it only
 * counts until a later sample arrives; then it is the median of the later ones.
 */
export function measuredRtt(first: number | null, later: number[]): number | null {
  if (later.length) return Math.round(median(later));
  return first === null ? null : Math.round(first);
}

export function isByte(v: unknown): v is number {
  return Number.isInteger(v) && (v as number) >= 0 && (v as number) <= 255;
}

export function isU32(v: unknown): v is number {
  return Number.isInteger(v) && (v as number) >= 0 && (v as number) <= 0xffff_ffff;
}
