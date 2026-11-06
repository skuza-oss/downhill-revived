// One lobby and its race. Lobby control is JSON; the race is binary frames relayed to the other
// racers through the SFU; this object owns lobby and signaling. Protocol: docs/online-protocol.md.
import { Realtime, Turn, TURN_TTL_S, type SfuSession, type SfuSecrets } from "./sfu";
import { DurableObject } from "cloudflare:workers";
import { LISTING_REFRESH_MS, type LobbyEntry } from "./lobbies";
import {
  CLOSE_CODES,
  COUNTDOWN_MS,
  DEFAULT_MAX_PLAYERS,
  FEATURE_COUNTDOWN,
  FRAME_PING,
  FRAME_PONG,
  HELLO_TIMEOUT_MS,
  OUTDATED_MESSAGE,
  MAX_BINARY_BYTES,
  MAX_MEMBERS,
  MAX_TEXT_BYTES,
  PING_INTERVAL_MS,
  PING_MAX_PAYLOAD,
  PROTOCOL,
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
import { placeOf, regionOfPlace, roomRegion, traceColo } from "./region";
import {
  CAPS, TOO_FAST_MESSAGE, TOO_FAST_WARNING, banMessage, findBan, installIdOf, logJoin, newTally, onlineState, tally,
  type Caps, type GuardEnv, type OnlineState, type Tally,
} from "./guard";

/** A racer's Realtime setup, from the race's start message to its channels acknowledged, has
    this long (SFU_SETUP_MS overrides it); one not done by then is out of the race (finding 158,
    release review round 2). Measured setups take 0.2-0.5 s over UDP and 2.4-3.1 s through the
    TURN fallback; ten racers' subscriptions are 90 serialized API calls more. 25 s leaves room for
    those and for one call that runs into its 15 s timeout, and ends before the game's 30 s wait
    for the others' rides (port/hle/online.c), which travel over the released channels. */
const SFU_SETUP_MS = 25_000;
/** Sent to a racer whose Realtime setup failed or ran out of time; it is out of the race (the room
    stays). The footer holds about 45 characters. */
const SFU_FAILED_MESSAGE = "Realtime connection failed: out of the race.";

/** A Realtime session or TURN credential whose close failed (release review round 2, finding 158).
    Kept apart from the members, under its own storage key that outlives the room ("cleanup"), and
    tried again by the alarm until it closes or its time is up. */
interface Cleanup {
  /** An SFU session and its channels still open... */
  session?: string;
  channels?: number[];
  /** ...or a TURN username not revoked. */
  turnUser?: string;
  /** Given up then: the credential's TTL ends it; a session gets SFU_CLEANUP_MS. */
  until: number;
  /** The next try, and the failed ones so far. */
  at: number;
  tries: number;
}
/** A failed close is tried again after 5 s, then twice as long each time, at most every 5 minutes. */
const CLEANUP_FIRST_MS = 5_000;
const CLEANUP_MAX_MS = 300_000;
/** A session's channels are given up after an hour, as a TURN credential is. Cloudflare documents
    no end for a connected DataChannel-only session (Realtime SFU limits: only a session that lost
    its connection ends, after 30 s), and a game drops its association at the race's end, so only
    a modified one keeps it that long. */
const SFU_CLEANUP_MS = TURN_TTL_S * 1000;

/** A claimed room nobody joined is dropped after this long. */
const UNUSED_ROOM_MS = 60_000;
/** The game's room options ride in `laps` (port/hle/online_menu.c ROOM_NO_BOTS 1, ROOM_CHEATS 2);
    only cheats on or off makes another kind of race (finding 128). */
const LAPS_CHEATS = 2;

/** A member whose connection dropped without a close from the game (1006) stays this long for
    its rejoin; a member whose socket has said nothing for this long is dropped. */
const REJOIN_GRACE_MS = 20_000;
/** Close code and reason for the old socket of a member taken back by a rejoin: an ordinary
    close. Only the game holding the member's rejoin token takes it back, and that game has already
    given the old socket up. */
const REPLACED_CLOSE = 1000;

/** A member's rejoin token (finding 137): 16 random bytes in hex, sent only to that member's game
    in its welcome. Its rejoin hello sends it back and gets the member again; nothing else does. */
function rejoinToken(): string {
  const b = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
}

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
  /** Their region as Cloudflare places them ("EU", "NA"...; "" unknown), shown to the room. Never
      their country: rooms are public and players post their logs (finding 135). */
  region?: string;
  /** Their connection dropped (no close from the game) at this time: kept REJOIN_GRACE_MS for the
      game's rejoin, without a socket (finding 137). */
  awaySince?: number;
  /** The rejoin token (rejoinToken): only a hello with it takes this member back. Members made
      before tokens have none; their games (older releases) are below MIN_VERSION. */
  token?: string;
  sfu?: SfuSession;
  sfuRace?: number;
  sfuReady?: boolean;
  /** The race this member got TURN credentials for (one mint per member per race), and their
      username, kept to revoke them; neither ever goes to a client but its own sfu_turn. */
  turnRace?: number;
  turnUser?: string;
  /** When those credentials end by themselves (their TTL). */
  turnUntil?: number;
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
  /** Racers whose Realtime setup isn't done at this time are out of the race (SFU_SETUP_MS). */
  sfuBy?: number;
  /** The racers' traffic was released (sfu_ready sent). */
  sfuReleased?: boolean;
}

interface RoomState {
  session?: boolean;
  rules?: Rules;
  nextExplicit?: boolean;
  rotation?: number[];
  trackLimits?: number[];
  /** Rotation tracks the host may pick but NEXT and RANDOM never choose (an arena, MOSH). */
  pickOnly?: number[];
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
  /** Where the room was made and where it runs (finding 135): the location hint, the creator's
      region and edge, and the Cloudflare location the object itself runs in (its trace). */
  origin?: RoomOrigin;
  colo?: string;
  /** In join order, so members[0] has been here longest. */
  members: Member[];
  race: Race | null;
}

/** Where a room was made (src/index.ts, finding 135). */
export interface RoomOrigin {
  /** The Durable Object location hint it was made with ("" none). */
  hint: string;
  /** The creator's region (src/region.ts regionOfPlace). */
  createdIn: string;
  /** The Cloudflare location the creator reached. */
  createdFrom: string;
}

/** The Cloudflare trace a room reads to learn where it runs; COLO_PROBE overrides it ("off": none). */
const COLO_PROBE_URL = "https://cloudflare.com/cdn-cgi/trace";

/** Kept on each socket across hibernation. */
interface SocketInfo {
  memberId: number | null;
  connectedAt: number;
  create: boolean;
  /** Left or refused; only its close is still to come. */
  gone?: boolean;
  /** Two-letter country of the connection (Cloudflare's): the usage stats and the player's own
      welcome only, never sent to anyone else (no address either). */
  country?: string;
  /** The connection's continent and the Cloudflare location it reached (the welcome), and its
      region (src/region.ts regionOfPlace): what the other members see. */
  continent?: string;
  region?: string;
  colo?: string;
  /** The address's keyed hash (src/guard.ts ipHash), from the Worker: the moderation log only,
      never sent to anyone (finding 161). */
  ipHash?: string;
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
  private sfuQueue: Promise<void> = Promise.resolve();
  /** Resources still to close (Cleanup), also in storage under "cleanup". */
  private cleanup: Cleanup[] = [];
  private state: RoomState | null = null;
  private sockets = new Map<number, WebSocket>();
  /** Round-trips from our pings, per member. Memory only; re-measured after a wake-up. */
  private rtts = new Map<number, number[]>();
  /** The first round-trip after connect, kept apart: see measuredRtt. */
  private firstRtts = new Map<number, number>();

  // Race relay state, rebuilt from `state.race` after a wake-up.
  private raceSockets: (WebSocket | null)[] = [];
  private slotOf = new Map<WebSocket, number>();

  private listedKey: string | null = null;
  private listedAt = 0;
  /** This instance's start. Members then without a socket are a restart's ghosts, kept for their
      token's rejoin (onHello); those not back within HELLO_TIMEOUT_MS leave as disconnects (alarm). */
  private readonly startedAt = Date.now();
  /** This instance asked the trace where it runs (once). */
  private probed = false;
  /** When each socket last sent anything. Memory only: a socket not here (a wake-up) counts from
      the instance's start. */
  private heard = new WeakMap<WebSocket, number>();
  /** Each socket's message counts (src/guard.ts tally, finding 161). Memory only: after a wake-up
      a socket starts a new window. */
  private tallies = new WeakMap<WebSocket, Tally>();
  /** Sockets whose hello waits for the ban list (onHello): their other messages are dropped. */
  private helloPending = new WeakSet<WebSocket>();
  private capsFor: unknown = null;
  private capsNow: Caps = CAPS;

  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(async () => {
      this.state = (await ctx.storage.get<RoomState>("room")) ?? null;
      this.cleanup = (await ctx.storage.get<Cleanup[]>("cleanup")) ?? [];
      for (const ws of ctx.getWebSockets()) {
        const info = ws.deserializeAttachment() as SocketInfo | null;
        if (info?.memberId != null) this.sockets.set(info.memberId, ws);
      }
      // After a wake-up mid-race the last relayed fields are unknown (see leave()).
      this.rebuildRace();
    });
  }

  /** Called by the Worker for a fresh code. False if the code is in use. */
  async claim(code: string, origin?: RoomOrigin): Promise<boolean> {
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
      ...(origin ? { origin } : {}),
    };
    await this.save();
    await this.ctx.storage.setAlarm(now + PING_INTERVAL_MS);
    this.probeColo();
    return true;
  }

  /** Learns the Cloudflare location this object runs in, once per room (rooms made before this
      code learn it on their next hello): request.cf.colo here is the client's edge, so the object
      reads a Cloudflare trace, whose colo is where its own requests leave from. In the background;
      the room shows its region as soon as the answer comes. */
  private probeColo(): void {
    if (this.probed || !this.state || this.state.colo) return;
    this.probed = true;
    const configured = (this.env as unknown as Record<string, unknown>).COLO_PROBE;
    const url = typeof configured === "string" ? configured : COLO_PROBE_URL;
    if (!url || url === "off") return;
    this.ctx.waitUntil((async () => {
      try {
        const r = await fetch(url, { signal: AbortSignal.timeout(5000) });
        const colo = traceColo(await r.text());
        if (!colo || !this.state || this.state.colo) return;
        this.state.colo = colo;
        await this.save();
        this.broadcastWhere();
        this.updateListing();
      } catch {}
    })());
  }

  /** Where the room runs, for players: "EU", "NA", "ASIA"... ("" unknown). */
  private region(st: RoomState): string {
    return roomRegion(st.colo, st.origin?.hint);
  }

  async fetch(request: Request): Promise<Response> {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return new Response("Expected a WebSocket upgrade", { status: 426 });
    const { 0: client, 1: server } = new WebSocketPair();
    this.ctx.acceptWebSocket(server);
    const place = placeOf(request);
    const info: SocketInfo = {
      memberId: null,
      connectedAt: Date.now(),
      create: request.headers.get("x-dhd-create") === "1",
      country: place.country ?? "",
      continent: place.continent ?? "",
      region: regionOfPlace(place),
      colo: place.colo ?? "",
      ipHash: request.headers.get("x-dhd-ip-hash") ?? "",
    };
    server.serializeAttachment(info);
    if ((await this.ctx.storage.getAlarm()) === null)
      await this.ctx.storage.setAlarm(Date.now() + PING_INTERVAL_MS);
    return new Response(null, { status: 101, webSocket: client });
  }

  // ---- WebSocket events ----

  async webSocketMessage(ws: WebSocket, msg: string | ArrayBuffer): Promise<void> {
    const now = Date.now();
    this.heard.set(ws, now);
    // Caps per member (finding 161): more than the game ever sends is dropped, warned once, then
    // closed.
    let t = this.tallies.get(ws);
    if (!t) this.tallies.set(ws, (t = newTally(now)));
    const verdict = tally(t, typeof msg !== "string", now, this.caps());
    if (verdict !== "ok") {
      if (verdict === "warn") this.error(ws, "too_fast", TOO_FAST_WARNING);
      if (verdict === "close") {
        const info = ws.deserializeAttachment() as SocketInfo | null;
        console.warn(`room ${this.state?.code ?? ""}: member ${info?.memberId ?? "-"} closed: too many messages`);
        return this.refuse(ws, "too_fast", TOO_FAST_MESSAGE);
      }
      return;
    }
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
    if (info.gone || this.helloPending.has(ws)) return;
    if (info.memberId === null) {
      if (m.type !== "hello") return this.refuse(ws, "bad_message", "Send hello first.");
      return this.onHello(ws, info, m);
    }
    const st = this.state;
    const me = st?.members.find((x) => x.id === info.memberId);
    if (!st || !me) return;
    const isHost = st.hostId === me.id;

    if (m.type.startsWith("sfu_")) {
      // Serialize across external awaits without holding the DO input gate.
      const run = this.sfuQueue.then(() => this.onSfu(ws, me, m));
      this.sfuQueue = run.then(() => {}, () => {});
      const failed = await run.catch(() => undefined);
      // Out of the race outside the queue: its end (closeRace) waits for the queue.
      if (failed) await this.retireSfu(failed.ids, failed.startAt);
      return;
    }
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
        await this.closeRaceSfu(st);
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

  async webSocketClose(ws: WebSocket, code: number, _reason?: string, wasClean?: boolean): Promise<void> {
    // 1006: the connection ended without a close frame (the game's network dropped, or it gave a
    // silent connection up): it may come back. A game that leaves sends "leave" first.
    await this.dropSocket(ws, code === 1006 || wasClean === false);
    try {
      ws.close(1000);
    } catch {}
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    await this.dropSocket(ws, true);
  }

  async alarm(): Promise<void> {
    const now = Date.now();
    await this.retryCleanup(now);
    let pending = 0;
    for (const ws of this.ctx.getWebSockets()) {
      const info = ws.deserializeAttachment() as SocketInfo | null;
      if (info && info.memberId === null && !info.gone) {
        if (now - info.connectedAt > HELLO_TIMEOUT_MS)
          this.close(ws, "hello_timeout", "No hello received.");
        else pending++;
      }
    }
    if (this.state)
      for (const x of [...this.state.members]) {
        const ws = this.sockets.get(x.id);
        if (ws) {
          // Dead connections: no message (a game sends one every second) for REJOIN_GRACE_MS. The
          // close event of a connection that died on the way may come minutes later, or never.
          if (now - this.heardAt(ws) > REJOIN_GRACE_MS) {
            await this.leave(x.id, "disconnect");
            try {
              ws.close(1001, "silent");
            } catch {}
          }
        } else if (now > (x.awaySince != null ? x.awaySince + REJOIN_GRACE_MS : this.startedAt + HELLO_TIMEOUT_MS)) {
          // Not back in time: after a drop (awaySince), or after a restart (a deploy drops every
          // socket and no close event comes: members without a socket at the start are ghosts).
          await this.leave(x.id, "disconnect");
        }
      }
    const st = this.state;
    if (!st) {
      // An ended room still wakes for sockets waiting for hello and for its pending cleanup.
      const at = Math.min(pending ? now + PING_INTERVAL_MS : Infinity, ...this.cleanup.map(x => Math.min(x.at, x.until)));
      if (at !== Infinity) await this.ctx.storage.setAlarm(Math.max(now, at));
      return;
    }
    // Realtime setup not done in time (SFU_SETUP_MS): those racers are out, the rest go on.
    const race = st.race;
    if (race && !race.sfuReleased && race.sfuBy != null && now >= race.sfuBy) {
      const late = st.members.filter(x => this.inRace(st, x.id) &&
        !(x.sfuRace === race.startAt && x.sfuReady && x.sfu?.phase === "ready")).map(x => x.id);
      if (late.length) await this.retireSfu(late, race.startAt);
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

  /** The next ping, or the countdown's end if that is sooner. */
  private nextAlarm(st: RoomState, now: number): number {
    const ping = now + PING_INTERVAL_MS;
    const sfuBy = st.race && !st.race.sfuReleased ? st.race.sfuBy : null;
    if (st.session) return Math.min(ping, ...[st.countdownEndsAt, st.nextRaceAt, st.race?.finishEndsAt, sfuBy]
      .filter((x): x is number => x != null).map(x => Math.max(now, x)));
    const at = st.countdownEndsAt != null && !st.race ? Math.min(ping, Math.max(now, st.countdownEndsAt)) : ping;
    return sfuBy != null ? Math.min(at, Math.max(now, sfuBy)) : at;
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
    const b = new Uint8Array(data);
    b[1] = slot;
    switch (b[0]) {
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
      return;
    }
    this.raceSockets = race.slots.map((id, slot) => {
      const ws = race.left.includes(slot) ? null : (this.sockets.get(id) ?? null);
      if (ws) this.slotOf.set(ws, slot);
      return ws;
    });
  }

  // ---- Lobby ----

  private heardAt(ws: WebSocket): number {
    return this.heard.get(ws) ?? this.startedAt;
  }

  /** The member a rejoin hello takes back (finding 137): the one whose token it sends, at once,
      whether the member's old socket has closed yet or not. A name alone takes nobody back: a
      hello under a member's name without its token is another player, who joins as a new member. */
  private rejoinOf(st: RoomState, token: unknown): Member | undefined {
    if (typeof token !== "string" || token.length === 0) return undefined;
    return st.members.find((x) => x.token === token);
  }

  private async onHello(ws: WebSocket, info: SocketInfo, m: Msg): Promise<void> {
    // The version gate first: releases older than MIN_VERSION (wrangler.jsonc) may not play, and
    // client_outdated is the refusal their ONLINE screen answers with Update Now (finding 054), also
    // when they speak an older protocol (0.7.11, protocol 2, against protocol 3: finding 158).
    const minVersion = minVersionOf(this.env.MIN_VERSION);
    const clientVersion = clientVersionOf(m);
    if (isOutdated(clientVersion, minVersion))
      return this.refuse(ws, "client_outdated", OUTDATED_MESSAGE, { minVersion, clientVersion });
    if (m.protocol !== PROTOCOL)
      return this.refuse(ws, "protocol_mismatch", `This server speaks protocol ${PROTOCOL}; update the game.`);
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
    // The kill switch, the install id (optional; releases before it send none, a malformed one
    // counts as none) and its ban (finding 161): the address's was checked before the room. Both
    // come from the store (kept per isolate); nothing else of this socket is handled meanwhile.
    const installId = installIdOf(m.installId);
    let sw: OnlineState = { on: true, message: "" };
    if ((this.env as GuardEnv).MODERATION) {
      this.helloPending.add(ws);
      let ban;
      try {
        [sw, ban] = await Promise.all([this.online(), installId ? findBan(this.env as GuardEnv, { install: installId }) : null]);
      } finally {
        this.helloPending.delete(ws);
      }
      if ((ws.deserializeAttachment() as SocketInfo | null)?.gone) return;
      if (ban) return this.refuse(ws, "banned", banMessage(ban));
    }

    const st = this.state;
    // Online off: only this room's own members come back (a dropped racer rejoins its race, which
    // goes on); nobody new gets in.
    if (!sw.on && !(st && st.version !== null && this.rejoinOf(st, m.rejoinToken)))
      return this.refuse(ws, "online_off", sw.message);
    const asHost = info.create && st !== null && st.version === null && st.members.length === 0;
    if (!st || (!asHost && st.version === null))
      return this.refuse(ws, "room_not_found", "No room has this code.");
    // A game back with its member's token (its connection dropped, or a restart dropped every
    // socket) gets its member again: id, host, rider, Ready and race place included (finding 137).
    const ghost = asHost ? undefined : this.rejoinOf(st, m.rejoinToken);
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

    const features = Array.isArray(m.features) ? m.features : [];
    const member: Member = ghost ?? {
      id: st.nextMemberId++,
      name,
      ready: false,
      rider: 0,
      bike: 0,
      countdown: features.includes(FEATURE_COUNTDOWN),
      token: rejoinToken(),
    };
    member.region = info.region ?? "";
    if (!ghost) st.members.push(member);
    if (asHost) st.hostId = member.id;
    if (ghost) {
      delete ghost.awaySince;
      ghost.countdown = features.includes(FEATURE_COUNTDOWN);
      // The old socket may not close for minutes (a dead path the room hasn't seen end yet):
      // forget it, so its late close doesn't remove the member, and close it.
      const old = this.sockets.get(ghost.id);
      if (old && old !== ws) {
        old.serializeAttachment({ ...(old.deserializeAttachment() as SocketInfo), memberId: null, gone: true });
        this.slotOf.delete(old);
        try {
          old.close(REPLACED_CLOSE, "replaced");
        } catch {}
      }
      this.rtts.delete(ghost.id);
      this.firstRtts.delete(ghost.id);
      // A game never rejoins during its race (it races on alone, port/net/client.c): a member
      // still in the race is out of it now.
      if (st.session && this.inRace(st, ghost.id)) this.retire(st, ghost.id, "left");
    }
    info.memberId = member.id;
    ws.serializeAttachment(info);
    this.sockets.set(member.id, ws);
    if (ghost) {
      this.rebuildRace();
      if (st.session && st.race && !this.anyRacing(st)) await this.closeRace(st, false);
    } else this.stat(["join", clientVersion, info.country ?? "", nameHash(name), asHost ? "host" : "guest"], [st.members.length]);
    // The moderation log (finding 161): who entered which room, kept 14 days, for bans.
    this.ctx.waitUntil(logJoin(this.env as GuardEnv, {
      at: Date.now(), room: st.code, member: member.id, kind: asHost ? "host" : ghost ? "rejoin" : "guest", name: member.name,
      clientVersion, country: info.country ?? "", ipHash: info.ipHash ?? "", installId,
    }));
    this.send(ws, {
      type: "welcome",
      protocol: PROTOCOL,
      code: st.code,
      memberId: member.id,
      host: st.hostId === member.id,
      serverTime: Date.now(),
      // Where this connection and the room are (finding 135; older clients ignore these): the
      // Cloudflare location the player reached, their own country and continent as Cloudflare
      // places them (only to them), and where the room runs ("" while the room's trace hasn't answered: the room
      // messages carry it then).
      edgeColo: info.colo ?? "",
      country: info.country ?? "",
      continent: info.continent ?? "",
      roomColo: st.colo ?? "",
      roomRegion: this.region(st),
      // Only this game gets it; its rejoin hello sends it back (finding 137).
      rejoinToken: member.token,
    });
    this.probeColo();
    // First ping right away so the lobby shows a delay quickly.
    this.send(ws, { type: "ping", s: Date.now() });
    await this.changed();
    this.broadcastWhere();
  }

  /** One Realtime signaling message (docs/online-protocol.md), run in the SFU queue. Returns the
      racers whose setup failed for good: the caller takes them out of the race (retireSfu). */
  private async onSfu(ws: WebSocket, me: Member, m: Msg): Promise<{ startAt: number; ids: number[] } | undefined> {
    if (this.sockets.get(me.id) !== ws || !this.state?.members.includes(me)) return;
    const race = this.state.race;
    if (!race || !this.inRace(this.state, me.id)) {
      this.error(ws, "bad_value", "Realtime requires an active race.");
      return;
    }
    const api = new Realtime(this.env as Env & SfuSecrets);
    try {
      if (m.type === "sfu_offer") {
        if (me.sfuRace === race.startAt) {
          this.error(ws, "bad_value", "One Realtime session per race.");
          return;
        }
        me.sfuRace = race.startAt;
        me.sfuReady = false;
        await this.save();
        if (me.sfu) await this.clearSfu(me);
        const opened = await api.open(m.sdp);
        me.sfu = opened.session;
        if (!this.sfuCurrent(race, me)) {
          await this.clearSfu(me); return;
        }
        await this.save();
        this.send(ws, opened.message);
      } else if (m.type === "sfu_connected" && me.sfu) {
        const channels = await api.publish(me.id, me.sfu);
        this.send(ws, { type: "sfu_channels", channels });
        if (!this.sfuCurrent(race, me)) {
          await this.clearSfu(me); return;
        }
        const failed = await this.subscribeRacers(api, race);
        await this.save();
        if (failed.length) return { startAt: race.startAt, ids: failed };
      } else if (m.type === "sfu_turn" && me.sfu && me.sfuRace === race.startAt) {
        // UDP to the SFU did not connect for this racer: TURN over TLS (finding 158, round 4).
        if (me.turnRace === race.startAt) {
          this.error(ws, "bad_value", "One TURN credential per race.");
          return;
        }
        me.turnRace = race.startAt;
        await this.save();
        await this.clearTurn(me);
        let minted: Awaited<ReturnType<Turn["mint"]>>;
        try {
          minted = await new Turn(this.env as Env & SfuSecrets).mint(`room-${this.state?.code ?? ""}-m${me.id}`);
        } catch (error) {
          // No relay: the racer's UDP attempt goes on (its session stays usable); nothing sent.
          console.warn("TURN credentials unavailable", error instanceof Error ? error.message.slice(0, 80) : "unknown");
          return;
        }
        me.turnUser = minted.username;
        me.turnUntil = Date.now() + TURN_TTL_S * 1000;
        if (!this.sfuCurrent(race, me) || this.sockets.get(me.id) !== ws) {
          await this.clearTurn(me); await this.save(); return;
        }
        await this.save();
        this.send(ws, minted.message);
      } else if (m.type === "sfu_ready" && me.sfu?.phase === "ready") {
        me.sfuReady = true;
        await this.save();
        this.releaseSfu(this.state!);
      } else throw new Error("Invalid signaling phase");
    } catch (error) {
      let detail = error instanceof Error ? error.message : "unknown";
      const secrets = this.env as Env & SfuSecrets;
      for (const value of [secrets.REALTIME_APP_ID, secrets.REALTIME_APP_TOKEN, secrets.TURN_KEY_ID, secrets.TURN_KEY_API_TOKEN])
        if (value) detail = detail.replaceAll(value, "<redacted>");
      console.warn("Realtime setup failed", detail.slice(0, 200));
      // No second attempt in this race (a partly made allocation must not be doubled): the racer
      // is out of it, and the others go on without waiting for it.
      if (me.sfu) me.sfu.phase = "failed";
      await this.save();
      return { startAt: race.startAt, ids: [me.id] };
    }
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
      if (m.pickOnly !== undefined && (m.rotation === undefined || !Array.isArray(m.pickOnly) ||
          !m.pickOnly.every(x => (m.rotation as number[]).includes(x)))) return bad("pickOnly");
      if (m.finishWindow !== undefined) st.rules!.finishWindow = m.finishWindow as number;
      if (m.then !== undefined) st.rules!.then = m.then as Rules["then"];
      if (m.nextRace !== undefined) { st.rules!.nextRace = m.nextRace as Rules["nextRace"]; st.nextExplicit = true; }
      if (m.public !== undefined && !st.nextExplicit) st.rules!.nextRace = m.public ? "auto" : "host";
      if (m.rotation !== undefined) {
        st.rotation = m.rotation as number[];
        st.trackLimits = m.trackLimits as number[];
        st.pickOnly = (m.pickOnly as number[] | undefined) ?? [];
      }
      if (m.track !== undefined && m.track !== st.track && st.nextRaceAt != null)
        st.nextRaceAt = Math.max(st.nextRaceAt, Date.now() + 15_000);
    }
    // Ready means "count me in for the next race", not "I agree to these settings": a new track or
    // computer riders on or off keep it. Cheats on or off make another kind of race, so everyone
    // outside the running race readies again for it (finding 128).
    if (m.laps !== undefined && (((m.laps as number) ^ st.laps) & LAPS_CHEATS))
      for (const x of st.members) if (!st.session || !this.inRace(st, x.id)) x.ready = false;
    const newTrack = m.track !== undefined && m.track !== st.track;
    if (m.track !== undefined) st.track = m.track as number;
    // A new track during the start countdown: the countdown runs again in full, so the ready
    // players see the change (session rooms lock their grid again, which checks the new
    // track's limit).
    if (newTrack && !st.race && st.countdownEndsAt != null) {
      if (st.session && st.grid) this.lockGrid(st);
      else if (!st.session) st.countdownEndsAt = Date.now() + this.countdownMs();
    }
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
    const sw = await this.online();
    if (st !== this.state) return;
    if (st.race) return this.error(ws, "room_started", "The race is already running.");
    if (!sw.on) return this.error(ws, "online_off", sw.message);
    if (st.session) {
      if (!st.members.some(x => x.ready && !x.choosing)) return this.error(ws, "not_ready", "Nobody is ready.");
      if (st.countdownEndsAt == null) this.lockGrid(st);
      return this.changed();
    }
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

  /** The caps per member (src/guard.ts CAPS), worked out once per env (every message asks, the race
      relay's too). The tests change them through CAP_WINDOW_MS and CAP_BINARY (vitest.config.ts;
      a test may give a room a new env). */
  private caps(): Caps {
    if (this.capsFor === this.env) return this.capsNow;
    const env = this.env as unknown as Record<string, unknown>;
    const knob = (name: string, value: number) => {
      const v = Number(env[name]);
      return env[name] !== undefined && Number.isFinite(v) && v > 0 ? v : value;
    };
    this.capsFor = this.env;
    this.capsNow = { windowMs: knob("CAP_WINDOW_MS", CAPS.windowMs), text: CAPS.text, binary: knob("CAP_BINARY", CAPS.binary) };
    return this.capsNow;
  }

  private sfuSetupMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).SFU_SETUP_MS);
    return Number.isFinite(v) && v > 0 ? v : SFU_SETUP_MS;
  }

  private countdownMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).COUNTDOWN_MS);
    return Number.isFinite(v) && v >= 0 ? v : COUNTDOWN_MS;
  }

  /** The kill switch (finding 161; src/guard.ts onlineState, kept 30 s per isolate): while online
      is off no race starts here and nobody new joins; the room stays a lobby for its members (a
      race already running goes on). */
  private online(): Promise<OnlineState> {
    return onlineState(this.env as GuardEnv);
  }

  /** Starts or stops the countdown after a change (none while online is off); true when it
      started now. */
  private updateCountdown(st: RoomState, off: boolean): boolean {
    if (off && !st.race) {
      st.countdownEndsAt = null;
      st.grid = undefined; st.gridRules = undefined;
      return false;
    }
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
    const sw = await this.online();
    if (st !== this.state || st.race) return;
    st.countdownEndsAt = null;
    if (!sw.on) {
      // A countdown that ran out with online off (it was turned off since): no race.
      st.grid = undefined; st.gridRules = undefined;
      await this.save();
      this.broadcast({ type: "error", code: "online_off", message: sw.message });
      this.broadcastRoom();
      return;
    }

    // Host first, then join order.
    // Members away after a drop (finding 137) don't race: their game isn't here to start.
    const racers = st.members.filter(x => this.sockets.has(x.id) && (!st.session || st.grid?.includes(x.id))).sort((a, b) => Number(b.id === st.hostId) - Number(a.id === st.hostId));
    const now = Date.now();
    st.race = {
      ...(st.session ? {rules: {...st.gridRules!}, results: racers.map(x => ({id:x.id, name:x.name, rider:x.rider, place:0}))} : {}),
      slots: racers.map((x) => x.id),
      left: [],
      track: st.track,
      laps: st.laps,
      seed: st.seed ?? randomSeed(),
      startAt: now + START_LEAD_MS,
      sfuBy: now + this.sfuSetupMs(),
    };
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

  /** A socket ended. `dropped`: without a close from the game, which may rejoin: outside a race
      the member stays REJOIN_GRACE_MS without a socket (finding 137); in it, or after a refusal,
      the member leaves now. */
  private async dropSocket(ws: WebSocket, dropped = false): Promise<void> {
    const info = ws.deserializeAttachment() as SocketInfo | null;
    const id = info?.memberId;
    if (id == null) return;
    const st = this.state;
    const member = st?.members.find((x) => x.id === id);
    if (!dropped || !st || !member || this.inRace(st, id) || this.sockets.get(id) !== ws)
      return this.leave(id, "disconnect");
    member.awaySince = Date.now();
    this.sockets.delete(id);
    this.rtts.delete(id);
    this.firstRtts.delete(id);
    ws.serializeAttachment({ ...info, memberId: null, gone: true });
    this.slotOf.delete(ws);
    await this.changed();
  }

  /** `member` is still a racer of `race`, the room's running race. */
  private sfuCurrent(race: Race, member: Member): boolean {
    const st = this.state;
    return !!st && st.race === race && st.members.includes(member) && this.inRace(st, member.id);
  }

  /** The racers of `race` whose publication is up (their own channels allocated). */
  private sfuRacers(race: Race): Member[] {
    const st = this.state;
    if (!st || st.race !== race) return [];
    return st.members.filter(x => this.inRace(st, x.id) && x.sfuRace === race.startAt && x.sfu?.phase === "ready");
  }

  /** A subscriber and the publisher it has no subscription to yet, among those racers. */
  private missingSubscription(race: Race): [Member, Member] | null {
    const racers = this.sfuRacers(race);
    for (const a of racers) for (const b of racers)
      if (a !== b && !a.sfu!.channels.some(c => !c.local && c.memberId === b.id)) return [a, b];
    return null;
  }

  /** Subscribes every published racer of `race` to every other one's publication it lacks, each
      endpoint told only its own allocations. Leaves and kicks are not queued with this, so the
      racers are read again after every API call: a departure during one can't make it skip a
      remaining racer (release review round 2, finding 158). It ends when no pair is missing.
      Returns the subscribers whose call failed: their session is failed (no second attempt) and
      the caller takes them out of the race. */
  private async subscribeRacers(api: Realtime, race: Race): Promise<number[]> {
    const failed: number[] = [];
    // Bounded: every call allocates a missing pair, finds it allocated or fails its subscriber.
    for (let calls = 0; calls <= MAX_MEMBERS * MAX_MEMBERS; calls++) {
      const pair = this.missingSubscription(race);
      if (!pair) break;
      const [sub, pub] = pair;
      try {
        const channels = await api.subscribe(sub.sfu!, pub.id, pub.sfu!);
        const peer = this.sockets.get(sub.id);
        if (channels.length && peer && this.sfuCurrent(race, sub)) this.send(peer, { type: "sfu_channels", channels });
      } catch (error) {
        console.warn("Realtime subscription failed", error instanceof Error ? error.message.slice(0, 80) : "unknown");
        if (sub.sfu) sub.sfu.phase = "failed";
        failed.push(sub.id);
      }
    }
    return failed;
  }

  /** A subscription missing at release (none should be: every racer's sfu_connected subscribes
      all pairs): allocated in the queue, then the release is checked again. */
  private reconciling = false;
  private queueReconcile(race: Race): void {
    if (this.reconciling) return;
    this.reconciling = true;
    const run = this.sfuQueue.then(() => this.subscribeRacers(new Realtime(this.env as Env & SfuSecrets), race));
    this.sfuQueue = run.then(() => {}, () => {});
    this.ctx.waitUntil(run.then(async (failed) => {
      this.reconciling = false;
      await this.save();
      if (failed.length) await this.retireSfu(failed, race.startAt);
      else if (this.state?.race === race) this.releaseSfu(this.state);
    }, () => { this.reconciling = false; }));
  }

  /** Lets race traffic flow once every racer acknowledged its channels and every pair of racers
      is subscribed both ways. */
  private releaseSfu(st: RoomState): void {
    const race = st.race;
    if (!race) return;
    const racers = st.members.filter(m => this.inRace(st, m.id));
    if (!racers.length || !racers.every(m => m.sfuRace === race.startAt && m.sfuReady && m.sfu?.phase === "ready")) return;
    if (this.missingSubscription(race)) return this.queueReconcile(race);
    race.sfuReleased = true;
    for (const member of racers) {
      const peer = this.sockets.get(member.id);
      if (peer) this.send(peer, {type:"sfu_ready"});
    }
  }

  private async closeRaceSfu(st: RoomState): Promise<void> {
    const cleanup = this.sfuQueue.then(async () => {
      // A copy: a leave during an API call must not make the loop skip anyone.
      for (const member of [...st.members]) if (member.sfu || member.turnUser) {
        try { await this.clearSfu(member); } catch (error) { console.warn("Realtime cleanup failed", String(error).slice(0, 80)); }
      }
    });
    this.sfuQueue = cleanup.catch(() => {});
    await cleanup;
  }

  /** Revokes a member's TURN credentials. A revoke that fails goes to the pending cleanup (until
      the credentials' TTL ends them), so the username outlives the member. Never throws. */
  private async clearTurn(member: Member): Promise<void> {
    const username = member.turnUser;
    if (!username) return;
    const until = member.turnUntil ?? Date.now() + TURN_TTL_S * 1000;
    try {
      await new Turn(this.env as Env & SfuSecrets).revoke(username);
    } catch (error) {
      console.warn("TURN revoke failed; tried again later", error instanceof Error ? error.message.slice(0, 80) : "unknown");
      await this.keepCleanup({ turnUser: username, until });
    }
    if (member.turnUser === username) {
      delete member.turnUser;
      delete member.turnUntil;
    }
  }

  /** Closes a member's SFU session and the other racers' subscriptions to it, and revokes its TURN
      credentials. Never throws, and one failure doesn't stop the rest (release review round 2):
      a session that doesn't close goes to the pending cleanup, retried by the alarm after the
      member is gone; a subscription that doesn't close stays on its subscriber's session, which
      closes it with the rest. */
  private async clearSfu(member: Member): Promise<void> {
    const api = new Realtime(this.env as Env & SfuSecrets);
    await this.clearTurn(member);
    const session = member.sfu;
    if (session) {
      delete member.sfu;
      try {
        await api.close(session);
      } catch (error) {
        console.warn("Realtime close failed; tried again later", error instanceof Error ? error.message.slice(0, 80) : "unknown");
        if (session.channels.length)
          await this.keepCleanup({ session: session.id, channels: session.channels.map(c => c.channelId), until: Date.now() + SFU_CLEANUP_MS });
      }
    }
    for (const other of [...(this.state?.members ?? [])]) {
      const sub = other.sfu;
      if (other.id === member.id || !sub) continue;
      const channels = sub.channels.filter(c => !c.local && c.memberId === member.id);
      if (!channels.length) continue;
      try {
        await api.close(sub, channels);
      } catch (error) {
        console.warn("Realtime close failed", error instanceof Error ? error.message.slice(0, 80) : "unknown");
      }
      // Told either way: the publication is gone for it (the room said so).
      const peer = this.sockets.get(other.id);
      if (peer) this.send(peer, { type: "sfu_closed", channelIds: channels.map(c => c.channelId) });
    }
  }

  /** Adds a resource to the pending cleanup and stores it. */
  private async keepCleanup(item: Omit<Cleanup, "at" | "tries">): Promise<void> {
    this.cleanup.push({ ...item, at: Date.now() + CLEANUP_FIRST_MS, tries: 0 });
    await this.saveCleanup();
  }

  /** Stores the pending cleanup (apart from the room, which may have ended) and makes sure an
      alarm comes for it. */
  private async saveCleanup(): Promise<void> {
    if (!this.cleanup.length) {
      await this.ctx.storage.delete("cleanup");
      return;
    }
    await this.ctx.storage.put("cleanup", this.cleanup);
    const at = Math.min(...this.cleanup.map(x => Math.min(x.at, x.until)));
    const alarm = await this.ctx.storage.getAlarm();
    if (alarm === null || alarm > at) await this.ctx.storage.setAlarm(at);
  }

  /** Tries the pending cleanup that is due again, in the SFU queue; what closes or has run out of
      time is dropped, what fails waits twice as long (CLEANUP_MAX_MS at most). */
  private async retryCleanup(now: number): Promise<void> {
    if (!this.cleanup.some(x => x.at <= now || x.until <= now)) return;
    const run = this.sfuQueue.then(async () => {
      const api = new Realtime(this.env as Env & SfuSecrets), turn = new Turn(this.env as Env & SfuSecrets);
      for (const item of [...this.cleanup]) {
        let done = item.until <= now;
        if (done) console.warn("Realtime cleanup given up: its time is up");
        else if (item.at <= now) {
          try {
            if (item.turnUser) await turn.revoke(item.turnUser);
            else if (item.session && item.channels?.length) {
              const session: SfuSession = { id: item.session, phase: "failed",
                channels: item.channels.map(channelId => ({ memberId: -1, name: "", channelId, local: true })) };
              try {
                await api.close(session);
              } finally {
                item.channels = session.channels.map(c => c.channelId);
              }
            }
            done = true;
          } catch {
            item.tries++;
            item.at = Date.now() + Math.min(CLEANUP_MAX_MS, CLEANUP_FIRST_MS * 2 ** item.tries);
          }
        }
        if (done) this.cleanup = this.cleanup.filter(x => x !== item);
      }
      await this.saveCleanup();
    });
    this.sfuQueue = run.catch(() => {});
    await run.catch((error) => console.warn("Realtime cleanup failed", String(error).slice(0, 80)));
  }

  /** Racers whose Realtime setup failed for good or ran out of time (SFU_SETUP_MS) leave the race
      `startAt` (release review round 2, finding 158): the others' traffic would otherwise wait for
      them forever. They stay in the room. As for any racer who leaves, the others hear `left` and
      the host's AI takes the rider; the racer hears the error, and the room's next snapshot (not
      in the race) takes its game back to the room. Its session and TURN credentials are closed. */
  private async retireSfu(ids: number[], startAt: number): Promise<void> {
    const st = this.state;
    let out = 0;
    for (const id of ids) {
      const race = st?.race;
      if (!st || !race || race.startAt !== startAt || !this.inRace(st, id)) continue;
      out++;
      const member = st.members.find((x) => x.id === id);
      console.warn(`room ${st.code}: member ${id} out of the race: no Realtime connection`);
      if (member?.sfu) member.sfu.phase = "failed";
      const ws = this.sockets.get(id);
      if (ws) this.error(ws, "bad_value", SFU_FAILED_MESSAGE);
      if (st.session) this.retire(st, id, "left");
      else {
        const slot = race.slots.indexOf(id);
        race.left.push(slot);
        if (member) member.ready = false;
        this.rebuildRace();
        for (const s of this.raceSockets) if (s) this.send(s, { type: "left", slot, memberId: id, reason: "left" });
      }
      if (member && (member.sfu || member.turnUser)) this.queueClear(member);
    }
    if (!out || !st) return;
    this.releaseSfu(st);
    if (st.session && st.race && !this.anyRacing(st)) await this.closeRace(st, false);
    await this.changed();
  }

  /** Clears a member's SFU session and TURN credentials after the queued work. */
  private queueClear(member: Member): void {
    const cleanup = this.sfuQueue.then(() => this.clearSfu(member));
    this.sfuQueue = cleanup.catch(() => {});
    this.ctx.waitUntil(cleanup.catch(() => {}).then(() => this.save()));
  }

  /** Removes a member (idempotent). A racer's rider goes to the host's AI (the game decides). */
  private async leave(memberId: number, reason: "disconnect" | "leave" | "kicked"): Promise<void> {
    const st = this.state;
    if (!st) return;
    const i = st.members.findIndex((x) => x.id === memberId);
    if (i < 0) return;
    const departed = st.members[i];
    if (departed.sfu || departed.turnUser) this.queueClear(departed);
    if (st.session) this.retire(st, memberId, "left");
    st.members.splice(i, 1);
    this.releaseSfu(st);
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
    return ordered.filter(track => !st.pickOnly?.includes(track) && this.trackLimit(st, track) >= count);
  }

  private lockGrid(st: RoomState): void {
    st.grid = st.members.filter(x => x.ready && !x.choosing && this.sockets.has(x.id)).map(x => x.id);
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
    if ((member?.sfu || member?.turnUser) && reason !== "left") this.queueClear(member!);
    this.releaseSfu(st);
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
    await this.closeRaceSfu(st);
  }

  private async destroy(): Promise<void> {
    if (this.listedKey !== null) this.updateListing(false, true);
    this.state = null;
    this.rebuildRace();
    await this.ctx.storage.deleteAlarm();
    await this.ctx.storage.deleteAll();
    // What didn't close outlives the room (Cleanup): stored again, with its alarm.
    if (this.cleanup.length) await this.saveCleanup();
    // Sockets still waiting for hello get room_not_found then; keep their timeout running.
    if (this.ctx.getWebSockets().length) await this.ctx.storage.setAlarm(Date.now() + PING_INTERVAL_MS);
  }

  // ---- Helpers ----

  private rttOf(memberId: number): number | null {
    return measuredRtt(this.firstRtts.get(memberId) ?? null, this.rtts.get(memberId) ?? []);
  }

  private async changed(): Promise<void> {
    const off = !(await this.online()).on;
    const st = this.state;
    if (st?.session) {
      this.updateCountdown(st, off);
      await this.ctx.storage.setAlarm(this.nextAlarm(st, Date.now()));
    }
    if (st && !st.session && this.updateCountdown(st, off)) {
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
      })),
    };
  }

  private broadcastRoom(): void {
    if (this.state) this.broadcast(this.snapshot(this.state));
  }

  /** Where the room runs and each member's region, never their country (finding 135). Apart from
      the room snapshot: that one stays within its 4 KB for ten members, and this changes only on a
      join or when the room learns its colo. Older clients ignore the type. */
  private broadcastWhere(): void {
    const st = this.state;
    if (!st) return;
    this.broadcast({
      type: "where",
      region: this.region(st),
      colo: st.colo ?? "",
      members: st.members.map((x) => ({ id: x.id, region: x.region ?? "" })),
    });
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
        region: this.region(st),
        colo: st.colo ?? "",
        createdIn: st.origin?.createdIn ?? "",
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
