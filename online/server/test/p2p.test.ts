// P2P rooms (src/p2p.ts, finding 180): membership, host-star signaling and privacy. The race itself
// runs between the games, so these tests check what the server must and must not do.
import { runDurableObjectAlarm, runInDurableObject } from "cloudflare:test";
import { env, exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import { TURN_RENEW_MS } from "../src/p2p";
import { CLOSE_CODES } from "../src/protocol";
import { Client, VERSION } from "./helpers";

async function p2pHost(name = "Host") {
  const host = await Client.connect("/v1/p2p/rooms/new");
  host.hello(name);
  const welcome = await host.json("p2p_welcome");
  return { host, welcome, code: welcome.code as string };
}

async function p2pJoin(code: string, name: string, extra: object = {}) {
  const guest = await Client.connect(`/v1/p2p/rooms/${code}`);
  guest.hello(name, extra);
  return guest;
}

const get = (path: string) => exports.default.fetch(`https://dhd.test${path}`);

const room = (code: string) => env.P2P_ROOM.get(env.P2P_ROOM.idFromName(code));

/** Cloudflare's TURN API as a room sees it from now on: credentials u1, u2... and revokes, recorded. */
async function fakeTurn(code: string) {
  const turn = { minted: [] as string[], revoked: [] as string[] };
  await runInDurableObject(room(code), (r: any) => {
    r.env = { ...r.env, TURN_KEY_ID: "key", TURN_KEY_API_TOKEN: "token" };
    r.turnFetch = async (input: RequestInfo | URL) => {
      const url = String(input);
      if (url.endsWith("/credentials/generate-ice-servers")) {
        const username = `u${turn.minted.length + 1}`;
        turn.minted.push(username);
        return Response.json({ iceServers: [{ urls: ["turns:turn.example:443?transport=tcp"], username, credential: "secret" }] }, { status: 201 });
      }
      const revoke = url.match(/\/credentials\/([^/]+)\/revoke$/);
      if (revoke) turn.revoked.push(decodeURIComponent(revoke[1]));
      return new Response(null, { status: revoke ? 204 : 404 });
    };
  });
  return turn;
}

async function until(what: string, done: () => boolean) {
  for (let i = 0; i < 100 && !done(); i++) await new Promise((r) => setTimeout(r, 10));
  if (!done()) throw new Error(`timed out waiting for ${what}`);
}

describe("P2P rooms", () => {
  it("are never listed: not in the lobby list, and no P2P list exists", async () => {
    const { host, code } = await p2pHost();
    // A host asking to be public is not a thing here; the old listing message is ignored.
    host.sendJson({ type: "p2p_listing", public: true, maxPlayers: 10, track: 0, roomName: "Open" });
    host.sendJson({ type: "ping", t: 1 });
    await host.json("pong");
    const list = await (await get("/v1/lobbies")).json<{ lobbies: { code: string }[] }>();
    expect(list.lobbies.some((l) => l.code === code)).toBe(false);
    expect((await get("/v1/p2p/lobbies")).status).toBe(404);
    host.ws.close();
  });

  it("have seven-character codes, so the game can tell them from an SFU room's", async () => {
    const { host, code } = await p2pHost();
    expect(code).toMatch(/^[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{7}$/);
    // A six-character (SFU) code is no P2P room.
    expect((await get(`/v1/p2p/rooms/${code.slice(0, 6)}`)).status).toBe(404);
    host.ws.close();
  });

  it("relay signaling host-star only, from the connection's own member", async () => {
    const { host, welcome, code } = await p2pHost();
    const a = await p2pJoin(code, "A");
    const wa = await a.json("p2p_welcome");
    const b = await p2pJoin(code, "B");
    const wb = await b.json("p2p_welcome");
    expect(wa.hostId).toBe(welcome.memberId);
    // A forged "from" is replaced by the sender's own id.
    a.sendJson({ type: "p2p_signal", to: welcome.memberId, from: wb.memberId, signal: { type: "offer", sdp: "x" } });
    const signal = await host.json("p2p_signal");
    expect(signal.from).toBe(wa.memberId);
    expect(signal.signal).toEqual({ type: "offer", sdp: "x" });
    // Guest to guest is refused: guests never learn each other's addresses.
    a.sendJson({ type: "p2p_signal", to: wb.memberId, signal: { type: "offer", sdp: "x" } });
    expect((await a.json("error")).message).toContain("host-star");
    expect(await a.closed()).toBe(CLOSE_CODES.bad_message);
    expect(b.seen.some((s) => JSON.parse(s).type === "p2p_signal")).toBe(false);
    host.ws.close();
    b.ws.close();
  });

  it("tell every member who is in the room, and give no member another's address", async () => {
    const { host, code } = await p2pHost();
    const guest = await p2pJoin(code, "Guest");
    await guest.json("p2p_welcome");
    let members = await host.json("p2p_members");
    if (members.members.length < 2) members = await host.json("p2p_members");  // the host's own join first
    expect(members.members.map((m: { name: string }) => m.name).sort()).toEqual(["Guest", "Host"]);
    for (const text of [...host.seen, ...guest.seen])
      expect(text).not.toMatch(/198\.1[89]\.\d+\.\d+|ipHash|installId|session/);
    host.ws.close();
    guest.ws.close();
  });

  it("let only the host set how many may join", async () => {
    const { host, code } = await p2pHost();
    const a = await p2pJoin(code, "A");
    await a.json("p2p_welcome");
    a.sendJson({ type: "p2p_capacity", maxPlayers: 10 });
    host.sendJson({ type: "p2p_capacity", maxPlayers: 2 });
    host.sendJson({ type: "ping", t: 2 });
    await host.json("pong");
    const c = await p2pJoin(code, "C");
    expect((await c.json("error")).code).toBe("room_full");
    expect(await c.closed()).toBe(CLOSE_CODES.room_full);
    host.ws.close();
    a.ws.close();
  });

  it("refuse another build or disc than the host's", async () => {
    const { host, code } = await p2pHost();
    const other = await p2pJoin(code, "Other", { version: VERSION + " other" });
    expect((await other.json("error")).code).toBe("version_mismatch");
    const disc = await p2pJoin(code, "Disc", { isoSha256: "b".repeat(64) });
    expect((await disc.json("error")).code).toBe("iso_mismatch");
    host.ws.close();
  });

  it("let the host remove a guest, and no guest remove anyone", async () => {
    const { host, welcome, code } = await p2pHost();
    const a = await p2pJoin(code, "A");
    const wa = await a.json("p2p_welcome");
    a.sendJson({ type: "p2p_kick", member: welcome.memberId });
    host.sendJson({ type: "p2p_kick", member: wa.memberId });
    expect((await a.json("error")).code).toBe("kicked");
    expect(await a.closed()).toBe(CLOSE_CODES.kicked);
    expect(host.closeCode).toBeNull();
    // A guest the host's game could not reach is told so.
    const b = await p2pJoin(code, "B");
    const wb = await b.json("p2p_welcome");
    host.sendJson({ type: "p2p_kick", member: wb.memberId, reason: "unreachable" });
    expect((await b.json("error")).message).toContain("could not connect");
    host.ws.close();
  });

  it("end on host loss, without migrating the host", async () => {
    const { host, code } = await p2pHost();
    const guest = await p2pJoin(code, "Guest");
    await guest.json("p2p_welcome");
    host.ws.close();
    expect((await guest.json("error")).code).toBe("host_left");
    expect(await guest.closed()).toBe(CLOSE_CODES.host_left);
    const late = await p2pJoin(code, "Late");
    expect((await late.json("error")).code).toBe("room_not_found");
  });

  it("run no race: room commands get no answer and nothing is relayed", async () => {
    const { host, code } = await p2pHost();
    const guest = await p2pJoin(code, "Guest");
    await guest.json("p2p_welcome");
    host.sendJson({ type: "start" });
    host.sendJson({ type: "ready", ready: true });
    host.ws.send(new Uint8Array([5, 0, 6, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]));
    guest.sendJson({ type: "ping", t: 12 });
    expect((await guest.json("pong")).t).toBe(12);
    expect(guest.seen.some((s) => ["start", "room", "ended"].includes(JSON.parse(s).type))).toBe(false);
    host.ws.close();
    guest.ws.close();
  });

  it("answer the clock's binary PING with the server's time", async () => {
    const { host } = await p2pHost();
    const ping = new Uint8Array(10);
    ping[0] = 3;
    new DataView(ping.buffer).setFloat64(2, 1234.5, true);
    host.ws.send(ping);
    const pong = await host.binary();
    expect(pong[0]).toBe(4);
    expect(new DataView(pong.buffer, pong.byteOffset).getFloat64(2, true)).toBe(1234.5);
    expect(new DataView(pong.buffer, pong.byteOffset).getFloat64(10, true)).toBeGreaterThan(0);
    host.ws.close();
  });

  it("renew each member's TURN credentials before they expire, and revoke them all when it leaves", async () => {
    const { host, code } = await p2pHost();
    const turn = await fakeTurn(code);
    const guest = await p2pJoin(code, "Guest");
    await guest.json("p2p_welcome");
    expect((await guest.json("p2p_turn")).username).toBe("u1");
    // Not due yet: the sweep leaves them.
    await runDurableObjectAlarm(room(code));
    expect(turn.minted).toEqual(["u1"]);
    // Made TURN_RENEW_MS ago: the next sweep sends fresh ones, once.
    await runInDurableObject(room(code), (_r, state) => {
      for (const ws of state.getWebSockets()) {
        const a = ws.deserializeAttachment();
        if (a.turnUser) ws.serializeAttachment({ ...a, turnAt: a.turnAt - TURN_RENEW_MS });
      }
    });
    await runDurableObjectAlarm(room(code));
    expect((await guest.json("p2p_turn")).username).toBe("u2");
    await runDurableObjectAlarm(room(code));
    expect(turn.minted).toEqual(["u1", "u2"]);
    // Leaving revokes both: the old ones are valid until they expire.
    guest.sendJson({ type: "leave" });
    await until("the revokes", () => turn.revoked.length === 2);
    expect(turn.revoked.sort()).toEqual(["u1", "u2"]);
    host.ws.close();
  });

  it("end with the host: every guest's credentials revoked at once, and the room's storage gone", async () => {
    const { host, code } = await p2pHost();
    const turn = await fakeTurn(code);
    const a = await p2pJoin(code, "A");
    expect((await a.json("p2p_turn")).username).toBe("u1");
    const b = await p2pJoin(code, "B");
    expect((await b.json("p2p_turn")).username).toBe("u2");
    // The host leaves; the guests' sockets stay open on their side (no close from them).
    host.sendJson({ type: "leave" });
    expect((await a.json("error")).code).toBe("host_left");
    await until("the guests' revokes", () => turn.revoked.length === 2);
    expect(turn.revoked.sort()).toEqual(["u1", "u2"]);
    const kept = await runInDurableObject(room(code), async (_r, state) => ({
      keys: (await state.storage.list()).size, alarm: await state.storage.getAlarm(),
    }));
    expect(kept).toEqual({ keys: 0, alarm: null });
    expect((await (await p2pJoin(code, "Late")).json("error")).code).toBe("room_not_found");
  });

  it("forget a room whose host went silent", async () => {
    const { code } = await p2pHost();
    await runInDurableObject(room(code), (_r, state) => {
      for (const ws of state.getWebSockets()) ws.serializeAttachment({ ...ws.deserializeAttachment(), heard: 0 });
    });
    await runDurableObjectAlarm(room(code));
    const keys = await runInDurableObject(room(code), async (_r, state) => (await state.storage.list()).size);
    expect(keys).toBe(0);
  });
});
