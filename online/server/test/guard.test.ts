// The abuse guard (finding 161): the kill switch, limits, caps, bans and the moderation log.
import { runInDurableObject } from "cloudflare:test";
import { env } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import worker from "../src/index";
import { OFF_MESSAGE } from "../src/guard";
import { Client } from "./helpers";

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
