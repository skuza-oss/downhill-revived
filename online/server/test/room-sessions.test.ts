// One Realtime session per member per room stay (finding 177): made when the member joins, kept
// across races with its publications and subscriptions; each race adds only the missing pairs, made
// at the grid's lock (the countdown before the start message), so a room whose members were all
// connected is released as soon as their games acknowledge the start.
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { describe, it, expect } from "vitest";
import { Client } from "./helpers";
import { fakeRealtime } from "./sfu-fake";

const FEATURES = ["session", "countdown", "race_rejoin"];
const room = (code: string) => env.ROOM.get(env.ROOM.idFromName(code));
const snap = async (c: Client, pred: (r: any) => boolean = () => true) =>
  JSON.parse(await c.next(f => typeof f === "string" && JSON.parse(f).type === "room" && pred(JSON.parse(f))) as string);
type Fake = ReturnType<typeof fakeRealtime>;
const subscriptions = (fake: Fake, from = 0) => fake.calls.slice(from).filter((c) => c.path.endsWith("datachannels/new") && c.body.dataChannels[0].location === "remote")
  .map((c) => `${c.path.split("/").at(-3)}<-${c.body.dataChannels[0].sessionId}`);

/** A session room of `n` members with Realtime configured (the fake API). */
async function sessionRoom(n: number) {
  const host = await Client.connect("/v1/rooms/new");
  host.hello("Host", { features: FEATURES });
  const { code } = await host.json("welcome");
  await runInDurableObject(room(code), async (r: any) => { r.env = { ...r.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" }; });
  const cs = [host];
  for (let i = 1; i < n; i++) {
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello(`P${i}`, { features: FEATURES });
    await c.json("welcome");
    cs.push(c);
  }
  return { cs, code };
}

/** A member's game connects to the SFU as it does on joining: its offer, then its connection. */
async function connect(c: Client) {
  c.sendJson({ type: "sfu_offer", sdp: "offer" });
  await c.json("sfu_answer");
  c.sendJson({ type: "sfu_connected" });
  const own = await c.json("sfu_channels");
  expect(own.channels.every((x: any) => x.local)).toBe(true);
}

/** Waits until no subscription pass runs. */
async function passDone(code: string) {
  await runInDurableObject(room(code), async (r: any) => {
    for (let i = 0; i < 300 && (r.subscribing || r.sfuWork()); i++) await new Promise((x) => setTimeout(x, 10));
    await r.sfuQueue;
  });
}

/** Everyone ready: the grid locks (the countdown), then its end starts the race. Returns the
    start messages, after checking what the pre-setup did before them. */
async function race(cs: Client[], code: string, beforeStart: () => Promise<void> = async () => {}) {
  for (const c of cs) c.sendJson({ type: "ready", ready: true });
  await snap(cs[0], (r) => r.members.filter((m: any) => m.ready).length === cs.length);
  if (await runInDurableObject(room(code), async (r: any) => r.state.countdownEndsAt == null)) {
    cs[0].sendJson({ type: "start" });
    await snap(cs[0], (r) => r.phase === "starting");
  }
  await passDone(code);
  await beforeStart();
  await runInDurableObject(room(code), async (r: any, ctx) => {
    r.state.countdownEndsAt = Date.now() - 1;
    await ctx.storage.put("room", r.state);
  });
  await runDurableObjectAlarm(room(code));
  return Promise.all(cs.map((c) => c.json("start")));
}

/** Each game acknowledges its channels for the race; the room releases it. */
async function release(cs: Client[], startAt: number) {
  for (const c of cs) c.sendJson({ type: "sfu_ready", race: startAt });
  for (const c of cs) expect((await c.json("sfu_ready")).race).toBe(startAt);
}

/** The race ends (every racer back in the room). */
async function finish(cs: Client[], code: string) {
  for (const c of cs) c.sendJson({ type: "back" });
  await cs[0].json("ended");
  await snap(cs[0], (r) => r.phase !== "racing");
  await passDone(code);
}

describe("one Realtime session per member per room (finding 177)", () => {
  it("sessions and subscriptions stay across races; the grid's lock makes the missing pairs, a newcomer's only", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code } = await sessionRoom(3);
      for (const c of cs) await connect(c);
      await passDone(code);
      // In the room, between races: publications, no subscriptions yet.
      expect(subscriptions(fake)).toEqual([]);
      const sessions = await runInDurableObject(room(code), async (r: any) => r.state.members.map((m: any) => m.sfu.id));

      // Race 1: the pre-setup subscribes every ordered pair during the countdown, before the start.
      let starts = await race(cs, code, async () => expect(subscriptions(fake)).toHaveLength(6));
      const atStart = fake.calls.length;
      await release(cs, starts[0].startAt);
      await finish(cs, code);
      expect(subscriptions(fake, atStart)).toEqual([]);  // nothing left to do at the start

      // Race 2: the same sessions, nothing to subscribe.
      const before2 = fake.calls.length;
      starts = await race(cs, code);
      await release(cs, starts[0].startAt);
      await finish(cs, code);
      expect(fake.calls.slice(before2).filter((c) => !c.path.endsWith("datachannels/close"))).toEqual([]);
      expect(await runInDurableObject(room(code), async (r: any) => r.state.members.map((m: any) => m.sfu.id))).toEqual(sessions);

      // A newcomer between races: at race 3's lock only the pairs with it.
      const p3 = await Client.connect(`/v1/rooms/${code}`);
      p3.hello("P3", { features: FEATURES });
      expect((await p3.json("welcome")).sfu).toBe("setup");
      await connect(p3);
      const before3 = fake.calls.length;
      const everyone = [...cs, p3];
      starts = await race(everyone, code);
      const pairs = subscriptions(fake, before3);
      const newcomer = await runInDurableObject(room(code), async (r: any) => r.state.members[3].sfu.id);
      expect(pairs).toHaveLength(6);
      expect(pairs.every((p) => p.includes(newcomer))).toBe(true);
      await release(everyone, starts[0].startAt);
    } finally {
      fake.restore();
    }
  });

  it("a member who leaves has its session and the others' subscriptions to it closed", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code } = await sessionRoom(3);
      for (const c of cs) await connect(c);
      const starts = await race(cs, code);
      await release(cs, starts[0].startAt);
      const gone = await runInDurableObject(room(code), async (r: any) => r.state.members[2].sfu.id);
      const before = fake.calls.length;
      cs[2].sendJson({ type: "leave" });
      for (const c of cs.slice(0, 2)) expect((await c.json("sfu_closed")).channelIds).toHaveLength(2);
      await passDone(code);
      const closes = fake.calls.slice(before).filter((c) => c.path.endsWith("datachannels/close")).map((c) => c.path.split("/").at(-3));
      expect(closes).toHaveLength(3);  // its own session, and each subscriber's channels to it
      expect(closes).toContain(gone);
      await runInDurableObject(room(code), async (r: any) => {
        for (const m of r.state.members) expect(m.sfu.channels.some((x: any) => x.memberId === 2)).toBe(false);
      });
    } finally {
      fake.restore();
    }
  });

  it("the release comes as soon as every game acknowledges the start, with no API call between", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code } = await sessionRoom(4);
      for (const c of cs) await connect(c);
      const starts = await race(cs, code);
      const at = Date.now(), calls = fake.calls.length;
      await release(cs, starts[0].startAt);
      const took = Date.now() - at;
      expect(fake.calls.length).toBe(calls);
      expect(took).toBeLessThan(1000);
      console.log(`release ${took} ms after the acknowledgements were sent (4 racers, connected before the grid locked)`);
    } finally {
      fake.restore();
    }
  });

  it("a member sitting a race out has its subscriptions closed at the start, and gets them back at its next grid", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code } = await sessionRoom(3);
      for (const c of cs) await connect(c);
      let starts = await race(cs, code);
      await release(cs, starts[0].startAt);
      await finish(cs, code);
      // P2 sits race 2 out: nobody sends it race frames it would drop.
      const racers = cs.slice(0, 2);
      starts = await race(racers, code);
      expect((await cs[2].json("sfu_closed")).channelIds).toHaveLength(4);
      await passDone(code);
      await runInDurableObject(room(code), async (r: any) => {
        expect(r.state.members[2].sfu.channels.filter((x: any) => !x.local)).toEqual([]);
        expect(r.state.members[0].sfu.channels.filter((x: any) => !x.local)).toHaveLength(4);  // racers keep theirs
      });
      await release(racers, starts[0].startAt);
      await finish(racers, code);
      const before = fake.calls.length;
      starts = await race(cs, code);
      expect(subscriptions(fake, before)).toHaveLength(2);  // only P2's own two, back
      await release(cs, starts[0].startAt);
    } finally {
      fake.restore();
    }
  });

  it("a member back after a drop between races keeps its session: the welcome says ready, its channels follow", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code } = await sessionRoom(2);
      for (const c of cs) await connect(c);
      const starts = await race(cs, code);
      await release(cs, starts[0].startAt);
      await finish(cs, code);
      const token = cs[1].welcome.rejoinToken;
      await runInDurableObject(room(code), async (r: any) => { await r.webSocketClose(r.sockets.get(1), 1006, "", false); });
      const c = await Client.connect(`/v1/rooms/${code}`);
      c.hello("P1", { features: FEATURES, rejoinToken: token });
      const welcome = await c.json("welcome");
      expect(welcome.sfu).toBe("ready");
      expect(welcome.race).toBeUndefined();
      const channels = await c.json("sfu_channels");
      expect(channels.full).toBe(true);
      expect(channels.channels.map((x: any) => [x.memberId, x.local])).toEqual([[1, true], [1, true], [0, false], [0, false]]);
      expect(JSON.stringify(channels)).not.toMatch(/session/);
    } finally {
      fake.restore();
    }
  });
});
