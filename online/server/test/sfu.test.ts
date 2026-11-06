import { describe, it, expect, vi } from "vitest";
import { env, runInDurableObject } from "cloudflare:test";
import { Client, startedRace } from "./helpers";
import { Realtime, Turn, TURN_TTL_S } from "../src/sfu";

describe("Realtime control plane", () => {
  it("releases race traffic only after every subscriber acknowledges", async () => {
    const {clients, code} = await startedRace(2);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
      for (const member of room.state.members) {
        member.sfuRace = room.state.race.startAt;
        member.sfu = {id:`fake-${member.id}`, phase:"ready", channels:[]};
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
    const {clients} = await startedRace(2);
    clients[0].sendJson({type:"sfu_offer", sdp:"private"});
    // Fake test environment has no app secrets: a failed attempt still consumes the cap.
    expect((await clients[0].json("error")).message).toContain("failed");
    clients[0].sendJson({type:"sfu_offer", sdp:"private"});
    expect((await clients[0].json("error")).message).toContain("One Realtime session");
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

  it("refuses TURN outside a race or before the racer's SFU session", async () => {
    const { host } = await Client.host();
    host.sendJson({ type: "sfu_turn" });
    expect((await host.json("error")).message).toContain("active race");
    const { clients } = await startedRace(2);
    clients[0].sendJson({ type: "sfu_turn" });
    expect((await clients[0].json("error")).message).toContain("Realtime connection failed");
  });
});
