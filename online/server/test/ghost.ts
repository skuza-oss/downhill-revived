// Builders for the ghost/replay container the leaderboard tests submit (src/replay.ts).
import { GHOST_HEADER_BYTES, GHOST_MAGIC, GHOST_VERSION, SNAPSHOT_WIRE_VERSION } from "../src/replay";

function put32(out: Uint8Array, at: number, v: number): void {
  out[at] = v & 0xff;
  out[at + 1] = (v >>> 8) & 0xff;
  out[at + 2] = (v >>> 16) & 0xff;
  out[at + 3] = (v >>> 24) & 0xff;
}

function concat(parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

/** A wire STATE snapshot: version, rider, race clock (ms), then any padding. */
export function snapshot(clockMs: number, opts: { wireVersion?: number; rider?: number; pad?: number } = {}): Uint8Array {
  const out = new Uint8Array(6 + (opts.pad ?? 0));
  out[0] = opts.wireVersion ?? SNAPSHOT_WIRE_VERSION;
  out[1] = opts.rider ?? 0xff; // 0xFF: the sender's own rider (finding 044)
  put32(out, 2, clockMs >>> 0);
  return out;
}

/** A length-prefixed stream entry: a 2-byte little-endian size then the payload. */
export function entry(payload: Uint8Array, lenOverride?: number): Uint8Array {
  const len = lenOverride ?? payload.length;
  const out = new Uint8Array(2 + payload.length);
  out[0] = len & 0xff;
  out[1] = (len >>> 8) & 0xff;
  out.set(payload, 2);
  return out;
}

export interface GhostParts {
  track: number;
  laps: number;
  seed: number;
  rider?: number;
  timeMs: number;
  entries: Uint8Array[];
  version?: number;
  magic?: string;
}

/** A whole DDGHOST1 container from already-built stream entries. */
export function container(p: GhostParts): Uint8Array {
  const header = new Uint8Array(GHOST_HEADER_BYTES);
  const magic = p.magic ?? GHOST_MAGIC;
  for (let i = 0; i < 8; i++) header[i] = magic.charCodeAt(i) & 0xff;
  header[8] = p.version ?? GHOST_VERSION;
  header[9] = p.track & 0xff;
  header[10] = p.laps & 0xff;
  header[11] = (p.laps >>> 8) & 0xff;
  put32(header, 12, p.timeMs >>> 0);
  put32(header, 16, p.seed >>> 0);
  header[20] = (p.rider ?? 0) & 0xff;
  header[21] = ((p.rider ?? 0) >>> 8) & 0xff;
  return concat([header, ...p.entries]);
}

/** A valid ghost: snapshots at the given race clocks (ms), finish time = the last clock. */
export function ghost(p: { track: number; laps: number; seed: number; rider?: number; clocks: number[]; timeMs?: number }): Uint8Array {
  return container({
    track: p.track,
    laps: p.laps,
    seed: p.seed,
    rider: p.rider,
    timeMs: p.timeMs ?? p.clocks[p.clocks.length - 1],
    entries: p.clocks.map((c) => entry(snapshot(c))),
  });
}

/** Snapshots every `step` ms from 0 to `untilMs`, as a run's close would look. */
export function clocks(untilMs: number, step = 50): number[] {
  const out: number[] = [];
  for (let t = 0; t <= untilMs; t += step) out.push(t);
  return out;
}
