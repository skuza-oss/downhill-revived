// The abuse guard (finding 161): the kill switch, limits, caps, bans and the moderation log.
import { runInDurableObject } from "cloudflare:test";
import { env } from "cloudflare:workers";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import worker from "../src/index";
import {
  BAN_CACHE_MAX, BAN_CACHE_MS, BINARY_PER_WINDOW, FAILED_CACHE_MS, MESSAGE_MAX, OFF_MESSAGE, RATE_MESSAGE, SWITCH_CACHE_MS,
  TEXT_PER_WINDOW, TOO_FAST_MESSAGE, TOO_FAST_WARNING, findBan, ipHash, newTally, offMessage, onlineState, resetGuardCaches, tally,
} from "../src/guard";
import { FRAME_PING, FRAME_PONG } from "../src/protocol";
import { Client, randomIp, startedRace } from "./helpers";

/** The Worker with some variables changed (the test run's own are fixed). */
function fetchWith(vars: Record<string, string>, path: string, init: RequestInit = {}): Promise<Response> {
  return worker.fetch(new Request(`https://dhd.test${path}`, init), { ...env, ...vars } as Env);
}

async function refusalOf(res: Response): Promise<{ error: any; code: number }> {
  expect(res.status).toBe(101);
  const ws = res.webSocket!;
  const got = new Promise<{ error: any; code: number }>((resolve) => {
    let error: any = null;
    ws.addEventListener("message", (e) => {
      error = JSON.parse(e.data as string);
    });
    ws.addEventListener("close", (e) => {
      resolve({ error, code: e.code });
    });
  });
  ws.accept();
  return got;
}

/** The kill switch as tools/moderate.mjs sets it (`online off [--message]`, `online on`). */
async function setOnline(on: boolean, message = ""): Promise<void> {
  const now = Date.now();
  await env.MODERATION.batch(
    [["online", on ? "on" : "off"], ["online_message", message]].map(([k, v]) =>
      env.MODERATION.prepare(
        "INSERT INTO settings (key, value, updated_at) VALUES (?1, ?2, ?3)" +
          " ON CONFLICT (key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at",
      ).bind(k, v, now),
    ),
  );
}

/** A D1 database that counts its queries (the caches' tests). */
function counting(db: D1Database): { db: D1Database; reads: () => number } {
  let n = 0;
  const wrapped = { prepare: (q: string) => (n++, db.prepare(q)) } as unknown as D1Database;
  return { db: wrapped, reads: () => n };
}

const ROOM_UPGRADE = { headers: { Upgrade: "websocket" } };

describe("kill switch", () => {
  afterEach(() => setOnline(true));

  it("is on by default and says so in the health check", async () => {
    await env.MODERATION.prepare("DELETE FROM settings").run();
    const health = await (await fetchWith({}, "/v1/health")).json<any>();
    expect(health).toMatchObject({ ok: true, online: true });
    expect(health.message).toBeUndefined();
    const sw = await onlineState({}); // no store: on
    expect(sw.on).toBe(true);
  });

  it("refuses new rooms at once and joins at hello with a plain message while off", async () => {
    const { code: open } = await Client.host("Ann");
    await setOnline(false);
    const { error, code } = await refusalOf(await fetchWith({}, "/v1/rooms/new", ROOM_UPGRADE));
    expect(error).toEqual({ type: "error", code: "online_off", message: OFF_MESSAGE });
    expect(code).toBe(4010);
    for (const room of [open, "ABCDEF"]) {
      const c = await Client.connect(`/v1/rooms/${room}`);
      c.hello("Late");
      expect(await c.json("error")).toEqual({ type: "error", code: "online_off", message: OFF_MESSAGE });
      expect(await c.closed()).toBe(4010);
    }
    expect(OFF_MESSAGE.length).toBeLessThanOrEqual(45); // the ONLINE screen's footer
    const health = await (await fetchWith({}, "/v1/health")).json<any>();
    expect(health).toMatchObject({ ok: true, online: false, message: OFF_MESSAGE });
    const list = await (await fetchWith({}, "/v1/lobbies")).json<any>();
    expect(list).toMatchObject({ online: false, lobbies: [] });
    // On again: rooms again.
    await setOnline(true);
    expect((await (await fetchWith({}, "/v1/health")).json<any>()).online).toBe(true);
    await Client.join(open, "Bo");
  });

  it("takes the operator's own message", async () => {
    await setOnline(false, "  Back at 18:00 UTC. ");
    const { error } = await refusalOf(await fetchWith({}, "/v1/rooms/new", ROOM_UPGRADE));
    expect(error.message).toBe("Back at 18:00 UTC.");
    expect(offMessage("x".repeat(500)).length).toBe(MESSAGE_MAX);
    expect(offMessage("  ")).toBe(OFF_MESSAGE);
  });

  it("starts no race in a room while off; its members stay and come back, nobody new gets in", async () => {
    const { host, code } = await Client.host("Ann");
    const bo = await Client.join(code, "Bo");
    await setOnline(false);
    for (const c of [host, bo]) c.sendJson({ type: "ready", ready: true });
    await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members.every((m: any) => m.ready));
    host.sendJson({ type: "start" });
    const err = await host.json("error");
    expect(err).toMatchObject({ code: "online_off", message: OFF_MESSAGE });
    host.sendJson({ type: "ping", t: 1 });
    expect((await host.json("pong")).t).toBe(1); // still connected
    expect(host.closeCode).toBeNull();
    // Bo's connection drops and comes back with its token: let back in.
    const again = await Client.connect(`/v1/rooms/${code}`);
    again.hello("Bo", { rejoinToken: bo.welcome.rejoinToken });
    expect((await again.json("welcome")).memberId).toBe(bo.welcome.memberId);
    // A new player is not.
    const cy = await Client.connect(`/v1/rooms/${code}`);
    cy.hello("Cy");
    expect((await cy.json("error")).code).toBe("online_off");
  });

  // (A rejoin during a race is a session room's (finding 137); the lobby's rejoin above is the
  // same check at hello.)
  it("lets a race that is running go on", async () => {
    const { clients, code } = await startedRace(2);
    await setOnline(false);
    // Race frames go over the SFU (finding 158); the room keeps answering the race's pings and
    // keeps its race.
    for (const c of clients) {
      c.ws.send(new Uint8Array([FRAME_PING, 0, 7]).buffer);
      expect([...(await c.binary())]).toEqual([FRAME_PONG, clients.indexOf(c), 7]);
    }
    const race = await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), (room: any) => room.state?.race?.startAt);
    expect(race).toBeTypeOf("number");
  });
});

describe("what an isolate keeps of the store", () => {
  // These pass envs without GUARD_CACHE (vitest.config.ts turns the cache off for the other tests).
  beforeEach(() => resetGuardCaches());
  afterEach(() => setOnline(true));

  it("reads the kill switch once per SWITCH_CACHE_MS, and sees a change after it", async () => {
    const { db, reads } = counting(env.MODERATION);
    const t = Date.now();
    await setOnline(true);
    const [a, b] = await Promise.all([onlineState({ MODERATION: db }, t), onlineState({ MODERATION: db }, t)]);
    expect([a.on, b.on, reads()]).toEqual([true, true, 1]); // one read for both
    await setOnline(false, "Maintenance.");
    expect((await onlineState({ MODERATION: db }, t + SWITCH_CACHE_MS - 1)).on).toBe(true);
    expect(reads()).toBe(1);
    expect(await onlineState({ MODERATION: db }, t + SWITCH_CACHE_MS)).toEqual({ on: false, message: "Maintenance." });
    expect(reads()).toBe(2);
  });

  it("keeps the last state when the store can't be read, and asks again soon", async () => {
    const t = Date.now();
    await setOnline(false);
    expect((await onlineState({ MODERATION: env.MODERATION }, t)).on).toBe(false);
    let fails = 0;
    const broken = { prepare: () => { fails++; throw new Error("D1 is down"); } } as unknown as D1Database;
    expect((await onlineState({ MODERATION: broken }, t)).on).toBe(false); // still off, not on
    expect((await onlineState({ MODERATION: broken }, t + FAILED_CACHE_MS - 1)).on).toBe(false);
    expect(fails).toBe(1);
    await onlineState({ MODERATION: broken }, t + FAILED_CACHE_MS);
    expect(fails).toBe(2);
    resetGuardCaches();
    expect((await onlineState({ MODERATION: broken }, t)).on).toBe(true); // nothing read yet: on
  });

  it("keeps a ban lookup BAN_CACHE_MS: a new ban, or a lifted one, bites after it", async () => {
    const { db, reads } = counting(env.MODERATION);
    const install = "1".repeat(31) + String(Math.floor(Math.random() * 10));
    const t = Date.now();
    const look = (at: number) => findBan({ MODERATION: db }, { install }, at);
    expect(await look(t)).toBeNull();
    await env.MODERATION.prepare("INSERT INTO bans (kind, value, created_at, expires_at) VALUES ('install', ?1, ?2, NULL)")
      .bind(install, t).run();
    expect(await look(t + BAN_CACHE_MS - 1)).toBeNull(); // not yet
    expect(reads()).toBe(1);
    expect(await look(t + BAN_CACHE_MS)).toEqual({ kind: "install", expiresAt: null });
    await env.MODERATION.prepare("DELETE FROM bans WHERE value = ?1").bind(install).run();
    expect(await look(t + 2 * BAN_CACHE_MS - 1)).not.toBeNull(); // lifted, not yet seen
    expect(await look(t + 2 * BAN_CACHE_MS)).toBeNull();
    expect(reads()).toBe(3);
  });

  it("drops a kept ban once it has ended, without asking the store", async () => {
    const { db, reads } = counting(env.MODERATION);
    const install = "2".repeat(31) + String(Math.floor(Math.random() * 10));
    const t = Date.now();
    await env.MODERATION.prepare("INSERT INTO bans (kind, value, created_at, expires_at) VALUES ('install', ?1, ?2, ?3)")
      .bind(install, t, t + 5_000).run();
    expect(await findBan({ MODERATION: db }, { install }, t)).toEqual({ kind: "install", expiresAt: t + 5_000 });
    expect(await findBan({ MODERATION: db }, { install }, t + 5_000)).toBeNull();
    expect(reads()).toBe(1);
  });

  it("keeps a failed ban lookup only FAILED_CACHE_MS, and at most BAN_CACHE_MAX lookups", async () => {
    let n = 0;
    const broken = { prepare: () => { n++; throw new Error("D1 is down"); } } as unknown as D1Database;
    const t = Date.now();
    expect(await findBan({ MODERATION: broken }, { ip: "a".repeat(64) }, t)).toBeNull();
    expect(await findBan({ MODERATION: broken }, { ip: "a".repeat(64) }, t + FAILED_CACHE_MS - 1)).toBeNull();
    expect(n).toBe(1);
    await findBan({ MODERATION: broken }, { ip: "a".repeat(64) }, t + FAILED_CACHE_MS);
    expect(n).toBe(2);

    let reads = 0;
    const empty = { prepare: () => ({ bind: () => ({ first: async () => (reads++, null) }) }) } as unknown as D1Database;
    for (let i = 0; i <= BAN_CACHE_MAX; i++) await findBan({ MODERATION: empty }, { ip: `ip${i}` }, t);
    expect(reads).toBe(BAN_CACHE_MAX + 1);
    await findBan({ MODERATION: empty }, { ip: `ip${BAN_CACHE_MAX}` }, t); // the newest: kept
    expect(reads).toBe(BAN_CACHE_MAX + 1);
    await findBan({ MODERATION: empty }, { ip: "ip0" }, t); // the oldest: made room for the newest
    expect(reads).toBe(BAN_CACHE_MAX + 2);
  });
});

describe("limits per connection source", () => {
  async function upgrade(path: string, ip: string): Promise<Response> {
    return worker.fetch(new Request(`https://dhd.test${path}`, { headers: { Upgrade: "websocket", "CF-Connecting-IP": ip } }), env);
  }

  it("refuses an address that makes too many rooms in a minute, not another address", async () => {
    const ip = randomIp();
    let refused: { error: any; code: number } | null = null;
    let made = 0;
    for (let i = 0; i < 15 && !refused; i++) {
      const res = await upgrade("/v1/rooms/new", ip);
      // a room's socket waits for hello; a refusal closes at once
      const c = new Client(res.webSocket!);
      res.webSocket!.accept();
      c.hello(`P${i}`);
      try {
        await c.json("welcome");
        made++;
      } catch {
        refused = { error: null, code: c.closeCode! };
      }
    }
    expect(made).toBe(10); // wrangler.jsonc RL_CREATE
    expect(refused?.code).toBe(4011);
    const other = await Client.connect("/v1/rooms/new", undefined, randomIp());
    other.hello("Other");
    expect((await other.json("welcome")).code).toMatch(/^[A-Z0-9]{6}$/);
  });

  it("refuses an address that opens too many connections, with its own code and message", async () => {
    const ip = randomIp();
    const { code } = await Client.host("Ann");
    let last: { error: any; code: number } | null = null;
    for (let i = 0; i < 31; i++) {
      const res = await upgrade(`/v1/rooms/${code}`, ip);
      if (i < 30) {
        res.webSocket!.accept();
        res.webSocket!.close(1000);
      } else last = await refusalOf(res);
    }
    expect(last).toEqual({ error: { type: "error", code: "rate_limited", message: RATE_MESSAGE }, code: 4011 });
    expect(RATE_MESSAGE.length).toBeLessThanOrEqual(45);
  });

  it("hashes the address with the key: stable, keyed, and never the address", async () => {
    const req = (ip: string) => new Request("https://dhd.test/", { headers: { "CF-Connecting-IP": ip } });
    const a = await ipHash({ IP_HASH_KEY: "k1" }, req("203.0.113.7"));
    expect(a).toMatch(/^[0-9a-f]{64}$/);
    expect(await ipHash({ IP_HASH_KEY: "k1" }, req("203.0.113.7"))).toBe(a);
    expect(await ipHash({ IP_HASH_KEY: "k2" }, req("203.0.113.7"))).not.toBe(a);
    expect(await ipHash({ IP_HASH_KEY: "k1" }, req("203.0.113.8"))).not.toBe(a);
    expect(a).not.toContain("203");
    expect(await ipHash({ IP_HASH_KEY: "k1" }, new Request("https://dhd.test/"))).toBe("");
  });
});

describe("caps per member", () => {
  it("caps binary and text at ten times what a game sends", () => {
    expect(BINARY_PER_WINDOW / 5).toBe(20); // a second: the race's two pings, ten times
    expect(TEXT_PER_WINDOW / 5).toBe(20);
  });

  it("counts per window: ok, then warn once, drop, and close the next window over", () => {
    const t = newTally(0);
    for (let i = 0; i < TEXT_PER_WINDOW; i++) expect(tally(t, false, 10)).toBe("ok");
    expect(tally(t, false, 10)).toBe("warn");
    expect(tally(t, false, 20)).toBe("drop");
    expect(tally(t, true, 20)).toBe("ok"); // binary has its own count
    expect(tally(t, false, 5_000)).toBe("ok"); // a new window
    for (let i = 1; i < TEXT_PER_WINDOW; i++) tally(t, false, 5_001);
    expect(tally(t, false, 5_002)).toBe("close");
  });

  it("warns a member who sends too many messages, then closes it the next time", async () => {
    const { host, code } = await Client.host("Ann");
    const bo = await Client.join(code, "Bo");
    // Windows of a minute (a loaded test runtime takes over 1 s for 100 messages), and every
    // socket's window starts anew when the test says (nextWindow).
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const nextWindow = () =>
      runInDurableObject(stub, (room: any, state) => {
        room.env = { ...room.env, CAP_WINDOW_MS: "60000" };
        for (const ws of state.getWebSockets()) {
          const t = room.tallies.get(ws);
          if (t) t.start = -1e12;
        }
      });
    await nextWindow();
    for (let i = 0; i <= TEXT_PER_WINDOW; i++) bo.sendJson({ type: "ping", t: i });
    expect(await bo.json("error")).toEqual({ type: "error", code: "too_fast", message: TOO_FAST_WARNING });
    expect(bo.closeCode).toBeNull();
    await nextWindow();
    for (let i = 0; i <= TEXT_PER_WINDOW; i++) bo.sendJson({ type: "ping", t: i });
    const err = await bo.next((f) => typeof f === "string" && JSON.parse(f).type === "error" && JSON.parse(f).code === "too_fast" && JSON.parse(f).message === TOO_FAST_MESSAGE);
    expect(err).toBeTruthy();
    expect(await bo.closed()).toBe(4012);
    // Bo is gone from the room; the host stays.
    await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members.length === 1);
    expect(host.closeCode).toBeNull();
  });

  it("answers no more binary frames than the cap and warns the sender", async () => {
    const { clients, code } = await startedRace(2);
    const [a, b] = clients;
    // A long window, so the test runtime's speed does not matter; this room's cap is 50.
    const cap = 50;
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), (room: any) => {
      room.env = { ...room.env, CAP_WINDOW_MS: "60000", CAP_BINARY: String(cap) };
    });
    for (let i = 0; i < cap + 20; i++) a.ws.send(new Uint8Array([FRAME_PING, 0, i]).buffer);
    let got = 0;
    let warned: unknown = null;
    try {
      // Count every pong until the frames stop (the warning comes among them).
      await a.next((f) => {
        if (typeof f !== "string") got++;
        else if (JSON.parse(f).type === "error") warned = JSON.parse(f).code;
        return false;
      }, 1_500);
    } catch {}
    expect(warned).toBe("too_fast");
    expect(got).toBe(cap);
    // A pong goes to its sender only: nothing on the socket reaches another member.
    await expect(b.next((f) => typeof f !== "string", 300)).rejects.toThrow();
  });
});
