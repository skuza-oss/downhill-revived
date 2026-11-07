// A racer whose room connection drops in its race gets back into the room and the race (finding
// 175): the race data goes over the Realtime SFU, so the game races on while the socket is down;
// the room keeps its seat and race place for REJOIN_GRACE_MS and tells it what it missed. Run A
// and C of finding 169's PC<->Mac check lost both games at a race start this way (1006): before,
// a drop in the race took the racer out at once and the game never tried to rejoin.
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { describe, it, expect } from "vitest";
import { Client, randomIp } from "./helpers";
import { fakeRealtime, publishedSession } from "./sfu-fake";

const FEATURES = ["session", "countdown", "race_rejoin"];
const room = (code: string) => env.ROOM.get(env.ROOM.idFromName(code));
const isType = (type: string) => (f: unknown) => typeof f === "string" && JSON.parse(f).type === type;
const snap = async (c: Client, pred: (r: any) => boolean = () => true) =>
  JSON.parse(await c.next(f => typeof f === "string" && JSON.parse(f).type === "room" && pred(JSON.parse(f))) as string);

/** A session room of `n` racers whose race has started, every game on one address (the PC and the
    Mac of the runs share one home connection), so the limits per address count them together. */
async function raceSetup(n: number, features = FEATURES, ip = randomIp()) {
  const h = await Client.connect("/v1/rooms/new", undefined, ip);
  h.hello("Host", { features });
  const { code } = await h.json("welcome");
  const cs = [h];
  for (let i = 1; i < n; i++) {
    const c = await Client.connect(`/v1/rooms/${code}`, undefined, ip);
    c.hello(`P${i}`, { features });
    await c.json("welcome");
    cs.push(c);
  }
  for (const c of cs) c.sendJson({ type: "ready", ready: true });
  await snap(h, r => r.members.length === n && r.members.every((m: any) => m.ready));
  h.sendJson({ type: "start" });
  await snap(h, r => r.phase === "starting");
  await runInDurableObject(room(code), async (r: any, ctx) => {
    r.state.countdownEndsAt = Date.now() - 1;
    await ctx.storage.put("room", r.state);
  });
  await runDurableObjectAlarm(room(code));
  const starts = await Promise.all(cs.map(c => c.json("start")));
  return { cs, code, ip, startAt: starts[0].startAt as number, starts };
}

/** The room sees a member's connection end without a close frame (1006). */
async function dropped(code: string, memberId: number) {
  await runInDurableObject(room(code), async (r: any) => {
    await r.webSocketClose(r.sockets.get(memberId), 1006, "", false);
  });
}

/** The game's rejoin: a new connection from `ip`, its hello with the token and its race. */
async function rejoin(code: string, name: string, token: string, ip: string, extra: object = {}) {
  const c = await Client.connect(`/v1/rooms/${code}`, undefined, ip);
  c.hello(name, { features: FEATURES, rejoinToken: token, ...extra });
  return { c, welcome: await c.json("welcome") };
}

/** Every racer's Realtime session published, subscribed to the others, acknowledged, released. */
async function released(code: string) {
  await runInDurableObject(room(code), async (r: any, ctx) => {
    const ids = r.state.members.map((m: any) => m.id);
    for (const m of r.state.members) {
      m.sfu = publishedSession(m.id, ids.filter((id: number) => id !== m.id));
      m.sfuRace = r.state.race.startAt;
      m.sfuReady = true;
    }
    r.state.race.sfuReleased = true;
    await ctx.storage.put("room", r.state);
  });
}

describe("a racer's room connection drops in its race (finding 175)", () => {
  it("at the race start: the seat is kept, the rejoin (with the guard's limits on) gets the race back and sets its SFU up again", async () => {
    const { cs, code, ip, startAt } = await raceSetup(2);
    const [h, p1] = cs;
    const token = p1.welcome.rejoinToken;
    // Both games lose the room at the start, as in runs A and C; the host first.
    await dropped(code, 0);
    await dropped(code, 1);
    await runInDurableObject(room(code), async (r: any) => {
      expect(r.state.race?.startAt).toBe(startAt);
      expect(r.state.members.map((m: any) => [m.id, m.awaySince != null, r.inRace(r.state, m.id)])).toEqual([[0, true, true], [1, true, true]]);
    });
    // The alarm inside the grace keeps them (and the room: it would end with no member left).
    await runDurableObjectAlarm(room(code));
    // Each game's backoff makes a few attempts that fail before one gets through: a burst from the
    // one shared address. The limit per address (30 a minute) lets them all in.
    for (let i = 0; i < 6; i++) {
      const tried = await Client.connect(`/v1/rooms/${code}`, undefined, ip);
      tried.ws.close(1000);
    }
    const host = await rejoin(code, "Host", h.welcome.rejoinToken, ip, { raceStartAt: startAt });
    const guest = await rejoin(code, "P1", token, ip, { raceStartAt: startAt });
    expect(host.welcome).toMatchObject({ memberId: 0, host: true, race: { startAt, sfu: "setup" } });
    expect(guest.welcome).toMatchObject({ memberId: 1, race: { startAt, sfu: "setup" } });
    const r = await snap(host.c, r => r.members.length === 2);
    expect(r.phase).toBe("racing");
    expect(r.members.map((m: any) => [m.id, m.inRace, m.raceState])).toEqual([[0, true, "racing"], [1, true, "racing"]]);
    // A session still being set up starts again: sfu_restart, then a new offer is taken.
    const fake = fakeRealtime();
    try {
      await runInDurableObject(room(code), async (r: any) => {
        r.env = { ...r.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        r.state.members[1].sfuRace = startAt;  // its first offer had gone out before the drop
        r.state.members[1].sfu = { id: "session-old", phase: "connect", channels: [] };
      });
      guest.c.sendJson({ type: "sfu_restart" });
      guest.c.sendJson({ type: "sfu_offer", sdp: "offer" });
      expect((await guest.c.json("sfu_answer")).sdp).toBe("SFU");
      await runInDurableObject(room(code), async (r: any) => {
        await r.sfuQueue;
        expect(r.state.members[1].sfu.id).not.toBe("session-old");
        expect(r.inRace(r.state, 1)).toBe(true);
      });
      // At most SFU_RESTARTS (3) a race.
      for (let i = 0; i < 2; i++) guest.c.sendJson({ type: "sfu_restart" });
      guest.c.sendJson({ type: "sfu_restart" });
      expect((await guest.c.json("error")).message).toContain("No more Realtime restarts");
    } finally {
      fake.restore();
    }
  });

  it("mid-race: the racer keeps its seat and its session, and hears who left, its channels and the release", async () => {
    const { cs, code, ip, startAt } = await raceSetup(3);
    const [h, p1, p2] = cs;
    await released(code);
    await dropped(code, 1);
    // While P1 is away P2 gives up (P1 misses the left) and the host finishes.
    p2.sendJson({ type: "back" });
    await h.json("left");
    h.sendJson({ type: "finish", ticks: 900, place: 1 });
    await snap(h, r => r.members[0].raceState === "finished");
    await runDurableObjectAlarm(room(code));
    const { c, welcome } = await rejoin(code, "P1", p1.welcome.rejoinToken, ip, { raceStartAt: startAt });
    expect(welcome).toMatchObject({ memberId: 1, race: { startAt, sfu: "ready" } });
    expect(await c.json("left")).toMatchObject({ slot: 2, memberId: 2, reason: "gave_up" });
    const channels = await c.json("sfu_channels");
    expect(channels.full).toBe(true);
    expect(channels.channels.map((x: any) => [x.memberId, x.name, x.local])).toEqual(
      [[1, "state", true], [1, "events", true], [0, "state", false], [0, "events", false], [2, "state", false], [2, "events", false]]);
    await c.json("sfu_ready");
    expect(c.seen.some(s => JSON.parse(s).type === "start")).toBe(false);
    // Its finish counts; the race then closes with both results.
    c.sendJson({ type: "finish", ticks: 950, place: 2 });
    const done = await snap(h, r => !!r.lastRace);
    expect(done.lastRace.entrants.map((e: any) => [e.id, e.ticks ?? null, e.dnfReason ?? null])).toEqual(
      [[0, 900, null], [1, 950, null], [2, null, "gave_up"]]);
  });

  it("a game that missed the start (its hello names no race) gets the start again", async () => {
    const { cs, code, ip, starts } = await raceSetup(2);
    await dropped(code, 1);
    const { c, welcome } = await rejoin(code, "P1", cs[1].welcome.rejoinToken, ip);
    expect(welcome.race).toMatchObject({ startAt: starts[1].startAt, sfu: "setup" });
    const start = await c.json("start");
    expect(start).toMatchObject({ yourSlot: starts[1].yourSlot, startAt: starts[1].startAt, seed: starts[1].seed, players: starts[1].players });
  });

  it("not back within the grace: out of the race (the host's AI takes the rider), the race goes on", async () => {
    const { cs, code } = await raceSetup(2);
    await dropped(code, 1);
    await runInDurableObject(room(code), async (r: any) => { r.state.members[1].awaySince = Date.now() - 21_000; });
    await runDurableObjectAlarm(room(code));
    expect(await cs[0].json("left")).toMatchObject({ slot: 1, memberId: 1 });
    const r = await snap(cs[0], r => r.members.length === 1);
    expect([r.phase, r.members[0].inRace]).toEqual(["racing", true]);
  });

  it("an older game (no race_rejoin) dropped in its race is out of it at once, as before", async () => {
    const { cs, code } = await raceSetup(2, ["session", "countdown"]);
    await dropped(code, 1);
    expect(await cs[0].json("left")).toMatchObject({ slot: 1, memberId: 1 });
    expect((await snap(cs[0], r => r.members.length === 1)).members[0].id).toBe(0);
  });
});
