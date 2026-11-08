// Code updates at a safe moment (src/rollout.ts, finding 184).
import { env, runInDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { afterEach, describe, expect, it } from "vitest";
import { CLIENT_VERSION, Client, ISO, VERSION } from "./helpers";
import { PROTOCOL } from "../src/protocol";
import {
  NEXT_RACE_MARGIN_MS, RESTART_CLOSE, RETRY_MS, UPDATE_NOTICE, WORKER_VERSION_HEADER, newerVersion, newest, nextRecord,
  restartTarget, rolloutOf, safeMoment, versionHeader, versionOf, type RoomMoment,
} from "../src/rollout";
import { resetGuardCaches } from "../src/guard";

const A = { id: "aaaaaaaa-0000-0000-0000-000000000001", timestamp: "2026-11-08T10:00:00.000Z" };
const B = { id: "bbbbbbbb-0000-0000-0000-000000000002", timestamp: "2026-11-08T12:00:00.000Z" };
const lobby: RoomMoment = { race: false, grid: false, countdown: false, setup: false, hello: false, nextRaceInMs: null, members: 3 };

describe("the safe moment", () => {
  it("is the lobby or the results between races, never a race, a grid, a countdown or setup work", () => {
    expect(safeMoment(lobby)).toBe(true);
    // the results: the race closed, the automatic next one 30 s away
    expect(safeMoment({ ...lobby, nextRaceInMs: 30_000 })).toBe(true);
    for (const busy of [{ race: true }, { grid: true }, { countdown: true }, { setup: true }, { hello: true }])
      expect(safeMoment({ ...lobby, ...busy })).toBe(false);
    // members must be back before the next race's grid locks
    expect(safeMoment({ ...lobby, nextRaceInMs: NEXT_RACE_MARGIN_MS })).toBe(false);
    expect(safeMoment({ ...lobby, nextRaceInMs: -5 })).toBe(false);
    // an empty room adopts on its next start
    expect(safeMoment({ ...lobby, members: 0 })).toBe(false);
  });
});

describe("versions", () => {
  it("reads the binding, the header and nonsense", () => {
    expect(versionOf(A)).toEqual(A);
    expect(versionOf(versionHeader(B))).toEqual(B);
    expect(versionOf({ id: "x" })).toEqual({ id: "x", timestamp: "" });
    for (const bad of [null, undefined, 3, "", "  ", { id: 4 }, { id: "x".repeat(65) }]) expect(versionOf(bad)).toBeNull();
  });
  it("is newer by upload time only, never when unreadable or the same id", () => {
    expect(newerVersion(B, A)).toBe(true);
    expect(newerVersion(A, B)).toBe(false);
    expect(newerVersion({ ...B, id: A.id }, A)).toBe(false);
    expect(newerVersion({ id: B.id, timestamp: "" }, A)).toBe(false);
    expect(newerVersion(B, { id: A.id, timestamp: "?" })).toBe(false);
    expect(newerVersion(null, A)).toBe(false);
    expect(newest(A, B)).toEqual(B);
    expect(newest(B, A)).toEqual(B);
    expect(newest(null, A)).toEqual(A);
  });
  it("the rollout row: an id, empty during a gradual deployment, else none", () => {
    expect(rolloutOf(` ${B.id} `)).toBe(B.id);
    expect(rolloutOf("")).toBe("");
    expect(rolloutOf("DROP TABLE")).toBeUndefined();
    expect(rolloutOf(null)).toBeUndefined();
  });
});

describe("the restart target", () => {
  const now = 1_000_000_000;
  it("follows the rollout row when there is one", () => {
    expect(restartTarget(A, B.id, null, null, now)).toBe(B.id);
    expect(restartTarget(A, A.id, B, null, now)).toBeNull();
    // a gradual deployment: nobody moves, whatever the Worker says
    expect(restartTarget(A, "", B, null, now)).toBeNull();
    // a rollback: a room on the newer version goes back
    expect(restartTarget(B, A.id, null, null, now)).toBe(A.id);
  });
  it("without a row, a newer Worker version", () => {
    expect(restartTarget(A, undefined, B, null, now)).toBe(B.id);
    expect(restartTarget(B, undefined, A, null, now)).toBeNull();
    expect(restartTarget(A, undefined, A, null, now)).toBeNull();
    expect(restartTarget(A, undefined, null, null, now)).toBeNull();
    expect(restartTarget(null, B.id, B, null, now)).toBeNull();
  });
  it("a restart that left the room on its version waits, then is given up", () => {
    let last = nextRecord(A, B.id, true, null, now, 4);
    expect(last).toEqual({ target: B.id, from: A.id, at: now, tries: 1, members: 4 });
    expect(restartTarget(A, B.id, null, last, now + 1000)).toBeNull();
    expect(restartTarget(A, B.id, null, last, now + RETRY_MS[0])).toBe(B.id);
    for (let i = 1; i <= RETRY_MS.length; i++) last = nextRecord(A, B.id, true, last, now, 4);
    expect(last.tries).toBe(RETRY_MS.length + 1);
    expect(restartTarget(A, B.id, null, last, now + 365 * 86_400_000)).toBeNull();
    // a different target, or a room that did move, starts over
    expect(restartTarget(A, "cccccccc-0000", null, last, now)).toBe("cccccccc-0000");
    expect(restartTarget(B, A.id, null, last, now)).toBe(A.id);
    expect(nextRecord(B, A.id, true, last, now, 4).tries).toBe(1);
  });
});

describe("the notice", () => {
  it("is a new message type with a footer-sized line", () => {
    expect(UPDATE_NOTICE.type).toBe("notice");
    expect(UPDATE_NOTICE.code).toBe("server_update");
    // the game's footer holds about 45 characters (src/guard.ts OFF_MESSAGE)
    expect(UPDATE_NOTICE.message.length).toBeLessThanOrEqual(45);
    // the game's JSON reader keeps a type of up to 23 characters (port/net/client.c)
    expect(UPDATE_NOTICE.type.length).toBeLessThan(24);
    // a close code every release rejoins after (below the 4000-4099 refusals)
    expect(RESTART_CLOSE).toBeLessThan(4000);
  });
});

// ---- The room ----

const stub = (code: string) => env.ROOM.get(env.ROOM.idFromName(code));
const snap = async (c: Client, pred: (r: any) => boolean = () => true) =>
  JSON.parse(await c.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && pred(JSON.parse(f))) as string);
const features = { features: ["session", "countdown", "race_rejoin"] };

async function setRollout(value: string | null): Promise<void> {
  if (value === null) await env.MODERATION.prepare("DELETE FROM settings WHERE key = 'rollout'").run();
  else await env.MODERATION.prepare("INSERT INTO settings (key, value, updated_at) VALUES ('rollout', ?1, ?2) " +
    "ON CONFLICT (key) DO UPDATE SET value = ?1, updated_at = ?2").bind(value, Date.now()).run();
  resetGuardCaches();
}
afterEach(() => setRollout(null));

async function room(n: number) {
  const h = await Client.connect("/v1/rooms/new");
  h.hello("Host", features);
  const { code } = await h.json("welcome");
  const cs = [h];
  for (let i = 1; i < n; i++) {
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello(`P${i}`, features);
    await c.json("welcome");
    cs.push(c);
  }
  await snap(h, (r) => r.members.length === n);
  return { cs, code };
}

/** What the game does after a 1012: its rejoin hello with the welcome's token. */
async function rejoin(code: string, c: Client, name: string): Promise<Client> {
  const back = await Client.connect(`/v1/rooms/${code}`);
  back.hello(name, { ...features, rejoinToken: c.welcome.rejoinToken });
  await back.json("welcome");
  return back;
}

/** The room's stored record of its last restart, its own version and whether it is restarting. */
async function look(code: string): Promise<{ update: any; own: string; restarting: boolean }> {
  return runInDurableObject(stub(code), async (r: any, ctx) => ({
    update: (await ctx.storage.get("update")) ?? null, own: r.ownVersion()?.id ?? "", restarting: r.restarting,
  }));
}

describe("rooms restart for new code only at a safe moment", () => {
  it("in the lobby: notice, 1012, everyone back by token with their seat, host and Ready", async () => {
    const { cs, code } = await room(3);
    cs[1].sendJson({ type: "ready", ready: true });
    const before = await snap(cs[0], (r) => r.members[1].ready);
    expect((await look(code)).own).not.toBe("");
    // P2 holds a Realtime session (finding 177): stored with the room, so it outlives the restart.
    await runInDurableObject(stub(code), async (r: any, ctx) => {
      r.state.members[2].sfu = { id: "session-p2", phase: "ready", channels: [] };
      await ctx.storage.put("room", r.state);
    });
    await setRollout("ffffffff-0000-0000-0000-00000000000f");
    await runDurableObjectAlarm(stub(code)).catch(() => {});
    for (const c of cs) {
      expect(await c.json("notice")).toEqual(UPDATE_NOTICE);
      expect(await c.closed()).toBe(RESTART_CLOSE);
    }
    const back = [await rejoin(code, cs[0], "Host"), await rejoin(code, cs[1], "P1"), await rejoin(code, cs[2], "P2")];
    expect(back.map((c) => c.welcome.memberId)).toEqual(before.members.map((m: any) => m.id));
    expect(back[0].welcome.host).toBe(true);
    expect(back.map((c) => c.welcome.sfu)).toEqual(["setup", "setup", "ready"]);
    const after = await snap(back[0], (r) => r.members.length === 3);
    expect(after.members.map((m: any) => [m.id, m.name, m.ready])).toEqual(before.members.map((m: any) => [m.id, m.name, m.ready]));
    const { update } = await look(code);
    expect(update).toMatchObject({ target: "ffffffff-0000-0000-0000-00000000000f", tries: 1, members: 3 });
    // The test runtime brings it back on the same code: the next alarm waits (RETRY_MS) instead
    // of restarting again.
    await runDurableObjectAlarm(stub(code));
    back[0].sendJson({ type: "ping", t: 1 });
    await back[0].json("pong");
    expect(back[0].closeCode).toBeNull();
  });

  it("never in a race, a locked grid or a countdown; at the results after the race", async () => {
    const { cs, code } = await room(2);
    for (const c of cs) c.sendJson({ type: "ready", ready: true });
    await snap(cs[0], (r) => r.phase === "starting");
    await setRollout("ffffffff-0000-0000-0000-00000000000f");
    // the countdown with its locked grid
    await runDurableObjectAlarm(stub(code));
    expect((await look(code)).update).toBeNull();
    await runInDurableObject(stub(code), async (r: any, ctx) => { r.state.countdownEndsAt = Date.now() - 1; await ctx.storage.put("room", r.state); });
    await runDurableObjectAlarm(stub(code));
    await Promise.all(cs.map((c) => c.json("start")));
    // the race
    await runDurableObjectAlarm(stub(code));
    await runDurableObjectAlarm(stub(code));
    expect((await look(code)).update).toBeNull();
    expect(cs.every((c) => c.closeCode === null)).toBe(true);
    // both finish: the results
    cs[0].sendJson({ type: "finish", ticks: 600, place: 1 });
    cs[1].sendJson({ type: "finish", ticks: 700, place: 2 });
    await snap(cs[0], (r) => r.lastRace && r.phase !== "racing");
    await runDurableObjectAlarm(stub(code)).catch(() => {});
    expect(await cs[0].json("notice")).toEqual(UPDATE_NOTICE);
    expect(await cs[0].closed()).toBe(RESTART_CLOSE);
    const back = await rejoin(code, cs[0], "Host");
    const r = await snap(back);
    expect(r.lastRace.entrants.map((x: any) => x.ticks)).toEqual([600, 700]);
  });

  it("not while Realtime setup work or a hello is in flight, nor just before an automatic race", async () => {
    const { cs, code } = await room(2);
    await setRollout("ffffffff-0000-0000-0000-00000000000f");
    await runInDurableObject(stub(code), (r: any) => { r.sfuCalls = 1; });
    await runDurableObjectAlarm(stub(code));
    await runInDurableObject(stub(code), (r: any) => { r.sfuCalls = 0; r.subscribing = true; });
    await runDurableObjectAlarm(stub(code));
    await runInDurableObject(stub(code), async (r: any, ctx) => {
      r.subscribing = false; r.state.nextRaceAt = Date.now() + 5_000; await ctx.storage.put("room", r.state);
    });
    await runDurableObjectAlarm(stub(code));
    const pending = await Client.connect(`/v1/rooms/${code}`);  // no hello yet
    await runInDurableObject(stub(code), async (r: any, ctx) => { r.state.nextRaceAt = null; await ctx.storage.put("room", r.state); });
    await runDurableObjectAlarm(stub(code));
    expect((await look(code)).update).toBeNull();
    expect(cs.every((c) => c.closeCode === null)).toBe(true);
    pending.ws.close();
  });

  it("a gradual deployment (an empty rollout row) moves nobody, even with a newer Worker heard", async () => {
    const { cs, code } = await room(2);
    await setRollout("");
    await newerWorkerKnocks(code);
    await runDurableObjectAlarm(stub(code));
    expect((await look(code)).update).toBeNull();
    expect(cs[0].closeCode).toBeNull();
  });

  it("without a rollout row, a newer Worker's version on a request restarts it; an older one doesn't", async () => {
    const { cs, code } = await room(2);
    await newerWorkerKnocks(code, "2000-01-01T00:00:00.000Z");
    await runDurableObjectAlarm(stub(code));
    expect((await look(code)).update).toBeNull();
    await newerWorkerKnocks(code);
    await runDurableObjectAlarm(stub(code)).catch(() => {});
    expect(await cs[0].json("notice")).toEqual(UPDATE_NOTICE);
    expect((await look(code)).update).toMatchObject({ target: "", tries: 1 });
  });
});

/** A request from a Worker on another version: straight to the room, with the header the Worker
    sets (the Worker drops a client's own). The socket says nothing and is closed. */
async function newerWorkerKnocks(code: string, timestamp = "2999-01-01T00:00:00.000Z"): Promise<void> {
  const res = await stub(code).fetch("https://dhd.test/v1/rooms/" + code, {
    headers: { Upgrade: "websocket", [WORKER_VERSION_HEADER]: versionHeader({ id: "eeeeeeee-0000", timestamp }) },
  });
  res.webSocket!.accept();
  res.webSocket!.close(1000);
  await new Promise((r) => setTimeout(r, 20));
}

describe("old and new code side by side (a rollout runs both)", () => {
  it("the Worker drops a client's version header and sends its own", async () => {
    const { cs, code } = await room(1);
    const res = await (await import("cloudflare:workers")).exports.default.fetch(`https://dhd.test/v1/rooms/${code}`, {
      headers: { Upgrade: "websocket", [WORKER_VERSION_HEADER]: versionHeader({ id: "evil", timestamp: "2999-01-01T00:00:00Z" }) },
    });
    res.webSocket!.accept();
    const heard = await runInDurableObject(stub(code), (r: any) => r.heardVersion);
    expect(heard?.id).not.toBe("evil");
    res.webSocket!.close(1000);
    cs[0].ws.close(1000);
  });

  it("a room stored by older code (no restart record) loads, and a request without the header is served", async () => {
    const { cs, code } = await room(2);
    await runInDurableObject(stub(code), async (_r: any, ctx) => { await ctx.storage.delete("update"); });
    const res = await stub(code).fetch("https://dhd.test/v1/rooms/" + code, { headers: { Upgrade: "websocket" } });
    expect(res.status).toBe(101);
    const c = new Client(res.webSocket!);
    res.webSocket!.accept();
    c.sendJson({ type: "hello", protocol: PROTOCOL, version: VERSION, clientVersion: CLIENT_VERSION, isoSha256: ISO, name: "Old", ...features });
    expect((await c.json("welcome")).memberId).toBe(2);
    expect(cs[0].closeCode).toBeNull();
  });

  it("the stored room keeps the shape older code reads; the restart record has its own key", async () => {
    const { code } = await room(2);
    await setRollout("ffffffff-0000-0000-0000-00000000000f");
    await runDurableObjectAlarm(stub(code)).catch(() => {});
    const stored = await runInDurableObject(stub(code), async (_r, ctx) => ({
      keys: [...(await ctx.storage.list()).keys()].sort(), room: await ctx.storage.get<any>("room"),
    }));
    expect(stored.keys).toEqual(["room", "update"]);
    // Every field is one 0.7.13's server (main 0574b05) already stores: an older room reading this
    // record, or this code reading one of 0.7.13's, sees nothing new.
    const known = new Set(["session", "rules", "nextExplicit", "rotation", "trackLimits", "pickOnly", "grid", "gridRules",
      "nextRaceAt", "waitingForRider", "lastEvent", "raceId", "lastRace", "code", "createdAt", "hostId", "nextMemberId",
      "version", "isoSha256", "roomName", "public", "maxPlayers", "track", "laps", "seed", "countdownEndsAt", "origin",
      "colo", "members", "race"]);
    expect(Object.keys(stored.room).filter((k) => !known.has(k))).toEqual([]);
    const memberKeys = new Set(["id", "name", "ready", "rider", "bike", "choosing", "countdown", "region", "awaySince",
      "token", "raceRejoin", "sfu", "sfuAttempt", "sfuOffers", "sfuConnected", "sfuReadyRace", "releasedRace",
      "turnAttempt", "turnUser", "turnUntil"]);
    expect(stored.room.members.flatMap((m: any) => Object.keys(m)).filter((k: string) => !memberKeys.has(k))).toEqual([]);
  });
});
