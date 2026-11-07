// Realtime setup after finding 175: a gone session's close (410) counted as closed, and a release
// that is asked again until it comes.
// Room SHCF93 (production, 2026-11-07 02:28-02:36): every racer ready at 68 s, released at 222 s
// only because a deploy reset the room.
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
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
