// Realtime setup after finding 175: one datachannels/new per subscriber for all its publishers, a
// gone session's close (410) counted as closed, and a release that is asked again until it comes.
// Room SHCF93 (production, 2026-11-07 02:28-02:36): every racer ready at 68 s, released at 222 s
// only because a deploy reset the room.
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { describe, it, expect } from "vitest";
import { startedRace } from "./helpers";
import { fakeRealtime, publishedSession } from "./sfu-fake";
import { Realtime, RealtimeError } from "../src/sfu";

const ready = (ids: number[]) => ids.map((id) => publishedSession(id, ids.filter((x) => x !== id)));

describe("one subscription call per subscriber", () => {
  it("asks for every publisher in one call and maps the answer by name and session, in any order", async () => {
    const calls: any[] = [];
    let next = 500;
    const api = new Realtime({ REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" }, async (_input, init) => {
      const body = JSON.parse(String(init!.body));
      calls.push(body);
      // answered in reverse, as nothing in Cloudflare's docs promises the request's order
      return Response.json({ dataChannels: [...body.dataChannels].reverse().map((x: any) => ({ ...x, id: next++ })) });
    });
    const sub = { id: "s-sub", phase: "ready" as const, channels: [] };
    const pubs = [1, 2, 3].map((m) => ({ memberId: m, session: { id: `s-${m}`, phase: "ready" as const, channels: [] } }));
    const got = await api.subscribeAll(sub, pubs);
    expect(calls).toHaveLength(1);
    expect(calls[0].dataChannels.map((x: any) => [x.sessionId, x.dataChannelName, x.location])).toEqual([
      ["s-1", "state", "remote"], ["s-1", "events", "remote"], ["s-2", "state", "remote"], ["s-2", "events", "remote"],
      ["s-3", "state", "remote"], ["s-3", "events", "remote"]]);
    expect(calls[0].dataChannels[0]).toMatchObject({ ordered: false, maxRetransmits: 0 });
    // reversed answer: s-3 events got 500 ... s-1 state got 505
    expect(got.map((c) => [c.memberId, c.name, c.channelId])).toEqual([
      [1, "state", 505], [1, "events", 504], [2, "state", 503], [2, "events", 502], [3, "state", 501], [3, "events", 500]]);
    expect(sub.channels).toHaveLength(6);
    // nothing missing: no call
    expect(await api.subscribeAll(sub, pubs)).toEqual([]);
    expect(calls).toHaveLength(1);
  });

  it("maps by position when the answer names no session, keeps the successes of a partial failure and throws", async () => {
    let next = 700;
    const api = new Realtime({ REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" }, async (_input, init) => {
      const body = JSON.parse(String(init!.body));
      return Response.json({ dataChannels: body.dataChannels.map((x: any, i: number) => i === 2
        ? { dataChannelName: x.dataChannelName, errorCode: "internal_error" }
        : { dataChannelName: x.dataChannelName, id: next++ }) });
    });
    const sub = { id: "s-sub", phase: "ready" as const, channels: [] as any[] };
    const pubs = [4, 5].map((m) => ({ memberId: m, session: { id: `s-${m}`, phase: "ready" as const, channels: [] } }));
    await expect(api.subscribeAll(sub, pubs)).rejects.toThrow("Channel allocation failed");
    // member 5's state failed; the other three are kept for teardown
    expect(sub.channels.map((c) => [c.memberId, c.name, c.channelId])).toEqual([[4, "state", 700], [4, "events", 701], [5, "events", 702]]);
  });

  it("a room of five: each racer's subscriptions in one call", async () => {
    const { clients, code } = await startedRace(5);
    const fake = fakeRealtime();
    try {
      await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
        room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        for (const m of room.state.members) {
          m.sfu = { id: `session-${m.id}`, phase: m.id === 4 ? "connect" : "ready",
            channels: [{ memberId: m.id, name: "state", channelId: 10 * m.id + 1, local: true }, { memberId: m.id, name: "events", channelId: 10 * m.id + 2, local: true }] };
          m.sfuRace = room.state.race.startAt;
        }
      });
      clients[4].sendJson({ type: "sfu_connected" });  // the last one: publishes, then everyone subscribes
      await Promise.all(clients.map((c) => c.json("sfu_channels")));
      await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
        await room.sfuQueue;
        for (const m of room.state.members) expect(m.sfu.channels.filter((c: any) => !c.local)).toHaveLength(8);
      });
      const subscriptions = fake.calls.filter((c) => c.path.endsWith("datachannels/new") && c.body.dataChannels[0].location === "remote");
      expect(subscriptions).toHaveLength(5);
      expect(subscriptions.every((c) => c.body.dataChannels.length === 8)).toBe(true);
    } finally {
      fake.restore();
    }
  });
});

describe("a gone session and a release that waited (room SHCF93)", () => {
  it("a close answered 410 (the session is gone) counts as closed, other failures still throw", async () => {
    const gone = new Realtime({ REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" }, async () => new Response("gone", { status: 410 }));
    const session = publishedSession(1, [2]);
    await gone.close(session);
    expect(session.channels).toEqual([]);
    const broken = new Realtime({ REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" }, async () => new Response("no", { status: 500 }));
    const kept = publishedSession(1, [2]);
    await expect(broken.close(kept)).rejects.toBeInstanceOf(RealtimeError);
    expect(kept.channels).toHaveLength(4);
  });

  it("every racer ready, no release yet, an older race's reconcile pending: the alarm releases", async () => {
    const { clients, code } = await startedRace(3);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
      const sessions = ready([0, 1, 2]);
      room.state.members.forEach((m: any, i: number) => { m.sfu = sessions[i]; m.sfuRace = room.state.race.startAt; m.sfuReady = true; });
      room.reconciling = room.state.race.startAt - 60_000;  // the previous race's, still queued
    });
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    await Promise.all(clients.map((c) => c.json("sfu_ready")));
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
      expect(room.state.race.sfuReleased).toBe(true);
    });
  });

  it("an older race's reconcile doesn't swallow this race's: the missing pair is subscribed and released", async () => {
    const { clients, code } = await startedRace(2);
    const fake = fakeRealtime();
    try {
      await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
        room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        const [a, b] = room.state.members;
        a.sfu = publishedSession(a.id, [b.id]);
        b.sfu = publishedSession(b.id, []);  // its subscription to a is missing
        for (const m of [a, b]) { m.sfuRace = room.state.race.startAt; m.sfuReady = true; }
        room.reconciling = room.state.race.startAt - 60_000;
      });
      clients[1].sendJson({ type: "sfu_ready" });
      await Promise.all(clients.map((c) => c.json("sfu_ready")));
      expect(fake.calls.filter((c) => c.path.endsWith("datachannels/new"))).toHaveLength(1);
    } finally {
      fake.restore();
    }
  });

  it("the alarm doesn't wait for a cleanup pass whose API call hangs", async () => {
    const { code } = await startedRace(2);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    fake.hold = (path) => path.endsWith("datachannels/close");
    try {
      await runInDurableObject(stub, async (room: any) => {
        room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        room.cleanup = [{ session: "old", channels: [7], until: Date.now() + 60_000, at: Date.now() - 1, tries: 0 }];
        room.state.race.sfuConnectBy = Date.now() - 1;  // the setup check is due
      });
      const started = Date.now();
      await runDurableObjectAlarm(stub);
      expect(Date.now() - started).toBeLessThan(1000);
      for (let i = 0; i < 100 && !fake.held; i++) await new Promise((r) => setTimeout(r, 10));
      expect(fake.held).toBe(1);  // the pass runs, in the background
      await runInDurableObject(stub, async (room: any) => {
        expect(room.state.race.sfuConnectBy).toBeUndefined();  // the setup check ran meanwhile
      });
    } finally {
      fake.release();
      fake.restore();
    }
  });
});
