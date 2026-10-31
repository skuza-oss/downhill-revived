// Worst-time leaderboards: one global Durable Object holding a per-(track, laps, mode) best list.
//
// The project stores everything a server must remember in Durable Objects (rooms, the lobby
// index), so this uses Durable Object storage rather than introducing KV or D1: the write rate is
// one row per finished run, the read is one small list, and the object gives a single serialisation
// point where "keep the best per player" is race-free (Durable Objects' input gates make the
// read-modify-write of a list atomic).
//
// Storage layout in this object:
//   list:<track>:<laps>:<mode>        LeaderboardEntry[], ascending by time, at most LEADERBOARD_MAX
//   best:<track>:<laps>:<mode>:<key>  number, that player's best time (see nameKey), so a worse
//                                     later run does not re-enter after its earlier row aged out
//   rl:<clientKey>                    number[], recent submission times for the rate limit
//
// Nothing here verifies the run: the Worker validates the shape and the replay, then calls submit.
// What is checked and what is only best-effort is in docs/online-protocol.md and the finding.
import { DurableObject } from "cloudflare:workers";
import { nameKey } from "./protocol";

export const MODES = ["tt", "race"] as const;
export type Mode = (typeof MODES)[number];
export function isMode(v: unknown): v is Mode {
  return v === "tt" || v === "race";
}

/** The longest list a single (track, laps, mode) keeps. */
export const LEADERBOARD_MAX = 50;
/** The most rows a `GET /v1/leaderboard` may ask for. */
export const LEADERBOARD_LIMIT_MAX = 100;
/** Submissions allowed per client in the window (a Worker variable overrides it). */
export const SUBMIT_MAX = 30;
export const SUBMIT_WINDOW_MS = 60_000;

export interface LeaderboardEntry {
  name: string;
  /** Finish time in milliseconds. */
  timeMs: number;
  /** The same time in race ticks (1/60 s), as the game shows it. */
  ticks: number;
  rider: number;
  seed: number;
  laps: number;
  mode: Mode;
  /** Server time of the run that stored this row. */
  at: number;
}

export interface SubmitInput {
  track: number;
  laps: number;
  mode: Mode;
  name: string;
  timeMs: number;
  rider: number;
  seed: number;
  /** For the rate limit: cf-connecting-ip when present, else the name. Best-effort. */
  clientKey: string;
}

export interface SubmitResult {
  /** A new personal best was recorded (the only thing that changes storage). */
  kept: boolean;
  /** 1-based position this time has among everyone's bests. */
  rank: number;
  timeMs: number;
  entries: LeaderboardEntry[];
}

const listKey = (track: number, laps: number, mode: Mode) => `list:${track}:${laps}:${mode}`;
const bestKey = (track: number, laps: number, mode: Mode, key: string) => `best:${track}:${laps}:${mode}:${key}`;

export class Leaderboard extends DurableObject<Env> {
  private async list(track: number, laps: number, mode: Mode): Promise<LeaderboardEntry[]> {
    return (await this.ctx.storage.get<LeaderboardEntry[]>(listKey(track, laps, mode))) ?? [];
  }

  /** The best runs for one board, ascending by time. */
  async top(track: number, laps: number, mode: Mode, limit: number): Promise<LeaderboardEntry[]> {
    const list = await this.list(track, laps, mode);
    return list.slice(0, Math.max(0, limit));
  }

  /**
   * Records a run when it beats that player's stored best, and reports its position either way.
   * The list is capped at LEADERBOARD_MAX; the per-player best (bestKey) survives a row aging out,
   * so a later slower run never re-enters.
   */
  async submit(input: SubmitInput): Promise<SubmitResult> {
    const { track, laps, mode, name, timeMs } = input;
    const key = nameKey(name);
    const list = await this.list(track, laps, mode);
    const previous = await this.ctx.storage.get<number>(bestKey(track, laps, mode, key));

    if (previous !== undefined && timeMs >= previous) {
      // Not the player's best: storage is untouched; still report where this time would sit.
      let rank = 1;
      for (const e of list) if (e.timeMs < timeMs) rank++;
      return { kept: false, rank, timeMs, entries: list };
    }

    const entry: LeaderboardEntry = {
      name,
      timeMs,
      ticks: Math.round(timeMs * 0.06),
      rider: input.rider,
      seed: input.seed,
      laps,
      mode,
      at: Date.now(),
    };
    const merged = list.filter((e) => nameKey(e.name) !== key);
    merged.push(entry);
    merged.sort((a, b) => a.timeMs - b.timeMs || a.at - b.at);
    const rank = 1 + merged.findIndex((e) => nameKey(e.name) === key);
    const capped = merged.slice(0, LEADERBOARD_MAX);
    await this.ctx.storage.put(listKey(track, laps, mode), capped);
    await this.ctx.storage.put(bestKey(track, laps, mode, key), timeMs);
    return { kept: true, rank, timeMs, entries: capped };
  }

  /** The submission rate limit: false when this client has used `max` in the last `windowMs`. */
  async allow(clientKey: string, max: number, windowMs: number): Promise<boolean> {
    const now = Date.now();
    const key = `rl:${clientKey}`;
    const recent = ((await this.ctx.storage.get<number[]>(key)) ?? []).filter((t) => now - t < windowMs);
    if (recent.length >= max) return false;
    recent.push(now);
    await this.ctx.storage.put(key, recent);
    return true;
  }
}
