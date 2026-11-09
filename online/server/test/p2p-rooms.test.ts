// P2P rooms (finding 186): a server room whose race frames travel over peer connections in a star
// around one member, the hub, and over the SFU for a member who can't reach it. The room runs every
// rule as an SFU room's, picks the hub, relays the star's signaling, decides who falls back, and
// subscribes on the SFU only what the fallback needs (the hub bridges it to the star).
import { env, runDurableObjectAlarm, runInDurableObject } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import { Client } from "./helpers";
import { fakeRealtime } from "./sfu-fake";

const FEATURES = ["session", "countdown", "race_rejoin", "p2p"];
const room = (code: string) => env.ROOM.get(env.ROOM.idFromName(code));
const isType = (type: string) => (f: unknown) => typeof f === "string" && JSON.parse(f).type === type;
const snap = async (c: Client, pred: (r: any) => boolean = () => true) =>
  JSON.parse(await c.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && pred(JSON.parse(f))) as string);
/** The next routes message (`p2p`) that `pred` accepts. */
const routes = async (c: Client, pred: (r: any) => boolean = () => true) =>
  JSON.parse(await c.next((f) => typeof f === "string" && JSON.parse(f).type === "p2p" && pred(JSON.parse(f))) as string);
const linkOf = (r: any, id: number) => r.members.find((m: any) => m.id === id)?.link;
type Fake = ReturnType<typeof fakeRealtime>;
/** The pairs subscribed, "subscriber<-publisher", by member id. */
async function pairs(fake: Fake, code: string, from = 0): Promise<string[]> {
  const ids: Record<string, number> = await runInDurableObject(room(code), async (r: any) =>
    Object.fromEntries(r.state.members.filter((m: any) => m.sfu).map((m: any) => [m.sfu.id, m.id])));
  return fake.calls.slice(from).filter((c) => c.path.endsWith("datachannels/new") && c.body.dataChannels[0].location === "remote")
    .flatMap((c) => [...new Set(c.body.dataChannels.map((x: any) => x.sessionId))].map((pub) => `${ids[c.path.split("/").at(-3)!]}<-${ids[pub as string]}`))
    .sort();
}

/** A P2P room of `n` members (P0 made it); `realtime`: the room has the (fake) Realtime API. */
async function p2pRoom(n: number, realtime = false) {
  const host = await Client.connect("/v1/rooms/new?transport=p2p");
  host.hello("P0", { features: FEATURES });
  const welcome = await host.json("welcome");
  const code: string = welcome.code;
  if (realtime) await setEnv(code, { REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" });
  const cs = [host];
  for (let i = 1; i < n; i++) {
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello(`P${i}`, { features: FEATURES });
    await c.json("welcome");
    cs.push(c);
  }
  const ids = await runInDurableObject(room(code), async (r: any) => r.state.members.map((m: any) => m.id as number));
  return { cs, code, ids, welcome };
}

async function setEnv(code: string, vars: Record<string, string>) {
  await runInDurableObject(room(code), async (r: any) => { r.env = { ...r.env, ...vars }; });
}

/** The link between the hub and member `id`, reported by `from` (either end). */
function link(from: Client, hub: number, id: number, up: boolean) {
  from.sendJson({ type: "p2p_link", hub, member: id, up });
}

/** A member's game connects to the SFU as it does on joining. */
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

/** Everyone ready, the countdown's end: the race starts. Returns the start messages. */
async function race(cs: Client[], code: string) {
  for (const c of cs) c.sendJson({ type: "ready", ready: true });
  await snap(cs[0], (r) => r.members.filter((m: any) => m.ready).length === cs.length);
  if (await runInDurableObject(room(code), async (r: any) => r.state.countdownEndsAt == null)) {
    cs[0].sendJson({ type: "start" });
    await snap(cs[0], (r) => r.phase === "starting");
  }
  await runInDurableObject(room(code), async (r: any, ctx) => {
    r.state.countdownEndsAt = Date.now() - 1;
    await ctx.storage.put("room", r.state);
  });
  await runDurableObjectAlarm(room(code));
  return Promise.all(cs.map((c) => c.json("start")));
}

/** Moves a deadline of the room into the past, then runs its alarm. */
async function expire(code: string, change: (st: any) => void) {
  await runInDurableObject(room(code), async (r: any, ctx) => {
    change(r.state);
    await ctx.storage.put("room", r.state);
  });
  await runDurableObjectAlarm(room(code));
}

describe("P2P rooms (finding 186)", () => {
  it("are server rooms: a 7-character code, never public or listed, the creator is the hub, a game without P2P is refused", async () => {
    const { cs, code, ids, welcome } = await p2pRoom(2);
    expect(code).toMatch(/^[23456789A-HJKMNP-Z]{7}$/);
    expect(welcome.transport).toBe("p2p");
    const [host, guest] = cs;
    // The routes: the creator is the hub, the newcomer's link is being made.
    const r = await routes(guest);
    expect(r.hub).toBe(ids[0]);
    expect(linkOf(r, ids[1])).toBeUndefined();
    expect(r.race).toBeNull();
    // Public is ignored: the members see each other's addresses.
    host.sendJson({ type: "settings", public: true, roomName: "Open" });
    const s = await snap(host, (x) => x.roomName === "Open");
    expect(s.public).toBe(false);
    const list = await (await exports.default.fetch("https://dhd.test/v1/lobbies")).json<{ lobbies: { code: string }[] }>();
    expect(list.lobbies.some((l) => l.code === code)).toBe(false);
    // A game that can't race in a P2P room.
    const old = await Client.connect(`/v1/rooms/${code}`);
    old.hello("Old", { features: ["session", "countdown", "race_rejoin"] });
    expect((await old.json("error")).code).toBe("bad_message");
    expect(await old.closed()).toBe(4000);
  });

  it("relay the star's signaling only between the hub and a member, with the sender's id", async () => {
    const { cs, ids } = await p2pRoom(3);
    const [hub, a, b] = cs;
    hub.sendJson({ type: "p2p_signal", to: ids[1], signal: { type: "offer", sdp: "v=0" } });
    const offer = await a.json("p2p_signal");
    expect(offer).toEqual({ type: "p2p_signal", from: ids[0], signal: { type: "offer", sdp: "v=0" } });
    a.sendJson({ type: "p2p_signal", to: ids[0], signal: { type: "answer", sdp: "v=0" } });
    expect((await hub.json("p2p_signal")).from).toBe(ids[1]);
    // Never between two members.
    a.sendJson({ type: "p2p_signal", to: ids[2], signal: { type: "candidate", candidate: "x" } });
    a.sendJson({ type: "ping", t: 1 });
    await a.json("pong");
    b.sendJson({ type: "ping", t: 2 });
    await b.json("pong");
    expect(b.seen.some((t) => JSON.parse(t).type === "p2p_signal")).toBe(false);
  });

  it("an SFU room has no star: no routes, and P2P messages are unknown", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("Host", { features: FEATURES });
    const welcome = await host.json("welcome");
    expect(welcome.code).toHaveLength(6);
    expect(welcome.transport).toBeUndefined();
    host.sendJson({ type: "p2p_link", hub: 0, member: 1, up: true });
    expect((await host.json("error")).code).toBe("bad_message");
    expect(host.seen.some((t) => JSON.parse(t).type === "p2p")).toBe(false);
  });

  it("a link is up when either end says so; one not up in time, or down, races over the SFU; a report about an earlier hub counts for nothing", async () => {
    const { cs, code, ids } = await p2pRoom(4);
    const [hub, a, b, c] = cs;
    link(hub, ids[0], ids[1], true);                     // the hub's report
    expect(linkOf(await routes(c, (r) => linkOf(r, ids[1])), ids[1])).toBe("star");
    link(b, ids[0], ids[2], false);                      // the member's own report
    expect(linkOf(await routes(c, (r) => linkOf(r, ids[2])), ids[2])).toBe("sfu");
    link(b, ids[0], ids[2], true);                       // decided for this try: stays on the SFU
    link(a, ids[1], ids[1], true);                       // about a hub that isn't one
    link(a, ids[0], ids[3], false);                      // about another member's link
    // C's link was never reported: the SFU once its time is up.
    await expire(code, (st) => { st.members[3].linkFrom = Date.now() - 60_000; });
    const r = await routes(hub, (x) => linkOf(x, ids[3]) === "sfu");
    expect([linkOf(r, ids[1]), linkOf(r, ids[2]), linkOf(r, ids[3])]).toEqual(["star", "sfu", "sfu"]);
    expect(linkOf(r, ids[0])).toBeUndefined();  // the hub has none
  });

  it("bridge a fallback racer through the hub: only its pairs with the hub and the other fallback racers are subscribed", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code, ids } = await p2pRoom(4, true);
      for (const c of cs) await connect(c);
      const [hub, star, f1, f2] = cs;
      link(hub, ids[0], ids[1], true);
      link(f1, ids[0], ids[2], false);
      link(hub, ids[0], ids[3], false);
      await routes(star, (r) => linkOf(r, ids[3]) === "sfu");
      await passDone(code);
      const before = fake.calls.length;
      const starts = await race(cs, code);
      await passDone(code);
      // Pre-setup during the countdown, or at the start: the hub and the two fallback racers, each way.
      const made = await pairs(fake, code);
      expect(made).toEqual([`${ids[0]}<-${ids[2]}`, `${ids[0]}<-${ids[3]}`, `${ids[2]}<-${ids[0]}`, `${ids[2]}<-${ids[3]}`,
        `${ids[3]}<-${ids[0]}`, `${ids[3]}<-${ids[2]}`].sort());
      expect(made.some((p) => p.includes(`${ids[1]}`))).toBe(false);  // the star racer: never
      expect(fake.calls.length).toBeGreaterThanOrEqual(before);
      // The routes give the race to everyone, before its start (the hub's forwarding included).
      const seen = star.seen.map((t) => JSON.parse(t));
      const r = seen.find((x) => x.type === "p2p" && x.race);
      expect(seen.indexOf(r)).toBeLessThan(seen.findIndex((x) => x.type === "start"));
      expect(r.race.startAt).toBe(starts[0].startAt);
      expect(r.race.slots).toEqual(ids);
      // The release waits for the SFU's part only: the hub and the fallback racers.
      for (const c of [hub, f1]) c.sendJson({ type: "sfu_ready", race: starts[0].startAt });
      await runDurableObjectAlarm(room(code));
      expect(f1.seen.some((t) => JSON.parse(t).type === "sfu_ready")).toBe(false);
      f2.sendJson({ type: "sfu_ready", race: starts[0].startAt });
      for (const c of [hub, f1, f2]) expect((await c.json("sfu_ready")).race).toBe(starts[0].startAt);
      expect(star.seen.some((t) => JSON.parse(t).type === "sfu_ready")).toBe(false);
    } finally {
      fake.restore();
    }
  });

  it("a star racer that loses its link in the race falls back: its pairs are made then and it is released", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code, ids } = await p2pRoom(3, true);
      for (const c of cs) await connect(c);
      const [hub, a, b] = cs;
      link(hub, ids[0], ids[1], true);
      link(hub, ids[0], ids[2], true);
      await routes(b, (r) => linkOf(r, ids[1]) === "star" && linkOf(r, ids[2]) === "star");
      const starts = await race(cs, code);
      await passDone(code);
      expect(await pairs(fake, code)).toEqual([]);  // an all-star race: nothing on the SFU
      // Every racer acknowledges anyway (its game does): nothing to release, nothing sent.
      for (const c of cs) c.sendJson({ type: "sfu_ready", race: starts[0].startAt });
      await runDurableObjectAlarm(room(code));
      expect(hub.seen.some((t) => JSON.parse(t).type === "sfu_ready")).toBe(false);
      // B's link dies in the race.
      link(b, ids[0], ids[2], false);
      await routes(a, (r) => linkOf(r, ids[2]) === "sfu");
      await passDone(code);
      expect(await pairs(fake, code)).toEqual([`${ids[0]}<-${ids[2]}`, `${ids[2]}<-${ids[0]}`]);
      for (const c of [hub, b]) expect((await c.json("sfu_ready")).race).toBe(starts[0].startAt);
      expect(a.seen.some((t) => JSON.parse(t).type === "sfu_ready")).toBe(false);
    } finally {
      fake.restore();
    }
  });

  it("a fallback racer whose SFU never connects is out of the race after the setup deadline; the star races on", async () => {
    const { cs, code, ids } = await p2pRoom(3);
    const [hub, a, f] = cs;
    link(hub, ids[0], ids[1], true);
    link(f, ids[0], ids[2], false);
    await routes(a, (r) => linkOf(r, ids[2]) === "sfu");
    // The setup deadlines run from its fallback or the start message, whichever is later.
    await setEnv(code, { SFU_CONNECT_MS: "1", SFU_SETUP_MS: "1" });
    await race(cs, code);
    await runDurableObjectAlarm(room(code));
    const left = await a.json("left");
    expect(left.memberId).toBe(ids[2]);
    expect((await f.json("error")).message).toMatch(/Realtime/);
    const s = await snap(hub, (x) => x.members.find((m: any) => m.id === ids[2])?.inRace === false);
    expect(s.phase).toBe("racing");  // the hub and A race on
  });

  it("a star racer whose SFU session fails stays in the race (only a fallback racer needs the SFU)", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code, ids } = await p2pRoom(2, true);
      const [hub, a] = cs;
      link(hub, ids[0], ids[1], true);
      await routes(a, (r) => linkOf(r, ids[1]) === "star");
      await race(cs, code);
      fake.fail = (path) => path.endsWith("sessions/new");
      a.sendJson({ type: "sfu_offer", sdp: "offer" });
      await a.json("sfu_reset");
      await runDurableObjectAlarm(room(code));
      expect(a.seen.some((t) => JSON.parse(t).type === "left")).toBe(false);
      expect(await runInDurableObject(room(code), async (r: any) => r.state.race.left)).toEqual([]);
    } finally {
      fake.restore();
    }
  });

  it("results reach every member when the finish window ends the race (finding 185's lost results)", async () => {
    const { cs, code, ids } = await p2pRoom(3);
    const [hub, a, b] = cs;
    link(hub, ids[0], ids[1], true);
    link(hub, ids[0], ids[2], true);
    await race(cs, code);
    hub.sendJson({ type: "finish", ticks: 3600, place: 1 });
    await snap(a, (r) => r.finishEndsAt !== null);
    // The window closes: the others are time_up, the race ends, and every game hears it from the room.
    await expire(code, (st) => { st.race.finishEndsAt = Date.now() - 1; });
    for (const c of [hub, a, b]) {
      expect((await c.json("ended")).reason).toBe("done");
      const r = await snap(c, (x) => x.lastRace !== null);
      expect(r.lastRace.entrants.map((e: any) => [e.id, e.ticks ?? e.dnfReason])).toEqual([[ids[0], 3600], [ids[1], "time_up"], [ids[2], "time_up"]]);
    }
    // The routes say the race is over (the hub stops forwarding).
    expect(b.seen.map((t) => JSON.parse(t)).filter((m) => m.type === "p2p").at(-1).race).toBeNull();
  });

  it("the hub leaving mid-race: the next hub is chosen, everyone links anew, the race goes on and its results reach all", async () => {
    const { cs, code, ids } = await p2pRoom(4);
    const [hub, a, b, c] = cs;
    link(hub, ids[0], ids[1], false);   // A never reached the hub: it can't host either, likely
    link(hub, ids[0], ids[2], true);
    link(hub, ids[0], ids[3], true);
    await routes(a, (r) => linkOf(r, ids[3]) === "star");
    await race(cs, code);
    hub.sendJson({ type: "leave" });
    // B: on the star (direct links work for it), here longer than C.
    const r = await routes(a, (x) => x.hub !== ids[0]);
    expect(r.hub).toBe(ids[2]);
    expect(r.members.map((m: any) => m.link ?? null)).toEqual([null, null, null]);  // all being made
    expect(r.race.left).toEqual([0]);  // the old hub's rider is the AI's now
    expect(a.seen.map((t) => JSON.parse(t)).find((m) => m.type === "left")?.memberId).toBe(ids[0]);
    const s = await snap(b, (x) => x.members.length === 3);
    expect(s.phase).toBe("racing");
    // The new hub's links come up (C) or don't (A, in time); the race finishes with results for all.
    link(c, ids[2], ids[3], true);
    await expire(code, (st) => { for (const m of st.members) if (m.linkFrom) m.linkFrom = Date.now() - 60_000; });
    const after = await routes(b, (x) => linkOf(x, ids[1]) === "sfu");
    expect(linkOf(after, ids[3])).toBe("star");
    for (const x of [a, b, c]) x.sendJson({ type: "finish", ticks: 4000, place: 2 });
    for (const x of [a, b, c]) {
      await x.json("ended");
      expect((await snap(x, (y) => y.lastRace !== null)).lastRace.entrants).toHaveLength(4);
    }
  });

  it("hub choice: on a star before, then a working SFU session, then the lowest ping, then the one here longest", async () => {
    const { code, ids } = await p2pRoom(5);
    const order = await runInDurableObject(room(code), async (r: any) => {
      const st = r.state;
      const pick = () => r.bestHub(st, Date.now(), { exclude: ids[0] }).id;
      const out = [pick()];                                   // nothing known: the earliest joiner
      r.rtts.set(ids[3], [80, 80]); r.rtts.set(ids[4], [20, 20]); r.rtts.set(ids[1], [90]); r.rtts.set(ids[2], [95]);
      out.push(pick());                                       // the lowest ping
      st.members[2].sfu = { id: "s", phase: "ready", channels: [] };
      out.push(pick());                                       // a working SFU session first
      st.members[3].direct = true;
      out.push(pick());                                       // a star before that
      return out;
    });
    expect(order).toEqual([ids[1], ids[4], ids[2], ids[3]]);
  });

  it("one member's link going down never moves the hub, however often (review: a blip or a false report)", async () => {
    const { cs, code, ids } = await p2pRoom(3);
    const [hub, a, b] = cs;
    link(hub, ids[0], ids[1], true);    // A on the star
    link(b, ids[0], ids[2], false);     // B never links: on the SFU
    await routes(hub, (r) => linkOf(r, ids[1]) === "star" && linkOf(r, ids[2]) === "sfu");
    await race(cs, code);
    // A blips: every member present is on the SFU now. The hub stays, now and after its minute
    // (A just lost its link; B has never been on a star).
    link(a, ids[0], ids[1], false);
    await routes(hub, (r) => linkOf(r, ids[1]) === "sfu");
    await runDurableObjectAlarm(room(code));
    await expire(code, (st) => { st.hubChangedAt = Date.now() - 120_000; });
    expect(await runInDurableObject(room(code), async (r: any) => r.state.hub)).toBe(ids[0]);
    expect(hub.seen.map((t) => JSON.parse(t)).filter((m) => m.type === "p2p").every((m) => m.hub === ids[0])).toBe(true);
  });

  it("a hub whose links all died moves after its minute and their grace, never to a member that just lost its link", async () => {
    const { cs, code, ids } = await p2pRoom(4);
    const [hub, a, b, c] = cs;
    for (const id of [ids[1], ids[2], ids[3]]) link(hub, ids[0], id, true);
    await routes(c, (r) => [1, 2, 3].every((k) => linkOf(r, ids[k]) === "star"));
    // The hub's network goes: every link dies (each member says so).
    for (const [x, k] of [[a, 1], [b, 2], [c, 3]] as const) link(x, ids[0], ids[k], false);
    await routes(hub, (r) => [1, 2, 3].every((k) => linkOf(r, ids[k]) === "sfu"));
    await expire(code, (st) => { st.hubChangedAt = Date.now() - 120_000; });
    // All three just lost their link: none is picked yet.
    expect(await runInDurableObject(room(code), async (r: any) => r.state.hub)).toBe(ids[0]);
    // C lost it long ago (and links work for it): the next hub, once the others' grace is over too.
    await expire(code, (st) => {
      for (const m of st.members) if (m.lostLinkAt) m.lostLinkAt = Date.now() - 10_000;
      st.members[3].lostLinkAt = Date.now() - 120_000;
    });
    expect((await routes(a, (r) => r.hub !== ids[0])).hub).toBe(ids[3]);
    // No second change within the minute, whatever the links do.
    link(a, ids[3], ids[1], false);
    link(b, ids[3], ids[2], false);
    link(hub, ids[3], ids[0], false);
    await runDurableObjectAlarm(room(code));
    expect(await runInDurableObject(room(code), async (r: any) => r.state.hub)).toBe(ids[3]);
  });

  it("a hub whose room connection drops hands the hub on at once, and is a member when it comes back", async () => {
    const { cs, code, ids } = await p2pRoom(3);
    const [hub, a, b] = cs;
    link(hub, ids[0], ids[1], true);
    link(hub, ids[0], ids[2], true);
    await routes(b, (r) => linkOf(r, ids[2]) === "star");
    await race(cs, code);
    const token = hub.welcome.rejoinToken;
    await runInDurableObject(room(code), async (r: any) => { await r.webSocketClose(r.sockets.get(ids[0]), 1006, "", false); });
    const r = await routes(a, (x) => x.hub !== ids[0]);
    expect(r.hub).toBe(ids[1]);
    expect(r.race.left).toEqual([]);  // its rider races on: its game may come back (finding 175)
    const back = await Client.connect(`/v1/rooms/${code}`);
    back.hello("P0", { features: FEATURES, rejoinToken: token });
    expect((await back.json("welcome")).memberId).toBe(ids[0]);
    const after = await routes(back);
    expect([after.hub, linkOf(after, ids[0])]).toEqual([ids[1], undefined]);  // a member, linking to the new hub
  });

  it("a hub that can't bridge its racers on the SFU (no SFU session) gives way to one that can", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code, ids } = await p2pRoom(3, true);
      const [hub, a, f] = cs;
      await connect(a);
      await connect(f);   // the hub never connects to the SFU (a browser's game)
      link(hub, ids[0], ids[1], true);
      link(f, ids[0], ids[2], false);
      await routes(a, (r) => linkOf(r, ids[2]) === "sfu");
      await race(cs, code);
      // Unbridged since the race's first alarm: not before its grace.
      await runDurableObjectAlarm(room(code));
      expect(await runInDurableObject(room(code), async (r: any) => r.state.hub)).toBe(ids[0]);
      await expire(code, (st) => { st.unbridgedSince = Date.now() - 60_000; });
      expect((await routes(f, (r) => r.hub !== ids[0])).hub).toBe(ids[1]);
    } finally {
      fake.restore();
    }
  });

  it("a hub whose SFU session is being renewed mid-race keeps the hub (review 2)", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code, ids } = await p2pRoom(3, true);
      for (const c of cs) await connect(c);
      const [hub, a, f] = cs;
      link(hub, ids[0], ids[1], true);
      link(f, ids[0], ids[2], false);
      await routes(a, (r) => linkOf(r, ids[2]) === "sfu");
      await race(cs, code);
      await expire(code, (st) => { st.hubChangedAt = Date.now() - 120_000; });
      // A new session for the hub (a lost association, a TURN renewal): not ready for a moment.
      hub.sendJson({ type: "sfu_offer", sdp: "offer" });
      await hub.json("sfu_answer");
      await runDurableObjectAlarm(room(code));
      expect(await runInDurableObject(room(code), async (r: any) => r.state.hub)).toBe(ids[0]);
      hub.sendJson({ type: "sfu_connected" });
      await hub.json("sfu_channels");
      await runDurableObjectAlarm(room(code));
      expect(await runInDurableObject(room(code), async (r: any) => [r.state.hub, r.state.unbridgedSince])).toEqual([ids[0], undefined]);
    } finally {
      fake.restore();
    }
  });

  it("one fallback racer that isn't ready holds back no other's release (review 3)", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code, ids } = await p2pRoom(4, true);
      const [hub, a, f1, f2] = cs;
      for (const c of [hub, a, f1]) await connect(c);   // F2's SFU never comes up
      link(hub, ids[0], ids[1], true);
      link(f1, ids[0], ids[2], false);
      link(f2, ids[0], ids[3], false);
      await routes(a, (r) => linkOf(r, ids[2]) === "sfu" && linkOf(r, ids[3]) === "sfu");
      const starts = await race(cs, code);
      await passDone(code);
      for (const c of [hub, f1]) c.sendJson({ type: "sfu_ready", race: starts[0].startAt });
      for (const c of [hub, f1]) expect((await c.json("sfu_ready")).race).toBe(starts[0].startAt);
      expect(f2.seen.some((t) => JSON.parse(t).type === "sfu_ready")).toBe(false);
    } finally {
      fake.restore();
    }
  });

  it("a hub that can't bridge moves rather than its fallback racers leaving at their deadline (review 3)", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code, ids } = await p2pRoom(3, true);
      const [hub, a, f] = cs;
      await connect(a);
      await connect(f);   // the hub has no SFU session (a browser's game)
      link(hub, ids[0], ids[1], true);
      link(f, ids[0], ids[2], false);
      await routes(a, (r) => linkOf(r, ids[2]) === "sfu");
      await race(cs, code);
      // F's setup deadlines have passed, the hub's grace not yet: F waits for the next hub.
      await setEnv(code, { SFU_CONNECT_MS: "1", SFU_SETUP_MS: "1" });
      await expire(code, (st) => { st.unbridgedSince = Date.now(); });
      expect(await runInDurableObject(room(code), async (r: any) => r.state.race.left)).toEqual([]);
      await expire(code, (st) => { st.unbridgedSince = Date.now() - 60_000; });
      expect((await routes(f, (r) => r.hub !== ids[0])).hub).toBe(ids[1]);
      expect(f.seen.some((t) => JSON.parse(t).type === "left")).toBe(false);
    } finally {
      fake.restore();
    }
  });

  it("a fallback racer released through one hub has its deadlines again after a hub change (review 3)", async () => {
    const fake = fakeRealtime();
    try {
      const { cs, code, ids } = await p2pRoom(3, true);
      const [hub, f, x] = cs;
      await connect(hub);
      await connect(f);   // X has no SFU session
      link(hub, ids[0], ids[1], true);
      link(hub, ids[0], ids[2], true);
      link(f, ids[0], ids[1], false);   // F's link died: not the next hub for a while
      await routes(x, (r) => linkOf(r, ids[1]) === "sfu" && linkOf(r, ids[2]) === "star");
      const starts = await race(cs, code);
      await passDone(code);
      for (const c of [hub, f]) c.sendJson({ type: "sfu_ready", race: starts[0].startAt });
      await f.json("sfu_ready");
      // The hub leaves; X (on the star, no SFU) is the next; F can't reach it either.
      hub.sendJson({ type: "leave" });
      expect((await routes(f, (r) => r.hub === ids[2])).hub).toBe(ids[2]);
      link(f, ids[2], ids[1], false);
      await routes(x, (r) => linkOf(r, ids[1]) === "sfu");
      // Nothing can bridge F now: at its deadline it leaves the race (it no longer races unseen).
      await setEnv(code, { SFU_CONNECT_MS: "1", SFU_SETUP_MS: "1" });
      await runDurableObjectAlarm(room(code));
      expect((await x.json("left")).memberId).toBe(ids[1]);
    } finally {
      fake.restore();
    }
  });

  it("a member back by its token keeps the hub and its link, and hears the routes again", async () => {
    const { cs, code, ids } = await p2pRoom(2);
    const [hub, a] = cs;
    link(hub, ids[0], ids[1], true);
    await routes(a, (r) => linkOf(r, ids[1]) === "star");
    const token = a.welcome.rejoinToken;
    // The room sees A's connection end without a close frame (1006): a dropped network.
    await runInDurableObject(room(code), async (r: any) => { await r.webSocketClose(r.sockets.get(ids[1]), 1006, "", false); });
    const back = await Client.connect(`/v1/rooms/${code}`);
    back.hello("P1", { features: FEATURES, rejoinToken: token });
    expect((await back.json("welcome")).memberId).toBe(ids[1]);
    const r = await routes(back);
    expect([r.hub, linkOf(r, ids[1])]).toEqual([ids[0], "star"]);
  });
});
