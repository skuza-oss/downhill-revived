// Public lobby list: one global Durable Object. Rooms push their entry when it changes and
// refresh it every 30 s; entries not refreshed for 90 s are dropped (a room that died without
// saying so).
import { DurableObject } from "cloudflare:workers";

export const LISTING_REFRESH_MS = 30_000;
const LISTING_TTL_MS = 90_000;

export interface LobbyEntry {
  phase?: string;
  raceStartedAt?: number | null;
  finishEndsAt?: number | null;
  nextRaceAt?: number | null;
  full?: boolean;
  code: string;
  roomName: string;
  version: string;
  track: number;
  players: number;
  maxPlayers: number;
  hostName: string;
}

interface Stored extends LobbyEntry {
  updatedAt: number;
}

export class LobbyIndex extends DurableObject<Env> {
  async upsert(entry: LobbyEntry): Promise<void> {
    await this.ctx.storage.put<Stored>(`room:${entry.code}`, { ...entry, updatedAt: Date.now() });
  }

  async remove(code: string): Promise<void> {
    await this.ctx.storage.delete(`room:${code}`);
  }

  async list(): Promise<LobbyEntry[]> {
    const now = Date.now();
    const rows = await this.ctx.storage.list<Stored>({ prefix: "room:" });
    const out: LobbyEntry[] = [];
    const stale: string[] = [];
    for (const [key, { updatedAt, ...entry }] of rows) {
      if (now - updatedAt > LISTING_TTL_MS) stale.push(key);
      else out.push(entry);
    }
    if (stale.length) await this.ctx.storage.delete(stale);
    return out.sort((a,b) => b.players - a.players);
  }
}
