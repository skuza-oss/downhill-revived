// A member outside the running race watches it (finding 269): it gets the race's start without a slot,
// its session is subscribed to every racer's channels and no racer to its own, it is in no result, the
// race is released and closes without it, and it is back in the room with everyone at the close.
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { describe, it, expect } from "vitest";
import { Client } from "./helpers";
import { fakeRealtime } from "./sfu-fake";

const FEATURES = ["session", "countdown", "race_rejoin", "race_go"];
const room = (code: string) => env.ROOM.get(env.ROOM.idFromName(code));
const snap = async (c: Client, pred: (r: any) => boolean = () => true) =>
  JSON.parse(await c.next(f => typeof f === "string" && JSON.parse(f).type === "room" && pred(JSON.parse(f))) as string);
type Fake = ReturnType<typeof fakeRealtime>;
const subscriptions = (fake: Fake, from = 0) => fake.calls.slice(from).filter((c) => c.path.endsWith("datachannels/new") && c.body.dataChannels[0].location === "remote")
  .flatMap((c) => [...new Set(c.body.dataChannels.map((x: any) => x.sessionId))].map((pub) => `${c.path.split("/").at(-3)}<-${pub}`));

async function sessionRoom(n: number) {
  const host = await Client.connect("/v1/rooms/new");
  host.hello("Host", { features: FEATURES });
  const welcome = await host.json("welcome");
  const code = welcome.code;
  await runInDurableObject(room(code), async (r: any) => { r.env = { ...r.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" }; });
  const cs = [host];
  for (let i = 1; i < n; i++) {
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello(`P${i}`, { features: FEATURES });
    await c.json("welcome");
    cs.push(c);
  }
  return { cs, code, welcome };
}

async function connect(c: Client) {
  c.sendJson({ type: "sfu_offer", sdp: "offer" });
  await c.json("sfu_answer");
  c.sendJson({ type: "sfu_connected" });
  await c.json("sfu_channels");
}

async function passDone(code: string) {
  await runInDurableObject(room(code), async (r: any) => {
    for (let i = 0; i < 300 && (r.subscribing || r.sfuWork()); i++) await new Promise((x) => setTimeout(x, 10));
    await r.sfuQueue;
  });
}

/** The racers ready, the countdown run out: their start messages. */
async function race(racers: Client[], code: string) {
  for (const c of racers) c.sendJson({ type: "ready", ready: true });
  await snap(racers[0], (r) => r.members.filter((m: any) => m.ready).length === racers.length);
  if (await runInDurableObject(room(code), async (r: any) => r.state.countdownEndsAt == null)) {
    racers[0].sendJson({ type: "start" });
    await snap(racers[0], (r) => r.phase === "starting");
  }
  await passDone(code);
  await runInDurableObject(room(code), async (r: any, ctx) => {
    r.state.countdownEndsAt = Date.now() - 1;
    await ctx.storage.put("room", r.state);
  });
  await runDurableObjectAlarm(room(code));
  return Promise.all(racers.map((c) => c.json("start")));
}

describe("watching a running race (finding 269)", () => {
  it("the welcome lists watch", async () => {
    const { welcome } = await sessionRoom(1);
    expect(welcome.features).toContain("watch");
  });

  it("a watcher gets the start without a slot, subscriptions one way, no result; the race closes without it", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code } = await sessionRoom(3);
      for (const c of cs) await connect(c);
      const [a, b, w] = cs;
      // No race yet: nothing to watch.
      w.sendJson({ type: "watch" });
      expect((await w.json("error")).code).toBe("bad_value");
      const starts = await race([a, b], code);
      for (const c of [a, b]) c.sendJson({ type: "sfu_ready", race: starts[0].startAt });
      for (const c of [a, b]) expect((await c.json("sfu_ready")).race).toBe(starts[0].startAt);
      await passDone(code);
      const sessions: string[] = await runInDurableObject(room(code), async (r: any) => r.state.members.map((m: any) => m.sfu.id));
      const before = fake.calls.length;

      // A racer can't watch its own race.
      a.sendJson({ type: "watch" });
      expect((await a.json("error")).code).toBe("room_started");

      w.sendJson({ type: "watch" });
      const start = await w.json("start");
      expect(start).toMatchObject({ watch: true, yourSlot: -1, startAt: starts[0].startAt, track: starts[0].track, seed: starts[0].seed });
      expect(start.players.map((p: any) => p.memberId)).toEqual(starts[0].players.map((p: any) => p.memberId));
      const shown = await snap(a, (r) => r.members.some((m: any) => m.watching));
      expect(shown.members.find((m: any) => m.id === 2)).toMatchObject({ watching: true, inRace: false });
      expect(shown.members.find((m: any) => m.id === 2).raceState).toBeUndefined();
      await passDone(code);
      // The watcher's session subscribes to both racers'; nobody subscribes to the watcher's.
      const pairs = subscriptions(fake, before);
      expect(pairs.sort()).toEqual([`${sessions[2]}<-${sessions[0]}`, `${sessions[2]}<-${sessions[1]}`].sort());
      expect(pairs.some((p) => p.endsWith(`<-${sessions[2]}`))).toBe(false);
      // The watcher's own acknowledgement and finish count for nothing.
      w.sendJson({ type: "sfu_ready", race: starts[0].startAt });
      w.sendJson({ type: "finish", ticks: 100, place: 1 });

      // The racers finish: the race closes with the watcher still watching; it gets ended like everyone.
      a.sendJson({ type: "finish", ticks: 3600, place: 1 });
      b.sendJson({ type: "finish", ticks: 3700, place: 2 });
      expect((await w.json("ended")).reason).toBe("done");
      const after = await snap(w, (r) => r.phase !== "racing");
      expect(after.lastRace.entrants.map((x: any) => x.id)).toEqual([0, 1]);
      expect(after.members.some((m: any) => m.watching)).toBe(false);
      w.sendJson({ type: "back" });

      // The next race: the watcher races normally (its subscriptions stay, the racers' to it are made).
      const next = await race([a, b, w], code);
      expect(next.map((s: any) => s.yourSlot).sort()).toEqual([0, 1, 2]);
      expect(next.every((s: any) => !s.watch)).toBe(true);
    } finally {
      fake.restore();
    }
  });

  it("a watcher's BACK ends its watching; the racers' race is untouched", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code } = await sessionRoom(3);
      for (const c of cs) await connect(c);
      const [a, b, w] = cs;
      const starts = await race([a, b], code);
      w.sendJson({ type: "watch" });
      await w.json("start");
      await snap(a, (r) => r.members.some((m: any) => m.watching));
      w.sendJson({ type: "back" });
      const shown = await snap(a, (r) => !r.members.some((m: any) => m.watching));
      expect(shown.phase).toBe("racing");
      expect(shown.members.filter((m: any) => m.inRace).map((m: any) => m.id)).toEqual([0, 1]);
      // A racer who leaves while watched: the watcher hears `left` as the racers do.
      w.sendJson({ type: "watch" });
      await w.json("start");
      b.sendJson({ type: "back" });
      expect(await w.json("left")).toMatchObject({ slot: 1, memberId: 1, reason: "gave_up" });
      expect(starts).toHaveLength(2);
    } finally {
      fake.restore();
    }
  });

  it("a watcher who joins after a racer left is told who left", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code } = await sessionRoom(4);
      for (const c of cs) await connect(c);
      const [a, b, c2, w] = cs;
      await race([a, b, c2], code);
      b.sendJson({ type: "back" });
      await snap(a, (r) => r.members.find((m: any) => m.id === 1)?.inRace === false);
      w.sendJson({ type: "watch" });
      expect((await w.json("start")).watch).toBe(true);
      expect(await w.json("left")).toMatchObject({ slot: 1, memberId: 1 });
    } finally {
      fake.restore();
    }
  });
});
