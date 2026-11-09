// A racer whose race link delivers nothing (finding 187): its report (sfu_silent) gets it a new
// session in its race, or takes it out of the race when that fails; the race's start moment goes to
// the racers over the room as well (race_go); a session on which the room closed subscriptions its
// game never had (their stream ids stay dead on Cloudflare's side) gets no new subscription and is
// replaced once that costs no race anything.
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { describe, it, expect } from "vitest";
import { Client, startedRace } from "./helpers";
import { fakeRealtime, publishedSession } from "./sfu-fake";

const SFU_OUT = "Realtime connection failed: out of the race.";
const room = (code: string) => env.ROOM.get(env.ROOM.idFromName(code));
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const types = (c: Client) => c.seen.map((f) => JSON.parse(f).type);
const outErrors = (c: Client) => c.seen.filter((f) => f.includes(SFU_OUT));
const newCalls = (fake: ReturnType<typeof fakeRealtime>, session: string) =>
  fake.calls.filter((c) => c.path.endsWith(`/sessions/${session}/datachannels/new`) && c.body.dataChannels[0].location === "remote");

/** Waits for the room's subscription pass and session changes to settle. */
async function settled(code: string) {
  await runInDurableObject(room(code), async (r: any) => {
    for (let i = 0; i < 400 && (r.subscribing || [...r.sessionTail.keys()].length); i++) await sleep(10);
    await r.sfuQueue;
  });
}

/** A started race of `n` whose racers are published, subscribed both ways and released. */
async function releasedRace(n: number, extraEnv: Record<string, string> = {}) {
  const { clients, code, starts } = await startedRace(n);
  const startAt = starts[0].startAt;
  await runInDurableObject(room(code), async (r: any) => {
    r.env = { ...r.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token", ...extraEnv };
    const ids = r.state.members.map((m: any) => m.id);
    for (const m of r.state.members) m.sfu = publishedSession(m.id, ids.filter((id: number) => id !== m.id));
  });
  for (const c of clients) c.sendJson({ type: "sfu_ready", race: startAt });
  for (const c of clients) expect((await c.json("sfu_ready")).race).toBe(startAt);
  return { clients, code, startAt };
}

describe("a race link that delivers nothing (finding 187)", () => {
  it("the report gets the racer a new session in its race; it is back once subscribed both ways", async () => {
    const fake = fakeRealtime();
    try {
      const { clients, code, startAt } = await releasedRace(3);
      for (const c of clients) c.drain();
      // Before its release or for another race: nothing happens.
      clients[1].sendJson({ type: "sfu_silent", race: startAt - 1 });
      clients[1].sendJson({ type: "sfu_silent", race: startAt });
      await clients[1].json("sfu_reset");
      // The others lose their subscriptions to its old session.
      for (const k of [0, 2]) expect((await clients[k].json("sfu_closed")).channelIds).toHaveLength(2);
      const renewing = await runInDurableObject(room(code), async (r: any) => {
        await r.sfuQueue;
        const m = r.state.members[1];
        return { renewedRace: m.renewedRace, renewBy: m.renewBy, sfu: m.sfu, left: r.state.race.left };
      });
      expect(renewing.renewedRace).toBe(startAt);
      expect(renewing.renewBy).toBeGreaterThan(Date.now());
      expect(renewing.sfu).toBeUndefined();
      expect(renewing.left).toEqual([]);

      // Its game offers a new session (as after any sfu_reset) and connects.
      clients[1].sendJson({ type: "sfu_offer", sdp: "offer" });
      await clients[1].json("sfu_answer");
      clients[1].sendJson({ type: "sfu_connected" });
      const own = await clients[1].json("sfu_channels");
      expect(own.channels.every((x: any) => x.local)).toBe(true);
      await settled(code);
      const theirs = clients[1].seen.map((f) => JSON.parse(f)).filter((m) => m.type === "sfu_channels" && m.channels.some((x: any) => !x.local));
      expect(new Set(theirs.flatMap((m) => m.channels.map((x: any) => x.memberId)))).toEqual(new Set([0, 2]));
      for (const k of [0, 2]) {
        const got = await clients[k].next((f) => typeof f === "string" && JSON.parse(f).type === "sfu_channels");
        expect(JSON.parse(got as string).channels.map((x: any) => x.memberId)).toEqual([1, 1]);
      }
      // Its acknowledgement releases it again (the others raced on meanwhile).
      clients[1].sendJson({ type: "sfu_ready", race: startAt });
      expect((await clients[1].json("sfu_ready")).race).toBe(startAt);
      const after = await runInDurableObject(room(code), async (r: any) => ({ renewBy: r.state.members[1].renewBy, left: r.state.race.left }));
      expect(after).toEqual({ renewBy: undefined, left: [] });
      for (const c of clients) expect(outErrors(c)).toEqual([]);
      expect(types(clients[0]).filter((t) => t === "sfu_ready")).toHaveLength(1);  // its first release only
    } finally {
      fake.restore();
    }
  });

  it("a racer whose new session isn't up in time is out of the race, and so is one still silent on its new session", async () => {
    const fake = fakeRealtime();
    try {
      const { clients, code, startAt } = await releasedRace(3, { SFU_RENEW_MS: "200" });
      clients[1].sendJson({ type: "sfu_silent", race: startAt });
      await clients[1].json("sfu_reset");
      // It never offers: at the deadline the alarm takes it out; the others go on.
      await sleep(250);
      await runDurableObjectAlarm(room(code));
      expect((await clients[1].json("error")).message).toBe(SFU_OUT);
      for (const k of [0, 2]) expect(await clients[k].json("left")).toMatchObject({ slot: 1, reason: "left" });
      const state = await runInDurableObject(room(code), async (r: any) => ({ left: r.state.race.left, renewBy: r.state.members[1].renewBy }));
      expect(state).toEqual({ left: [1], renewBy: undefined });

      // Member 2's session was replaced in this race already: a second report takes it out at once.
      await runInDurableObject(room(code), async (r: any) => { r.state.members[2].renewedRace = startAt; });
      clients[2].sendJson({ type: "sfu_silent", race: startAt });
      expect((await clients[2].json("error")).message).toBe(SFU_OUT);
      expect(await clients[0].json("left")).toMatchObject({ slot: 2 });
      expect(outErrors(clients[0])).toEqual([]);
    } finally {
      fake.restore();
    }
  });
});

describe("the race's start moment over the room (finding 187)", () => {
  it("goes once, from the racer whose game names it, to the racers that take it from the room", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("Host", { features: ["race_rejoin", "race_go"] });
    const welcome = await host.json("welcome");
    expect(welcome.features).toEqual(["sfu_silent", "race_go"]);
    const code = welcome.code;
    const newer = await Client.connect(`/v1/rooms/${code}`);
    newer.hello("New", { features: ["race_rejoin", "race_go"] });
    await newer.json("welcome");
    const older = await Client.join(code, "Old");  // 0.7.13: no race_go
    for (const c of [host, newer, older]) c.sendJson({ type: "ready", ready: true });
    await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members.every((m: any) => m.ready) && JSON.parse(f).members.length === 3);
    host.sendJson({ type: "start" });
    const starts = await Promise.all([host, newer, older].map((c) => c.json("start")));
    const startAt = starts[0].startAt;
    expect(starts[0].yourSlot).toBe(0);

    // Not the namer (slot 1), a moment before the start, another race: ignored.
    newer.sendJson({ type: "race_go", race: startAt, at: startAt + 4000 });
    host.sendJson({ type: "race_go", race: startAt, at: startAt - 1 });
    host.sendJson({ type: "race_go", race: startAt + 1, at: startAt + 4000 });
    host.sendJson({ type: "race_go", race: startAt, at: startAt + 5000 });
    for (const c of [host, newer]) expect(await c.json("race_go")).toEqual({ type: "race_go", race: startAt, at: startAt + 5000 });
    // A second one changes nothing.
    host.sendJson({ type: "race_go", race: startAt, at: startAt + 9000 });
    host.sendJson({ type: "ping", t: 1 });
    await host.json("pong");
    expect(host.seen.filter((f) => JSON.parse(f).type === "race_go")).toHaveLength(1);
    expect(older.seen.some((f) => JSON.parse(f).type === "race_go")).toBe(false);
    expect(await runInDurableObject(room(code), async (r: any) => r.state.race.goAt)).toBe(startAt + 5000);
  });

  it("names the lowest slot still racing once the host left, and comes again in a rejoin's catch-up", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("Host", { features: ["race_rejoin", "race_go"] });
    const code = (await host.json("welcome")).code;
    const b = await Client.connect(`/v1/rooms/${code}`);
    b.hello("B", { features: ["race_rejoin", "race_go"] });
    const token = (await b.json("welcome")).rejoinToken;
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello("C", { features: ["race_rejoin", "race_go"] });
    await c.json("welcome");
    for (const x of [host, b, c]) x.sendJson({ type: "ready", ready: true });
    await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members.every((m: any) => m.ready) && JSON.parse(f).members.length === 3);
    host.sendJson({ type: "start" });
    const startAt = (await b.json("start")).startAt;
    await c.json("start");
    host.sendJson({ type: "back" });  // the host leaves the race: slot 1 (B) names it now
    await b.json("left");
    c.sendJson({ type: "race_go", race: startAt, at: startAt + 3000 });
    b.sendJson({ type: "race_go", race: startAt, at: startAt + 4000 });
    expect((await c.json("race_go")).at).toBe(startAt + 4000);
    // B's connection drops (no close frame) and comes back: its catch-up has the moment.
    await runInDurableObject(room(code), async (r: any) => { await r.webSocketClose(r.sockets.get(1), 1006, "", false); });
    const back = await Client.connect(`/v1/rooms/${code}`);
    back.hello("B", { features: ["race_rejoin", "race_go"], rejoinToken: token, raceStartAt: startAt });
    await back.json("welcome");
    expect((await back.json("race_go")).at).toBe(startAt + 4000);
  });
});

describe("sessions whose stream ids are dead (finding 187)", () => {
  it("a member outside the race whose inspection closes subscriptions its game never had gets a new session", async () => {
    const fake = fakeRealtime();
    try {
      const { code } = await releasedRace(2);
      const late = await Client.join(code, "Late");  // sits the race out
      await runInDurableObject(room(code), async (r: any) => {
        const m = r.state.members[2];
        m.sfu = { ...publishedSession(m.id, []), unsure: Date.now() - 1 };  // an aborted call's inspection is due
      });
      // What the aborted call made: subscriptions to both racers, never on the record.
      fake.sessions.set("session-2", [
        { id: 500, dataChannelName: "state", location: "remote", sessionId: "session-0", status: "active" },
        { id: 501, dataChannelName: "events", location: "remote", sessionId: "session-0", status: "active" },
        { id: 502, dataChannelName: "state", location: "remote", sessionId: "session-1", status: "active" },
        { id: 503, dataChannelName: "events", location: "remote", sessionId: "session-1", status: "active" }]);
      await runDurableObjectAlarm(room(code));
      await settled(code);
      const closes = fake.calls.filter((c) => c.path.endsWith("/sessions/session-2/datachannels/close"));
      expect(closes[0].body.dataChannels.map((x: any) => x.id)).toEqual([500, 501, 502, 503]);
      await late.json("sfu_reset");
      expect(late.seen.some((f) => /"sfu_(channels|closed)"/.test(f) && f.includes("500"))).toBe(false);
      expect(newCalls(fake, "session-2")).toEqual([]);
    } finally {
      fake.restore();
    }
  });

  it("a racer keeps its poisoned session while its pairs work, and gets a new one once its race is over", async () => {
    const fake = fakeRealtime();
    try {
      const { clients, code } = await releasedRace(3);
      await runInDurableObject(room(code), async (r: any) => { r.state.members[1].sfu.unsure = Date.now() - 1; });
      // A second pair to member 2, made late by an aborted call: closed, never announced.
      fake.sessions.set("session-1", [
        { id: 900, dataChannelName: "state", location: "remote", sessionId: "session-2", status: "active" },
        { id: 901, dataChannelName: "events", location: "remote", sessionId: "session-2", status: "active" }]);
      await runDurableObjectAlarm(room(code));
      await settled(code);
      expect(fake.calls.filter((c) => c.path.endsWith("/sessions/session-1/datachannels/close"))
        .map((c) => c.body.dataChannels.map((x: any) => x.id))).toEqual([[900, 901]]);
      const during = await runInDurableObject(room(code), async (r: any) => r.state.members[1].sfu);
      expect(during.poisoned).toBe(true);
      expect(during.phase).toBe("ready");
      expect(types(clients[1])).not.toContain("sfu_reset");
      // The race ends: now it is replaced.
      clients[0].sendJson({ type: "end" });
      await clients[1].json("ended");
      await runDurableObjectAlarm(room(code));
      await clients[1].json("sfu_reset");
      for (const c of clients) expect(outErrors(c)).toEqual([]);
    } finally {
      fake.restore();
    }
  });

  it("a racer whose poisoned session needs a new pair gets no call on it but a new session in its race", async () => {
    const fake = fakeRealtime();
    try {
      const { clients, code, starts } = await startedRace(3);
      const startAt = starts[0].startAt;
      await runInDurableObject(room(code), async (r: any) => {
        r.env = { ...r.env, REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
        const [a, b, c] = r.state.members;
        a.sfu = publishedSession(a.id, [b.id]);
        b.sfu = { ...publishedSession(b.id, [a.id]), poisoned: true };
        c.sfu = { id: `session-${c.id}`, phase: "connect", channels: [] };
      });
      clients[2].sendJson({ type: "sfu_connected" });
      await clients[1].json("sfu_reset");
      await settled(code);
      expect(newCalls(fake, "session-1")).toEqual([]);
      expect(newCalls(fake, "session-0")).toHaveLength(1);
      const m = await runInDurableObject(room(code), async (r: any) => r.state.members[1]);
      expect(m.renewedRace).toBe(startAt);
      expect(m.renewBy).toBeGreaterThan(Date.now());
      expect(outErrors(clients[1])).toEqual([]);
    } finally {
      fake.restore();
    }
  });
});
