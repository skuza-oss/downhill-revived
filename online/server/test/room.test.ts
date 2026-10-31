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
    // The snapshot first: waiting for the error would discard it (json() skips other frames).
    const r = JSON.parse(
      (await host.next((f) => typeof f === "string" && JSON.parse(f).settings?.track === 5 && JSON.parse(f).members?.[1]?.rider === 7)) as string,
    );
    expect(r.settings).toEqual({ track: 5, laps: 3, seed: 1234 });
    expect(r.members[1]).toMatchObject({ rider: 7, bike: 2 });
    host.sendJson({ type: "settings", track: 999 });
    expect((await host.json("error")).code).toBe("bad_value");
  });

  it("says in the welcome that it takes chat", async () => {
    const c = await Client.connect("/v1/rooms/new");
    c.hello("Ann");
    expect((await c.json("welcome")).chat).toBe(true);
  });

  it("sends a chat message to everyone in the room, the sender too", async () => {
    const { host, code } = await Client.host("Ann");
    const bo = await Client.join(code, "Bo");
    bo.sendJson({ type: "chat", text: "  hi\u0007 all \n " });
    for (const c of [host, bo]) {
      const m = await c.json("chat");
      expect(m).toMatchObject({ name: "Bo", text: "hi all" });
      expect(m.memberId).toBeTypeOf("number");
      expect(m.at).toBeTypeOf("number");
    }
    host.sendJson({ type: "chat", text: " \u0000 " });
    expect((await host.json("error")).code).toBe("bad_value");
    host.sendJson({ type: "chat", text: "x".repeat(121) });
    expect((await host.json("error")).code).toBe("bad_value");
  });

  it("limits chat to 5 messages in 10 s", async () => {
    const { host, code } = await Client.host("Ann");
    const bo = await Client.join(code, "Bo");
    for (let i = 0; i < 6; i++) bo.sendJson({ type: "chat", text: `m${i}` });
    for (let i = 0; i < 5; i++) expect((await host.json("chat")).text).toBe(`m${i}`);
    expect((await bo.json("error")).code).toBe("slow_down");
  });

  it("does not start with the host alone", async () => {
    const { host } = await Client.host("H");
    host.sendJson({ type: "ready", ready: true });
    await host.next((f) => typeof f === "string" && JSON.parse(f).members?.[0]?.ready);
    host.sendJson({ type: "start" });
    expect((await host.json("error")).code).toBe("not_ready");
    host.sendJson({ type: "settings", track: 3 });
    const r = JSON.parse((await host.next((f) => typeof f === "string" && JSON.parse(f).settings?.track === 3)) as string);
    expect(r.started).toBe(false);
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

  it("unreadies everyone when the track or options change, not when the room goes public", async () => {
    const { host, code } = await Client.host("H");
    const a = await Client.join(code, "A");
    host.sendJson({ type: "ready", ready: true });
    a.sendJson({ type: "ready", ready: true });
    await snap(host, (r) => r.members.length === 2 && r.members.every((m: any) => m.ready));
    host.sendJson({ type: "settings", public: true });
    expect((await snap(host, (r) => r.public)).members.every((m: any) => m.ready)).toBe(true);
    host.sendJson({ type: "settings", track: 3 });
    expect((await snap(host, (r) => r.settings.track === 3)).members.some((m: any) => m.ready)).toBe(false);
    a.sendJson({ type: "ready", ready: true });
    await snap(host, (r) => r.members[1]?.ready);
    host.sendJson({ type: "settings", laps: 1 });
    expect((await snap(host, (r) => r.settings.laps === 1)).members[1].ready).toBe(false);
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

  it("relays chat during a race", async () => {
    const { clients } = await startedRace(2);
    clients[1].sendJson({ type: "chat", text: "gg" });
    expect(await clients[0].json("chat")).toMatchObject({ name: "P1", text: "gg" });
  });

  it("drops a racer who goes silent without a close", async () => {
    // vitest.config.ts: RACE_SILENT_MS 1000 here (10 s by default).
    const { clients, code } = await startedRace(3);
    const wait = (ms: number) => new Promise((done) => setTimeout(done, ms));
    await wait(600);
    for (const i of [0, 1]) clients[i].ws.send(new Uint8Array([3, 0]).buffer);  // race pings
    for (const i of [0, 1]) await clients[i].binary();
    await wait(600);
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    for (const i of [0, 1]) expect(await clients[i].json("left")).toMatchObject({ slot: 2, reason: "timeout" });
    expect(await clients[2].closed()).toBe(1001);
    // The two who were heard keep racing.
    clients[1].ws.send(new Uint8Array([5, 0, 8]).buffer);
    expect([...(await clients[0].binary())]).toEqual([5, 1, 8]);
  });

  it("hands the host to the next racer when the host leaves mid-race", async () => {
    const { clients } = await startedRace(3);
    clients[0].ws.close(1000);
    expect(await clients[1].json("left")).toMatchObject({ slot: 0, reason: "disconnect" });
    const room = await clients[1].json("room");
    expect(room.members.find((m: any) => m.id === room.hostId).name).toBe("P1");  // slot 1, the next to join
    clients[2].ws.send(new Uint8Array([5, 0, 9]).buffer);
    expect([...(await clients[1].binary())]).toEqual([5, 2, 9]);
    clients[2].sendJson({ type: "end" });
    expect((await clients[2].json("error")).code).toBe("not_host");
    clients[1].sendJson({ type: "end" });
    await Promise.all([clients[1].json("ended"), clients[2].json("ended")]);
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

describe("reconnect in the lobby (finding 126)", () => {
  const room = (f: unknown) => typeof f === "string" && JSON.parse(f).type === "room";
  const reconnect = { features: ["reconnect"] };
  const TOKEN = /^[0-9a-f]{64}$/;

  /** Joins as a member that declared the "reconnect" feature, returning its welcome (its token). */
  async function joinReconnecting(code: string, name: string) {
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello(name, reconnect);
    return { c, welcome: await c.json("welcome") };
  }

  it("re-attaches a dropped member to its own slot, name and state with its token", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("H", reconnect);
    const code = (await host.json("welcome")).code;
    const { c: bo, welcome: bw } = await joinReconnecting(code, "Bo");
    expect(bw.memberId).toBe(1);
    expect(bw.reconnectToken).toMatch(TOKEN);

    // Bo picks a rider and readies: the held slot must come back exactly as it was.
    bo.sendJson({ type: "pick", rider: 5, bike: 2 });
    bo.sendJson({ type: "ready", ready: true });
    await host.next((f) => room(f) && JSON.parse(f as string).members.length === 2 && JSON.parse(f as string).members[1].ready);

    bo.ws.close(1000);
    const held = JSON.parse(
      (await host.next((f) => room(f) && JSON.parse(f as string).members.some((m: any) => m.id === 1 && m.reconnecting))) as string,
    );
    expect(held.members.find((m: any) => m.id === 1)).toMatchObject({
      id: 1, name: "Bo", ready: true, rider: 5, bike: 2, reconnecting: true,
    });

    const back = await Client.connect(`/v1/rooms/${code}`);
    back.hello("Bo", { ...reconnect, reconnectToken: bw.reconnectToken });
    const bw2 = await back.json("welcome");
    expect(bw2.memberId).toBe(1);
    expect(bw2.reconnectToken).toMatch(TOKEN);
    expect(bw2.reconnectToken).not.toBe(bw.reconnectToken);  // the token that came back is spent
    const online = JSON.parse(
      (await host.next((f) => room(f) && JSON.parse(f as string).members.some((m: any) => m.id === 1 && !m.reconnecting))) as string,
    );
    expect(online.members.find((m: any) => m.id === 1)).toMatchObject({ name: "Bo", ready: true, rider: 5, bike: 2 });
    expect(online.members).toHaveLength(2);  // nobody else took the slot meanwhile
  });

  it("rejects a wrong or absent token: the fresh socket is a new join, never the held slot", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("H", reconnect);
    const code = (await host.json("welcome")).code;
    const { c: bo, welcome: bw } = await joinReconnecting(code, "Bo");
    bo.sendJson({ type: "ready", ready: true });
    await host.next((f) => room(f) && JSON.parse(f as string).members[1]?.ready);
    bo.ws.close(1000);
    await host.next((f) => room(f) && JSON.parse(f as string).members.some((m: any) => m.id === 1 && m.reconnecting));

    // A well-formed but wrong token: it does not find the member.
    const wrong = await Client.connect(`/v1/rooms/${code}`);
    wrong.hello("Mallory", { ...reconnect, reconnectToken: "0".repeat(64) });
    const ww = await wrong.json("welcome");
    expect(ww.memberId).not.toBe(1);
    expect(ww.memberId).toBe(2);
    expect(ww.host).toBe(false);

    // No token at all: a new join too (the net client never reconnects without one).
    const fresh = await Client.connect(`/v1/rooms/${code}`);
    fresh.hello("Fresh", reconnect);
    expect((await fresh.json("welcome")).memberId).toBe(3);

    // The held slot is still Bo's, not Mallory's or Fresh's.
    const snap = JSON.parse((await host.next((f) => room(f) && JSON.parse(f as string).members.length === 4)) as string);
    expect(snap.members.find((m: any) => m.id === 1)).toMatchObject({ name: "Bo", reconnecting: true });
    expect(snap.members.find((m: any) => m.id === 2).name).toBe("Mallory");

    // Bo's real token still re-attaches it.
    const back = await Client.connect(`/v1/rooms/${code}`);
    back.hello("Bo", { ...reconnect, reconnectToken: bw.reconnectToken });
    expect((await back.json("welcome")).memberId).toBe(1);
  });

  it("does not let a token take over a member that is still connected", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("H", reconnect);
    const code = (await host.json("welcome")).code;
    const { c: bo, welcome: bw } = await joinReconnecting(code, "Bo");

    // Bo is live; someone presents Bo's token. It must not become Bo.
    const attacker = await Client.connect(`/v1/rooms/${code}`);
    attacker.hello("Attacker", { ...reconnect, reconnectToken: bw.reconnectToken });
    const aw = await attacker.json("welcome");
    expect(aw.memberId).toBe(2);
    expect(aw.memberId).not.toBe(bw.memberId);

    // Bo is still here, and still Bo: its own traffic reaches the room from slot 1.
    const snap = JSON.parse((await host.next((f) => room(f) && JSON.parse(f as string).members.length === 3)) as string);
    expect(snap.members.map((m: any) => m.name)).toEqual(["H", "Bo", "Attacker"]);
    expect(snap.members.find((m: any) => m.id === 1).reconnecting).toBeUndefined();
    bo.sendJson({ type: "ready", ready: true });
    const ready = JSON.parse((await host.next((f) => room(f) && JSON.parse(f as string).members[1]?.ready)) as string);
    expect(ready.members[1].id).toBe(1);
    expect(ready.members[1].name).toBe("Bo");
  });

  it("frees the slot when the reconnect window ends: the same token is a normal join", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("H", reconnect);
    const code = (await host.json("welcome")).code;
    const { c: bo, welcome: bw } = await joinReconnecting(code, "Bo");
    bo.ws.close(1000);
    await host.next((f) => room(f) && JSON.parse(f as string).members.some((m: any) => m.id === 1 && m.reconnecting));

    // The window is 300 ms here (vitest.config.ts). After it, the alarm drops the slot.
    await new Promise((done) => setTimeout(done, 350));
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    const gone = JSON.parse(
      (await host.next((f) => room(f) && !JSON.parse(f as string).members.some((m: any) => m.id === 1))) as string,
    );
    expect(gone.members.map((m: any) => m.name)).toEqual(["H"]);

    // The old token now claims nothing: it joins as a fresh member (id 2), not Bo's id 1.
    const late = await Client.connect(`/v1/rooms/${code}`);
    late.hello("Bo", { ...reconnect, reconnectToken: bw.reconnectToken });
    const lw = await late.json("welcome");
    expect(lw.memberId).toBe(2);
    expect(lw.memberId).not.toBe(1);
  });

  it("keeps the host role across a reconnect and refuses a start while a slot is held", async () => {
    const host = await Client.connect("/v1/rooms/new");
    host.hello("H", reconnect);
    const hw = await host.json("welcome");
    const code = hw.code;
    const { c: bo } = await joinReconnecting(code, "Bo");
    host.sendJson({ type: "ready", ready: true });
    bo.sendJson({ type: "ready", ready: true });
    await bo.next((f) => room(f) && JSON.parse(f as string).members.length === 2 && JSON.parse(f as string).members.every((m: any) => m.ready));

    // Bo's slot is held: no race may start into it.
    bo.ws.close(1000);
    await host.next((f) => room(f) && JSON.parse(f as string).members.some((m: any) => m.id === 1 && m.reconnecting));
    host.sendJson({ type: "start" });
    expect((await host.json("error")).code).toBe("not_ready");

    // The host drops too; it keeps the host role, and its token brings it back as host.
    host.ws.close(1000);
    const rehost = await Client.connect(`/v1/rooms/${code}`);
    rehost.hello("H", { ...reconnect, reconnectToken: hw.reconnectToken });
    const rw = await rehost.json("welcome");
    expect(rw.memberId).toBe(hw.memberId);
    expect(rw.host).toBe(true);
  });
});

describe("rejoin a race (finding 126)", () => {
  const reconnect = { features: ["countdown", "reconnect"] };
  const all = () => reconnect;
  const wait = (ms: number) => new Promise((done) => setTimeout(done, ms));
  const frame = (...b: number[]) => new Uint8Array(b).buffer;

  /** Racer `slot` comes back on the room's code with its token; returns the new client and its welcome. */
  async function rejoin(code: string, token: string, extra: object = {}) {
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello("again", { ...reconnect, reconnectToken: token, ...extra });
    return c;
  }

  it("holds a dropped racer's slot: away at once, back on rejoin, relayed both ways", async () => {
    const { clients, code } = await startedRace(3, all);
    const token = clients[1].welcome.reconnectToken;
    const id = clients[1].welcome.memberId;
    expect(token).toMatch(/^[0-9a-f]{64}$/);
    clients[1].ws.close(1000);
    for (const i of [0, 2]) expect(await clients[i].json("away")).toEqual({ type: "away", slot: 1, memberId: id });
    // While away, the others' frames go nowhere and nothing fails.
    clients[0].ws.send(frame(5, 0, 1));
    expect([...(await clients[2].binary())]).toEqual([5, 0, 1]);

    const back = await rejoin(code, token);
    const w = await back.json("welcome");
    expect(w).toMatchObject({ memberId: id, slot: 1, host: false });
    expect(w.reconnectToken).toMatch(/^[0-9a-f]{64}$/);
    expect(w.reconnectToken).not.toBe(token);
    for (const c of [clients[0], clients[2], back]) expect(await c.json("back")).toEqual({ type: "back", slot: 1, memberId: id });
    const room = await clients[0].json("room");  // the snapshot after back
    expect(room.started).toBe(true);
    expect(room.members.find((m: any) => m.id === id).reconnecting).toBeUndefined();
    // Relayed both ways from the same slot, no `left` for anyone.
    back.ws.send(frame(5, 7, 42));
    for (const i of [0, 2]) expect([...(await clients[i].binary())]).toEqual([5, 1, 42]);
    clients[2].ws.send(frame(6, 0, 9));
    expect([...(await back.binary())]).toEqual([6, 2, 9]);
    back.ws.send(frame(3, 0, 5));
    expect([...(await back.binary())]).toEqual([4, 1, 5]);
  });

  it("refuses a race rejoin with a wrong token, and the slot stays held", async () => {
    const { clients, code } = await startedRace(2, all);
    const token = clients[1].welcome.reconnectToken;
    clients[1].ws.close(1000);
    await clients[0].json("away");
    const wrong = await rejoin(code, "0".repeat(64));
    expect((await wrong.json("error")).code).toBe("room_started");
    expect(await wrong.closed()).toBe(4005);
    // Another version with the right token: refused too.
    const other = await rejoin(code, token, { version: "other build" });
    expect((await other.json("error")).code).toBe("version_mismatch");
    // The right one still works.
    const back = await rejoin(code, token);
    expect((await back.json("welcome")).slot).toBe(1);
    await clients[0].json("back");
  });

  it("sends left when the grace period ends without a rejoin; the token is spent", async () => {
    const { clients, code } = await startedRace(3, all);
    const token = clients[2].welcome.reconnectToken;
    clients[2].ws.close(1000);
    await clients[0].json("away");
    await wait(350);  // RECONNECT_WINDOW_MS is 300 here (vitest.config.ts)
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    for (const i of [0, 1]) expect(await clients[i].json("left")).toMatchObject({ slot: 2, reason: "timeout" });
    const late = await rejoin(code, token);
    expect((await late.json("error")).code).toBe("room_started");
    clients[1].ws.send(frame(5, 0, 3));
    expect([...(await clients[0].binary())]).toEqual([5, 1, 3]);
  });

  it("holds a silent racer that can rejoin instead of dropping it", async () => {
    // vitest.config.ts: RACE_SILENT_MS 1000.
    const { clients, code } = await startedRace(2, all);
    await wait(600);
    clients[0].ws.send(frame(3, 0));
    await clients[0].binary();
    await wait(600);
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    expect(await clients[0].json("away")).toMatchObject({ slot: 1 });
    expect(await clients[1].closed()).toBe(1001);
    const back = await rejoin(code, clients[1].welcome.reconnectToken);
    expect((await back.json("welcome")).slot).toBe(1);
  });

  it("tells a racer who comes back after the race ended that it is in the lobby", async () => {
    const { clients, code } = await startedRace(2, all);
    const token = clients[1].welcome.reconnectToken;
    clients[1].ws.close(1000);
    await clients[0].json("away");
    clients[0].sendJson({ type: "end" });
    await clients[0].json("ended");
    const back = await rejoin(code, token);
    const w = await back.json("welcome");
    expect(w.memberId).toBe(clients[1].welcome.memberId);
    expect(w.slot).toBeUndefined();
    expect((await back.json("room")).started).toBe(false);
  });

  it("leaves an older client's drop as it was: left at once, no away", async () => {
    // Slot 1 is an older client (no "reconnect"); slot 2 a newer one.
    const { clients } = await startedRace(3, (i) => (i === 2 ? reconnect : {}));
    expect(clients[1].welcome.reconnectToken).toBeUndefined();
    clients[1].ws.close(1000);
    const f = JSON.parse((await clients[0].next((x) => typeof x === "string" && ["away", "left"].includes(JSON.parse(x).type))) as string);
    expect(f).toMatchObject({ type: "left", slot: 1, reason: "disconnect" });
  });

  it("hands a racer who sends leave to AI at once, token or not", async () => {
    const { clients } = await startedRace(2, all);
    clients[1].sendJson({ type: "leave" });
    expect(await clients[0].json("left")).toMatchObject({ slot: 1, reason: "leave" });
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
