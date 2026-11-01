// One lobby and its race. Lobby control is JSON; the race is binary frames relayed to the other
// racers with no game logic. Protocol: docs/online-protocol.md.
import { DurableObject } from "cloudflare:workers";
import { LISTING_REFRESH_MS, type LobbyEntry } from "./lobbies";
import {
  CHAT_BURST,
  CHAT_WINDOW_MS,
  CLOSE_CODES,
  COUNTDOWN_MS,
  DEFAULT_MAX_PLAYERS,
  FEATURE_COUNTDOWN,
  FEATURE_RECONNECT,
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
  RECONNECT_WINDOW_MS,
  RTT_SAMPLES,
  START_LEAD_MS,
  type ErrorCode,
  chatText,
  clientVersionOf,
  isReconnectToken,
  isOutdated,
  isByte,
  isU32,
  measuredRtt,
  minVersionOf,
  randomSeed,
  reconnectToken,
  tokensEqual,
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
  /** False while the socket is gone and the slot is held for a reconnect (RECONNECT_WINDOW_MS).
      A member stored before this feature has no field; undefined counts as connected. */
  online?: boolean;
  /** The secret a reconnect must present. Only a member whose hello listed the "reconnect"
      feature has one; without it the slot is never held (older clients behave as before). */
  token?: string;
  /** When the socket went; the reconnect window runs from here. */
  detachedAt?: number;
}

interface Rules { finishWindow: number; nextRace: "auto" | "host"; then: "same" | "next" | "random"; }
interface Result { id: number; name: string; rider: number; ticks?: number; place: number; dnfReason?: string; }
interface Race { 
  rules?: Rules;
  results?: Result[];
  finishEndsAt?: number | null;
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
  session?: boolean;
  rules?: Rules;
  nextExplicit?: boolean;
  rotation?: number[];
  trackLimits?: number[];
  grid?: number[];
  gridRules?: Rules;
  nextRaceAt?: number | null;
  waitingForRider?: boolean;
  lastEvent?: string | null;
  raceId?: number;
  lastRace?: { id: number; track: number; entrants: Result[] };
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

/** Bound JSON escaping without changing valid UTF-16 names. */
function sessionName(s: string): string {
  return s.replace(/[\u0000-\u001f]|[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/g, "\ufffd");
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
  /** When each member's recent chat messages went out (CHAT_BURST in CHAT_WINDOW_MS). */
  private chatTimes = new Map<number, number[]>();
  /** When each slot last sent a frame (RACE_SILENT_MS); a wake-up starts every slot afresh. */
  private heardAt: number[] = [];

  private listedKey: string | null = null;
  private listedAt = 0;
  /** This instance's start. Members then without a socket are a restart's ghosts (see onHello);
      those not back within HELLO_TIMEOUT_MS leave as disconnects (alarm). */
  private readonly startedAt = Date.now();

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(async () => {
      this.state = (await ctx.storage.get<RoomState>("room")) ?? null;
      for (const ws of ctx.getWebSockets()) {
        const info = ws.deserializeAttachment() as SocketInfo | null;
        if (info?.memberId != null) this.sockets.set(info.memberId, ws);
      }
      // A member whose socket the platform dropped while this object slept is not here: treat it
      // as a drop (finding 140) so it can reconnect or runs out, instead of lingering in the room
      // forever. A member stored before this feature has no `online` field and counts as
      // connected.
      if (this.state) {
        let held = false;
        for (const m of this.state.members) {
          if (m.online !== false && !this.sockets.has(m.id)) {
            m.online = false;
            m.detachedAt = m.detachedAt ?? Date.now();
            held = true;
            const slot = this.state.race?.slots.indexOf(m.id) ?? -1;
            if (slot >= 0 && !this.state.race!.left.includes(slot))
              this.broadcast({ type: "away", slot, memberId: m.id });
          }
        }
        if (held) {
          await this.save();
          await this.armExpiry();
        }
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
        if (st.race && (!st.session || (this.inRace(st, me.id) && st.race.results!.find(x => x.id === me.id)?.ticks === undefined))) return this.error(ws, "room_started", "The race is running.");
        return this.onSettings(ws, st, m);
      case "pick":
        return this.onPick(ws, st, me, isHost, m);
      case "ready":
        if (st.race && (!st.session || this.inRace(st, me.id))) return this.error(ws, "room_started", "The race is running.");
        if (m.ready === true && me.choosing) return this.error(ws, "not_ready", "Choose your rider first.");
        me.ready = m.ready === true;
        if (st.session && st.grid && !me.ready) st.grid = st.grid.filter(id => id !== me.id);
        if (st.session && st.waitingForRider && me.ready) {
          st.waitingForRider = false;
          st.nextRaceAt = Date.now() + 30_000;
        }
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
      case "finish":
        if (!st.session) return this.refuse(ws, "bad_message", "Unknown message type.");
        if (!isU32(m.ticks) || !Number.isInteger(m.place) || (m.place as number) < 1 || (m.place as number) > 255)
          return this.error(ws, "bad_value", "Invalid finish.");
        if (st.race && this.inRace(st, me.id)) {
          const result = st.race.results!.find(x => x.id === me.id)!;
          if (result.ticks === undefined && !result.dnfReason) {
            result.ticks = m.ticks; result.place = m.place as number;
            if (st.race.rules!.finishWindow && st.race.finishEndsAt == null)
              st.race.finishEndsAt = Date.now() + st.race.rules!.finishWindow * 1000;
          }
          if (!this.anyRacing(st)) await this.closeRace(st, false);
          await this.changed();
        }
        return;
      case "back":
        if (!st.session) return this.refuse(ws, "bad_message", "Unknown message type.");
        this.retire(st, me.id, "gave_up");
        if (st.race && !this.anyRacing(st)) await this.closeRace(st, false);
        return this.changed();
      case "end":
        if (!isHost) return this.error(ws, "not_host", "Only the host can end the race.");
        if (!st.race) return;
        if (st.session) { await this.closeRace(st, true); return this.changed(); }
        st.race = null;
        st.countdownEndsAt = null;
        for (const x of st.members) x.ready = false;
        this.rebuildRace();
        this.broadcast({ type: "ended" });
        return this.changed();
      case "chat": {
        // In the room and in a race alike: everyone in the room gets it, the sender too.
        const text = chatText(m.text);
        if (text === null) return this.error(ws, "bad_value", "A chat message is 1-120 characters.");
        const now = Date.now();
        const recent = (this.chatTimes.get(me.id) ?? []).filter((t) => now - t < CHAT_WINDOW_MS);
        if (recent.length >= CHAT_BURST) return this.error(ws, "slow_down", "Too many messages: wait a moment.");
        recent.push(now);
        this.chatTimes.set(me.id, recent);
        this.broadcast({ type: "chat", memberId: me.id, name: me.name, text, at: now });
        return;
      }
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
    if (this.state && now - this.startedAt > HELLO_TIMEOUT_MS)
      for (const x of [...this.state.members]) if (!this.sockets.has(x.id)) await this.leave(x.id, "disconnect");
    const st = this.state;
    if (!st) {
      if (pending) await this.ctx.storage.setAlarm(now + PING_INTERVAL_MS);
      return;
    }
    if (st.session) {
      if (st.race?.finishEndsAt != null && now >= st.race.finishEndsAt) {
        for (const id of st.race.slots) this.retire(st, id, "time_up");
        await this.closeRace(st, false);
        await this.changed();
      }
      if (!st.race && st.nextRaceAt != null && now >= st.nextRaceAt) {
        st.nextRaceAt = null;
        if (st.members.some(x => x.ready && !x.choosing)) this.lockGrid(st);
        else st.waitingForRider = true;
        await this.changed();
      }
    }
    // The countdown's end. Alarms fire at or after their time; a little early still counts.
    if (st.countdownEndsAt != null && now >= st.countdownEndsAt - 20 && !st.race) {
      if (st.session ? !!st.grid?.length : this.countdownReady(st)) await this.startRace(st, "");
      else st.countdownEndsAt = null;
    }
    // A held slot whose reconnect window has run out becomes a normal one (finding 140).
    await this.expireDetached(now);
    if (!this.state) return;  // the last member went: the room is gone
    if (st.race) {
      await this.dropSilent(now);
      if (!this.state) return;  // the last racer went: the room is gone
    }
    if (st.members.length === 0) {
      if (pending === 0 && now - st.createdAt > UNUSED_ROOM_MS) return this.destroy();
    } else if (!st.race || st.session) {
      const ping = JSON.stringify({ type: "ping", s: now });
      for (const ws of this.sockets.values()) this.sendRaw(ws, ping);
      this.broadcastRoom();
      if (now - this.listedAt > LISTING_REFRESH_MS) this.updateListing(true);
    }
    await this.ctx.storage.setAlarm(this.nextAlarm(st, now));
  }

  /** The next ping, the countdown's end or a held slot's expiry, whichever is sooner. */
  private nextAlarm(st: RoomState, now: number): number {
    let next = now + PING_INTERVAL_MS;
    // A session room wakes for its countdown, its next race and the finish window; an older room
    // for its countdown before the race. Both for a held slot's expiry (finding 140).
    const times = st.session ? [st.countdownEndsAt, st.nextRaceAt, st.race?.finishEndsAt]
      : [st.countdownEndsAt != null && !st.race ? st.countdownEndsAt : null];
    times.push(this.earliestExpiry());
    for (const t of times) if (t != null) next = Math.min(next, Math.max(now, t));
    return next;
  }

  // ---- Race relay (hot path: no awaits, no allocation beyond the frame) ----

  /** Usage stats: one Workers Analytics Engine point (dataset dhd_online; wrangler.jsonc STATS).
      Joins carry the release, the country and a hash of the name; races the player count, track
      and laps; a join refused for a full room the member count and the limit. No addresses or names.
      A server without the binding keeps none. */
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
      const me = this.state?.members.find((x) => x.id === id);
      // A member that can rejoin is held (`away`), as if its socket had closed (finding 140).
      if (ws && me?.token && this.state) await this.detach(this.state, me, ws, ws.deserializeAttachment() as SocketInfo);
      else await this.leave(id, "timeout");
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
    const session = this.state?.session || (info.create && Array.isArray(m.features) && m.features.includes("session"));
    const clean = (s: string) => session ? sessionName(s) : s;
    const name = typeof m.name === "string" ? clean(m.name).trim() : "";
    if (typeof version !== "string" || version.length < 1 || version.length > 64)
      return this.refuse(ws, "bad_message", "hello.version must be 1-64 characters.");
    if (typeof iso !== "string" || !/^[0-9a-f]{64}$/.test(iso))
      return this.refuse(ws, "bad_message", "hello.isoSha256 must be 64 lowercase hex digits.");
    if (name.length < 1 || name.length > 24)
      return this.refuse(ws, "bad_message", "hello.name must be 1-24 characters.");

    const st = this.state;
    const features = Array.isArray(m.features) ? m.features : [];

    // A dropped client that declared the "reconnect" feature comes back with the secret token its
    // welcome carried (finding 140). Re-attach it to the slot held for it; a missing, wrong or
    // expired token finds nothing and falls through to the normal join below, so it can never
    // impersonate the member or take over a member that is still connected.
    const token = m.reconnectToken;
    if (st && isReconnectToken(token)) {
      const held = this.findReconnect(token);
      if (held) {
        if (version !== st.version)
          return this.refuse(ws, "version_mismatch", `The host runs "${st.version}", you run "${version}".`);
        if (iso !== st.isoSha256)
          return this.refuse(ws, "iso_mismatch", "Your game ISO does not match the host's.");
        const fresh = reconnectToken();  // rotate: the token that came back is spent
        held.online = true;
        held.detachedAt = undefined;
        held.token = fresh;
        info.memberId = held.id;
        ws.serializeAttachment(info);
        this.sockets.set(held.id, ws);
        this.rtts.delete(held.id);
        this.firstRtts.delete(held.id);
        // Back in a race: the same race slot, relayed both ways again from now on (finding 140).
        const slot = st.race ? st.race.slots.indexOf(held.id) : -1;
        if (slot >= 0) {
          this.raceSockets[slot] = ws;
          this.slotOf.set(ws, slot);
          this.heardAt[slot] = Date.now();
        }
        this.stat(["reconnect", clientVersion, info.country ?? "", nameHash(held.name), slot >= 0 ? "race" : ""], [st.members.length]);
        this.send(ws, {
          type: "welcome",
          protocol: PROTOCOL,
          code: st.code,
          memberId: held.id,
          host: st.hostId === held.id,
          serverTime: Date.now(),
          chat: true,  // this server takes chat (an older one closes on it)
          reconnectToken: fresh,
          // Still racing in this slot. Without it the race the client left has ended meanwhile.
          ...(slot >= 0 ? { slot } : {}),
        });
        if (slot >= 0) this.broadcast({ type: "back", slot, memberId: held.id });
        else this.send(ws, { type: "ping", s: Date.now() });
        await this.changed();
        return;
      }
    }

    const asHost = info.create && st !== null && st.version === null && st.members.length === 0;
    if (!st || (!asHost && st.version === null))
      return this.refuse(ws, "room_not_found", "No room has this code.");
    // Back after a restart (a deploy drops every socket and no close event comes): a member left
    // without a socket under this name is theirs again, host, rider and Ready included.
    const ghost = asHost ? undefined : st.members.find((x) => x.name === name && !this.sockets.has(x.id));
    if (!asHost) {
      if (st.race && !st.session) return this.refuse(ws, "room_started", "The race has already started.");
      if (version !== st.version)
        return this.refuse(ws, "version_mismatch", `The host runs "${st.version}", you run "${version}".`);
      if (iso !== st.isoSha256)
        return this.refuse(ws, "iso_mismatch", "Your game ISO does not match the host's.");
      if (!ghost && st.members.length >= st.maxPlayers) {
        // counted: how often players find a room full (are bigger rooms wanted?)
        this.stat(["full", clientVersion, info.country ?? ""], [st.members.length, st.maxPlayers]);
        return this.refuse(ws, "room_full", "The room is full.");
      }
    } else {
      st.session = Array.isArray(m.features) && m.features.includes("session");
      if (st.session) { st.rules = {finishWindow: 60, nextRace: "host", then: "next"}; st.rotation = []; }
      st.version = version;
      st.isoSha256 = iso;
      st.roomName = `${name}'s room`;
    }

    const supportsReconnect = features.includes(FEATURE_RECONNECT);
    const member: Member = ghost ?? {
      id: st.nextMemberId++,
      name,
      ready: false,
      rider: 0,
      bike: 0,
      countdown: features.includes(FEATURE_COUNTDOWN),
      // Only a client that can reconnect gets a token (and so a held slot); every older client
      // keeps the old behavior: removed the moment its socket goes.
      token: supportsReconnect ? reconnectToken() : undefined,
    };
    if (!ghost) st.members.push(member);
    if (asHost) st.hostId = member.id;
    info.memberId = member.id;
    ws.serializeAttachment(info);
    this.sockets.set(member.id, ws);
    if (ghost) this.rebuildRace();
    else this.stat(["join", clientVersion, info.country ?? "", nameHash(name), asHost ? "host" : "guest"], [st.members.length]);
    this.send(ws, {
      type: "welcome",
      protocol: PROTOCOL,
      code: st.code,
      memberId: member.id,
      host: st.hostId === member.id,
      serverTime: Date.now(),
      chat: true,  // this server takes chat (an older one closes on it)
      ...(member.token ? { reconnectToken: member.token } : {}),
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
    if (st.session) {
      if (m.finishWindow !== undefined && ![0,30,60,120].includes(m.finishWindow as number)) return bad("finishWindow");
      if (m.nextRace !== undefined && !["auto","host"].includes(m.nextRace as string)) return bad("nextRace");
      if (m.then !== undefined && !["same","next","random"].includes(m.then as string)) return bad("then");
      if (m.rotation !== undefined && (!Array.isArray(m.rotation) || m.rotation.length > 64 || !m.rotation.every(isByte))) return bad("rotation");
      if (m.rotation !== undefined && (!Array.isArray(m.trackLimits) ||
          m.trackLimits.length !== (m.rotation as number[]).length ||
          !m.trackLimits.every(x => x === 6 || x === 8 || x === 10) ||
          !m.trackLimits.includes(10))) return bad("trackLimits");
      if (m.finishWindow !== undefined) st.rules!.finishWindow = m.finishWindow as number;
      if (m.then !== undefined) st.rules!.then = m.then as Rules["then"];
      if (m.nextRace !== undefined) { st.rules!.nextRace = m.nextRace as Rules["nextRace"]; st.nextExplicit = true; }
      if (m.public !== undefined && !st.nextExplicit) st.rules!.nextRace = m.public ? "auto" : "host";
      if (m.rotation !== undefined) {
        st.rotation = m.rotation as number[];
        st.trackLimits = m.trackLimits as number[];
      }
      if (m.track !== undefined && m.track !== st.track && st.nextRaceAt != null)
        st.nextRaceAt = Math.max(st.nextRaceAt, Date.now() + 15_000);
    }
    // A new track or new options make a different race: everyone readies again for it.
    if ((m.track !== undefined && m.track !== st.track) || (m.laps !== undefined && m.laps !== st.laps))
      for (const x of st.members) if (!st.session || !this.inRace(st, x.id)) x.ready = false;
    if (m.track !== undefined) st.track = m.track as number;
    if (m.laps !== undefined) st.laps = m.laps as number;
    if (m.seed !== undefined) st.seed = m.seed as number | null;
    if (m.public !== undefined) st.public = m.public as boolean;
    if (m.roomName !== undefined) st.roomName = (st.session ? sessionName(m.roomName as string) : m.roomName as string).trim();
    if (m.maxPlayers !== undefined) st.maxPlayers = m.maxPlayers as number;
    await this.changed();
  }

  private async onPick(ws: WebSocket, st: RoomState, me: Member, isHost: boolean, m: Msg): Promise<void> {
    if (st.race && !st.session) return this.error(ws, "room_started", "The race is running.");
    const target = m.member === undefined ? me : st.members.find((x) => x.id === m.member);
    if (target && st.session && this.inRace(st, target.id)) return this.error(ws, "room_started", "The rider is racing.");
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
    // A held slot can't race: its client isn't here to receive `start` (finding 140).
    if (st.members.some((x) => x.online === false))
      return this.error(ws, "not_ready", "A player is reconnecting.");
    if (st.session) {
      if (!st.members.some(x => x.ready && !x.choosing)) return this.error(ws, "not_ready", "Nobody is ready.");
      if (st.countdownEndsAt == null) this.lockGrid(st);
      return this.changed();
    }
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
      // A held slot blocks the countdown: its client isn't here to start (finding 140).
      st.members.every((x) => x.ready && !x.choosing && x.countdown && x.online !== false)
    );
  }

  private countdownMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).COUNTDOWN_MS);
    return Number.isFinite(v) && v >= 0 ? v : COUNTDOWN_MS;
  }

  /** Starts or stops the countdown after a change; true when it started now. */
  private updateCountdown(st: RoomState): boolean {
    if (st.session) {
      if (st.race) return false;
      if (st.grid) {
        st.grid = st.grid.filter(id => st.members.some(x => x.id === id && x.ready && !x.choosing));
        if (st.grid.length) return false;
        st.grid = undefined; st.gridRules = undefined; st.countdownEndsAt = null;
        st.lastEvent = "start_cancelled";
        if (st.rules!.nextRace === "auto") st.nextRaceAt = Date.now() + 30_000;
        return false;
      }
      // Everyone ready starts it in any phase, an intermission's timer or a host-started room's too.
      if (st.members.length >= 2 && st.members.every(x => x.ready && !x.choosing)) { this.lockGrid(st); return true; }
      return false;
    }
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
    const racers = st.members.filter(x => !st.session || st.grid?.includes(x.id)).sort((a, b) => Number(b.id === st.hostId) - Number(a.id === st.hostId));
    const now = Date.now();
    st.race = {
      ...(st.session ? {rules: {...st.gridRules!}, results: racers.map(x => ({id:x.id, name:x.name, rider:x.rider, place:0}))} : {}),
      slots: racers.map((x) => x.id),
      left: [],
      track: st.track,
      laps: st.laps,
      seed: st.seed ?? randomSeed(),
      startAt: now + START_LEAD_MS,
    };
    this.heardAt = [];  // a new race: every racer starts heard now
    st.grid = undefined; st.gridRules = undefined;
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
    if (info?.memberId == null) return;
    const st = this.state;
    const me = st?.members.find((x) => x.id === info.memberId);
    if (!st || !me) return;
    // Only the member's current socket may detach it (a late event from an old one must not).
    if (this.sockets.get(me.id) !== ws) return;
    if (me.token && this.canHold(st, me)) return this.detach(st, me, ws, info);
    await this.leave(info.memberId, "disconnect");
  }

  /** A member that can come back keeps its slot: in the lobby always, in a race while its slot is
      still racing (finding 140). */
  private canHold(st: RoomState, me: Member): boolean {
    if (!st.race) return true;
    const slot = st.race.slots.indexOf(me.id);
    return slot >= 0 && !st.race.left.includes(slot);
  }

  /** Holds a dropped member's slot (id, name, rider/bike, ready, host role; in a race its race
      slot) for RECONNECT_WINDOW_MS. The others see it as reconnecting in the lobby, and in a race
      get `away` at once: its rider is not being driven. `left` follows if the window runs out. */
  private async detach(st: RoomState, me: Member, ws: WebSocket, info: SocketInfo): Promise<void> {
    me.online = false;
    me.detachedAt = Date.now();
    this.sockets.delete(me.id);
    this.rtts.delete(me.id);
    this.chatTimes.delete(me.id);
    this.firstRtts.delete(me.id);
    ws.serializeAttachment({ ...info, memberId: null, gone: true });
    this.slotOf.delete(ws);
    this.stat(["detach", st.race ? "race" : "", info.country ?? "", nameHash(me.name)], [st.members.length]);
    const race = st.race;
    if (race) {
      const slot = race.slots.indexOf(me.id);
      if (slot >= 0) {
        this.raceSockets[slot] = null;
        this.broadcast({ type: "away", slot, memberId: me.id });
      }
    }
    // Persist the held slot, arm its expiry, drop any countdown it blocks, tell everyone.
    await this.changed();
  }

  /** The reconnect window (RECONNECT_WINDOW_MS), overridable through the environment as
      COUNTDOWN_MS and RACE_SILENT_MS are. */
  private reconnectWindowMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).RECONNECT_WINDOW_MS);
    return Number.isFinite(v) && v > 0 ? v : RECONNECT_WINDOW_MS;
  }

  /** The detached member a token belongs to, within its window; null when none matches. A member
      that is still connected is never returned, so a token can't take over a live slot (finding
      126). The room is per code, so a token only ever fits the room that issued it. */
  private findReconnect(token: string): Member | null {
    const st = this.state;
    if (!st) return null;
    const now = Date.now();
    const window = this.reconnectWindowMs();
    for (const x of st.members) {
      if (
        x.online === false &&
        x.token !== undefined &&
        x.detachedAt !== undefined &&
        now - x.detachedAt <= window &&
        this.canHold(st, x) &&
        tokensEqual(x.token, token)
      )
        return x;
    }
    return null;
  }

  /** When the earliest held slot runs out, or null when none is held. */
  private earliestExpiry(): number | null {
    const st = this.state;
    if (!st) return null;
    const window = this.reconnectWindowMs();
    let when: number | null = null;
    for (const x of st.members) {
      if (x.online === false && x.detachedAt !== undefined) {
        const e = x.detachedAt + window;
        if (when === null || e < when) when = e;
      }
    }
    return when;
  }

  /** Makes sure the alarm wakes the room when the earliest held slot runs out. */
  private async armExpiry(): Promise<void> {
    const when = this.earliestExpiry();
    if (when === null) return;
    const at = await this.ctx.storage.getAlarm();
    if (at === null || at > when) await this.ctx.storage.setAlarm(when);
  }

  /** Removes every member whose reconnect window has run out: the slot is a normal one again. */
  private async expireDetached(now: number): Promise<void> {
    const st = this.state;
    if (!st) return;
    const window = this.reconnectWindowMs();
    const expired = st.members.filter(
      (m) => m.online === false && m.detachedAt !== undefined && now - m.detachedAt >= window,
    );
    for (const x of expired) {
      await this.leave(x.id, "timeout");
      if (!this.state) return;
    }
  }

  /** Removes a member (idempotent). A racer's rider goes to the host's AI (the game decides). */
  private async leave(memberId: number, reason: "disconnect" | "leave" | "kicked" | "timeout"): Promise<void> {
    const st = this.state;
    if (!st) return;
    const i = st.members.findIndex((x) => x.id === memberId);
    if (i < 0) return;
    if (st.session) this.retire(st, memberId, "left");
    st.members.splice(i, 1);
    const ws = this.sockets.get(memberId);
    this.sockets.delete(memberId);
    this.rtts.delete(memberId);
    this.chatTimes.delete(memberId);
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
    if (st.session && st.race && !this.anyRacing(st)) await this.closeRace(st, false);
    await this.changed();
  }

  private phase(st: RoomState): string {
    return st.race ? "racing" : st.countdownEndsAt != null ? "starting" : st.nextRaceAt != null ? "intermission" : "open";
  }

  private inRace(st: RoomState, id: number): boolean {
    const slot = st.race?.slots.indexOf(id) ?? -1;
    return slot >= 0 && !st.race!.left.includes(slot);
  }

  /** Someone in the race has neither finished nor become DNF: the race goes on. */
  private anyRacing(st: RoomState): boolean {
    return !!st.race?.results?.some(x => x.ticks === undefined && !x.dnfReason);
  }

  private memberRace(st: RoomState, id: number) {
    const r = st.race?.results?.find(x => x.id === id);
    return {inRace: this.inRace(st,id), ...(r ? {
      raceState: r.dnfReason ? "dnf" : r.ticks !== undefined ? "finished" : "racing",
      ...(r.ticks !== undefined ? {finishTicks:r.ticks} : {}), place:r.place,
      ...(r.dnfReason ? {dnfReason:r.dnfReason} : {}),
    } : {})};
  }

  private trackLimit(st: RoomState, track: number): number {
    return st.trackLimits?.[(st.rotation ?? []).indexOf(track)] ?? 10;
  }

  private fittingRotation(st: RoomState, count: number): number[] {
    const rotation = st.rotation ?? [];
    const at = rotation.indexOf(st.track);
    const ordered = [...rotation.slice(at + 1), ...rotation.slice(0, at + 1)];
    return ordered.filter(track => this.trackLimit(st, track) >= count);
  }

  private lockGrid(st: RoomState): void {
    st.grid = st.members.filter(x => x.ready && !x.choosing).map(x => x.id);
    st.gridRules = {...st.rules!};
    st.nextRaceAt = null; st.waitingForRider = false; st.lastEvent = null;
    if (this.trackLimit(st, st.track) < st.grid.length) {
      const next = this.fittingRotation(st, st.grid.length)[0];
      if (next !== undefined) {
        st.lastEvent = `track_switch:${st.track}:${this.trackLimit(st, st.track)}:${next}`;
        st.track = next;
      }
    }
    st.countdownEndsAt = Date.now() + this.countdownMs();
  }

  private retire(st: RoomState, id: number, reason: string): void {
    if (!st.race || !this.inRace(st,id)) return;
    const r = st.race.results!.find(x => x.id === id)!;
    if (r.ticks === undefined) r.dnfReason = reason;
    const slot = st.race.slots.indexOf(id);
    st.race.left.push(slot);
    // Back in the room: the Ready that put them in this race is spent; a new one is for the next.
    const member = st.members.find(x => x.id === id);
    if (member) member.ready = false;
    this.rebuildRace();
    for (const ws of this.raceSockets) if (ws)
      this.send(ws, {type:"left", slot, memberId:id, reason:r.ticks !== undefined ? "finished" : reason});
  }

  private async closeRace(st: RoomState, voided: boolean): Promise<void> {
    const race = st.race;
    if (!race) return;
    if (!voided) {
      const entrants = [...race.results!].sort((a,b) => (a.ticks ?? Infinity) - (b.ticks ?? Infinity));
      let place = 0;
      const limit = this.trackLimit(st, race.track);
      for (let i = 0; i < entrants.length; i++) {
        const r = entrants[i];
        // Leave a distinct place for every remaining human, including DNFs. Reported
        // places include computer riders, but may be stale while snapshots catch up.
        r.place = Math.max(place + 1, Math.min(r.place, limit - (entrants.length - 1 - i)));
        place = r.place;
      }
      st.lastRace = {id: (st.raceId ?? 0) + 1, track:race.track, entrants};
      st.raceId = st.lastRace.id;
      // The rotation picks the next track unless the host already chose one during the race.
      const rotation = st.track === race.track ? this.fittingRotation(st, st.members.length) : [];
      if (race.rules!.then === "next" && rotation.length) st.track = rotation[0];
      if (race.rules!.then === "random" && rotation.length) {
        const choices = rotation.filter(x => x !== race.track);
        st.track = choices.length ? choices[randomSeed() % choices.length] : rotation[0];
      }
    }
    // Those still riding spend their Ready now; one pressed in the room during the race stays.
    for (const x of st.members) if (this.inRace(st, x.id)) x.ready = false;
    st.race = null;
    st.countdownEndsAt = null;
    st.nextRaceAt = race.rules!.nextRace === "auto" ? Date.now() + 30_000 : null;
    this.rebuildRace();
    await this.save();
    this.broadcast({type:"ended", reason:voided ? "void" : "done"});
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
    if (st?.session) {
      this.updateCountdown(st);
      await this.ctx.storage.setAlarm(this.nextAlarm(st, Date.now()));
    }
    if (st && !st.session && this.updateCountdown(st)) {
      const at = await this.ctx.storage.getAlarm();
      if (at === null || at > st.countdownEndsAt!) await this.ctx.storage.setAlarm(st.countdownEndsAt!);
    }
    if (st) await this.armExpiry();  // a held slot runs out even if nothing else happens
    await this.save();
    this.broadcastRoom();
    this.updateListing();
  }

  private async save(): Promise<void> {
    if (this.state) await this.ctx.storage.put("room", this.state);
  }

  private snapshot(st: RoomState) {
    return {
      ...(st.session ? {
        phase: this.phase(st), raceStartedAt: st.race?.startAt ?? null,
        finishEndsAt: st.race?.finishEndsAt ?? null, nextRaceAt: st.nextRaceAt ?? null,
        waitingForRider: st.waitingForRider ?? false, lastEvent: st.lastEvent ?? null,
        lastRace: st.lastRace ?? null, rules: st.rules, rotation: st.rotation,
        rulesPending: !!((st.race?.rules || st.gridRules) &&
          JSON.stringify(st.race?.rules || st.gridRules) !== JSON.stringify(st.rules)),
      } : {}),
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
        ...(st.session ? this.memberRace(st, x.id) : {}),
        id: x.id,
        name: x.name,
        ready: x.ready,
        rider: x.rider,
        ...(x.bike ? {bike: x.bike} : {}),
        rttMs: this.rttOf(x.id),
        ...(x.choosing ? {choosing: true} : {}),
        // A held slot: its client dropped and may still come back (finding 140). Older clients
        // ignore the field; new ones show the slot as reconnecting.
        ...(x.online === false ? { reconnecting: true } : {}),
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
    if (!forceRemove && st && st.public && (st.session || !st.race) && st.hostId !== null && (st.session || st.members.length < st.maxPlayers)) {
      entry = {
        ...(st.session ? {phase: this.phase(st), raceStartedAt: st.race?.startAt ?? null,
          finishEndsAt: st.race?.finishEndsAt ?? null, nextRaceAt: st.nextRaceAt ?? null,
          full: st.members.length >= st.maxPlayers} : {}),
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
