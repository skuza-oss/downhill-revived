// Wire protocol constants and pure helpers. The specification is docs/online-protocol.md.

export const PROTOCOL = 4;

export const MAX_MEMBERS = 10;
export const DEFAULT_MAX_PLAYERS = 10;
export const MAX_TEXT_BYTES = 4096;

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

/** The one binary message on a room's WebSocket: the game's PING and the server's PONG, which adds
 *  the server's clock. The race's frames travel over the Realtime SFU (finding 158) or, in a P2P
 *  room, the peer connections (finding 180); any other binary message is dropped. */
export const FRAME_PING = 3;
export const FRAME_PONG = 4;
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
  /** A P2P room's host left: the room ends with it (src/p2p.ts; no host migration). */
  host_left: 4014,
} as const;
export type ErrorCode = keyof typeof CLOSE_CODES | "not_host" | "not_ready" | "bad_value";

// ---- The client version gate ----

/**
 * The oldest release that may play when wrangler.jsonc's MIN_VERSION is missing or not a version:
 * the first that speaks protocol 4.
 */
export const DEFAULT_MIN_VERSION = "0.7.13";
/**
 * The refusal text. Every release shows an error's `message` in the online menu's footer, which
 * holds about 45 characters at 640 pixels (finding 054); this is 44. Since 0.2.0 the game also
 * offers Update Now for its code.
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

/** The release a hello comes from, "" when it names none (0.1.0 did not): unreadable, so outdated. */
export function clientVersionOf(hello: Record<string, unknown>): string {
  return hello.clientVersion === undefined ? "" : String(hello.clientVersion).slice(0, 32);
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
/** A P2P room's code is one longer (src/p2p.ts, finding 180): the game tells the two kinds apart
    by length alone, and a room that shares addresses is harder to guess. */
export const P2P_CODE_LENGTH = 7;

export function randomCode(length = CODE_LENGTH): string {
  const bytes = new Uint8Array(length);
  crypto.getRandomValues(bytes);
  let code = "";
  for (const b of bytes) code += CODE_ALPHABET[b % CODE_ALPHABET.length];
  return code;
}

export function isValidCode(code: string, length = CODE_LENGTH): boolean {
  if (code.length !== length) return false;
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
