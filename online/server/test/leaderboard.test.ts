import { env, exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import {
  GHOST_MAGIC,
  MIN_RACE_MS,
  REPLAY_MAX_BYTES,
  REPLAY_MAX_SNAPSHOTS,
  parseGhost,
  validateGhost,
} from "../src/replay";
import { clocks, container, entry, ghost, snapshot } from "./ghost";

const post = (body: unknown) =>
  exports.default.fetch("https://dhd.test/v1/leaderboard", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

const get = (query: string) => exports.default.fetch(`https://dhd.test/v1/leaderboard?${query}`);

/** base64 the way the client will: standard alphabet, padded. */
function b64(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

/** A submission body for a table-compatible ghost, with the replay already base64. */
function body(over: Record<string, unknown> = {}, bytes?: Uint8Array): Record<string, unknown> {
  const replay = bytes ?? ghost({ track: 1, laps: 2, seed: 1234, rider: 5, clocks: clocks(5000) });
  return {
    track: 1,
    laps: 2,
    mode: "tt",
    name: "Ann",
    rider: 5,
    seed: 1234,
    timeMs: 5000,
    replay: b64(replay),
    ...over,
  };
}

describe("replay validator", () => {
  it("accepts a well-formed ghost and reports its facts", () => {
    const g = ghost({ track: 3, laps: 1, seed: 7, rider: 9, clocks: [0, 50, 100, 5000] });
    const r = parseGhost(g);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.facts).toMatchObject({ version: 1, track: 3, laps: 1, seed: 7, rider: 9, timeMs: 5000, snapshots: 4 });
    expect(r.facts.firstMs).toBe(0);
    expect(r.facts.lastMs).toBe(5000);
  });

  it("rejects short, mistyped and empty ghosts", () => {
    expect(parseGhost(new Uint8Array(4))).toMatchObject({ ok: false, error: "short" });
    expect(parseGhost(new Uint8Array(22))).toMatchObject({ ok: false, error: "bad_magic" });
    expect(parseGhost(container({ track: 0, laps: 0, seed: 1, timeMs: 5000, entries: [] }))).toMatchObject({
      ok: false,
      error: "empty",
    });
  });

  it("rejects an unknown version", () => {
    const g = ghost({ track: 0, laps: 0, seed: 1, clocks: [0, 5000], timeMs: 5000 });
    g[8] = 9;
    expect(parseGhost(g)).toMatchObject({ ok: false, error: "bad_version" });
  });

  it("bounds every snapshot length", () => {
    // A length that runs past the end of the buffer.
    const truncated = container({ track: 0, laps: 0, seed: 1, timeMs: 5000, entries: [entry(snapshot(0), 200)] });
    expect(parseGhost(truncated)).toMatchObject({ ok: false, error: "truncated" });
    // A length below the smallest snapshot or above 255 is rejected before any read.
    expect(parseGhost(container({ track: 0, laps: 0, seed: 1, timeMs: 5000, entries: [entry(snapshot(0), 3)] }))).toMatchObject(
      { ok: false, error: "bad_snapshot" },
    );
    expect(parseGhost(container({ track: 0, laps: 0, seed: 1, timeMs: 5000, entries: [entry(snapshot(0), 0xffff)] }))).toMatchObject(
      { ok: false, error: "bad_snapshot" },
    );
  });

  it("rejects an unknown snapshot wire version and a backwards clock", () => {
    expect(parseGhost(ghost({ track: 0, laps: 0, seed: 1, clocks: [0], timeMs: 5000 }))).toMatchObject({ ok: true });
    const wrongVersion = container({
      track: 0,
      laps: 0,
      seed: 1,
      timeMs: 5000,
      entries: [entry(snapshot(0, { wireVersion: 2 }))],
    });
    expect(parseGhost(wrongVersion)).toMatchObject({ ok: false, error: "bad_snapshot" });
    const backwards = ghost({ track: 0, laps: 0, seed: 1, clocks: [1000, 500, 2000], timeMs: 5000 });
    expect(parseGhost(backwards)).toMatchObject({ ok: false, error: "backwards" });
  });

  it("rejects more snapshots than the limit", () => {
    const many = container({
      track: 0,
      laps: 0,
      seed: 1,
      timeMs: 5000,
      entries: Array.from({ length: REPLAY_MAX_SNAPSHOTS + 1 }, (_, i) => entry(snapshot(i))),
    });
    expect(parseGhost(many)).toMatchObject({ ok: false, error: "too_many" });
  });

  it("checks the claim against the ghost", () => {
    const g = ghost({ track: 3, laps: 1, seed: 7, clocks: clocks(9000) });
    expect(validateGhost(g, { track: 3, laps: 1, seed: 7, timeMs: 9000, checkSeed: true })).toMatchObject({ ok: true });
    expect(validateGhost(g, { track: 4, laps: 1, seed: 7, timeMs: 9000, checkSeed: true })).toMatchObject({
      ok: false,
      error: "track_mismatch",
    });
    expect(validateGhost(g, { track: 3, laps: 2, seed: 7, timeMs: 9000, checkSeed: true })).toMatchObject({
      ok: false,
      error: "laps_mismatch",
    });
    expect(validateGhost(g, { track: 3, laps: 1, seed: 8, timeMs: 9000, checkSeed: true })).toMatchObject({
      ok: false,
      error: "seed_mismatch",
    });
    expect(validateGhost(g, { track: 3, laps: 1, seed: 7, timeMs: 4000, checkSeed: true })).toMatchObject({
      ok: false,
      error: "time_mismatch",
    });
  });
});

describe("leaderboard submissions", () => {
  it("accepts a valid entry and reports its rank", async () => {
    const res = await post(body());
    expect(res.status).toBe(200);
    const j = await res.json<any>();
    expect(j).toMatchObject({ ok: true, kept: true, rank: 1, track: 1, laps: 2, mode: "tt", timeMs: 5000 });
    expect(j.entries[0]).toMatchObject({ name: "Ann", timeMs: 5000, ticks: 300, laps: 2, mode: "tt" });
  });

  it("keeps only each player's best time, ascending", async () => {
    // Track 2: Bo 5000, Ann 5500, and Ann's slower 7000 changes nothing.
    expect(await (await post(body({ track: 2, name: "Ann", timeMs: 6000, replay: b64(ghost({ track: 2, laps: 2, seed: 1234, clocks: clocks(6000) })) }))).json<any>()).toMatchObject({ ok: true, kept: true, rank: 1 });
    expect(await (await post(body({ track: 2, name: "Bo", timeMs: 5000, replay: b64(ghost({ track: 2, laps: 2, seed: 1234, clocks: clocks(5000) })) }))).json<any>()).toMatchObject({ ok: true, kept: true, rank: 1 });
    expect(await (await post(body({ track: 2, name: "Ann", timeMs: 5500, replay: b64(ghost({ track: 2, laps: 2, seed: 1234, clocks: clocks(5500) })) }))).json<any>()).toMatchObject({ ok: true, kept: true, rank: 2 });

    const slower = await (await post(body({ track: 2, name: "Ann", timeMs: 7000, replay: b64(ghost({ track: 2, laps: 2, seed: 1234, clocks: clocks(7000) })) }))).json<any>();
    expect(slower).toMatchObject({ ok: true, kept: false, rank: 3 });

    const list = await (await get("track=2&laps=2&mode=tt")).json<any>();
    expect(list.entries.map((e: any) => [e.name, e.timeMs])).toEqual([
      ["Bo", 5000],
      ["Ann", 5500],
    ]);
  });

  it("rejects a corrupt or oversized replay", async () => {
    const badMagic = ghost({ track: 3, laps: 2, seed: 1234, clocks: clocks(5000) });
    badMagic[0] = 0;
    expect(await (await post(body({ track: 3 }, badMagic))).json<any>()).toMatchObject({ ok: false, error: "bad_magic" });

    const truncated = container({ track: 3, laps: 2, seed: 1234, timeMs: 5000, entries: [entry(snapshot(0), 200)] });
    expect(await (await post(body({ track: 3 }, truncated))).json<any>()).toMatchObject({ ok: false, error: "truncated" });

    const wrongVersion = container({ track: 3, laps: 2, seed: 1234, timeMs: 5000, entries: [entry(snapshot(0, { wireVersion: 2 }))] });
    expect(await (await post(body({ track: 3 }, wrongVersion))).json<any>()).toMatchObject({ ok: false, error: "bad_snapshot" });

    // Not base64 at all.
    expect(await (await post(body({ track: 3, replay: "not base64!!" }))).json<any>()).toMatchObject({
      ok: false,
      error: "bad_replay",
    });

    // A whole replay past the size limit.
    const huge = new Uint8Array(REPLAY_MAX_BYTES + 2);
    for (let i = 0; i < 8; i++) huge[i] = GHOST_MAGIC.charCodeAt(i);
    const res = await post(body({ track: 3 }, huge));
    expect(res.status).toBe(400);
    expect(await res.json<any>()).toMatchObject({ ok: false, error: "too_large" });
  });

  it("rejects an implausible time", async () => {
    expect(await (await post(body({ track: 4, timeMs: MIN_RACE_MS - 1 }))).json<any>()).toMatchObject({
      ok: false,
      error: "bad_time",
    });
    expect(await (await post(body({ track: 4, timeMs: 3 * 60 * 60 * 1000 }))).json<any>()).toMatchObject({
      ok: false,
      error: "bad_time",
    });
    // A plausible number that disagrees with the replay's own finish time.
    const off = b64(ghost({ track: 4, laps: 2, seed: 1234, clocks: clocks(5000) }));
    expect(await (await post(body({ track: 4, timeMs: 4000, replay: off }))).json<any>()).toMatchObject({
      ok: false,
      error: "time_mismatch",
    });
  });

  it("rejects a track, laps or seed that disagrees with the replay", async () => {
    expect(await (await post(body({ track: 6 }))).json<any>()).toMatchObject({ ok: false, error: "track_mismatch" });
    expect(await (await post(body({ laps: 1 }))).json<any>()).toMatchObject({ ok: false, error: "laps_mismatch" });
    expect(await (await post(body({ seed: 99 }))).json<any>()).toMatchObject({ ok: false, error: "seed_mismatch" });
  });

  it("lists a board and bounds the query", async () => {
    await post(body({ track: 5, laps: 1, name: "One", timeMs: 8000, replay: b64(ghost({ track: 5, laps: 1, seed: 1234, clocks: clocks(8000) })) }));
    const list = await (await get("track=5&laps=1&mode=tt")).json<any>();
    expect(list).toMatchObject({ protocol: 2, track: 5, laps: 1, mode: "tt" });
    expect(list.entries.map((e: any) => e.name)).toEqual(["One"]);

    expect((await get("track=5&laps=1&mode=tt&limit=999")).status).toBe(400);
    expect((await get("track=5&mode=nope")).status).toBe(400);
    expect((await get("laps=1")).status).toBe(400);
    expect(await (await get("track=5&laps=1&mode=race")).json<any>()).toMatchObject({ mode: "race", entries: [] });
  });

  it("rate-limits a client", async () => {
    const board = env.LEADERBOARDS.get(env.LEADERBOARDS.idFromName("global"));
    const key = `test-${Math.random()}`;
    expect(await board.allow(key, 2, 60_000)).toBe(true);
    expect(await board.allow(key, 2, 60_000)).toBe(true);
    expect(await board.allow(key, 2, 60_000)).toBe(false);
  });
});
