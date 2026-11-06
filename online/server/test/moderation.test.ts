// Bans and the moderation log (finding 161), and the rule that no player ever receives another
// player's address, address hash or install id.
import { createExecutionContext, createScheduledController, waitOnExecutionContext } from "cloudflare:test";
import { env, exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import worker from "../src/index";
import { LOG_DAYS, banMessage, cleanup, findBan, ipHash, logJoin } from "../src/guard";
import { Client, randomIp } from "./helpers";

const KEY = { IP_HASH_KEY: "test-ip-hash-key" }; // vitest.config.ts
const hashOf = (ip: string) => ipHash(KEY, new Request("https://dhd.test/", { headers: { "CF-Connecting-IP": ip } }));
const randomInstall = () =>
  Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => b.toString(16).padStart(2, "0")).join("");
const DAY = 86_400_000;

/** The log's rows for a room, once `n` are there (the room writes them in the background). */
async function joinsOf(room: string, n: number): Promise<any[]> {
  for (let i = 0; i < 100; i++) {
    const { results } = await env.MODERATION.prepare("SELECT * FROM joins WHERE room = ?1 ORDER BY id").bind(room).all();
    if (results.length >= n) return results;
    await new Promise((r) => setTimeout(r, 20));
  }
  throw new Error(`fewer than ${n} joins logged for ${room}`);
}

async function ban(kind: "ip" | "install", value: string, expiresAt: number | null = null): Promise<void> {
  await env.MODERATION.prepare("INSERT INTO bans (kind, value, created_at, expires_at) VALUES (?1, ?2, ?3, ?4)")
    .bind(kind, value, Date.now(), expiresAt)
    .run();
}

async function hosted(name: string, ip: string, extra: object = {}): Promise<{ host: Client; code: string }> {
  const host = await Client.connect("/v1/rooms/new", { country: "PL", continent: "EU" }, ip);
  host.hello(name, extra);
  return { host, code: (await host.json("welcome")).code };
}

describe("moderation log", () => {
  it("records who entered which room, with the address's keyed hash and the install id", async () => {
    const [ipA, ipB] = [randomIp(), randomIp()];
    const installA = randomInstall();
    const { code } = await hosted("Ann", ipA, { installId: installA });
    const bo = await Client.connect(`/v1/rooms/${code}`, { country: "US", continent: "NA" }, ipB);
    bo.hello("Bo"); // an older release: no install id
    await bo.json("welcome");
    const rows = await joinsOf(code, 2);
    expect(rows.map((r) => [r.member, r.kind, r.name, r.client_version, r.country])).toEqual([
      [0, "host", "Ann", "0.2.0", "PL"],
      [1, "guest", "Bo", "0.2.0", "US"],
    ]);
    expect(rows[0].ip_hash).toBe(await hashOf(ipA));
    expect(rows[1].ip_hash).toBe(await hashOf(ipB));
    expect(rows[0].install_id).toBe(installA);
    expect(rows[1].install_id).toBeNull();
    for (const r of rows) expect(JSON.stringify(r)).not.toMatch(/198\.1[89]\./); // no address
  });

  it("takes a malformed install id as none, and logs a rejoin", async () => {
    const { host, code } = await hosted("Ann", randomIp(), { installId: "NOT-AN-ID" });
    const bo = await Client.join(code, "Bo");
    // Bo's connection drops (no close frame from the game) and comes back with its token.
    const again = await Client.connect(`/v1/rooms/${code}`);
    again.hello("Bo", { rejoinToken: bo.welcome.rejoinToken, installId: "c".repeat(32) });
    expect((await again.json("welcome")).memberId).toBe(bo.welcome.memberId);
    const rows = await joinsOf(code, 3);
    expect(rows.map((r) => [r.kind, r.name, r.install_id])).toEqual([
      ["host", "Ann", null],
      ["guest", "Bo", null],
      ["rejoin", "Bo", "c".repeat(32)],
    ]);
    expect(host.closeCode).toBeNull();
  });

  it("deletes rows older than 14 days and ended bans each day, and keeps the rest", async () => {
    const now = Date.now();
    const row = (at: number, room: string) => ({
      at, room, member: 0, kind: "guest" as const, name: "X", clientVersion: "0.7.11", country: "", ipHash: "", installId: null,
    });
    await logJoin(env, row(now - (LOG_DAYS + 1) * DAY, "OLDOLD"));
    await logJoin(env, row(now - (LOG_DAYS - 1) * DAY, "NEWNEW"));
    await ban("install", "e".repeat(32), now - 1000);
    await ban("install", "f".repeat(32), now + DAY);
    await ban("install", "0".repeat(32), null);
    const ctx = createExecutionContext();
    await worker.scheduled!(createScheduledController({ cron: "17 4 * * *" }), env, ctx);
    await waitOnExecutionContext(ctx);
    const rooms = (await env.MODERATION.prepare("SELECT room FROM joins WHERE room IN ('OLDOLD', 'NEWNEW')").all()).results;
    expect(rooms.map((r: any) => r.room)).toEqual(["NEWNEW"]);
    const bans = (await env.MODERATION.prepare("SELECT value FROM bans WHERE kind = 'install' ORDER BY value").all()).results;
    expect(bans.map((r: any) => r.value)).toEqual(["0".repeat(32), "f".repeat(32)]);
    expect(await cleanup({})).toEqual({ joins: 0, bans: 0 }); // no store: nothing to do
  });
});

describe("bans", () => {
  it("refuses a banned address before any room, with a plain message", async () => {
    const ip = randomIp();
    await ban("ip", await hashOf(ip), Date.parse("2099-01-02T00:00:00Z"));
    const c = await Client.connect("/v1/rooms/new", undefined, ip);
    expect(await c.json("error")).toEqual({ type: "error", code: "banned", message: "You can't play online until 2099-01-02." });
    expect(await c.closed()).toBe(4013);
    const other = await Client.connect("/v1/rooms/new", undefined, randomIp());
    other.hello("Fine");
    await other.json("welcome");
  });

  it("refuses a banned install id at hello; an ended ban, or none, lets the player in", async () => {
    const { code } = await Client.host("Ann");
    const banned = randomInstall();
    const ended = randomInstall();
    await ban("install", banned, null);
    await ban("install", ended, Date.now() - 1);
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello("Cheater", { installId: banned });
    expect(await c.json("error")).toEqual({ type: "error", code: "banned", message: "You can't play online on this server." });
    expect(await c.closed()).toBe(4013);
    for (const extra of [{ installId: ended }, {}]) {
      const ok = await Client.connect(`/v1/rooms/${code}`);
      ok.hello("Fine", extra);
      expect((await ok.json("welcome")).code).toBe(code);
    }
  });

  it("lets players in when the store can't be read (the failure is logged)", async () => {
    const broken = {
      prepare() {
        throw new Error("D1 is down");
      },
    } as unknown as D1Database;
    expect(await findBan({ MODERATION: broken }, { ip: "a".repeat(64), install: "b".repeat(32) })).toBeNull();
    await logJoin({ MODERATION: broken }, {
      at: 0, room: "X", member: 0, kind: "host", name: "X", clientVersion: "", country: "", ipHash: "", installId: null,
    });
    expect(await findBan({}, { ip: "a".repeat(64) })).toBeNull();
    expect(banMessage({ kind: "ip", expiresAt: null }).length).toBeLessThanOrEqual(45);
    expect(banMessage({ kind: "ip", expiresAt: Date.parse("2026-11-20T12:00:00Z") })).toBe("You can't play online until 2026-11-20.");
  });

  it("never takes a client's own x-dhd-ip-hash header for the log", async () => {
    const res = await exports.default.fetch("https://dhd.test/v1/rooms/new", {
      headers: { Upgrade: "websocket", "CF-Connecting-IP": "198.18.0.1", "x-dhd-ip-hash": "forged" },
    });
    res.webSocket!.accept();
    const c = new Client(res.webSocket!);
    c.hello("Forger");
    const { code } = await c.json("welcome");
    expect((await joinsOf(code, 1))[0].ip_hash).toBe(await hashOf("198.18.0.1"));
  });
});

describe("privacy", () => {
  it("no player ever receives another player's address, address hash or install id", async () => {
    const ips = [randomIp(), randomIp(), randomIp()];
    const installs = [randomInstall(), randomInstall(), randomInstall()];
    const secrets = [...ips, ...installs, ...(await Promise.all(ips.map(hashOf)))];
    const features = ["countdown", "session"];
    const { host, code } = await hosted("Ann", ips[0], { installId: installs[0], features });
    const others: Client[] = [];
    for (let i = 1; i < 3; i++) {
      const c = await Client.connect(`/v1/rooms/${code}`, { country: "DE", continent: "EU", colo: "FRA" }, ips[i]);
      c.hello(`P${i}`, { installId: installs[i], features });
      await c.json("welcome");
      others.push(c);
    }
    const all = [host, ...others];
    host.sendJson({ type: "settings", public: true, roomName: "Open room" });
    for (const c of all) c.sendJson({ type: "pick", rider: 2 });
    for (const c of all) c.sendJson({ type: "ready", ready: true });
    await Promise.all(all.map((c) => c.json("start")));
    host.sendJson({ type: "finish", ticks: 6000, place: 1 });
    others[0].sendJson({ type: "kick", member: 0 }); // an error to a non-host
    await others[0].json("error");
    others[1].sendJson({ type: "leave" });
    await host.json("room");
    const lobbies = await (await exports.default.fetch("https://dhd.test/v1/lobbies")).text();
    const health = await (await exports.default.fetch("https://dhd.test/v1/health")).text();
    const received = [...all.flatMap((c) => c.seen), lobbies, health];
    expect(received.length).toBeGreaterThan(10);
    for (const text of received) {
      for (const s of secrets) expect(text).not.toContain(s);
      expect(text).not.toMatch(/ip_?hash|installId|install_id/i);
    }
    // A player's country goes only to that player's own welcome (finding 135).
    for (const c of others) expect(c.seen.filter((t) => t.includes('"PL"'))).toEqual([]);
  });

  it("no refusal names an address, a hash or an install id either", async () => {
    const ip = randomIp();
    const install = randomInstall();
    await ban("ip", await hashOf(ip), null);
    await ban("install", install, null);
    const a = await Client.connect("/v1/rooms/new", undefined, ip);
    const b = await Client.connect("/v1/rooms/new");
    b.hello("B", { installId: install });
    await a.closed();
    await b.closed();
    const hash = await hashOf(ip);
    for (const t of [...a.seen, ...b.seen]) {
      expect(t).not.toContain(ip);
      expect(t).not.toContain(hash);
      expect(t).not.toContain(install);
    }
    expect(a.seen.length + b.seen.length).toBe(2);
  });
});
