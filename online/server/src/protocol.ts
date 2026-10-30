// Wire protocol constants and pure helpers. The specification is docs/online-protocol.md.

export const PROTOCOL = 1;

export const MAX_MEMBERS = 8;
export const DEFAULT_MAX_PLAYERS = 4;
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
/** A racer heard from (any frame; the client pings every 500 ms in a race) no later than this ago
    is still there; after it, the server drops them as the client drops a silent server
    (port/net/client.c SILENT_MS). */
export const RACE_SILENT_MS = 10_000;
/** Text chat (finding 124): a message's length after trimming, in UTF-16 units, and how many a
    member may send in CHAT_WINDOW_MS. */
export const CHAT_MAX = 120;
export const CHAT_BURST = 5;
export const CHAT_WINDOW_MS = 10_000;

/** A chat message as the room shows it: control characters gone, runs of spaces one, trimmed.
    null when nothing is left or it is too long. */
export function chatText(v: unknown): string | null {
  if (typeof v !== "string") return null;
  const t = v.replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, " ").replace(/\s+/g, " ").trim();
  return t.length >= 1 && t.length <= CHAT_MAX ? t : null;
}
export const FEATURE_COUNTDOWN = "countdown";
export const RTT_SAMPLES = 5;

// ---- reconnect (finding 126) ----
/**
 * A member whose socket closes without a `leave` is a dropped connection: the room may hold its
 * slot (id, name, rider/bike, ready, host role) this long for it to come back with the secret
 * token its `welcome` carried. Only a member whose `hello` listed FEATURE_RECONNECT gets one; any
 * other member is removed at once, exactly as before, so older clients are untouched.
 * Overridable with the environment variable RECONNECT_WINDOW_MS (as COUNTDOWN_MS and
 * RACE_SILENT_MS are); the tests use a short one.
 */
export const RECONNECT_WINDOW_MS = 20_000;
/** The hello feature a client sends when it can reconnect and will keep the token. */
export const FEATURE_RECONNECT = "reconnect";
/** The reconnect token: 32 random bytes as lowercase hex (64 characters). */
export const RECONNECT_TOKEN_HEX = 64;

/** A fresh, unguessable reconnect token (crypto random). */
export function reconnectToken(): string {
  const bytes = new Uint8Array(RECONNECT_TOKEN_HEX / 2);
  crypto.getRandomValues(bytes);
  let token = "";
  for (const b of bytes) token += b.toString(16).padStart(2, "0");
  return token;
}

/** True when a `hello.reconnectToken` has the right shape before it is looked up. */
export function isReconnectToken(v: unknown): v is string {
  return typeof v === "string" && /^[0-9a-f]{64}$/.test(v);
}

/** Compares two equal-length tokens without an early exit (both are isReconnectToken-checked). */
export function tokensEqual(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

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
} as const;
export type ErrorCode = keyof typeof CLOSE_CODES | "not_host" | "not_ready" | "bad_value" | "slow_down";

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
