import { exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import { CLIENT_VERSION, ISO } from "./helpers";
import { MAX_GHOST_BYTES, RATE_POSTS, fromBase64, toBase64 } from "../src/times";

// Each test uses its own track number, so the boards (one Durable Object each) start empty.
let nextIp = 1;
const ip = () => `198.51.100.${nextIp++}`;

function post(body: object, from = "203.0.113.1") {
  return exports.default.fetch("https://dhd.test/v1/times", {
    method: "POST",
    headers: { "content-type": "application/json", "cf-connecting-ip": from },
    body: JSON.stringify(body),
  });
}

const time = (track: number, name: string, timeMs: number, extra: object = {}) => ({
  version: "dhd-port test",
  clientVersion: CLIENT_VERSION,
  isoSha256: ISO,
  track,
  name,
  timeMs,
  ...extra,
});

const get = async (q: string) => {
  const r = await exports.default.fetch(`https://dhd.test${q}`);
  return { status: r.status, cors: r.headers.get("access-control-allow-origin"), body: await r.json<any>() };
};

describe("time-trial boards (finding 127)", () => {
  it("keeps the best time per name, fastest first, with ranks", async () => {
    const from = ip();
    expect(await (await post(time(10, "Ann", 65_000), from)).json()).toMatchObject({ ok: true, rank: 1, best: true, timeMs: 65_000 });
    expect(await (await post(time(10, "Bo", 61_000), from)).json()).toMatchObject({ rank: 1, best: true });
    // A slower run by Ann does not replace her best; a faster one does. Names match case-insensitively.
    expect(await (await post(time(10, "ann", 70_000), from)).json()).toMatchObject({ rank: 2, best: false, timeMs: 65_000 });
    expect(await (await post(time(10, "ANN", 60_000), from)).json()).toMatchObject({ rank: 1, best: true, timeMs: 60_000 });
    const r = await get("/v1/times?track=10&limit=20");
    expect(r.status).toBe(200);
    expect(r.cors).toBe("*");
    expect(r.body.times.map((t: any) => [t.rank, t.name, t.timeMs, t.ghost])).toEqual([
      [1, "ANN", 60_000, false],
      [2, "Bo", 61_000, false],
    ]);
    expect((await get("/v1/times?track=10&limit=1")).body.times).toHaveLength(1);
    // Another lap count is another board.
    expect((await get("/v1/times?track=10&laps=3")).body.times).toEqual([]);
  });

  it("stores a ghost and hands it back by rank", async () => {
    const from = ip();
    const bytes = new Uint8Array(5000).map((_, i) => (i * 31) & 255);
    await post(time(11, "Ghosty", 50_000, { ghost: toBase64(bytes) }), from);
    await post(time(11, "Plain", 55_000), from);
    const list = (await get("/v1/times?track=11")).body.times;
    expect(list.map((t: any) => t.ghost)).toEqual([true, false]);
    const g = await get("/v1/ghost?track=11&rank=1");
    expect(g.status).toBe(200);
    expect(g.body).toMatchObject({ track: 11, laps: 0, rank: 1, name: "Ghosty", timeMs: 50_000 });
    expect([...fromBase64(g.body.ghost)!]).toEqual([...bytes]);
    expect((await get("/v1/ghost?track=11&rank=2")).status).toBe(404);  // no ghost posted
    expect((await get("/v1/ghost?track=11&rank=9")).status).toBe(404);  // no such rank
  });

  it("refuses outdated releases, bad fields, implausible times and big ghosts", async () => {
    const from = ip();
    const outdated = await post(time(12, "Old", 60_000, { clientVersion: "0.1.0" }), from);
    expect(outdated.status).toBe(403);
    expect(await outdated.json()).toMatchObject({ error: "client_outdated", minVersion: "0.2.0", clientVersion: "0.1.0" });
    const cases: [object, number, string][] = [
      [time(12, "X", 60_000, { clientVersion: undefined }), 400, "bad_request"],
      [time(12, "X", 60_000, { isoSha256: "A".repeat(64) }), 400, "bad_request"],
      [time(256, "X", 60_000), 400, "bad_request"],
      [time(12, "X", 60_000, { laps: -1 }), 400, "bad_request"],
      [time(12, "", 60_000), 400, "bad_request"],
      [time(12, "x".repeat(25), 60_000), 400, "bad_request"],
      [time(12, "tab\there", 60_000), 400, "bad_request"],
      [time(12, "X", 9_999), 400, "bad_time"],
      [time(12, "X", 30 * 60_000 + 1), 400, "bad_time"],
      [time(12, "X", 60_000.5), 400, "bad_time"],
      [time(12, "X", 60_000, { ghost: "not base64!" }), 400, "bad_request"],
      [time(12, "X", 60_000, { ghost: toBase64(new Uint8Array(MAX_GHOST_BYTES + 1)) }), 413, "too_large"],
    ];
    for (const [body, status, error] of cases) {
      const r = await post(body, ip());
      expect([JSON.stringify(body).slice(0, 80), r.status]).toEqual([JSON.stringify(body).slice(0, 80), status]);
      expect((await r.json<any>()).error).toBe(error);
    }
    const junk = await exports.default.fetch("https://dhd.test/v1/times", { method: "POST", body: "{" });
    expect(junk.status).toBe(400);
    // A ghost of exactly the cap is fine.
    expect((await post(time(12, "Max", 60_000, { ghost: toBase64(new Uint8Array(MAX_GHOST_BYTES)) }), from)).status).toBe(200);
    expect((await get("/v1/times?track=12")).body.times.map((t: any) => t.name)).toEqual(["Max"]);
  });

  it("limits posts per address", async () => {
    const from = ip();
    for (let i = 0; i < RATE_POSTS; i++) expect((await post(time(13, `R${i}`, 60_000 + i), from)).status).toBe(200);
    const r = await post(time(13, "Late", 50_000), from);
    expect(r.status).toBe(429);
    expect((await r.json<any>()).error).toBe("slow_down");
    // Another address may still post.
    expect((await post(time(13, "Other", 50_000), ip())).status).toBe(200);
    expect((await get("/v1/times?track=13&limit=1")).body.times[0].name).toBe("Other");
  });

  it("checks the query and answers CORS preflights", async () => {
    expect((await get("/v1/times")).status).toBe(400);
    expect((await get("/v1/times?track=abc")).status).toBe(400);
    expect((await get("/v1/times?track=1&limit=0")).status).toBe(400);
    expect((await get("/v1/times?track=1&limit=101")).status).toBe(400);
    expect((await get("/v1/ghost?track=1&rank=0")).status).toBe(400);
    const pre = await exports.default.fetch("https://dhd.test/v1/times", { method: "OPTIONS" });
    expect(pre.status).toBe(204);
    expect(pre.headers.get("access-control-allow-origin")).toBe("*");
    expect(pre.headers.get("access-control-allow-methods")).toContain("POST");
    expect((await exports.default.fetch("https://dhd.test/v1/times?track=1", { method: "DELETE" })).status).toBe(405);
  });
});
