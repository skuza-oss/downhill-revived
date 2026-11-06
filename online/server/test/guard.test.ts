// The abuse guard (finding 161): the kill switch, limits, caps, bans and the moderation log.
import { runInDurableObject } from "cloudflare:test";
import { env } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import worker from "../src/index";
import { OFF_MESSAGE, RATE_MESSAGE, ipHash } from "../src/guard";
import { Client, randomIp } from "./helpers";

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
