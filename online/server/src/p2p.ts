// P2P rooms (finding 180): the server admits members, relays host-star signaling and gives each
// member its own TURN credentials. Nothing else: the host's game runs the room's rules and carries
// the race (port/render/net/src/session.rs); no race frame passes through here.
//
// A P2P room is private. Signaling carries each member's network addresses to the host and the
// host's to every member, so a room is reached only by its code and never listed; the game says so
// when it creates or joins one. The official server runs these routes as their own Worker
// (wrangler.p2p.jsonc, src/p2p-entry.ts), so deploying them never restarts an SFU room; a local or
// self-hosted server serves them from the main Worker (src/index.ts).
import { DurableObject } from "cloudflare:workers";
import { Turn, type SfuSecrets } from "./sfu";
import {
  CLOSE_CODES, FRAME_PING, FRAME_PONG, HELLO_TIMEOUT_MS, MAX_MEMBERS, P2P_CODE_LENGTH, PROTOCOL,
  clientVersionOf, isOutdated, isValidCode, minVersionOf, randomCode,
} from "./protocol";
import {
  CAP_WINDOW_MS, RATE_MESSAGE, TOO_FAST_MESSAGE, TOO_FAST_WARNING, banMessage, findBan, installIdOf, ipHash,
  logJoin, newTally, onlineState, overLimit, refuseSocket, tally, type Caps, type GuardEnv, type Tally,
} from "./guard";

/** A member who has sent nothing for this long is gone (the game pings every second). */
const IDLE_MS = 20_000;
/** How often the room looks for idle and silent connections. */
const SWEEP_MS = 5_000;
/** The largest signaling message: an SDP offer or answer with its candidates fits in a few kB. */
const SIGNAL_MAX_BYTES = 24_000;
/** Connections a room accepts at once: its members and two more still saying hello. */
const MAX_CONNECTIONS = MAX_MEMBERS + 2;
const NAME_MAX = 24;
/** A binary PING: type, one byte of payload, the client's send time (f64). The PONG adds the
    server's clock, so the game keeps one clock for P2P rooms as for SFU rooms. */
const PING_BYTES = 10;
/** A member's message caps (src/guard.ts tally). A host signals every guest at once: an offer and
    its trickled candidates (host, server-reflexive, relay; a browser gathers one per network
    interface) to up to nine guests is about a hundred messages in its first second. Pings come
    once a second. Binary is only the clock's PING. */
const P2P_CAPS: Caps = { windowMs: CAP_WINDOW_MS, text: 400, binary: 20 };

type Message = Record<string, unknown>;

/** What the room keeps: the room's code, host and build, and how many may join. */
interface RoomMeta {
  code: string;
  hostId: number;
  nextId: number;
  /** The host's game build and disc hash: every member must have the same. "" before the host. */
  version: string;
  iso: string;
  maxPlayers: number;
  closed: boolean;
}

/** Each connection's own state, in its WebSocket attachment (it survives hibernation). */
interface Connection {
  id: number | null;
  name: string;
  opened: number;
  heard: number;
  tally: Tally;
  ipHash: string;
  country: string;
  turnUser?: string;
}

/** The P2P routes: a WebSocket to /v1/p2p/rooms/new makes a room, to /v1/p2p/rooms/CODE joins one
    (seven characters: an SFU room's code has six). Returns null for any other path. */
export async function p2pRoute(request: Request, env: Env, path: string): Promise<Response | null> {
  const m = path.match(/^\/v1\/p2p\/rooms\/([^/]+)$/);
  if (!m) return null;
  const create = m[1] === "new";
  const code = create ? "" : m[1].toUpperCase();
  if (!create && !isValidCode(code, P2P_CODE_LENGTH)) return Response.json({ error: "Bad room code" }, { status: 404 });
  if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
    return Response.json({ error: "Expected a WebSocket upgrade" }, { status: 426 });
  const guard = env as GuardEnv;
  const sw = await onlineState(guard);
  if (!sw.on) return refuseSocket("online_off", sw.message);
  const ip = await ipHash(guard, request);
  if (await overLimit(guard, ip, create)) return refuseSocket("rate_limited", RATE_MESSAGE);
  const ban = await findBan(guard, { ip });
  if (ban) return refuseSocket("banned", banMessage(ban));
  // The address's hash for the moderation log; never one a client sent.
  const headers = new Headers(request.headers);
  headers.delete("x-dhd-ip-hash");
  if (ip) headers.set("x-dhd-ip-hash", ip);
  const forward = new Request(request, { headers });
  if (!create) return env.P2P_ROOM.get(env.P2P_ROOM.idFromName(code)).fetch(forward);
  for (let attempt = 0; attempt < 8; attempt++) {
    const fresh = randomCode(P2P_CODE_LENGTH);
    const room = env.P2P_ROOM.get(env.P2P_ROOM.idFromName(fresh));
    if (await room.claim(fresh)) return room.fetch(forward);
  }
  return Response.json({ error: "Could not allocate a room code" }, { status: 503 });
}

export class P2pRoom extends DurableObject<Env> {
  private meta: RoomMeta | null = null;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(async () => {
      this.meta = (await ctx.storage.get<RoomMeta>("meta")) ?? null;
    });
  }

  /** Takes a fresh code for a new room; false when the code is in use. */
  async claim(code: string): Promise<boolean> {
    if (this.meta) return false;
    this.meta = { code, hostId: 0, nextId: 1, version: "", iso: "", maxPlayers: MAX_MEMBERS, closed: false };
    await this.save();
    return true;
  }

  async fetch(request: Request): Promise<Response> {
    if (!this.meta || this.meta.closed) return refuseSocket("room_not_found", "There is no room with that code.");
    if (this.ctx.getWebSockets().length >= MAX_CONNECTIONS) return refuseSocket("room_full", "This room is full.");
    const now = Date.now();
    const cf = request.cf as { country?: string } | undefined;
    const { 0: client, 1: server } = new WebSocketPair();
    this.ctx.acceptWebSocket(server);
    this.attach(server, {
      id: null, name: "", opened: now, heard: now, tally: newTally(now),
      ipHash: request.headers.get("x-dhd-ip-hash") ?? "", country: cf?.country ?? "",
    });
    if ((await this.ctx.storage.getAlarm()) === null) await this.ctx.storage.setAlarm(now + SWEEP_MS);
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws: WebSocket, data: string | ArrayBuffer): Promise<void> {
    const meta = this.meta, info = this.info(ws);
    if (!meta || meta.closed || !info) return;
    const now = Date.now();
    info.heard = now;
    const verdict = tally(info.tally, typeof data !== "string", now, P2P_CAPS);
    this.attach(ws, info);
    if (verdict === "close") return this.refuse(ws, "too_fast", TOO_FAST_MESSAGE);
    if (verdict === "warn") this.send(ws, { type: "error", code: "too_fast", message: TOO_FAST_WARNING });
    if (verdict !== "ok") return;
    if (typeof data !== "string") return this.pong(ws, info, data, now);
    if (data.length > SIGNAL_MAX_BYTES) return this.refuse(ws, "bad_message", "Message too large.");
    let m: Message;
    try {
      m = JSON.parse(data);
      if (!m || typeof m !== "object" || Array.isArray(m)) throw new Error("not an object");
    } catch {
      return this.refuse(ws, "bad_message", "Invalid message.");
    }
    if (m.type === "ping") return this.send(ws, { type: "pong", t: m.t, serverTime: now });
    if (m.type === "hello") return this.hello(ws, info, m);
    if (info.id === null) return this.refuse(ws, "bad_message", "Send hello first.");
    const host = info.id === meta.hostId;
    switch (m.type) {
      case "leave":
        await this.remove(ws);
        try { ws.close(1000, "left"); } catch { /* already closed */ }
        return;
      case "p2p_signal":
        return this.signal(ws, info.id, m);
      case "p2p_kick":
        if (host && m.member !== meta.hostId) {
          const target = this.members().find(([, i]) => i.id === m.member);
          if (target) {
            // "unreachable": the host's game could not connect to this one (port/render/session).
            this.refuse(target[0], "kicked", m.reason === "unreachable"
              ? "Your game could not connect to the host's." : "The host removed you from the room.");
            await this.remove(target[0]);
          }
        }
        return;
      case "p2p_capacity":
        // The host's MAX PLAYERS: joins beyond it are refused. Members already in stay.
        if (host && Number.isInteger(m.maxPlayers) && (m.maxPlayers as number) >= 2 && (m.maxPlayers as number) <= MAX_MEMBERS) {
          meta.maxPlayers = m.maxPlayers as number;
          await this.save();
        }
        return;
    }
    // Room commands and race frames go to the host over the peer connection, never through here.
  }

  async webSocketClose(ws: WebSocket): Promise<void> {
    await this.remove(ws);
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.remove(ws);
  }

  /** Drops connections that never said hello and members gone silent. */
  async alarm(): Promise<void> {
    if (!this.meta || this.meta.closed) return;
    const now = Date.now();
    for (const ws of this.ctx.getWebSockets()) {
      const info = this.info(ws);
      if (!info) continue;
      if (info.id === null ? now - info.opened > HELLO_TIMEOUT_MS : now - info.heard > IDLE_MS) {
        await this.remove(ws);
        try { ws.close(1001, "timed out"); } catch { /* already closed */ }
      }
    }
    if (!this.meta.closed && this.ctx.getWebSockets().length) await this.ctx.storage.setAlarm(now + SWEEP_MS);
  }

  private async hello(ws: WebSocket, info: Connection, m: Message): Promise<void> {
    const meta = this.meta!;
    if (info.id !== null) return this.refuse(ws, "bad_message", "Already joined.");
    if (m.protocol !== PROTOCOL) return this.refuse(ws, "protocol_mismatch", "This game speaks another online protocol.");
    const name = typeof m.name === "string" ? m.name.trim() : "";
    if (!name || name.length > NAME_MAX || typeof m.version !== "string" || m.version.length > 100 ||
        typeof m.isoSha256 !== "string" || !/^[a-f0-9]{64}$/.test(m.isoSha256))
      return this.refuse(ws, "bad_message", "Invalid player details.");
    const clientVersion = clientVersionOf(m);
    if (isOutdated(clientVersion, minVersionOf(this.env.MIN_VERSION)))
      return this.refuse(ws, "client_outdated", "A new version is out. Update to play online.");
    const installId = installIdOf(m.installId);
    const ban = await findBan(this.env as GuardEnv, { install: installId });
    if (ban) return this.refuse(ws, "banned", banMessage(ban));
    // The ban lookup waited: the room may have closed, or this socket gone, meanwhile.
    if (meta.closed || this.info(ws)?.id !== null) return;
    if (meta.version && meta.version !== m.version) return this.refuse(ws, "version_mismatch", "The host has another version of the game.");
    if (meta.iso && meta.iso !== m.isoSha256) return this.refuse(ws, "iso_mismatch", "The host's game files differ from yours.");
    if (this.members().length >= meta.maxPlayers) return this.refuse(ws, "room_full", "This room is full.");

    const asHost = !meta.hostId;
    info.id = meta.nextId++;
    info.name = name;
    if (asHost) {
      meta.hostId = info.id;
      meta.version = m.version;
      meta.iso = m.isoSha256;
    }
    this.attach(ws, info);
    await this.save();
    const turn = !!(this.secrets().TURN_KEY_ID && this.secrets().TURN_KEY_API_TOKEN);
    this.send(ws, {
      type: "p2p_welcome", code: meta.code, memberId: info.id, hostId: meta.hostId, version: meta.version,
      serverTime: Date.now(), members: this.roster(), turnPending: turn,
    });
    this.broadcastMembers();
    this.ctx.waitUntil(logJoin(this.env as GuardEnv, {
      at: Date.now(), room: meta.code, member: info.id, kind: asHost ? "host" : "guest", name, clientVersion,
      country: info.country, ipHash: info.ipHash, installId,
    }));
    if (turn) await this.giveTurn(ws, info.id);
  }

  /** The member's own TURN credentials: the fallback when no direct path between two games works. */
  private async giveTurn(ws: WebSocket, id: number): Promise<void> {
    const meta = this.meta!;
    const turn = new Turn(this.secrets());
    try {
      const credentials = await turn.mint(`p2p-${meta.code}-${id}`);
      const current = this.info(ws);
      if (meta.closed || current?.id !== id) {
        await turn.revoke(credentials.username).catch(() => {});
        return;
      }
      current.turnUser = credentials.username;
      this.attach(ws, current);
      this.send(ws, { ...(credentials.message as object), type: "p2p_turn" });
    } catch {
      this.send(ws, { type: "error", code: "turn_unavailable", message: "Relay setup failed. Direct connections may still work." });
    }
  }

  /** Host-star only: a guest signals the host, the host any guest. The sender is the connection's
      member, whatever the message claims. */
  private signal(ws: WebSocket, from: number, m: Message): void {
    const meta = this.meta!;
    if (!Number.isSafeInteger(m.to) || !m.signal || typeof m.signal !== "object" || Array.isArray(m.signal)) return;
    if (from !== meta.hostId && m.to !== meta.hostId) return this.refuse(ws, "bad_message", "Only host-star links are allowed.");
    const target = this.members().find(([, i]) => i.id === m.to && i.id !== from);
    if (target) this.send(target[0], { type: "p2p_signal", from, signal: m.signal });
  }

  private async remove(ws: WebSocket): Promise<void> {
    const meta = this.meta, info = this.info(ws);
    if (!meta || !info || info.id === null) return;
    const id = info.id;
    info.id = null;
    this.attach(ws, info);
    if (info.turnUser) this.ctx.waitUntil(new Turn(this.secrets()).revoke(info.turnUser).catch(() => {}));
    if (id !== meta.hostId) {
      this.broadcastMembers();
      return;
    }
    // No host migration: the room ends with its host (the host carries everyone's race).
    meta.closed = true;
    await this.save();
    for (const [other] of this.members()) this.refuse(other, "host_left", "The host left: the room has ended.");
  }

  /** A binary PING answered with the server's clock; anything else binary is not for the server. */
  private pong(ws: WebSocket, info: Connection, data: ArrayBuffer, now: number): void {
    if (info.id === null || data.byteLength !== PING_BYTES || new Uint8Array(data)[0] !== FRAME_PING) return;
    const out = new Uint8Array(PING_BYTES + 8);
    out.set(new Uint8Array(data));
    out[0] = FRAME_PONG;
    new DataView(out.buffer).setFloat64(PING_BYTES, now, true);
    try { ws.send(out); } catch { /* closing */ }
  }

  /** The TURN key: Worker secrets, so not in the generated Env. */
  private secrets(): Env & SfuSecrets {
    return this.env as Env & SfuSecrets;
  }

  private save(): Promise<void> {
    return this.ctx.storage.put("meta", this.meta);
  }

  private info(ws: WebSocket): Connection | null {
    return (ws.deserializeAttachment() as Connection | null) ?? null;
  }

  private attach(ws: WebSocket, info: Connection): void {
    ws.serializeAttachment(info);
  }

  private members(): [WebSocket, Connection][] {
    const out: [WebSocket, Connection][] = [];
    for (const ws of this.ctx.getWebSockets()) {
      const info = this.info(ws);
      if (info?.id != null) out.push([ws, info]);
    }
    return out;
  }

  private roster(): { id: number; name: string }[] {
    return this.members().map(([, i]) => ({ id: i.id!, name: i.name }));
  }

  private broadcastMembers(): void {
    const members = this.roster();
    for (const [ws] of this.members()) this.send(ws, { type: "p2p_members", members });
  }

  private send(ws: WebSocket, m: unknown): void {
    try { ws.send(JSON.stringify(m)); } catch { /* closing */ }
  }

  private refuse(ws: WebSocket, code: keyof typeof CLOSE_CODES, message: string): void {
    this.send(ws, { type: "error", code, message });
    try { ws.close(CLOSE_CODES[code], code); } catch { /* already closed */ }
  }
}
