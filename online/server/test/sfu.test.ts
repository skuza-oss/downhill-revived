import { describe, it, expect } from "vitest";
import { env, runInDurableObject } from "cloudflare:test";
import { Client, startedRace } from "./helpers";
import { Realtime } from "../src/sfu";

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
