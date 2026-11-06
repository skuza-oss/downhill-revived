// The abuse guard (finding 161): the kill switch, limits, caps, bans and the moderation log.
import { runInDurableObject } from "cloudflare:test";
import { env } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import worker from "../src/index";
import {
  BINARY_PER_WINDOW, OFF_MESSAGE, RATE_MESSAGE, TEXT_PER_WINDOW, TOO_FAST_MESSAGE, TOO_FAST_WARNING, ipHash, newTally, tally,
} from "../src/guard";
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

describe("kill switch", () => {
  it("is on by default and says so in the health check", async () => {
    const health = await (await fetchWith({}, "/v1/health")).json<any>();
    expect(health).toMatchObject({ ok: true, online: true });
    expect(health.message).toBeUndefined();
  });

  it("refuses new rooms and joins with a plain message while off", async () => {
    const off = { ONLINE: "off" };
    for (const path of ["/v1/rooms/new", "/v1/rooms/ABCDEF"]) {
      const { error, code } = await refusalOf(await fetchWith(off, path, { headers: { Upgrade: "websocket" } }));
      expect(error).toEqual({ type: "error", code: "online_off", message: OFF_MESSAGE });
      expect(code).toBe(4010);
    }
    expect(OFF_MESSAGE.length).toBeLessThanOrEqual(45); // the ONLINE screen's footer
    const health = await (await fetchWith(off, "/v1/health")).json<any>();
    expect(health).toMatchObject({ ok: true, online: false, message: OFF_MESSAGE });
    const list = await (await fetchWith(off, "/v1/lobbies")).json<any>();
    expect(list).toMatchObject({ online: false, lobbies: [] });
  });

  it("takes the operator's own message", async () => {
    const vars = { ONLINE: "OFF", ONLINE_MESSAGE: "Back at 18:00 UTC." };
    const { error } = await refusalOf(await fetchWith(vars, "/v1/rooms/new", { headers: { Upgrade: "websocket" } }));
    expect(error.message).toBe("Back at 18:00 UTC.");
  });

  it("starts no race in a room while off, and the room stays", async () => {
    const { host, code } = await Client.host("Ann");
    const bo = await Client.join(code, "Bo");
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    await runInDurableObject(stub, (room: any) => {
      room.env = { ...room.env, ONLINE: "off" };
    });
    for (const c of [host, bo]) c.sendJson({ type: "ready", ready: true });
    await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members.every((m: any) => m.ready));
    host.sendJson({ type: "start" });
    const err = await host.json("error");
    expect(err).toMatchObject({ code: "online_off", message: OFF_MESSAGE });
    host.sendJson({ type: "ping", t: 1 });
    expect((await host.json("pong")).t).toBe(1); // still connected
    expect(host.closeCode).toBeNull();
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
  const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

  it("caps binary at twice the worst a game sends, text at ten times", () => {
    expect(BINARY_PER_WINDOW / 5).toBe(400); // a second: 10 riders at 20 snapshots, twice
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
    await sleep(1100); // a new window (1 s in the tests)
    for (let i = 0; i <= TEXT_PER_WINDOW; i++) bo.sendJson({ type: "ping", t: i });
    expect(await bo.json("error")).toEqual({ type: "error", code: "too_fast", message: TOO_FAST_WARNING });
    expect(bo.closeCode).toBeNull();
    await sleep(1100);
    for (let i = 0; i <= TEXT_PER_WINDOW; i++) bo.sendJson({ type: "ping", t: i });
    const err = await bo.next((f) => typeof f === "string" && JSON.parse(f).type === "error" && JSON.parse(f).code === "too_fast" && JSON.parse(f).message === TOO_FAST_MESSAGE);
    expect(err).toBeTruthy();
    expect(await bo.closed()).toBe(4012);
    // Bo is gone from the room; the host stays.
    await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members.length === 1);
    expect(host.closeCode).toBeNull();
  });

  it("relays no more race frames than the cap and warns the sender", async () => {
    const { clients, code } = await startedRace(2);
    const [a, b] = clients;
    // The test runtime relays about one frame each 2 ms, so 2000 would take 4 s: this room's cap is
    // 50 a long window.
    const cap = 50;
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), (room: any) => {
      room.env = { ...room.env, CAP_WINDOW_MS: "60000", CAP_BINARY: String(cap) };
    });
    const frame = new Uint8Array([5, 0, 1, 2, 3]);
    for (let i = 0; i < cap + 20; i++) a.ws.send(frame);
    const warned = await a.next((f) => typeof f === "string" && JSON.parse(f).type === "error", 20_000);
    expect(JSON.parse(warned as string).code).toBe("too_fast");
    let got = 0;
    try {
      for (;;) {
        await b.binary();
        got++;
      }
    } catch {}
    expect(got).toBe(cap);

  });
});
