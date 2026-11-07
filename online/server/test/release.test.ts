// Realtime setup after finding 175: a gone session's close (410) counted as closed, and a release
// that is asked again until it comes.
// Room SHCF93 (production, 2026-11-07 02:28-02:36): every racer ready at 68 s, released at 222 s
// only because a deploy reset the room.
import { env, runInDurableObject, runDurableObjectAlarm, evictDurableObject } from "cloudflare:test";
import { describe, it, expect } from "vitest";
import { startedRace } from "./helpers";
import { fakeRealtime, publishedSession } from "./sfu-fake";
import { Realtime, RealtimeError } from "../src/sfu";

const ready = (ids: number[]) => ids.map((id) => publishedSession(id, ids.filter((x) => x !== id)));

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

  it("every racer ready, no release yet, a pass running: the alarm releases", async () => {
    const { clients, code } = await startedRace(3);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
      const sessions = ready([0, 1, 2]);
      room.state.members.forEach((m: any, i: number) => { m.sfu = sessions[i]; m.sfuRace = room.state.race.startAt; m.sfuReady = true; });
      room.subscribing = true;  // a subscription pass that never ends (a hung call)
    });
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    await Promise.all(clients.map((c) => c.json("sfu_ready")));
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
      expect(room.state.race.sfuReleased).toBe(true);
    });
  });

  it("a pass asked for an older race serves the running race: the missing pair is subscribed and released", async () => {
    const { clients, code } = await startedRace(2);
    const fake = fakeRealtime();
    try {
      await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room: any) => {
        room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        const [a, b] = room.state.members;
        a.sfu = publishedSession(a.id, [b.id]);
        b.sfu = publishedSession(b.id, []);  // its subscription to a is missing
        for (const m of [a, b]) { m.sfuRace = room.state.race.startAt; m.sfuReady = true; }
        // a pass for the previous race (another Race object), as one still running when this race started
        await room.subscribePass({ ...room.state.race, startAt: room.state.race.startAt - 60_000 });
      });
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

describe("a racer's own setup doesn't wait behind the others' (finding 175)", () => {
  it("while a subscription call hangs in the SFU queue, another racer's offer is answered and its TURN credentials come", async () => {
    const { clients, code } = await startedRace(3);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    fake.hold = (path) => path.endsWith("datachannels/new") && fake.held === 0;
    try {
      await runInDurableObject(stub, async (room: any) => {
        room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token", TURN_KEY_ID: "key", TURN_KEY_API_TOKEN: "token" };
        const [a, b] = room.state.members;
        a.sfu = { id: `session-${a.id}`, phase: "connect", channels: [] };
        b.sfu = publishedSession(b.id, []);
        a.sfuRace = b.sfuRace = room.state.race.startAt;
      });
      clients[0].sendJson({ type: "sfu_connected" });  // its publication's call hangs in the queue
      for (let i = 0; i < 200 && !fake.held; i++) await new Promise((r) => setTimeout(r, 10));
      expect(fake.held).toBe(1);
      clients[2].sendJson({ type: "sfu_offer", sdp: "offer" });
      expect((await clients[2].json("sfu_answer")).sdp).toBe("SFU");
      await runInDurableObject(stub, async (room: any) => { room.state.members[2].sfu.phase = "connect"; });
      clients[2].sendJson({ type: "sfu_turn" });
      expect((await clients[2].json("sfu_turn")).urls.length).toBeGreaterThan(0);
      expect(fake.released).toBe(false);
    } finally {
      fake.release();
      fake.restore();
    }
  });
});

describe("the coalesced subscription pass (hotfix 4)", () => {
  it("six racers connecting within a short window: one call per ordered pair, none twice, all released", async () => {
    const n = 6;
    const { clients, code } = await startedRace(n);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    const fake = fakeRealtime();
    try {
      await runInDurableObject(stub, async (room: any) => {
        room.env = { ...room.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        for (const m of room.state.members) { m.sfu = { id: `session-${m.id}`, phase: "connect", channels: [] }; m.sfuRace = room.state.race.startAt; }
      });
      const started = Date.now();
      for (const c of clients) { c.sendJson({ type: "sfu_connected" }); await new Promise((r) => setTimeout(r, 20)); }
      // each racer: its own channels, then the other five's
      await Promise.all(clients.map((c) => c.next((f) => typeof f === "string" && JSON.parse(f).type === "sfu_channels" &&
        JSON.parse(f).channels.some((x: any) => !x.local), 5000)));
      await runInDurableObject(stub, async (room: any) => {
        for (let i = 0; i < 300 && room.state.members.some((m: any) => m.sfu.channels.filter((c: any) => !c.local).length < 2 * (n - 1)); i++)
          await new Promise((r) => setTimeout(r, 10));
        for (const m of room.state.members) expect(m.sfu.channels.filter((c: any) => !c.local)).toHaveLength(2 * (n - 1));
      });
      const subs = fake.calls.filter((c) => c.path.endsWith("datachannels/new") && c.body.dataChannels[0].location === "remote");
      const pairs = subs.map((c) => `${c.path.split("/").at(-3)}<-${c.body.dataChannels[0].sessionId}`);
      expect(new Set(pairs).size).toBe(pairs.length);  // no pair asked twice
      expect(pairs).toHaveLength(n * (n - 1));          // every ordered pair once
      expect(fake.calls.filter((c) => c.path.endsWith("datachannels/new") && c.body.dataChannels[0].location === "local")).toHaveLength(n);
      for (const c of clients) c.sendJson({ type: "sfu_ready" });
      await Promise.all(clients.map((c) => c.json("sfu_ready")));
      console.log(`coalesced pass: ${n} racers, ${pairs.length} subscription calls, released ${Date.now() - started} ms after the first connect`);
    } finally {
      fake.restore();
    }
  });
});

describe("the release is stored and sent once (rooms SHCF93, 3G5MXD)", () => {
  /** Three racers published, subscribed to each other and acknowledged: the race is released. */
  async function releasedRace() {
    const { clients, code } = await startedRace(3);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    await runInDurableObject(stub, async (room: any) => {
      const sessions = ready([0, 1, 2]);
      room.state.members.forEach((m: any, i: number) => { m.sfu = sessions[i]; m.sfuRace = room.state.race.startAt; m.sfuConnectedRace = room.state.race.startAt; });
      await room.save();
    });
    for (const c of clients) c.sendJson({ type: "sfu_ready" });
    await Promise.all(clients.map((c) => c.json("sfu_ready")));
    return { clients, code, stub };
  }

  it("after an eviction the race is still released: no setup check again, no second sfu_ready", async () => {
    const { clients, stub } = await releasedRace();
    await evictDurableObject(stub);
    await runInDurableObject(stub, async (room: any) => {
      expect(room.state.race.sfuReleased).toBe(true);
      room.state.race.sfuConnectBy = Date.now() - 1;  // as if its check were due after the reload
      await room.save();
    });
    const logs: string[] = [];
    const log = console.log;
    console.log = (...a: unknown[]) => { logs.push(a.join(" ")); };
    try {
      await runDurableObjectAlarm(stub);
      clients[2].sendJson({ type: "back" });  // a retire after the release
      await clients[0].json("left");
    } finally {
      console.log = log;
    }
    expect(logs.filter((l) => l.includes("Realtime setup after") || l.includes("Realtime released"))).toEqual([]);
    await expect(clients[0].next((f) => typeof f === "string" && JSON.parse(f).type === "sfu_ready", 300)).rejects.toThrow("timed out");
  });

  it("the setup check's result is stored too: it doesn't run again after an eviction", async () => {
    const { code } = await startedRace(2);
    const stub = env.ROOM.get(env.ROOM.idFromName(code));
    await runInDurableObject(stub, async (room: any) => {
      for (const m of room.state.members) { m.sfu = { id: `session-${m.id}`, phase: "connect", channels: [] }; m.sfuRace = room.state.race.startAt; m.sfuConnectedRace = room.state.race.startAt; }
      room.state.race.sfuConnectBy = Date.now() - 1;
      await room.save();
    });
    await runDurableObjectAlarm(stub);
    await evictDurableObject(stub);
    await runInDurableObject(stub, async (room: any) => { expect(room.state.race.sfuConnectBy).toBeUndefined(); });
  });

  it("a racer that started its session again after the release gets sfu_ready once more, the others don't", async () => {
    const { clients, stub } = await releasedRace();
    await runInDurableObject(stub, async (room: any) => {
      const m = room.state.members[1];
      m.raceRejoin = true;
      delete m.releasedRace;  // as sfu_restart leaves it, with its new session set up again
    });
    clients[1].sendJson({ type: "sfu_ready" });
    await clients[1].json("sfu_ready");
    for (const c of [clients[0], clients[2]])
      await expect(c.next((f) => typeof f === "string" && JSON.parse(f).type === "sfu_ready", 300)).rejects.toThrow("timed out");
  });
});
