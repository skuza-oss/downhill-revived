import { describe, it, expect, vi } from "vitest";
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { Client, startedRace } from "./helpers";
import { fakeRealtime, publishedSession } from "./sfu-fake";
import { Realtime, Turn, TURN_TTL_S } from "../src/sfu";

describe("Realtime control plane", () => {
  it("releases race traffic only after every subscriber acknowledges", async () => {
    const {clients, code} = await startedRace(2);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
      const ids = room.state.members.map((m: any) => m.id);
      for (const member of room.state.members) {
        member.sfuRace = room.state.race.startAt;
        member.sfu = {id:`fake-${member.id}`, phase:"ready", channels: ids.filter((id: number) => id !== member.id)
          .map((id: number, k: number) => ({memberId:id, name:"state", channelId:20 + k, local:false}))};
      }
    });
    clients[0].sendJson({type:"sfu_ready"});
    await expect(clients[0].next(f => typeof f === "string" && JSON.parse(f).type === "sfu_ready", 50)).rejects.toThrow();
    clients[1].sendJson({type:"sfu_ready"});
    await Promise.all(clients.map(c => c.json("sfu_ready")));
    clients[0].sendJson({type:"end"});
    await clients[0].json("ended");
  });
  it("rejects lobby allocations and caps attempts at one per race", async () => {
    const { host } = await Client.host();
    host.sendJson({type:"sfu_offer", sdp:"private"});
    expect((await host.json("error")).message).toContain("active race");
    const {clients, code} = await startedRace(3);
    // A second offer in the same race is refused (the first one is under way or done).
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
      room.state.members[1].sfuRace = room.state.race.startAt;
    });
    clients[1].sendJson({type:"sfu_offer", sdp:"private"});
    expect((await clients[1].json("error")).message).toContain("One Realtime session");
    // Fake test environment has no app secrets: a failed attempt takes the racer out of the race,
    // so it has no second one either.
    clients[0].sendJson({type:"sfu_offer", sdp:"private"});
    expect((await clients[0].json("error")).message).toBe("Realtime connection failed: out of the race.");
    clients[0].sendJson({type:"sfu_offer", sdp:"private"});
    expect((await clients[0].json("error")).message).toContain("active race");
    clients[0].sendJson({type:"end"});
    await clients[0].json("ended");
    clients[0].sendJson({type:"sfu_offer", sdp:"private"});
    expect((await clients[0].json("error")).message).toContain("active race");
  });
  it("allocates matching policies and never discloses another player's SDP or addresses", async () => {
    const requests: { path: string; body: any }[] = [];
    let next = 0, channel = 10;
    const fake: typeof fetch = async (input, init) => {
      const path = String(input), body = init?.body ? JSON.parse(String(init.body)) : null;
      requests.push({ path, body });
      let result: object = {};
      if (path.endsWith("sessions/new")) result = { sessionId: `session-${++next}`, sessionDescription: { type: "answer", sdp: `SFU-${next}` }, address: "MUST-NOT-ESCAPE", clientSdp: body.sessionDescription.sdp };
      else if (path.endsWith("datachannels/close")) result = { dataChannels: body.dataChannels };
      else if (path.endsWith("datachannels/new")) result = { dataChannels: body.dataChannels.map((x: any) => ({ ...x, id: channel++ })) };
      return Response.json(result);
    };
    const api = new Realtime({ REALTIME_APP_ID: "test-app", REALTIME_APP_TOKEN: "fake" }, fake);
    const a = await api.open("PRIVATE-PLAYER-A"), b = await api.open("PRIVATE-PLAYER-B");
    const messagesA = [a.message, { type: "sfu_channels", channels: await api.publish(1, a.session) }];
    const messagesB = [b.message, { type: "sfu_channels", channels: await api.publish(2, b.session) }];
    messagesA.push({ type: "sfu_channels", channels: await api.subscribe(a.session, 2, b.session) });
    messagesB.push({ type: "sfu_channels", channels: await api.subscribe(b.session, 1, a.session) });
    for (const messages of [messagesA, messagesB]) {
      const wire = JSON.stringify(messages);
      expect(wire).not.toMatch(/PRIVATE|address|sessionId|session-/);
      for (const msg of messages) expect(Object.keys(msg).sort()).toEqual(msg.type === "sfu_answer" ? ["sdp", "type"] : ["channels", "type"]);
    }
    expect(JSON.stringify(messagesA)).not.toContain("SFU-2");
    expect(JSON.stringify(messagesB)).not.toContain("SFU-1");
    for (const request of requests.filter(x => x.path.endsWith("datachannels/new"))) {
      expect(request.body.dataChannels[0]).toMatchObject({ ordered: false, maxRetransmits: 0 });
      expect(request.body.dataChannels[1]).toMatchObject({ ordered: true });
      expect(request.body.dataChannels[1]).not.toHaveProperty("maxRetransmits");
    }
    expect(await api.subscribe(a.session, 2, b.session)).toEqual([]);
    await expect(api.publish(1, a.session)).rejects.toThrow();
    const subscriptions = a.session.channels.filter(c => !c.local);
    await api.close(a.session, subscriptions);
    expect(a.session.channels).toHaveLength(2);
    expect(requests.at(-1)?.body).toEqual({ dataChannels: subscriptions.map(c => ({ id: c.channelId })) });
  });
  it("does not bind the fetch function to the Realtime instance", async () => {
    const request: typeof fetch = async function(this: unknown) {
      expect(this).toBeUndefined();
      return Response.json({ sessionId: "binding-test", sessionDescription: { type: "answer", sdp: "SFU" } });
    };
    const result = await new Realtime({ REALTIME_APP_ID: "fake", REALTIME_APP_TOKEN: "fake" }, request).open("private");
    expect(result.message).toEqual({ type: "sfu_answer", sdp: "SFU" });
  });
  it("fails closed without credentials or with per-channel errors", async () => {
    await expect(new Realtime({}).open("test-offer")).rejects.toThrow("not configured");
    const api = new Realtime({ REALTIME_APP_ID: "fake", REALTIME_APP_TOKEN: "fake" }, async () => Response.json({ dataChannels: [{ errorCode: "failed" }] }));
    await expect(api.publish(1, { id: "fake", phase: "connect", channels: [] })).rejects.toThrow();
    const partial = new Realtime({ REALTIME_APP_ID: "fake", REALTIME_APP_TOKEN: "fake" }, async () => Response.json({ dataChannels: [
      { dataChannelName: "state", id: 10 }, { dataChannelName: "events", errorCode: "failed" }
    ] }));
    const session = { id: "fake", phase: "connect" as const, channels: [] };
    await expect(partial.publish(1, session)).rejects.toThrow();
    expect(session.channels).toEqual([{ memberId: 1, name: "state", channelId: 10, local: true }]);
  });
});

describe("Realtime setup while the race's members change", () => {
  it("subscribes every remaining racer when one leaves during sfu_connected's API calls", async () => {
    const { clients, code } = await startedRace(3);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    // The first subscription waits until the test lets it go: a leave comes in meanwhile.
    fake.hold = (path, body) => path.endsWith("datachannels/new") && !!body.dataChannels[0].sessionId && fake.held === 0;
    try {
      await runInDurableObject(stub, async (room: any) => {
        room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        const [a, b, c] = room.state.members;
        a.sfu = publishedSession(a.id, [b.id]);
        b.sfu = publishedSession(b.id, [a.id]);
        c.sfu = { id: `session-${c.id}`, phase: "connect", channels: [] };
        for (const m of room.state.members) m.sfuRace = room.state.race.startAt;
      });
      // Member 2 (slot 2) connects; its first subscription is to member 0's publication.
      clients[2].sendJson({ type: "sfu_connected" });
      for (let i = 0; i < 200 && !fake.held; i++) await new Promise((r) => setTimeout(r, 10));
      expect(fake.held).toBe(1);
      clients[0].sendJson({ type: "leave" });
      await clients[1].json("left");
      fake.release();
      // Members 1 and 2 subscribe to each other, so both can acknowledge and be released.
      const got1 = await clients[1].next((f) => typeof f === "string" && JSON.parse(f).type === "sfu_channels" &&
        JSON.parse(f).channels.some((c: any) => c.memberId === 2));
      expect(JSON.parse(got1 as string).channels.every((c: any) => !c.local)).toBe(true);
      await clients[2].next((f) => typeof f === "string" && JSON.parse(f).type === "sfu_channels" &&
        JSON.parse(f).channels.some((c: any) => c.memberId === 1));
      await runInDurableObject(stub, async (room: any) => {
        await room.sfuQueue;
        const [b, c] = room.state.members;
        expect(b.sfu.channels.some((x: any) => !x.local && x.memberId === c.id)).toBe(true);
        expect(c.sfu.channels.some((x: any) => !x.local && x.memberId === b.id)).toBe(true);
        expect(room.state.members.every((m: any) => m.sfu.channels.every((x: any) => x.memberId !== 0))).toBe(true);
      });
      clients[1].sendJson({ type: "sfu_ready" });
      clients[2].sendJson({ type: "sfu_ready" });
      await Promise.all([clients[1].json("sfu_ready"), clients[2].json("sfu_ready")]);
    } finally {
      fake.restore();
    }
  });
});

describe("A racer whose Realtime setup fails or never ends", () => {
  /** Members 0 and 1 published, subscribed to each other and acknowledged; member 2 connecting. */
  async function twoReady(stub: DurableObjectStub, extra: (room: any) => void = () => {}) {
    await runInDurableObject(stub, async (room: any) => {
      room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
      const [a, b, c] = room.state.members;
      a.sfu = publishedSession(a.id, [b.id]);
      b.sfu = publishedSession(b.id, [a.id]);
      c.sfu = { id: `session-${c.id}`, phase: "connect", channels: [] };
      for (const m of room.state.members) m.sfuRace = room.state.race.startAt;
      a.sfuReady = b.sfuReady = true;
      extra(room);
    });
  }

  it("takes a racer whose publication fails out of the race and releases the others", async () => {
    const { clients, code } = await startedRace(3);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    fake.fail = (path) => path.endsWith("datachannels/new");
    try {
      await twoReady(stub);
      clients[2].sendJson({ type: "sfu_connected" });
      expect((await clients[2].json("error")).message).toBe("Realtime connection failed: out of the race.");
      for (const c of clients.slice(0, 2)) {
        expect(await c.json("left")).toMatchObject({ slot: 2, memberId: 2 });
        await c.json("sfu_ready");
      }
      await runInDurableObject(stub, async (room: any) => {
        await room.sfuQueue;
        expect(room.state.race.left).toEqual([2]);
        expect(room.state.race.sfuReleased).toBe(true);
        // Still in the room, not in the race, its session closed.
        expect(room.state.members.map((m: any) => m.id)).toEqual([0, 1, 2]);
        expect(room.state.members[2].sfu).toBeUndefined();
      });
      clients[2].sendJson({ type: "sfu_connected" });
      expect((await clients[2].json("error")).message).toContain("active race");
    } finally {
      fake.restore();
    }
  });

  it("takes a racer whose subscription fails out, not the one that connected", async () => {
    const { clients, code } = await startedRace(3);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    // Member 1's session refuses the subscription to member 2's publication.
    fake.fail = (path) => path.includes("/sessions/session-1/datachannels/new");
    try {
      await twoReady(stub);
      clients[2].sendJson({ type: "sfu_connected" });
      expect((await clients[1].json("error")).message).toBe("Realtime connection failed: out of the race.");
      expect(await clients[0].json("left")).toMatchObject({ slot: 1 });
      expect(await clients[2].json("left")).toMatchObject({ slot: 1 });
      // Members 0 and 2 have each other; their acknowledgements release them.
      clients[2].sendJson({ type: "sfu_ready" });
      await Promise.all([clients[0].json("sfu_ready"), clients[2].json("sfu_ready")]);
    } finally {
      fake.restore();
    }
  });

  it("takes a racer that never finishes its setup out when the time is up (race room)", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("P0", { features: ["session"] });
    const { code } = await host.json("welcome");
    const clients = [host, await Client.join(code, "P1"), await Client.join(code, "P2")];
    for (const c of clients) c.sendJson({ type: "ready", ready: true });
    await Promise.all(clients.map((c) => c.json("start")));
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    try {
      // Member 2 never sends its offer.
      await twoReady(stub, (room) => {
        delete room.state.members[2].sfu;
        delete room.state.members[2].sfuRace;
        room.state.race.sfuBy = Date.now() - 1;
      });
      expect(await runDurableObjectAlarm(stub)).toBe(true);
      expect((await clients[2].json("error")).message).toBe("Realtime connection failed: out of the race.");
      for (const c of clients.slice(0, 2)) {
        expect(await c.json("left")).toMatchObject({ slot: 2, memberId: 2, reason: "left" });
        await c.json("sfu_ready");
      }
      // Its game reads that it is out of the race (port/net/client.c) and goes back to the room.
      const room = await clients[2].next((f) => typeof f === "string" && JSON.parse(f).type === "room" &&
        JSON.parse(f).members.some((m: any) => m.id === 2 && m.inRace === false));
      const me = JSON.parse(room as string).members.find((m: any) => m.id === 2);
      expect(me).toMatchObject({ raceState: "dnf", dnfReason: "left" });
      expect(JSON.parse(room as string).phase).toBe("racing");
    } finally {
      fake.restore();
    }
  });
});

describe("Cleanup that fails", () => {
  /** Every racer published and subscribed to every other; member 0 also has TURN credentials. */
  async function allPublished(stub: DurableObjectStub) {
    await runInDurableObject(stub, async (room: any) => {
      room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token", TURN_KEY_ID: "key", TURN_KEY_API_TOKEN: "token" };
      const ids = room.state.members.map((m: any) => m.id);
      for (const m of room.state.members) {
        m.sfu = publishedSession(m.id, ids.filter((id: number) => id !== m.id));
        m.sfuRace = room.state.race.startAt;
      }
      room.state.members[2].turnUser = "u2";
      room.state.members[2].turnUntil = Date.now() + 3_600_000;
    });
  }

  it("still closes the others' subscriptions to a kicked racer and tries its own close again later", async () => {
    const { clients, code } = await startedRace(3);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    let down = true;
    fake.fail = (path) => down && (path.endsWith("/revoke") || path.includes("/sessions/session-2/datachannels/close"));
    try {
      await allPublished(stub);
      clients[0].sendJson({ type: "kick", member: 2 });
      // Its revoke and its own close fail; the subscriptions to it close and the others hear so.
      const closed0 = await clients[0].json("sfu_closed");
      const closed1 = await clients[1].json("sfu_closed");
      expect(closed0.channelIds.sort()).toEqual([202, 302]);
      expect(closed1.channelIds.sort()).toEqual([212, 312]);
      await runInDurableObject(stub, async (room: any) => {
        await room.sfuQueue;
        expect(room.state.members.map((m: any) => m.id)).toEqual([0, 1]);
        expect(room.cleanup.map((x: any) => x.turnUser ?? x.session)).toEqual(["u2", "session-2"]);
        expect(room.cleanup[1].channels.sort((a: number, b: number) => a - b)).toEqual([21, 22, 220, 221, 320, 321]);
        expect((await room.ctx.storage.get("cleanup")).length).toBe(2);
        // Not yet due: the alarm leaves it.
        expect(room.cleanup.every((x: any) => x.at > Date.now())).toBe(true);
      });
      // A failed retry waits longer; then one that works drops it.
      await runInDurableObject(stub, async (room: any) => { for (const x of room.cleanup) x.at = 0; });
      await runDurableObjectAlarm(stub);
      await runInDurableObject(stub, async (room: any) => {
        expect(room.cleanup.map((x: any) => x.tries)).toEqual([1, 1]);
        expect(room.cleanup.every((x: any) => x.at > Date.now() + 5_000)).toBe(true);
        for (const x of room.cleanup) x.at = 0;
      });
      down = false;
      const before = fake.calls.length;
      await runDurableObjectAlarm(stub);
      const retried = fake.calls.slice(before).map((c) => c.path.split("/").slice(-3).join("/"));
      expect(retried).toEqual(["credentials/u2/revoke", "session-2/datachannels/close"]);
      await runInDurableObject(stub, async (room: any) => {
        expect(room.cleanup).toEqual([]);
        expect(await room.ctx.storage.get("cleanup")).toBeUndefined();
      });
    } finally {
      fake.restore();
    }
  });

  it("closes every racer at the race's end past one failure, and keeps what failed after the room ends", async () => {
    const { clients, code } = await startedRace(3);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    // The first racer's session refuses to close (the old loop stopped there).
    fake.fail = (path) => path.includes("/sessions/session-0/datachannels/close");
    try {
      await allPublished(stub);
      clients[0].sendJson({ type: "end" });
      await Promise.all(clients.map((c) => c.json("ended")));
      await runInDurableObject(stub, async (room: any) => {
        await room.sfuQueue;
        expect(room.state.members.every((m: any) => m.sfu === undefined && m.turnUser === undefined)).toBe(true);
        expect(room.cleanup.map((x: any) => x.session)).toEqual(["session-0"]);
      });
      const closes = fake.calls.filter((c) => c.path.endsWith("datachannels/close")).map((c) => c.path.split("/").at(-3));
      expect(new Set(closes)).toEqual(new Set(["session-0", "session-1", "session-2"]));
      expect(fake.calls.some((c) => c.path.endsWith("credentials/u2/revoke"))).toBe(true);
      // Everyone leaves: the room ends, its cleanup stays with an alarm.
      for (const c of clients) c.sendJson({ type: "leave" });
      for (const c of clients) await c.closed();
      await runInDurableObject(stub, async (room: any) => {
        await room.sfuQueue;
        expect(room.state).toBeNull();
        expect((await room.ctx.storage.get("cleanup")).length).toBe(1);
        expect(await room.ctx.storage.getAlarm()).not.toBeNull();
        // Its time is up: dropped without another call.
        room.cleanup[0].until = 0;
      });
      const before = fake.calls.length;
      await runDurableObjectAlarm(stub);
      expect(fake.calls.length).toBe(before);
      await runInDurableObject(stub, async (room: any) => {
        expect(room.cleanup).toEqual([]);
        expect(await room.ctx.storage.get("cleanup")).toBeUndefined();
      });
    } finally {
      fake.restore();
    }
  });
});

describe("TURN over TLS fallback", () => {
  const ICE = { iceServers: [
    { urls: ["stun:stun.cloudflare.com:3478"] },
    { urls: ["turn:turn.cloudflare.com:3478?transport=udp", "turns:turn.cloudflare.com:5349?transport=tcp", "turns:turn.cloudflare.com:443?transport=tcp"],
      username: "u".repeat(64), credential: "c".repeat(64) },
  ] };

  it("mints one racer's credentials with a TTL and a tag, and keeps only the TLS servers", async () => {
    const requests: { path: string; body: any }[] = [];
    const fake: typeof fetch = async (input, init) => {
      requests.push({ path: String(input), body: init?.body ? JSON.parse(String(init.body)) : null });
      return String(input).endsWith("/revoke") ? new Response(null, { status: 204 }) : Response.json(ICE, { status: 201 });
    };
    const turn = new Turn({ TURN_KEY_ID: "key", TURN_KEY_API_TOKEN: "token" }, fake);
    const { username, message } = await turn.mint("room-ABC234-m1");
    expect(requests[0].path).toBe("https://rtc.live.cloudflare.com/v1/turn/keys/key/credentials/generate-ice-servers");
    expect(requests[0].body).toEqual({ ttl: TURN_TTL_S, customIdentifier: "room-ABC234-m1" });
    expect(username).toBe("u".repeat(64));
    expect(Object.keys(message).sort()).toEqual(["credential", "type", "urls", "username"]);
    expect(message.urls).toEqual(["turns:turn.cloudflare.com:5349?transport=tcp", "turns:turn.cloudflare.com:443?transport=tcp"]);
    await turn.revoke(username);
    expect(requests[1].path).toBe(`https://rtc.live.cloudflare.com/v1/turn/keys/key/credentials/${"u".repeat(64)}/revoke`);
    await expect(new Turn({}).mint("x")).rejects.toThrow("not configured");
    await expect(new Turn({ TURN_KEY_ID: "k", TURN_KEY_API_TOKEN: "t" }, async () => Response.json({ iceServers: [{ urls: ["turn:x:3478"], username: "a", credential: "b" }] })).mint("x"))
      .rejects.toThrow("Invalid TURN");
  });

  it("gives credentials only to the racer that asks, once per race, and revokes them at the end", async () => {
    const { clients, code } = await startedRace(2);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const calls: string[] = [];
    const spy = vi.spyOn(globalThis, "fetch").mockImplementation(async (input) => {
      calls.push(String(input));
      return String(input).endsWith("/revoke") ? new Response(null, { status: 204 }) : Response.json(ICE, { status: 201 });
    });
    try {
      await runInDurableObject(stub, async (room: any) => {
        room.env = { ...room.env, TURN_KEY_ID: "key", TURN_KEY_API_TOKEN: "token" };
        for (const member of room.state.members) {
          member.sfuRace = room.state.race.startAt;
          member.sfu = { id: `fake-${member.id}`, phase: "connect", channels: [] };
        }
      });
      clients[0].sendJson({ type: "sfu_turn" });
      const got = await clients[0].json("sfu_turn");
      expect(got.credential).toBe("c".repeat(64));
      clients[0].sendJson({ type: "sfu_turn" });
      expect((await clients[0].json("error")).message).toContain("One TURN credential per race");
      expect(calls.filter(c => c.endsWith("generate-ice-servers"))).toHaveLength(1);
      // The other racer hears nothing of it.
      await expect(clients[1].next(f => typeof f === "string" && /sfu_turn|ccccccc|uuuuuuu/.test(f), 200)).rejects.toThrow();
      clients[0].sendJson({ type: "end" });
      await Promise.all(clients.map(c => c.json("ended")));
      await runInDurableObject(stub, async (room: any) => {
        await room.sfuQueue;
        expect(room.state.members.every((m: any) => m.turnUser === undefined)).toBe(true);
      });
      expect(calls.filter(c => c.endsWith(`/credentials/${"u".repeat(64)}/revoke`))).toHaveLength(1);
    } finally {
      spy.mockRestore();
    }
  });

  it("sends nothing and keeps the session when TURN is not configured", async () => {
    const { clients, code } = await startedRace(2);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    await runInDurableObject(stub, async (room: any) => {
      const me = room.state.members[0];
      me.sfuRace = room.state.race.startAt;
      me.sfu = { id: "fake", phase: "connect", channels: [] };
    });
    // Member 0's client asks (the other one, without a session, gets an error).
    for (const c of clients) c.sendJson({ type: "sfu_turn" });
    await runInDurableObject(stub, async (room: any) => {
      await room.sfuQueue;
      expect(room.state.members[0].sfu.phase).toBe("connect");
      expect(room.state.members[0].turnUser).toBeUndefined();
    });
  });

  it("refuses TURN outside a race or before the racer's SFU session", async () => {
    const { host } = await Client.host();
    host.sendJson({ type: "sfu_turn" });
    expect((await host.json("error")).message).toContain("active race");
    const { clients } = await startedRace(2);
    clients[0].sendJson({ type: "sfu_turn" });
    expect((await clients[0].json("error")).message).toContain("Realtime connection failed");
  });
});
