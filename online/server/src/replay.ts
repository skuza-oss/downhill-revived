// Replay / ghost validation for leaderboard submissions (docs/online-protocol.md).
//
// A submitted replay is the client's **ghost file**: a small self-describing container around the
// same snapshot stream the race recorder already writes (`port/hle/online.c` record(), the bytes
// `DHD_NET_RECORD` saves; each entry is a 2-byte little-endian length then that many bytes of the
// wire STATE snapshot, version 3, finding 114).
//
// The server cannot re-simulate the game (the PS2/PC core is not on the server, and the client
// runs no server-owned physics), so this validation is **structural and coherence-based**:
//   - every length, count and value is bounds-checked against a fixed limit;
//   - the snapshot stream must be well formed (each entry fits, its wire version is known, its
//     race clock is a plausible non-negative value that never goes backwards);
//   - the replay must agree with the submitted track, lap count, seed and finish time.
// It does NOT prove the rider rode the track, that no trainer/cheat produced the snapshots, or that
// the time was earned fairly. That needs a deterministic server-side replay of the race, which is
// future work; this is the honest limit of the current check. See docs/findings/127-time-trial-boards.md.

export const GHOST_MAGIC = "DDGHOST1";
export const GHOST_VERSION = 1;
/** magic(8) + version(1) + track(1) + laps(2) + timeMs(4) + seed(4) + rider(2). */
export const GHOST_HEADER_BYTES = 22;

/** A submitted replay is at most this many bytes (a full lap's snapshots at 20 a second fit
    comfortably: 20 * 255 B/s * 600 s ≈ 3 MB worst case, so 1 MiB is about 3 minutes). */
export const REPLAY_MAX_BYTES = 1 << 20;
export const REPLAY_MAX_SNAPSHOTS = 20_000;
/** A snapshot: wire version (1) + slot (1) + race clock ms (4) is the smallest legal one. */
export const SNAPSHOT_MIN_BYTES = 6;
/** online.c caps one snapshot at 255 bytes (its size byte is one byte). */
export const SNAPSHOT_MAX_BYTES = 255;
/** The snapshot wire version this server knows (findings 044, 047, 114). */
export const SNAPSHOT_WIRE_VERSION = 3;
/** A plausible finish time: at least a second, under two hours. */
export const MIN_RACE_MS = 1_000;
export const MAX_RACE_MS = 2 * 60 * 60 * 1000;
/** How far the claimed time may differ from the replay's own clocks (packet timing at the finish). */
export const TIME_TOLERANCE_MS = 250;

export interface GhostFacts {
  version: number;
  track: number;
  laps: number;
  /** The finish time written in the container's header. */
  timeMs: number;
  seed: number;
  rider: number;
  /** Snapshots in the stream. */
  snapshots: number;
  /** Race clock (ms) of the first and last snapshot. */
  firstMs: number;
  lastMs: number;
}

export type GhostResult =
  | { ok: true; facts: GhostFacts }
  | { ok: false; error: string; message: string };

const bad = (error: string, message: string): GhostResult => ({ ok: false, error, message });

function u16(b: Uint8Array, at: number): number {
  return b[at] | (b[at + 1] << 8);
}

function u32(b: Uint8Array, at: number): number {
  return (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
}

/**
 * Parse and bounds-check a ghost file. Returns its facts, or why it is not a usable replay.
 * Pure: no storage, no clock, so tests call it directly.
 */
export function parseGhost(bytes: Uint8Array): GhostResult {
  if (bytes.length < GHOST_HEADER_BYTES)
    return bad("short", `A replay is at least ${GHOST_HEADER_BYTES} bytes.`);
  if (bytes.length > REPLAY_MAX_BYTES) return bad("too_large", `A replay is at most ${REPLAY_MAX_BYTES} bytes.`);
  for (let i = 0; i < 8; i++)
    if (bytes[i] !== GHOST_MAGIC.charCodeAt(i)) return bad("bad_magic", "Not a Downhill Revived replay.");

  const version = bytes[8];
  if (version !== GHOST_VERSION) return bad("bad_version", `Unknown replay version ${version}.`);
  const track = bytes[9];
  const laps = u16(bytes, 10);
  const timeMs = u32(bytes, 12);
  const seed = u32(bytes, 16);
  const rider = u16(bytes, 20);

  if (timeMs < MIN_RACE_MS || timeMs > MAX_RACE_MS)
    return bad("bad_time", "The replay's finish time is not plausible.");

  let at = GHOST_HEADER_BYTES;
  let snapshots = 0;
  let firstMs = -1;
  let lastMs = -1;
  while (at < bytes.length) {
    if (at + 2 > bytes.length) return bad("truncated", "A snapshot length is cut off.");
    const len = u16(bytes, at);
    if (len < SNAPSHOT_MIN_BYTES || len > SNAPSHOT_MAX_BYTES)
      return bad("bad_snapshot", `Snapshot size ${len} is out of range.`);
    if (at + 2 + len > bytes.length) return bad("truncated", "A snapshot runs past the end of the replay.");
    const p = at + 2;
    if (bytes[p] !== SNAPSHOT_WIRE_VERSION)
      return bad("bad_snapshot", `Unknown snapshot version ${bytes[p]}.`);
    const clock = u32(bytes, p + 2);
    if (clock > MAX_RACE_MS) return bad("bad_time", "A snapshot's race clock is not plausible.");
    if (lastMs >= 0 && clock < lastMs) return bad("backwards", "The replay's race clock goes backwards.");
    if (snapshots === 0) firstMs = clock;
    lastMs = clock;
    if (++snapshots > REPLAY_MAX_SNAPSHOTS) return bad("too_many", "Too many snapshots in the replay.");
    at += 2 + len;
  }
  if (snapshots === 0) return bad("empty", "The replay holds no snapshots.");

  return { ok: true, facts: { version, track, laps, timeMs, seed, rider, snapshots, firstMs, lastMs } };
}

export interface GhostClaim {
  track: number;
  laps: number;
  seed: number;
  timeMs: number;
  /** Loose: a client's seed may differ if a lobby never pinned one. true skips the seed check. */
  checkSeed?: boolean;
}

/**
 * Validate a ghost against the entry the client submitted with it. Real checks: the container
 * parses and is bounded, and its track, laps and finish time agree with the claim. Best-effort: it
 * does not replay the race, so a determined client can still fabricate a well-formed stream.
 */
export function validateGhost(bytes: Uint8Array, claim: GhostClaim): GhostResult {
  const parsed = parseGhost(bytes);
  if (!parsed.ok) return parsed;
  const f = parsed.facts;
  if (f.track !== claim.track) return bad("track_mismatch", "The replay is for another track.");
  if (f.laps !== claim.laps) return bad("laps_mismatch", "The replay is for another lap count.");
  if (claim.checkSeed && f.seed !== claim.seed) return bad("seed_mismatch", "The replay is from another race.");
  // The claimed time must match the replay's own header and its snapshots' clocks. A finish is at
  // or just after the last snapshot, so allow the tolerance and no more.
  if (Math.abs(f.timeMs - claim.timeMs) > TIME_TOLERANCE_MS)
    return bad("time_mismatch", "The replay's finish time does not match the entry.");
  if (claim.timeMs + TIME_TOLERANCE_MS < f.lastMs)
    return bad("time_mismatch", "The entry's time is before the replay's last snapshot.");
  if (f.lastMs - claim.timeMs > 5_000)
    return bad("time_mismatch", "The replay runs on long after the claimed finish.");
  return { ok: true, facts: f };
}
