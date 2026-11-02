// A player's rejoin after their connection dropped (finding 137, GitHub issue 30): they get their
// own member back, and the old connection's member doesn't linger as a clone.
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { describe, it, expect } from "vitest";
import { Client } from "./helpers";

const FEATURES = { features: ["session", "countdown"] };
const room = (code: string) => env.ROOM.get(env.ROOM.idFromName(code));
const snap = async (c: Client, pred: (r: any) => boolean = () => true) =>
  JSON.parse(await c.next(f => typeof f === "string" && JSON.parse(f).type === "room" && pred(JSON.parse(f))) as string);
const names = (r: any) => r.members.map((m: any) => m.name);

/** A session room: Host and P1, P1 on rider 7 and ready. */
async function setup() {
  const h = await Client.connect("/v1/rooms/new");
  h.hello("Host", FEATURES);
  const { code } = await h.json("welcome");
  const p1 = await Client.join(code, "P1");
  p1.sendJson({ type: "pick", rider: 7, bike: 2, choosing: false });
  p1.sendJson({ type: "ready", ready: true });
  await snap(h, r => r.members.length === 2 && r.members[1].ready && r.members[1].rider === 7);
  return { h, p1, code };
}

/** The game again, as its rejoin does: a new connection, the same hello. */
async function rejoin(code: string, name: string) {
  const c = await Client.connect(`/v1/rooms/${code}`);
  c.hello(name, FEATURES);
  return { c, welcome: await c.json("welcome") };
}

/** The room sees a member's connection end without a close frame (1006), as a dropped network. */
async function dropped(code: string, memberId: number) {
  await runInDurableObject(room(code), async (r: any) => {
    await r.webSocketClose(r.sockets.get(memberId), 1006, "", false);
  });
}

describe("rejoin after a drop", () => {
  it("a player whose old socket hasn't closed yet gets their member back, Ready and rider kept", async () => {
    const { h, p1, code } = await setup();
    // The old connection is dead but still open here: P1 sends nothing more.
    const { c, welcome } = await rejoin(code, "P1");
    expect(welcome).toMatchObject({ memberId: 1, host: false });
    const r = await snap(h, r => r.members.length === 2);
    expect(names(r)).toEqual(["Host", "P1"]);
    expect(r.members[1]).toMatchObject({ id: 1, ready: true, rider: 7, bike: 2 });
    // The old socket is closed for the room's part; its late close changes nothing.
    expect(await p1.closed()).toBe(1000);
    await runInDurableObject(room(code), async (r: any, ctx) => {
      for (const ws of ctx.getWebSockets()) if ((ws.deserializeAttachment() as any).gone) await r.webSocketClose(ws, 1006, "", false);
    });
    h.sendJson({ type: "ping", t: 1 }); await h.json("pong");
    c.sendJson({ type: "ready", ready: false });
    const after = await snap(h, r => !r.members[1].ready);
    expect(names(after)).toEqual(["Host", "P1"]);
    expect(after.members[1].id).toBe(1);
  });

  it("the host back before its old socket closes stays host; no other player takes over", async () => {
    const { h, p1, code } = await setup();
    const { c, welcome } = await rejoin(code, "Host");
    expect(welcome).toMatchObject({ memberId: 0, host: true });
    const r = await snap(p1, r => r.members.length === 2);
    expect(r.hostId).toBe(0);
    expect(names(r)).toEqual(["Host", "P1"]);
    expect(await h.closed()).toBe(1000);
    // The rejoined host can act as host.
    c.sendJson({ type: "settings", roomName: "Back" });
    expect((await snap(p1, r => r.roomName === "Back")).hostId).toBe(0);
  });

  it("a member waits for its rejoin after a 1006 drop, host rights included", async () => {
    const { p1, code } = await setup();
    await dropped(code, 0);
    // Still in the room (no clone, no new host) while the game reconnects.
    const r = await snap(p1);
    expect([r.hostId, names(r)]).toEqual([0, ["Host", "P1"]]);
    await runDurableObjectAlarm(room(code));
    expect(names(await snap(p1))).toEqual(["Host", "P1"]);
    const { welcome } = await rejoin(code, "Host");
    expect(welcome).toMatchObject({ memberId: 0, host: true });
    const back = await snap(p1, r => r.members.length === 2);
    expect([back.hostId, names(back)]).toEqual([0, ["Host", "P1"]]);
  });

  it("a member not back within the grace leaves, and the host passes on", async () => {
    const { p1, code } = await setup();
    await dropped(code, 0);
    await runInDurableObject(room(code), async (r: any) => { r.state.members[0].awaySince = Date.now() - 60_000; });
    await runDurableObjectAlarm(room(code));
    const r = await snap(p1, r => r.members.length === 1);
    expect([r.hostId, names(r)]).toEqual([1, ["P1"]]);
  });

  it("a socket silent too long is dropped even though no close came", async () => {
    const { h, p1, code } = await setup();
    await runInDurableObject(room(code), async (r: any) => { r.heard.set(r.sockets.get(1), Date.now() - 60_000); });
    await runDurableObjectAlarm(room(code));
    expect(names(await snap(h, r => r.members.length === 1))).toEqual(["Host"]);
    expect(await p1.closed()).toBe(1001);
  });

  it("a player who speaks keeps their member: another player of the same name joins as a new one", async () => {
    const { h, p1, code } = await setup();
    // P1's game is alive: it pings more often than the rejoin's quiet time (300 ms in the tests).
    const timer = setInterval(() => p1.sendJson({ type: "ping", t: 1 }), 50);
    try {
      const { welcome } = await rejoin(code, "P1");
      expect(welcome.memberId).toBe(2);
      const r = await snap(h, r => r.members.length === 3);
      expect(names(r)).toEqual(["Host", "P1", "P1"]);
      expect(r.members[1]).toMatchObject({ id: 1, ready: true, rider: 7 });
      expect(p1.closeCode).toBe(null);
    } finally {
      clearInterval(timer);
    }
  });

  it("a rejoin while the old socket was quiet for a while takes the member back at once", async () => {
    const { h, code } = await setup();
    await runInDurableObject(room(code), async (r: any) => { r.heard.set(r.sockets.get(1), Date.now() - 10_000); });
    const t = Date.now();
    const { welcome } = await rejoin(code, "P1");
    expect(welcome.memberId).toBe(1);
    expect(Date.now() - t).toBeLessThan(250);
    expect(names(await snap(h))).toEqual(["Host", "P1"]);
  });

  it("a clean close (the game left) still removes the member at once", async () => {
    const { h, p1 } = await setup();
    p1.ws.close(1000);
    expect(names(await snap(h, r => r.members.length === 1))).toEqual(["Host"]);
  });
});
