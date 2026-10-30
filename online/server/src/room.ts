// One lobby and its race. Lobby control is JSON; the race is binary frames relayed to the other
// racers with no game logic. Protocol: docs/online-protocol.md.
import { DurableObject } from "cloudflare:workers";
import { LISTING_REFRESH_MS, type LobbyEntry } from "./lobbies";
import {
  CLOSE_CODES,
  COUNTDOWN_MS,
  DEFAULT_MAX_PLAYERS,
  FEATURE_COUNTDOWN,
  FRAME_PING,
  FRAME_PONG,
  FRAME_STATE,
  FRAME_EVENT,
  HELLO_TIMEOUT_MS,
  OUTDATED_MESSAGE,
  MAX_BINARY_BYTES,
  MAX_MEMBERS,
  MAX_TEXT_BYTES,
  PING_INTERVAL_MS,
  PING_MAX_PAYLOAD,
  PROTOCOL,
  RACE_SILENT_MS,
  RTT_SAMPLES,
  START_LEAD_MS,
  type ErrorCode,
  clientVersionOf,
  isOutdated,
  isByte,
  isU32,
  measuredRtt,
  minVersionOf,
  randomSeed,
} from "./protocol";

/** A claimed room nobody joined is dropped after this long. */
const UNUSED_ROOM_MS = 60_000;

interface Member {
  id: number;
  name: string;
  ready: boolean;
  rider: number;
  bike: number;
  /** On the rider screen: not ready until they come back with a rider. */
  choosing?: boolean;
  /** The hello listed the "countdown" feature (COUNTDOWN_MS). */
  countdown?: boolean;
}

interface Race {
  /** slot -> member id */
  slots: number[];
  /** Slots whose racer left; their rider is AI now. */
  left: number[];
  track: number;
  laps: number;
  seed: number;
  startAt: number;
}

interface RoomState {
  code: string;
  createdAt: number;
  hostId: number | null;
  nextMemberId: number;
  /** The host's build and ISO; every joiner must match them. Null until the host's hello. */
  version: string | null;
  isoSha256: string | null;
  roomName: string;
  public: boolean;
  maxPlayers: number;
  track: number;
  laps: number;
  seed: number | null;
  /** Everyone is ready: the race starts by itself at this server time (COUNTDOWN_MS); null when
      no countdown runs. */
  countdownEndsAt?: number | null;
  /** In join order, so members[0] has been here longest. */
  members: Member[];
  race: Race | null;
}

/** Kept on each socket across hibernation. */
interface SocketInfo {
  memberId: number | null;
  connectedAt: number;
  create: boolean;
  /** Left or refused; only its close is still to come. */
  gone?: boolean;
  /** Two-letter country of the connection (Cloudflare's), for the usage stats only. */
  country?: string;
}

/** FNV-1a: a player name becomes a number, so the stats can count players without names. */
function nameHash(name: string): string {
  let h = 0x811c9dc5;
  for (let i = 0; i < name.length; i++) h = Math.imul(h ^ name.charCodeAt(i), 0x01000193);
  return (h >>> 0).toString(16);
}

type Msg = Record<string, unknown> & { type: string };

export class Room extends DurableObject<Env> {
  private state: RoomState | null = null;
  private sockets = new Map<number, WebSocket>();
  /** Round-trips from our pings, per member. Memory only; re-measured after a wake-up. */
  private rtts = new Map<number, number[]>();
  /** The first round-trip after connect, kept apart: see measuredRtt. */
  private firstRtts = new Map<number, number>();

  // Race relay state, rebuilt from `state.race` after a wake-up.
  private raceSockets: (WebSocket | null)[] = [];
  private slotOf = new Map<WebSocket, number>();
  /** When each slot last sent a frame (RACE_SILENT_MS); a wake-up starts every slot afresh. */
  private heardAt: number[] = [];

  private listedKey: string | null = null;
  private listedAt = 0;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(async () => {
      this.state = (await ctx.storage.get<RoomState>("room")) ?? null;
      for (const ws of ctx.getWebSockets()) {
        const info = ws.deserializeAttachment() as SocketInfo | null;
        if (info?.memberId != null) this.sockets.set(info.memberId, ws);
      }
      // After a wake-up mid-race the last relayed fields are unknown (see leave()).
      this.rebuildRace();
    });
  }

  /** Called by the Worker for a fresh code. False if the code is in use. */
  async claim(code: string): Promise<boolean> {
    if (this.state) return false;
    const now = Date.now();
    this.state = {
      code,
      createdAt: now,
      hostId: null,
      nextMemberId: 0,
      version: null,
      isoSha256: null,
      roomName: "",
      public: false,
      maxPlayers: DEFAULT_MAX_PLAYERS,
      track: 0,
      laps: 0,
      seed: null,
      members: [],
      race: null,
    };
    await this.save();
    await this.ctx.storage.setAlarm(now + PING_INTERVAL_MS);
    return true;
  }

  async fetch(request: Request): Promise<Response> {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return new Response("Expected a WebSocket upgrade", { status: 426 });
    const { 0: client, 1: server } = new WebSocketPair();
    this.ctx.acceptWebSocket(server);
    const info: SocketInfo = {
      memberId: null,
      connectedAt: Date.now(),
      create: request.headers.get("x-dhd-create") === "1",
      country: (request.cf?.country as string | undefined) ?? request.headers.get("cf-ipcountry") ?? "",
    };
    server.serializeAttachment(info);
    if ((await this.ctx.storage.getAlarm()) === null)
      await this.ctx.storage.setAlarm(Date.now() + PING_INTERVAL_MS);
    return new Response(null, { status: 101, webSocket: client });
  }

  // ---- WebSocket events ----

  async webSocketMessage(ws: WebSocket, msg: string | ArrayBuffer): Promise<void> {
    if (typeof msg !== "string") {
      this.relay(ws, msg);
      return;
    }
    if (msg.length > MAX_TEXT_BYTES) return this.refuse(ws, "bad_message", "Message too long.");
    let m: Msg;
    try {
      m = JSON.parse(msg);
    } catch {
      return this.refuse(ws, "bad_message", "Invalid JSON.");
    }
    if (typeof m !== "object" || m === null || typeof m.type !== "string")
      return this.refuse(ws, "bad_message", "Missing message type.");

    const info = ws.deserializeAttachment() as SocketInfo;
    if (info.gone) return;
    if (info.memberId === null) {
      if (m.type !== "hello") return this.refuse(ws, "bad_message", "Send hello first.");
      return this.onHello(ws, info, m);
    }
    const st = this.state;
    const me = st?.members.find((x) => x.id === info.memberId);
    if (!st || !me) return;
    const isHost = st.hostId === me.id;

    switch (m.type) {
      case "pong":
        return this.onPong(me.id, m.s);
      case "ping":
        this.send(ws, { type: "pong", t: m.t, serverTime: Date.now() });
        return;
      case "settings":
        if (!isHost) return this.error(ws, "not_host", "Only the host can change the settings.");
        if (st.race) return this.error(ws, "room_started", "The race is running.");
        return this.onSettings(ws, st, m);
      case "pick":
        return this.onPick(ws, st, me, isHost, m);
      case "ready":
        if (st.race) return this.error(ws, "room_started", "The race is running.");
        if (m.ready === true && me.choosing) return this.error(ws, "not_ready", "Choose your rider first.");
        me.ready = m.ready === true;
        return this.changed();
      case "kick": {
        if (!isHost) return this.error(ws, "not_host", "Only the host can remove players.");
        const target = st.members.find((x) => x.id === m.member);
        if (!target || target.id === me.id) return this.error(ws, "bad_value", "No such player.");
        const tws = this.sockets.get(target.id);
        await this.leave(target.id, "kicked");
        if (tws) this.close(tws, "kicked", "The host removed you from the room.");
        return;
      }
      case "start":
        if (!isHost) return this.error(ws, "not_host", "Only the host can start the race.");
        return this.onStart(ws, st);
      case "end":
        if (!isHost) return this.error(ws, "not_host", "Only the host can end the race.");
        if (!st.race) return;
        st.race = null;
        st.countdownEndsAt = null;
        for (const x of st.members) x.ready = false;
        this.rebuildRace();
        this.broadcast({ type: "ended" });
        return this.changed();
      case "leave":
        await this.leave(me.id, "leave");
        try {
          ws.close(1000, "leave");
        } catch {}
        return;
      default:
        return this.refuse(ws, "bad_message", `Unknown message type "${m.type}".`);
    }
  }

  async webSocketClose(ws: WebSocket): Promise<void> {
    await this.dropSocket(ws);
    try {
      ws.close(1000);
    } catch {}
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.dropSocket(ws);
  }

  async alarm(): Promise<void> {
    const now = Date.now();
    let pending = 0;
    for (const ws of this.ctx.getWebSockets()) {
      const info = ws.deserializeAttachment() as SocketInfo | null;
      if (info && info.memberId === null && !info.gone) {
        if (now - info.connectedAt > HELLO_TIMEOUT_MS)
          this.close(ws, "hello_timeout", "No hello received.");
        else pending++;
      }
    }
    const st = this.state;
    if (!st) {
      if (pending) await this.ctx.storage.setAlarm(now + PING_INTERVAL_MS);
      return;
    }
    // The countdown's end. Alarms fire at or after their time; a little early still counts.
    if (st.countdownEndsAt != null && now >= st.countdownEndsAt - 20 && !st.race) {
      if (this.countdownReady(st)) await this.startRace(st, "");
      else st.countdownEndsAt = null;
    }
    if (st.race) {
      await this.dropSilent(now);
      if (!this.state) return;  // the last racer went: the room is gone
    }
    if (st.members.length === 0) {
      if (pending === 0 && now - st.createdAt > UNUSED_ROOM_MS) return this.destroy();
    } else if (!st.race) {
      const ping = JSON.stringify({ type: "ping", s: now });
      for (const ws of this.sockets.values()) this.sendRaw(ws, ping);
      this.broadcastRoom();
      if (now - this.listedAt > LISTING_REFRESH_MS) this.updateListing(true);
    }
    await this.ctx.storage.setAlarm(this.nextAlarm(st, now));
  }

  /** The next ping, or the countdown's end if that is sooner. */
  private nextAlarm(st: RoomState, now: number): number {
    const ping = now + PING_INTERVAL_MS;
    return st.countdownEndsAt != null && !st.race ? Math.min(ping, Math.max(now, st.countdownEndsAt)) : ping;
  }

  // ---- Race relay (hot path: no awaits, no allocation beyond the frame) ----

  /** Usage stats: one Workers Analytics Engine point (dataset dhd_online; wrangler.jsonc STATS).
      Joins carry the release, the country and a hash of the name; races the player count, track
      and laps. No addresses or names. A server without the binding keeps none. */
  private stat(blobs: string[], doubles: number[]): void {
    try {
      this.env.STATS?.writeDataPoint({ indexes: [blobs[0]], blobs, doubles });
    } catch {}
  }

  private relay(ws: WebSocket, data: ArrayBuffer): void {
    let slot = this.slotOf.get(ws);
    if (slot === undefined) {
      slot = this.findSlot(ws);
      if (slot === undefined) return; // not racing: drop
    }
    const n = data.byteLength;
    if (n < 2 || n > MAX_BINARY_BYTES) return;
    this.heardAt[slot] = Date.now();
    const b = new Uint8Array(data);
    b[1] = slot;
    switch (b[0]) {
      case FRAME_STATE:
      case FRAME_EVENT: {
        const socks = this.raceSockets;
        for (let i = 0; i < socks.length; i++) {
          const s = socks[i];
          if (s !== null && s !== ws) {
            try {
              s.send(data);
            } catch {}
          }
        }
        return;
      }
      case FRAME_PING:
        if (n > 2 + PING_MAX_PAYLOAD) return;
        b[0] = FRAME_PONG;
        try {
          ws.send(data);
        } catch {}
        return;
    }
  }

  private findSlot(ws: WebSocket): number | undefined {
    const race = this.state?.race;
    const info = ws.deserializeAttachment() as SocketInfo | null;
    if (!race || info?.memberId == null) return undefined;
    const slot = race.slots.indexOf(info.memberId);
    if (slot < 0 || race.left.includes(slot)) return undefined;
    this.slotOf.set(ws, slot);
    this.raceSockets[slot] = ws;
    return slot;
  }

  private rebuildRace(): void {
    const race = this.state?.race;
    this.slotOf.clear();
    if (!race) {
      this.raceSockets = [];
      this.heardAt = [];  // the next race's slots start afresh
      return;
    }
    this.raceSockets = race.slots.map((id, slot) => {
      const ws = race.left.includes(slot) ? null : (this.sockets.get(id) ?? null);
      if (ws) this.slotOf.set(ws, slot);
      return ws;
    });
    const now = Date.now();
    this.heardAt = race.slots.map((_, slot) => this.heardAt[slot] ?? now);
  }

  private raceSilentMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).RACE_SILENT_MS);
    return Number.isFinite(v) && v > 0 ? v : RACE_SILENT_MS;
  }

  /** Racers whose socket went quiet without a close (a lost connection the platform hasn't noticed
      yet) leave as if it had closed: the others hand their rider to the host's AI. */
  private async dropSilent(now: number): Promise<void> {
    const race = this.state?.race;
    if (!race) return;
    const limit = this.raceSilentMs();
    const silent = race.slots.filter((_, slot) => this.raceSockets[slot] && now - (this.heardAt[slot] ?? now) > limit);
    for (const id of silent) {
      const ws = this.sockets.get(id);
      await this.leave(id, "timeout");
      if (ws)
        try {
          ws.close(1001, "timeout");
        } catch {}
    }
  }

  // ---- Lobby ----

  private async onHello(ws: WebSocket, info: SocketInfo, m: Msg): Promise<void> {
    if (m.protocol !== PROTOCOL)
      return this.refuse(ws, "protocol_mismatch", `This server speaks protocol ${PROTOCOL}; update the game.`);
    // The version gate: releases older than MIN_VERSION (wrangler.jsonc) may not play.
    const minVersion = minVersionOf(this.env.MIN_VERSION);
    const clientVersion = clientVersionOf(m);
    if (isOutdated(clientVersion, minVersion))
      return this.refuse(ws, "client_outdated", OUTDATED_MESSAGE, { minVersion, clientVersion });
    const version = m.version;
    const iso = m.isoSha256;
    const name = typeof m.name === "string" ? m.name.trim() : "";
    if (typeof version !== "string" || version.length < 1 || version.length > 64)
      return this.refuse(ws, "bad_message", "hello.version must be 1-64 characters.");
    if (typeof iso !== "string" || !/^[0-9a-f]{64}$/.test(iso))
      return this.refuse(ws, "bad_message", "hello.isoSha256 must be 64 lowercase hex digits.");
    if (name.length < 1 || name.length > 24)
      return this.refuse(ws, "bad_message", "hello.name must be 1-24 characters.");

    const st = this.state;
    const asHost = info.create && st !== null && st.version === null && st.members.length === 0;
    if (!st || (!asHost && st.version === null))
      return this.refuse(ws, "room_not_found", "No room has this code.");
    if (!asHost) {
      if (st.race) return this.refuse(ws, "room_started", "The race has already started.");
      if (version !== st.version)
        return this.refuse(ws, "version_mismatch", `The host runs "${st.version}", you run "${version}".`);
      if (iso !== st.isoSha256)
        return this.refuse(ws, "iso_mismatch", "Your game ISO does not match the host's.");
      if (st.members.length >= st.maxPlayers) return this.refuse(ws, "room_full", "The room is full.");
    } else {
      st.version = version;
      st.isoSha256 = iso;
      st.roomName = `${name}'s room`;
    }

    const features = Array.isArray(m.features) ? m.features : [];
    const member: Member = {
      id: st.nextMemberId++,
      name,
      ready: false,
      rider: 0,
      bike: 0,
      countdown: features.includes(FEATURE_COUNTDOWN),
    };
    st.members.push(member);
    if (asHost) st.hostId = member.id;
    info.memberId = member.id;
    ws.serializeAttachment(info);
    this.sockets.set(member.id, ws);
    this.stat(["join", clientVersion, info.country ?? "", nameHash(name), asHost ? "host" : "guest"], [st.members.length]);
    this.send(ws, {
      type: "welcome",
      protocol: PROTOCOL,
      code: st.code,
      memberId: member.id,
      host: asHost,
      serverTime: Date.now(),
    });
    // First ping right away so the lobby shows a delay quickly.
    this.send(ws, { type: "ping", s: Date.now() });
    await this.changed();
  }

  private onPong(memberId: number, s: unknown): void {
    if (typeof s !== "number") return;
    const rtt = Date.now() - s;
    if (!(rtt >= 0 && rtt <= 30_000)) return;
    if (!this.firstRtts.has(memberId)) {
      this.firstRtts.set(memberId, rtt);
      return;
    }
    let list = this.rtts.get(memberId);
    if (!list) this.rtts.set(memberId, (list = []));
    list.push(rtt);
    if (list.length > RTT_SAMPLES) list.shift();
  }

  private async onSettings(ws: WebSocket, st: RoomState, m: Msg): Promise<void> {
    const bad = (what: string) => this.error(ws, "bad_value", `Invalid ${what}.`);
    if (m.track !== undefined && !isByte(m.track)) return bad("track");
    if (m.laps !== undefined && !isByte(m.laps)) return bad("laps");
    if (m.seed !== undefined && m.seed !== null && !isU32(m.seed)) return bad("seed");
    if (m.public !== undefined && typeof m.public !== "boolean") return bad("public");
    if (m.roomName !== undefined && (typeof m.roomName !== "string" || m.roomName.length > 32))
      return bad("roomName");
    if (m.maxPlayers !== undefined) {
      const mp = m.maxPlayers;
      if (!Number.isInteger(mp) || (mp as number) < 2 || (mp as number) > MAX_MEMBERS) return bad("maxPlayers");
      if ((mp as number) < st.members.length) return bad("maxPlayers (below the player count)");
    }
    // A new track or new options make a different race: everyone readies again for it.
    if ((m.track !== undefined && m.track !== st.track) || (m.laps !== undefined && m.laps !== st.laps))
      for (const x of st.members) x.ready = false;
    if (m.track !== undefined) st.track = m.track as number;
    if (m.laps !== undefined) st.laps = m.laps as number;
    if (m.seed !== undefined) st.seed = m.seed as number | null;
    if (m.public !== undefined) st.public = m.public as boolean;
    if (m.roomName !== undefined) st.roomName = (m.roomName as string).trim();
    if (m.maxPlayers !== undefined) st.maxPlayers = m.maxPlayers as number;
    await this.changed();
  }

  private async onPick(ws: WebSocket, st: RoomState, me: Member, isHost: boolean, m: Msg): Promise<void> {
    if (st.race) return this.error(ws, "room_started", "The race is running.");
    const target = m.member === undefined ? me : st.members.find((x) => x.id === m.member);
    if (!target) return this.error(ws, "bad_value", "No such player.");
    if (target !== me && !isHost) return this.error(ws, "not_host", "Only the host can pick for others.");
    if (m.rider !== undefined && !isByte(m.rider)) return this.error(ws, "bad_value", "Invalid rider.");
    if (m.bike !== undefined && !isByte(m.bike)) return this.error(ws, "bad_value", "Invalid bike.");
    if (m.choosing !== undefined && typeof m.choosing !== "boolean")
      return this.error(ws, "bad_value", "Invalid choosing.");
    if (m.rider !== undefined) target.rider = m.rider as number;
    if (m.bike !== undefined) target.bike = m.bike as number;
    if (m.choosing !== undefined) target.choosing = m.choosing as boolean;
    if (target.choosing) target.ready = false;
    await this.changed();
  }

  private async onStart(ws: WebSocket, st: RoomState): Promise<void> {
    if (st.race) return this.error(ws, "room_started", "The race is already running.");
    // As the countdown and the client's Start: a race needs two members.
    if (st.members.length < 2) return this.error(ws, "not_ready", "Need 2 players to start.");
    const waiting = st.members.filter((x) => !x.ready);
    if (waiting.length)
      return this.error(ws, "not_ready", `Not ready: ${waiting.map((x) => x.name).join(", ")}.`);
    const hostInfo = ws.deserializeAttachment() as SocketInfo | null;
    await this.startRace(st, hostInfo?.country ?? "");
  }

  /** Everyone can race now, and every client shows the countdown. */
  private countdownReady(st: RoomState): boolean {
    return (
      !st.race &&
      st.members.length >= 2 &&
      st.members.every((x) => x.ready && !x.choosing && x.countdown)
    );
  }

  private countdownMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).COUNTDOWN_MS);
    return Number.isFinite(v) && v >= 0 ? v : COUNTDOWN_MS;
  }

  /** Starts or stops the countdown after a change; true when it started now. */
  private updateCountdown(st: RoomState): boolean {
    if (!this.countdownReady(st)) {
      st.countdownEndsAt = null;
      return false;
    }
    if (st.countdownEndsAt != null) return false;
    st.countdownEndsAt = Date.now() + this.countdownMs();
    return true;
  }

  private async startRace(st: RoomState, country: string): Promise<void> {
    st.countdownEndsAt = null;

    // Host first, then join order.
    const racers = [...st.members].sort((a, b) => Number(b.id === st.hostId) - Number(a.id === st.hostId));
    const now = Date.now();
    st.race = {
      slots: racers.map((x) => x.id),
      left: [],
      track: st.track,
      laps: st.laps,
      seed: st.seed ?? randomSeed(),
      startAt: now + START_LEAD_MS,
    };
    this.heardAt = [];  // a new race: every racer starts heard now
    this.rebuildRace();
    await this.save();
    const race = st.race;
    this.stat(["race", "", country], [racers.length, race.track, race.laps]);
    const players = racers.map((x, slot) => ({ slot, memberId: x.id, name: x.name, rider: x.rider, bike: x.bike }));
    racers.forEach((x, slot) => {
      const s = this.sockets.get(x.id);
      if (s)
        this.send(s, {
          type: "start",
          yourSlot: slot,
          track: race.track,
          laps: race.laps,
          seed: race.seed,
          players,
          serverTime: now,
          startAt: race.startAt,
        });
    });
    this.broadcastRoom();
    this.updateListing();
  }

  private async dropSocket(ws: WebSocket): Promise<void> {
    const info = ws.deserializeAttachment() as SocketInfo | null;
    if (info?.memberId != null) await this.leave(info.memberId, "disconnect");
  }

  /** Removes a member (idempotent). A racer's rider goes to the host's AI (the game decides). */
  private async leave(memberId: number, reason: "disconnect" | "leave" | "kicked" | "timeout"): Promise<void> {
    const st = this.state;
    if (!st) return;
    const i = st.members.findIndex((x) => x.id === memberId);
    if (i < 0) return;
    st.members.splice(i, 1);
    const ws = this.sockets.get(memberId);
    this.sockets.delete(memberId);
    this.rtts.delete(memberId);
    this.firstRtts.delete(memberId);
    if (ws) {
      // Forget the socket so a late close event doesn't count twice.
      ws.serializeAttachment({ ...(ws.deserializeAttachment() as SocketInfo), memberId: null, gone: true });
      this.slotOf.delete(ws);
    }
    const race = st.race;
    if (race) {
      const slot = race.slots.indexOf(memberId);
      if (slot >= 0 && !race.left.includes(slot)) {
        race.left.push(slot);
        this.raceSockets[slot] = null;
        this.broadcast({ type: "left", slot, memberId, reason });
      }
    }
    if (st.hostId === memberId) st.hostId = st.members[0]?.id ?? null;
    if (st.members.length === 0) return this.destroy();
    await this.changed();
  }

  private async destroy(): Promise<void> {
    if (this.listedKey !== null) this.updateListing(false, true);
    this.state = null;
    this.rebuildRace();
    await this.ctx.storage.deleteAlarm();
    await this.ctx.storage.deleteAll();
    // Sockets still waiting for hello get room_not_found then; keep their timeout running.
    if (this.ctx.getWebSockets().length) await this.ctx.storage.setAlarm(Date.now() + PING_INTERVAL_MS);
  }

  // ---- Helpers ----

  private rttOf(memberId: number): number | null {
    return measuredRtt(this.firstRtts.get(memberId) ?? null, this.rtts.get(memberId) ?? []);
  }

  private async changed(): Promise<void> {
    const st = this.state;
    if (st && this.updateCountdown(st)) {
      const at = await this.ctx.storage.getAlarm();
      if (at === null || at > st.countdownEndsAt!) await this.ctx.storage.setAlarm(st.countdownEndsAt!);
    }
    await this.save();
    this.broadcastRoom();
    this.updateListing();
  }

  private async save(): Promise<void> {
    if (this.state) await this.ctx.storage.put("room", this.state);
  }

  private snapshot(st: RoomState) {
    return {
      type: "room",
      code: st.code,
      hostId: st.hostId,
      roomName: st.roomName,
      public: st.public,
      maxPlayers: st.maxPlayers,
      started: st.race !== null,
      version: st.version,
      settings: { track: st.track, laps: st.laps, seed: st.seed },
      countdownEndsAt: st.countdownEndsAt ?? null,
      serverTime: Date.now(),
      members: st.members.map((x) => ({
        id: x.id,
        name: x.name,
        ready: x.ready,
        rider: x.rider,
        bike: x.bike,
        rttMs: this.rttOf(x.id),
        choosing: x.choosing ?? false,
      })),
    };
  }

  private broadcastRoom(): void {
    if (this.state) this.broadcast(this.snapshot(this.state));
  }

  /** Keeps this room's entry in the public lobby list current. */
  private updateListing(refresh = false, forceRemove = false): void {
    const st = this.state;
    let entry: LobbyEntry | null = null;
    if (!forceRemove && st && st.public && !st.race && st.hostId !== null && st.members.length < st.maxPlayers) {
      entry = {
        code: st.code,
        roomName: st.roomName,
        version: st.version ?? "",
        track: st.track,
        players: st.members.length,
        maxPlayers: st.maxPlayers,
        hostName: st.members.find((x) => x.id === st.hostId)?.name ?? "",
      };
    }
    const key = entry ? JSON.stringify(entry) : null;
    if (key === this.listedKey && !(refresh && entry)) return;
    const index = this.env.LOBBIES.get(this.env.LOBBIES.idFromName("global"));
    const code = entry?.code ?? this.listedCode();
    const p = entry ? index.upsert(entry) : code ? index.remove(code) : Promise.resolve();
    this.listedKey = key;
    this.listedAt = Date.now();
    this.ctx.waitUntil(p.catch((e) => console.error("lobby index update failed", e)));
  }

  private listedCode(): string | null {
    return this.listedKey ? (JSON.parse(this.listedKey) as LobbyEntry).code : null;
  }

  private broadcast(obj: object): void {
    const text = JSON.stringify(obj);
    for (const ws of this.sockets.values()) this.sendRaw(ws, text);
  }

  private send(ws: WebSocket, obj: object): void {
    this.sendRaw(ws, JSON.stringify(obj));
  }

  private sendRaw(ws: WebSocket, text: string): void {
    try {
      ws.send(text);
    } catch {}
  }

  /** Non-fatal error: the socket stays open. */
  private error(ws: WebSocket, code: ErrorCode, message: string): void {
    this.send(ws, { type: "error", code, message });
  }

  /** Fatal error: error frame (with any extra fields), then close. */
  private async refuse(ws: WebSocket, code: keyof typeof CLOSE_CODES, message: string, extra: object = {}): Promise<void> {
    await this.dropSocket(ws);
    this.close(ws, code, message, extra);
  }

  private close(ws: WebSocket, code: keyof typeof CLOSE_CODES, message: string, extra: object = {}): void {
    const info = ws.deserializeAttachment() as SocketInfo | null;
    if (info && !info.gone) ws.serializeAttachment({ ...info, gone: true });
    this.send(ws, { type: "error", code, message, ...extra });
    try {
      ws.close(CLOSE_CODES[code], code);
    } catch {}
  }
}
