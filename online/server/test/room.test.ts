import { evictDurableObject, runDurableObjectAlarm } from "cloudflare:test";
import { env, exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import { Client, ISO, startedRace } from "./helpers";

describe("lobby", () => {
  it("creates a room with a join code and lets others join", async () => {
    const { host, code } = await Client.host("Ann");
    expect(code).toMatch(/^[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{6}$/);
    const bo = await Client.connect(`/v1/rooms/${code.toLowerCase()}`);
    bo.hello("Bo");
    const welcome = await bo.json("welcome");
    expect(welcome).toMatchObject({ code, host: false, protocol: 2 });
    const room = await host.next((f) => typeof f === "string" && JSON.parse(f).members?.length === 2);
    const r = JSON.parse(room as string);
    expect(r.members.map((m: any) => m.name)).toEqual(["Ann", "Bo"]);
    expect(r.hostId).toBe(0);
  });

  it("answers server pings and reports a round-trip", async () => {
    const { host } = await Client.host();
    const ping = await host.json("ping");
    host.sendJson({ type: "pong", s: ping.s });
    host.sendJson({ type: "ready", ready: true }); // any change re-sends the snapshot
    const room = await host.next((f) => typeof f === "string" && JSON.parse(f).members?.[0]?.rttMs !== null && JSON.parse(f).type === "room");
    expect(JSON.parse(room as string).members[0].rttMs).toBeGreaterThanOrEqual(0);
  });

  it("leaves the slow first ping out of the round-trip once later ones arrive", async () => {
    const { host, code } = await Client.host();
    const first = await host.json("ping");
    host.sendJson({ type: "pong", s: first.s - 5000 }); // the warm-up ping, reading 5 s
    host.sendJson({ type: "ready", ready: true });
    const early = await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members?.[0]?.rttMs !== null);
    expect(JSON.parse(early as string).members[0].rttMs).toBeGreaterThanOrEqual(5000);
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    const later = await host.json("ping");
    host.sendJson({ type: "pong", s: later.s });
    host.sendJson({ type: "ready", ready: false });
    const room = await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members?.[0]?.rttMs < 5000);
    expect(JSON.parse(room as string).members[0].rttMs).toBeLessThan(1000);
  });

  it("refuses a different version, ISO, protocol or unknown room", async () => {
    const { code } = await Client.host();
    const v = await Client.connect(`/v1/rooms/${code}`);
    v.hello("V", { version: "other build" });
    expect((await v.json("error")).code).toBe("version_mismatch");
    expect(await v.closed()).toBe(4002);

    const i = await Client.connect(`/v1/rooms/${code}`);
    i.hello("I", { isoSha256: "b".repeat(64) });
    expect((await i.json("error")).code).toBe("iso_mismatch");
    expect(await i.closed()).toBe(4003);

    const p = await Client.connect(`/v1/rooms/${code}`);
    p.hello("P", { protocol: 99 });
    expect(await p.closed()).toBe(4001);

    const n = await Client.connect(`/v1/rooms/ZZZZZZ`);
    n.hello("N");
    expect((await n.json("error")).code).toBe("room_not_found");
    expect(await n.closed()).toBe(4006);
  });

  it("refuses clients older than MIN_VERSION with a readable reason, hosts and joiners alike", async () => {
    // A 0.1.0 hello: no clientVersion, version is the exe hash. MIN_VERSION is 0.2.0 here.
    const old = await Client.connect("/v1/rooms/new");
    old.sendJson({ type: "hello", protocol: 2, version: "dhd-port 274f8c46f1af52d6", isoSha256: ISO, name: "Old" });
    const err = await old.json("error");
    expect(err).toMatchObject({
      code: "client_outdated",
      message: "A new version is out. Update to play online.",
      minVersion: "0.2.0",
      clientVersion: "0.1.0",
    });
    expect(await old.closed()).toBe(4009);

    const { code } = await Client.host();
    const older = await Client.connect(`/v1/rooms/${code}`);
    older.hello("Older", { clientVersion: "0.1.5" });
    expect((await older.json("error")).code).toBe("client_outdated");
    expect(await older.closed()).toBe(4009);

    const junk = await Client.connect(`/v1/rooms/${code}`);
    junk.hello("Junk", { clientVersion: "nightly" });
    expect(await junk.closed()).toBe(4009);

    const newer = await Client.connect(`/v1/rooms/${code}`);
    newer.hello("Newer", { clientVersion: "0.3.0" });
    expect((await newer.json("welcome")).code).toBe(code);
  });

  it("names the minimum version in health and the lobby list", async () => {
    const health = await (await exports.default.fetch("https://dhd.test/v1/health")).json<any>();
    expect(health).toMatchObject({ ok: true, protocol: 2, minVersion: "0.2.0" });
    const list = await (await exports.default.fetch("https://dhd.test/v1/lobbies")).json<any>();
    expect(list.minVersion).toBe("0.2.0");
  });

  it("refuses joins beyond maxPlayers", async () => {
    const { host, code } = await Client.host();
    host.sendJson({ type: "settings", maxPlayers: 2 });
    await Client.join(code, "Two");
    const three = await Client.connect(`/v1/rooms/${code}`);
    three.hello("Three");
    expect((await three.json("error")).code).toBe("room_full");
    expect(await three.closed()).toBe(4004);
  });

  it("keeps settings host-only and lets players pick their own rider", async () => {
    const { host, code } = await Client.host();
    const bo = await Client.join(code, "Bo");
    bo.sendJson({ type: "settings", track: 5 });
    expect((await bo.json("error")).code).toBe("not_host");
    bo.sendJson({ type: "pick", member: 0, rider: 3 });
    expect((await bo.json("error")).code).toBe("not_host");
    bo.sendJson({ type: "pick", rider: 7, bike: 2 });
    host.sendJson({ type: "settings", track: 5, laps: 3, seed: 1234 });
    host.sendJson({ type: "settings", track: 999 });
    expect((await host.json("error")).code).toBe("bad_value");
    const r = JSON.parse(
      (await host.next((f) => typeof f === "string" && JSON.parse(f).settings?.track === 5 && JSON.parse(f).members?.[1]?.rider === 7)) as string,
    );
    expect(r.settings).toEqual({ track: 5, laps: 3, seed: 1234 });
    expect(r.members[1]).toMatchObject({ rider: 7, bike: 2 });
  });

  it("starts only when everyone is ready, assigning slots host first", async () => {
    const { host, code } = await Client.host("H");
    const a = await Client.join(code, "A");
    host.sendJson({ type: "settings", track: 2, seed: 77 });
    host.sendJson({ type: "ready", ready: true });
    host.sendJson({ type: "start" });
    expect((await host.json("error")).code).toBe("not_ready");
    a.sendJson({ type: "ready", ready: true });
    await host.next((f) => typeof f === "string" && JSON.parse(f).members?.every((m: any) => m.ready));
    host.sendJson({ type: "start" });
    const [sh, sa] = await Promise.all([host.json("start"), a.json("start")]);
    expect(sh).toMatchObject({ yourSlot: 0, track: 2, seed: 77 });
    expect(sa.yourSlot).toBe(1);
    expect(sa.players.map((p: any) => p.name)).toEqual(["H", "A"]);
    expect(sa.inputDelay).toBeUndefined();
    expect(sa.startAt).toBeGreaterThan(sa.serverTime);

    const late = await Client.connect(`/v1/rooms/${code}`);
    late.hello("Late");
    expect((await late.json("error")).code).toBe("room_started");
  });

  it("hands the host role to the longest-present member", async () => {
    const { host, code } = await Client.host("H");
    const a = await Client.join(code, "A");
    await Client.join(code, "B");
    host.ws.close(1000);
    const r = JSON.parse(
      (await a.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && !JSON.parse(f).members.some((m: any) => m.name === "H"))) as string,
    );
    expect(r.members.map((m: any) => m.name)).toEqual(["A", "B"]);
    expect(r.hostId).toBe(1);
  });
});

describe("race room", () => {
  const room = (f: unknown) => typeof f === "string" && JSON.parse(f).type === "room";
  const snap = async (c: Client, pred: (r: any) => boolean) => JSON.parse((await c.next((f) => room(f) && pred(JSON.parse(f as string)))) as string);
  const countdown = { features: ["countdown"] };

  it("keeps Ready across a new track, computer riders and going public; cheats unready everyone", async () => {
    const { host, code } = await Client.host("H");
    const a = await Client.join(code, "A");
    host.sendJson({ type: "ready", ready: true });
    a.sendJson({ type: "ready", ready: true });
    await snap(host, (r) => r.members.length === 2 && r.members.every((m: any) => m.ready));
    host.sendJson({ type: "settings", public: true });
    expect((await snap(host, (r) => r.public)).members.every((m: any) => m.ready)).toBe(true);
    host.sendJson({ type: "settings", track: 3 });
    expect((await snap(host, (r) => r.settings.track === 3)).members.every((m: any) => m.ready)).toBe(true);
    host.sendJson({ type: "settings", laps: 1 });  // ROOM_NO_BOTS
    expect((await snap(host, (r) => r.settings.laps === 1)).members.every((m: any) => m.ready)).toBe(true);
    host.sendJson({ type: "settings", laps: 3 });  // ROOM_CHEATS on
    expect((await snap(host, (r) => r.settings.laps === 3)).members.some((m: any) => m.ready)).toBe(false);
  });

  it("lets two players ride the same rider, both ready (issue #17)", async () => {
    const { host, code } = await Client.host("H");
    const a = await Client.join(code, "A");
    host.sendJson({ type: "pick", rider: 5 });
    host.sendJson({ type: "ready", ready: true });
    await snap(host, (r) => r.members.length === 2 && r.members[0].ready);  // sockets aren't ordered with each other
    a.sendJson({ type: "pick", rider: 5 });
    a.sendJson({ type: "ready", ready: true });
    const r = await snap(host, (r) => r.members[1]?.ready);
    expect(r.members.map((m: any) => [m.rider, m.ready])).toEqual([[5, true], [5, true]]);
  });

  it("shows who is choosing a rider and keeps them unready", async () => {
    const { host, code } = await Client.host("H");
    const a = await Client.join(code, "A");
    a.sendJson({ type: "ready", ready: true });
    await snap(host, (r) => r.members[1]?.ready);
    a.sendJson({ type: "pick", choosing: true });
    expect((await snap(host, (r) => r.members[1].choosing)).members[1].ready).toBe(false);
    a.sendJson({ type: "ready", ready: true });
    expect((await a.json("error")).code).toBe("not_ready");
    a.sendJson({ type: "pick", rider: 9, choosing: false });
    a.sendJson({ type: "ready", ready: true });
    const r = await snap(host, (r) => r.members[1]?.ready);
    expect(r.members[1]).toMatchObject({ rider: 9 });
    expect(r.members[1].choosing ?? false).toBe(false);
  });

  it("counts down once everyone is ready and starts the race by itself", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("H", countdown);
    const { code } = await host.json("welcome");
    const a = await Client.connect(`/v1/rooms/${code}`);
    a.hello("A", countdown);
    await a.json("welcome");
    host.sendJson({ type: "ready", ready: true });
    a.sendJson({ type: "ready", ready: true });
    const r = await snap(host, (r) => r.countdownEndsAt !== null);
    expect(r.countdownEndsAt - r.serverTime).toBeGreaterThan(0);
    expect(r.countdownEndsAt - r.serverTime).toBeLessThanOrEqual(300);
    await new Promise((done) => setTimeout(done, 350));
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    const [sh, sa] = await Promise.all([host.json("start"), a.json("start")]);
    expect([sh.yourSlot, sa.yourSlot]).toEqual([0, 1]);
  });

  it("stops the countdown when someone unreadies, and Start still starts at once", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("H", countdown);
    const { code } = await host.json("welcome");
    const a = await Client.connect(`/v1/rooms/${code}`);
    a.hello("A", countdown);
    await a.json("welcome");
    host.sendJson({ type: "ready", ready: true });
    a.sendJson({ type: "ready", ready: true });
    await snap(host, (r) => r.countdownEndsAt !== null);
    a.sendJson({ type: "ready", ready: false });
    await snap(host, (r) => r.countdownEndsAt === null && !r.members[1].ready);
    await new Promise((done) => setTimeout(done, 350));
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    expect((await snap(host, () => true)).started).toBe(false);
    a.sendJson({ type: "ready", ready: true });
    await snap(host, (r) => r.countdownEndsAt !== null);
    host.sendJson({ type: "start" });
    await Promise.all([host.json("start"), a.json("start")]);
  });

  it("runs no countdown while anyone's client doesn't show one", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("H", countdown);
    const { code } = await host.json("welcome");
    const old = await Client.join(code, "Old");
    host.sendJson({ type: "ready", ready: true });
    old.sendJson({ type: "ready", ready: true });
    const r = await snap(host, (r) => r.members.length === 2 && r.members.every((m: any) => m.ready));
    expect(r.countdownEndsAt).toBeNull();
  });
});

describe("public lobby list", () => {
  it("lists public rooms that have not started and have room", async () => {
    const { host, code } = await Client.host("Lister");
    host.sendJson({ type: "settings", public: true, roomName: "Open race", track: 4 });
    await host.next((f) => typeof f === "string" && JSON.parse(f).public === true);
    const list = async () => (await (await exports.default.fetch("https://dhd.test/v1/lobbies")).json<any>()).lobbies;
    await expect.poll(list).toContainEqual(
      expect.objectContaining({ code, roomName: "Open race", track: 4, players: 1, maxPlayers: 10, hostName: "Lister" }),
    );
    host.sendJson({ type: "settings", public: false });
    await expect.poll(async () => (await list()).map((l: any) => l.code)).not.toContain(code);
  });
});

describe("race relay", () => {
  it("answers a race ping to the sender only, with its slot", async () => {
    const { clients } = await startedRace(3);
    clients[2].ws.send(new Uint8Array([3, 0, 9, 9]).buffer);
    expect([...(await clients[2].binary())]).toEqual([4, 2, 9, 9]);
  });

  it("relays STATE and EVENT frames to the other racers with the sender's slot", async () => {
    const { clients } = await startedRace(3);
    const state = new Uint8Array(200).fill(7);
    state[0] = 5;
    clients[1].ws.send(state.buffer);
    clients[2].ws.send(new Uint8Array([6, 0, 1, 2, 3]).buffer);
    const f = await clients[0].binary();
    expect(f.length).toBe(200);
    expect([f[0], f[1], f[2]]).toEqual([5, 1, 7]);
    expect([...(await clients[0].binary())]).toEqual([6, 2, 1, 2, 3]);
    expect([...(await clients[1].binary())]).toEqual([6, 2, 1, 2, 3]);
    expect((await clients[2].binary())[0]).toBe(5);
  });

  it("drops short frames and the old INPUT and HASH types", async () => {
    const { clients } = await startedRace(2);
    clients[0].ws.send(new Uint8Array([5]).buffer);
    clients[0].ws.send(new Uint8Array([1, 0, 0, 0, 0, 0, 1, 1, 2, 3, 4, 5, 6]).buffer);
    clients[0].ws.send(new Uint8Array([2, 0, 59, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]).buffer);
    clients[0].ws.send(new Uint8Array([5, 0, 7]).buffer);
    expect([...(await clients[1].binary())]).toEqual([5, 0, 7]);
  });

  it("announces a dropped racer", async () => {
    const { clients } = await startedRace(3);
    clients[1].ws.send(new Uint8Array([5, 0, 40]).buffer);
    await clients[0].binary();
    clients[1].ws.close(1000);
    const left = await clients[0].json("left");
    expect(left).toMatchObject({ slot: 1, reason: "disconnect" });
    expect(left.fromField).toBeUndefined();
    // The others keep relaying to each other.
    clients[2].ws.send(new Uint8Array([5, 0, 44]).buffer);
    expect((await clients[0].binary())[1]).toBe(2);
  });

  it("keeps relaying after the room is evicted with hibernated sockets", async () => {
    const { clients, code } = await startedRace(3);
    await evictDurableObject(env.ROOM.get(env.ROOM.idFromName(code)));
    clients[1].ws.send(new Uint8Array([5, 0, 10]).buffer);
    for (const i of [0, 2]) expect((await clients[i].binary())[1]).toBe(1);
    clients[2].ws.close(1000);
    expect(await clients[0].json("left")).toMatchObject({ slot: 2 });
    clients[1].ws.close(1000);
    expect(await clients[0].json("left")).toMatchObject({ slot: 1 });
  });

  it("hands a kicked racer to AI", async () => {
    const { clients } = await startedRace(2);
    clients[0].sendJson({ type: "kick", member: 1 });
    expect((await clients[1].json("error")).code).toBe("kicked");
    expect(await clients[1].closed()).toBe(4007);
    expect(await clients[0].json("left")).toMatchObject({ slot: 1, reason: "kicked" });
  });

  it("returns to the lobby when the host ends the race", async () => {
    const { clients } = await startedRace(2);
    clients[0].sendJson({ type: "end" });
    await clients[1].json("ended");
    const r = await clients[1].json("room");
    expect(r.started).toBe(false);
    expect(r.members.every((m: any) => !m.ready)).toBe(true);
  });
});

describe("worker routes", () => {
  it("health and 404", async () => {
    expect(await (await exports.default.fetch("https://dhd.test/v1/health")).json()).toEqual({ ok: true, protocol: 2, minVersion: "0.2.0" });
    expect((await exports.default.fetch("https://dhd.test/nope")).status).toBe(404);
    expect((await exports.default.fetch("https://dhd.test/v1/rooms/new")).status).toBe(426);
  });
  it("uses the ISO constant from the helpers", () => expect(ISO).toHaveLength(64));
});
