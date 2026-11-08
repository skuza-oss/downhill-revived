// A slow Realtime API call no longer takes a racer out of the race (finding 182): a call with no
// answer is an unknown outcome, the subscriber's session is inspected after a backoff, what the call
// made is adopted (or closed when it is a second one), the rest is made again; a definite failure
// still fails the session. The room's calls go through one gate, so none waits in the runtime's
// connection queue with its timeout running.
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { describe, it, expect } from "vitest";
import { Client, startedRace } from "./helpers";
import { fakeRealtime, publishedSession } from "./sfu-fake";
import { CallGate } from "../src/sfu";

const SFU_OUT = "Realtime connection failed: out of the race.";
const room = (code: string) => env.ROOM.get(env.ROOM.idFromName(code));
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
type Fake = ReturnType<typeof fakeRealtime>;
/** The subscription calls on `session`'s datachannels/new to `publisher`. */
const callsTo = (fake: Fake, session: string, publisher: string) => fake.calls.filter((c) => c.path.endsWith(`/sessions/${session}/datachannels/new`) &&
  c.body.dataChannels.some((x: any) => x.sessionId === publisher));
const outErrors = (c: Client) => c.seen.filter((f) => f.includes(SFU_OUT));

/** A started race of 3: members 0 and 1 published, subscribed to each other and acknowledged;
    member 2's session connecting. Subscription calls get `timeoutMs`. */
async function twoReady(code: string, timeoutMs = 150) {
  await runInDurableObject(room(code), async (r: any) => {
    r.env = { ...r.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token", SFU_SUBSCRIBE_TIMEOUT_MS: String(timeoutMs) };
    const [a, b, c] = r.state.members;
    a.sfu = publishedSession(a.id, [b.id]);
    b.sfu = publishedSession(b.id, [a.id]);
    c.sfu = { id: `session-${c.id}`, phase: "connect", channels: [] };
    a.sfuReadyRace = b.sfuReadyRace = r.state.race.startAt;
  });
}

/** Waits for the room's subscription pass and session changes to settle. */
async function settled(code: string) {
  await runInDurableObject(room(code), async (r: any) => {
    for (let i = 0; i < 400 && (r.subscribing || [...r.sessionTail.keys()].length); i++) await sleep(10);
    await r.sfuQueue;
  });
}

/** Waits until member `id`'s session waits for an unknown outcome (its call ran out), then for the pass. */
async function unsureOf(code: string, id: number) {
  await runInDurableObject(room(code), async (r: any) => {
    for (let i = 0; i < 400 && r.state.members[id].sfu?.unsure == null; i++) await sleep(10);
    expect(r.state.members[id].sfu.unsure).toBeGreaterThan(0);
  });
  await settled(code);
}

/** The time an inspection is due on member `id`'s session passes; the alarm runs the pass. */
async function inspectionDue(code: string, id: number) {
  const due = await runInDurableObject(room(code), async (r: any) => r.state.members[id].sfu.unsure ?? 0);
  await sleep(Math.max(0, due - Date.now()) + 20);
  await runDurableObjectAlarm(room(code));
  await settled(code);
}

/** Every racer acknowledges; the room releases all three. */
async function releaseAll(clients: Client[], startAt: number) {
  for (const c of clients) c.sendJson({ type: "sfu_ready", race: startAt });
  for (const c of clients) expect((await c.json("sfu_ready")).race).toBe(startAt);
}

describe("subscription calls with no answer (finding 182)", () => {
  it("a held call that times out does not fail its subscriber, and the pair is subscribed on the next pass", async () => {
    const { clients, code, starts } = await startedRace(3);
    const fake = fakeRealtime();
    // Member 1's subscription to member 2 hangs past its timeout, once.
    fake.hold = (path) => path.endsWith("/sessions/session-1/datachannels/new") && fake.held === 0;
    try {
      await twoReady(code);
      clients[2].sendJson({ type: "sfu_connected" });
      await unsureOf(code, 1);
      const after = await runInDurableObject(room(code), async (r: any) => ({
        unsure: r.state.members[1].sfu.unsure, phase: r.state.members[1].sfu.phase, left: r.state.race.left }));
      expect(fake.held).toBe(1);
      expect(after.phase).toBe("ready");
      expect(after.unsure).toBeGreaterThan(Date.now());  // inspected after the backoff
      expect(after.left).toEqual([]);

      // The next pass: the session is listed first (nothing there), then the pair is made.
      await inspectionDue(code, 1);
      expect(fake.calls.some((c) => c.method === "GET" && c.path.endsWith("/sessions/session-1"))).toBe(true);
      expect(callsTo(fake, "session-1", "session-2")).toHaveLength(2);
      const got = await clients[1].next((f) => typeof f === "string" && JSON.parse(f).type === "sfu_channels" &&
        JSON.parse(f).channels.some((x: any) => x.memberId === 2));
      expect(JSON.parse(got as string).channels.map((x: any) => x.name).sort()).toEqual(["events", "state"]);

      await releaseAll(clients, starts[0].startAt);
      for (const c of clients) expect(outErrors(c)).toEqual([]);
      expect(await runInDurableObject(room(code), async (r: any) => r.state.race.left)).toEqual([]);
    } finally {
      fake.restore();
    }
  });

  it("an aborted call whose channels exist on the server is adopted, not made again", async () => {
    const { clients, code, starts } = await startedRace(3);
    const fake = fakeRealtime();
    fake.hold = (path) => path.endsWith("/sessions/session-1/datachannels/new") && fake.held === 0;
    fake.allocateOnAbort = true;  // Cloudflare made them; only the answer was lost
    try {
      await twoReady(code);
      clients[2].sendJson({ type: "sfu_connected" });
      await unsureOf(code, 1);
      const made = fake.sessions.get("session-1")!.filter((c) => c.sessionId === "session-2").map((c) => c.id).sort();
      expect(made).toHaveLength(2);

      await inspectionDue(code, 1);
      // Adopted: the subscriber hears the ids Cloudflare made, and no second call is made.
      const got = JSON.parse(await clients[1].next((f) => typeof f === "string" && JSON.parse(f).type === "sfu_channels" &&
        JSON.parse(f).channels.some((x: any) => x.memberId === 2)) as string);
      expect(got.channels.map((x: any) => x.channelId).sort()).toEqual(made);
      expect(callsTo(fake, "session-1", "session-2")).toHaveLength(1);
      expect(fake.sessions.get("session-1")!.filter((c) => c.sessionId === "session-2")).toHaveLength(2);
      const record = await runInDurableObject(room(code), async (r: any) => r.state.members[1].sfu);
      expect(record.channels.filter((c: any) => c.memberId === 2).map((c: any) => c.channelId).sort()).toEqual(made);
      expect(record.unsure).toBeUndefined();
      expect(record.recheckAt).toBeGreaterThan(Date.now());

      await releaseAll(clients, starts[0].startAt);
      for (const c of clients) expect(outErrors(c)).toEqual([]);
    } finally {
      fake.restore();
    }
  });

  it("a second subscription an aborted call made after the inspection is closed at the recheck, unannounced", async () => {
    const { clients, code } = await startedRace(3);
    const fake = fakeRealtime();
    fake.hold = (path) => path.endsWith("/sessions/session-1/datachannels/new") && fake.held === 0;
    try {
      await twoReady(code);
      clients[2].sendJson({ type: "sfu_connected" });
      await unsureOf(code, 1);
      await inspectionDue(code, 1);  // nothing found: the pair is made again
      expect(callsTo(fake, "session-1", "session-2")).toHaveLength(2);
      // The aborted call lands late on Cloudflare's side: a second pair to the same publication.
      fake.sessions.get("session-1")!.push(
        { id: 900, dataChannelName: "state", location: "remote", sessionId: "session-2", status: "active" },
        { id: 901, dataChannelName: "events", location: "remote", sessionId: "session-2", status: "active" });
      await runInDurableObject(room(code), async (r: any) => { r.state.members[1].sfu.recheckAt = Date.now() - 1; });
      clients[1].drain();
      await runDurableObjectAlarm(room(code));
      await settled(code);
      const close = fake.calls.filter((c) => c.path.endsWith("/sessions/session-1/datachannels/close"));
      expect(close.map((c) => c.body.dataChannels.map((x: any) => x.id))).toEqual([[900, 901]]);
      const record = await runInDurableObject(room(code), async (r: any) => r.state.members[1].sfu);
      expect(record.channels.some((c: any) => c.channelId >= 900)).toBe(false);
      expect(record.recheckAt).toBeUndefined();
      expect(clients[1].seen.some((f) => f.includes("sfu_closed") && f.includes("900"))).toBe(false);
    } finally {
      fake.restore();
    }
  });

  it("a pass whose missing pairs all wait for an inspection ends without a call instead of running again at once", async () => {
    const { code } = await startedRace(3);
    const fake = fakeRealtime();
    try {
      await twoReady(code);
      const result = await runInDurableObject(room(code), async (r: any) => {
        const [a, b, c] = r.state.members;
        c.sfu = publishedSession(c.id, []);
        c.sfuReadyRace = r.state.race.startAt;
        // Every subscriber missing a pair waits for its inspection (the test Worker's case: the
        // release asked for a pass, the pass made no call and asked again, without end).
        for (const m of [a, b, c]) m.sfu.unsure = Date.now() + 60_000;
        let passes = 0;
        const pass = r.subscribeGroup.bind(r);
        r.subscribeGroup = (...args: any[]) => { passes++; return pass(...args); };
        r.releaseSfu(r.state);
        await r.subscribePass();
        r.releaseSfu(r.state);
        await r.subscribePass();
        return { passes, released: !!r.state.race.sfuReleased, work: r.sfuWork() };
      });
      expect(result.passes).toBeLessThanOrEqual(2);
      expect(result.released).toBe(false);
      expect(result.work).toBe(false);  // nothing for the alarm either until the inspection is due
      expect(fake.calls).toEqual([]);
    } finally {
      fake.restore();
    }
  });

  it("a 5xx or 429 is an unknown outcome too; a definite item error still fails the subscriber", async () => {
    const { clients, code } = await startedRace(3);
    const fake = fakeRealtime();
    fake.fail = (path) => path.endsWith("/sessions/session-1/datachannels/new") && fake.calls.filter((c) => c.path.endsWith("/sessions/session-1/datachannels/new")).length === 1;
    fake.failStatus = 429;
    try {
      await twoReady(code);
      clients[2].sendJson({ type: "sfu_connected" });
      await unsureOf(code, 1);
      expect(await runInDurableObject(room(code), async (r: any) => r.state.members[1].sfu.phase)).toBe("ready");
      expect(outErrors(clients[1])).toEqual([]);

      // Now Cloudflare refuses the items for certain: the subscriber is out, as before.
      fake.itemError = (path) => path.endsWith("/sessions/session-1/datachannels/new");
      await inspectionDue(code, 1);
      expect((await clients[1].json("error")).message).toBe(SFU_OUT);
      expect(await clients[0].json("left")).toMatchObject({ slot: 1 });
    } finally {
      fake.restore();
    }
  });
});

describe("the room's call gate (finding 182)", () => {
  it("runs at most `limit` at once and the background ones at most `background`; urgent ones go first", async () => {
    const gate = new CallGate(3, 2);
    const order: string[] = [];
    let release!: () => void;
    const held = new Promise<void>((r) => (release = r));
    const call = (name: string) => async () => { order.push(name); await held; };
    const runs = [gate.run(false, call("b1")), gate.run(false, call("b2")), gate.run(false, call("b3"))];
    await sleep(5);
    expect(gate.load).toEqual({ active: 2, waiting: 1 });
    runs.push(gate.run(true, call("u1")));  // the slot background work leaves free
    await sleep(5);
    expect(gate.load).toEqual({ active: 3, waiting: 1 });
    runs.push(gate.run(true, call("u2")));  // waits, but ahead of b3
    release();
    await Promise.all(runs);
    expect(order).toEqual(["b1", "b2", "u1", "u2", "b3"]);
  });

  it("a room's subscription pass after a reset makes one call per subscriber, at most 4 at once, and a newcomer's own session isn't held behind them", async () => {
    const n = 8;
    const host = await Client.connect("/v1/rooms/new");
    host.hello("Host", { features: ["session", "countdown", "race_rejoin"] });
    const { code } = await host.json("welcome");
    const cs = [host];
    for (let i = 1; i < n; i++) {
      const c = await Client.connect(`/v1/rooms/${code}`);
      c.hello(`P${i}`, { features: ["session", "countdown", "race_rejoin"] });
      await c.json("welcome");
      cs.push(c);
    }
    const fake = fakeRealtime();
    try {
      await runInDurableObject(room(code), async (r: any) => {
        r.env = { ...r.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        // Every member published (fresh sessions after a deploy), none subscribed yet.
        for (const m of r.state.members) m.sfu = publishedSession(m.id, []);
      });
      // Subscription calls hang until released; the grid's lock starts the pass.
      fake.hold = (path, body) => path.endsWith("datachannels/new") && body.dataChannels[0].location === "remote";
      for (const c of cs) c.sendJson({ type: "ready", ready: true });
      for (let i = 0; i < 200 && fake.held < 4; i++) await sleep(10);
      await sleep(50);
      expect(fake.held).toBe(4);
      expect(fake.maxInFlight).toBe(4);
      // A newcomer's offer meanwhile: its session answers at once.
      const p = await Client.connect(`/v1/rooms/${code}`);
      p.hello("New", { features: ["session", "countdown", "race_rejoin"] });
      await p.json("welcome");
      p.sendJson({ type: "sfu_offer", sdp: "offer" });
      await p.json("sfu_answer");
      expect(fake.held).toBe(4);
      fake.release();
      await settled(code);
      const subs = fake.calls.filter((c) => c.path.endsWith("datachannels/new") && c.body.dataChannels[0].location === "remote");
      expect(subs).toHaveLength(n);  // one per subscriber, each naming its 7 publishers
      for (const c of subs) expect(new Set(c.body.dataChannels.map((x: any) => x.sessionId)).size).toBe(n - 1);
      expect(fake.maxInFlight).toBeLessThanOrEqual(5);
      await runInDurableObject(room(code), async (r: any) => {
        for (const m of r.state.members.slice(0, n)) expect(m.sfu.channels.filter((c: any) => !c.local)).toHaveLength(2 * (n - 1));
      });
    } finally {
      fake.restore();
    }
  });
});
