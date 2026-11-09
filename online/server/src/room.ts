// One lobby and its races. Lobby control is JSON on the room WebSocket; the race's frames travel
// over the Realtime SFU, and this object owns the lobby and the SFU signaling. The only binary
// message here is the clock's PING. Protocol: docs/online-protocol.md.
import {
  CallGate, Realtime, Turn, TURN_TTL_S, clientChannels, unknownOutcome, type Allocation, type ListedChannel, type SfuSession,
  type SfuSecrets,
} from "./sfu";
import { DurableObject } from "cloudflare:workers";
import { LISTING_REFRESH_MS, type LobbyEntry } from "./lobbies";
import {
  CLOSE_CODES,
  COUNTDOWN_MS,
  DEFAULT_MAX_PLAYERS,
  FRAME_PING,
  FRAME_PONG,
  HELLO_TIMEOUT_MS,
  OUTDATED_MESSAGE,
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
import { placeOf, regionOfPlace, roomArea, roomRegion, traceColo, type HintSource } from "./region";
import {
  RESTART_CLOSE, RESTART_REASON, UPDATE_NOTICE, WORKER_VERSION_HEADER, newest, nextRecord, restartTarget, safeMoment,
  versionOf, type CodeVersion, type RestartRecord,
} from "./rollout";
import {
  CAPS, TOO_FAST_MESSAGE, TOO_FAST_WARNING, banMessage, findBan, installIdOf, logJoin, newTally, onlineState, tally,
  type Caps, type GuardEnv, type OnlineState, type Tally,
} from "./guard";

/** A racer's Realtime setup, from the race's start message to its channels acknowledged, has
    this long (SFU_SETUP_MS overrides it); one not done by then is out of the race (finding 158,
    release review round 2). A member connected before its grid locked has nothing left to do but
    acknowledge (finding 177); a newcomer's own session takes 0.7-1.5 s over UDP and about 3 s
    through the TURN fallback, then one subscription call per subscriber (finding 182). 25 s leaves
    room for one call that runs into its 10 s timeout, the inspection and the retry, and ends before the game's 30 s
    wait for the others' rides (port/hle/online.c), which travel over the released channels. */
const SFU_SETUP_MS = 25_000;
/** A racer whose own Realtime connection isn't up this long after the race's start message (no
    sfu_connected: UDP and the TURN fallback both failed, or the game never offered) leaves the race
    alone (SFU_CONNECT_MS overrides it). Without this, every other racer waited for its channels:
    a game reports sfu_ready only with every racer's channels, so at SFU_SETUP_MS the whole race
    was out (0.7.12, rooms of 4-7 players, 2026-11-06). The others hear `left`, stop expecting it
    and release. Measured between a PC and a Mac on 2026-11-07 (finding 175): from the SFU's answer
    to connected, UDP 0.69-1.46 s (92 setups) and TURN over TLS 2.9-3.2 s (2 s of UDP first). A
    racer's own session and TURN credentials don't wait in the room's SFU queue (onSfu), so that
    holds in a full room too: at most about 4 s from the start message; 10 s keeps 2.5 times that
    and shortens the others' wait on the load screen for a racer who never connects (15 s before). */
const SFU_CONNECT_MS = 10_000;
/** The room's Realtime and TURN calls run at most this many at once, and its subscription work
    (subscriptions, closes, inspections) at most BACKGROUND_CALLS of them, so a member's own
    session, publication or TURN call always finds a slot (CallGate, finding 182). A Durable Object
    has 6 outgoing connections waiting for headers; past that a fetch waits in the runtime's queue
    with its timeout running. One is left for anything else the object fetches. */
const ROOM_CALLS = 5;
const BACKGROUND_CALLS = 4;
/** A subscription pass's call has this long once it starts (SFU_SUBSCRIBE_TIMEOUT_MS overrides it).
    Production, 30 h to 2026-11-08: calls with at most 3 others of their room in flight took p99
    5.6 s, p99.9 8.3 s, at most 9.4 s (4,363 calls). A call that runs out is no longer a failure,
    only an unknown outcome, so a shorter limit gets to the inspection and retry sooner: at the race
    start a newcomer's 10 s, the backoff, a GET and the retry stay within SFU_SETUP_MS. */
const SUBSCRIBE_TIMEOUT_MS = 10_000;
/** After an unknown outcome a subscriber's session waits this long before it is inspected (twice as
    long for each one in a row, UNSURE_MAX_MS at most), and is inspected again RECHECK_MS after that
    for a second subscription an aborted call made late (finding 182). */
const UNSURE_FIRST_MS = 2_000;
const UNSURE_MAX_MS = 16_000;
const RECHECK_MS = 10_000;
/** A member may start this many Realtime sessions (sfu_offer) in SFU_OFFER_WINDOW_MS: one when it
    joins, and a new one after a failure, a lost connection or a TURN renewal (finding 177). Each
    costs API calls on the others' sessions too. */
const SFU_OFFERS = 6;
const SFU_OFFER_WINDOW_MS = 300_000;
/** Sent to a racer whose Realtime setup failed or ran out of time; it is out of the race (the room
    stays). The footer holds about 45 characters. */
const SFU_FAILED_MESSAGE = "Realtime connection failed: out of the race.";
/** A racer whose session is replaced during its race (finding 187: its race link delivers nothing,
    or a pair it needs can't be made on its session) has this long (SFU_RENEW_MS overrides it) until
    its new session is acknowledged for the race; then it is out of the race, as at SFU_SETUP_MS. A
    new session takes the game's 1 s pause, the answer (~0.4 s), the connection (UDP 0.7-1.5 s, TURN
    about 3 s), the publication (~1 s) and a pass that subscribes it both ways (1-3 s): about 8 s,
    with room for a slow API call. */
const SFU_RENEW_MS = 20_000;
/** What this server takes from a game beyond protocol 4, listed in the welcome (finding 187): a
    game reports a race link that delivers nothing (`sfu_silent`) and names its race's start moment
    to the room (`race_go`) only to a server that lists them; 0.7.13 ignores the list. */
const SERVER_FEATURES = ["sfu_silent", "race_go"];
/** A hello feature (finding 187): see Member.raceGo. */
const FEATURE_RACE_GO = "race_go";
/** A race's start moment comes at most this long after its startAt (the host's game names it once
    every racer has loaded, or 60 s after it got there itself). */
const RACE_GO_MAX_MS = 600_000;

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
    its connection ends, after 30 s); a session whose close failed belongs to a member who left or
    replaced it, and its game dropped that association, so it ends 30 s later anyway. */
const SFU_CLEANUP_MS = TURN_TTL_S * 1000;
/** A subscription whose close failed (Allocation.closeAt) is tried again by the subscription pass
    on the CLEANUP_FIRST_MS schedule (5, 10, 20, 40 s), this many times. Then, if it still forwards a
    live publication to a member outside the race (egress Realtime bills), that member's session is
    reset: its game drops the association and Cloudflare ends the session with its connection,
    whatever the API answers. One that leads nowhere live is forgotten (finding 179). */
const CLOSE_RETRIES = 4;

/** What a subscription pass did (its log line, finding 182): pairs subscribed, with an unknown
    outcome and failed for certain; sessions inspected, channels adopted and unrecorded ones closed. */
interface PassCounts { ok: number; unknown: number; failed: number; inspected: number; adopted: number; extra: number; }

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
/** A hello feature (finding 175): see Member.raceRejoin. */
const FEATURE_RACE_REJOIN = "race_rejoin";

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
  /** Their region as Cloudflare places them ("EU", "NA"...; "" unknown), shown to the room. Never
      their country: rooms are public and players post their logs (finding 135). */
  region?: string;
  /** Their connection dropped (no close from the game) at this time: kept REJOIN_GRACE_MS for the
      game's rejoin, without a socket (finding 137). */
  awaySince?: number;
  /** The rejoin token (rejoinToken): only a hello with it takes this member back. Members made
      before tokens have none; their games (older releases) are below MIN_VERSION. */
  token?: string;
  /** The hello listed FEATURE_RACE_REJOIN: this game keeps racing over the SFU while its room
      connection is down and rejoins its race (finding 175). Older games drop their SFU with the
      socket, so a drop in their race still takes them out of it at once. */
  raceRejoin?: boolean;
  /** This member's Realtime session for its whole stay in the room (finding 177): made from its
      game's sfu_offer when it joins, kept across races with its two publications and its
      subscriptions; closed when the member leaves, when it fails, or when the game offers again
      (a new session replaces it). */
  sfu?: SfuSession;
  /** Counts this member's offers: work begun for an older session is dropped when it ends. */
  sfuAttempt?: number;
  /** When this member's recent offers came (at most SFU_OFFERS in SFU_OFFER_WINDOW_MS). */
  sfuOffers?: number[];
  /** sfu_connected came for the current session (set on arrival, before its publication). */
  sfuConnected?: boolean;
  /** The race (startAt) this member acknowledged its channels for (sfu_ready). */
  sfuReadyRace?: number;
  /** The race (startAt) whose sfu_ready this member got: one per race, again only after a new
      session. Stored with the room (finding 175). */
  releasedRace?: number;
  /** The session (its sfuAttempt) this member got TURN credentials for (one mint per session), and
      their username, kept to revoke them; neither ever goes to a client but its own sfu_turn. */
  turnAttempt?: number;
  turnUser?: string;
  /** When those credentials end by themselves (their TTL). */
  turnUntil?: number;
  /** The hello listed FEATURE_RACE_GO: this game takes its race's start moment from the room
      (`race_go`) when its race link never brought the host's (finding 187). */
  raceGo?: boolean;
  /** The race (startAt) in which the room replaced this member's session (sfuRenew, finding 187),
      once per race, and the time its new session must be acknowledged by (SFU_RENEW_MS). */
  renewedRace?: number;
  renewBy?: number;
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
  /** Racers whose own Realtime connection isn't up at this time leave alone (SFU_CONNECT_MS). */
  sfuConnectBy?: number;
  /** The racers' traffic was released (sfu_ready sent). */
  sfuReleased?: boolean;
  /** The start message's players, kept for a racer whose rejoin missed the start (finding 175). */
  players?: StartPlayer[];
  /** The moment the race's countdown runs from (server ms), as the first racer's game that named it
      told the room (race_go, finding 187): sent to the racers that take it from the room. */
  goAt?: number;
}
interface StartPlayer { slot: number; memberId: number; name: string; rider: number; bike: number; }

interface RoomState {
  /** Always true: every room has the race room rules (finding 115) since the old rules for
      releases before 0.7.4 went (finding 185). Still written, so a previous version of this code
      that loads the room during a rollout keeps those rules. */
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
  /** The Durable Object location hint it was made with ("" none), and how it was picked (finding
      181; absent in rooms made before). */
  hint: string;
  how?: HintSource;
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
      this.lastRestart = (await ctx.storage.get<RestartRecord>("update")) ?? null;
      for (const ws of ctx.getWebSockets()) {
        const info = ws.deserializeAttachment() as SocketInfo | null;
        if (info?.memberId != null) this.sockets.set(info.memberId, ws);
      }
      this.backFromRestart();
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
        // One line per room for the server's logs: where hints really put rooms (finding 181).
        const o = this.state.origin;
        if (o) console.log(`room ${this.state.code}: runs in ${colo}, made with ${o.hint || "no hint"} (${o.how ?? "country"}) from ${o.createdFrom || "?"}`);
        this.broadcastWhere();
        this.updateListing();
      } catch {}
    })());
  }

  /** Where the room runs, for players: "EU", "NA", "ASIA"... ("" unknown). */
  private region(st: RoomState): string {
    return roomRegion(st.colo, st.origin?.hint);
  }

  /** The same, finer: "EU WEST", "ASIA SE"... ("" unknown; finding 181). */
  private area(st: RoomState): string {
    return roomArea(st.colo, st.origin?.hint);
  }

  async fetch(request: Request): Promise<Response> {
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket")
      return new Response("Expected a WebSocket upgrade", { status: 426 });
    // Restarting for new code (finding 184): the game's rejoin tries again after its backoff.
    if (this.restarting) return new Response("Restarting", { status: 503 });
    this.heardVersion = newest(this.heardVersion, versionOf(request.headers.get(WORKER_VERSION_HEADER)));
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
    if (this.restarting) return;
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
      this.pong(ws, msg);
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
      // Serialize across external awaits without holding the DO input gate. What touches only this
      // member's own session runs at once, in parallel with the others' (2026-11-07, finding 175):
      // its publish (sfu_connected; 7 racers' publishes took 6 s one after another in the queue),
      // its TURN credentials, its acknowledgement and its first session (the last racers' answers
      // and a TURN racer's credentials came seconds late behind the others' work). A session made
      // for an offer that a newer one replaced is dropped (onSfu's attempt checks). An offer that
      // replaces a session stays in the queue: the old one's channels close first.
      const own = m.type !== "sfu_offer" || !me.sfu;
      const run = own ? this.onSfu(ws, me, m) : this.sfuQueue.then(() => this.onSfu(ws, me, m));
      if (!own) this.sfuQueue = run.then(() => {}, () => {});
      this.sfuCalls++;  // no restart for new code meanwhile (finding 184)
      try {
        if (await run.catch(() => false)) await this.sfuFailed([me.id]);
      } finally {
        this.sfuCalls--;
      }
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
        if (st.race && this.inRace(st, me.id) && st.race.results!.find(x => x.id === me.id)?.ticks === undefined) return this.error(ws, "room_started", "The race is running.");
        return this.onSettings(ws, st, m);
      case "pick":
        return this.onPick(ws, st, me, isHost, m);
      case "ready":
        if (st.race && this.inRace(st, me.id)) return this.error(ws, "room_started", "The race is running.");
        if (m.ready === true && me.choosing) return this.error(ws, "not_ready", "Choose your rider first.");
        me.ready = m.ready === true;
        if (st.grid && !me.ready) st.grid = st.grid.filter(id => id !== me.id);
        if (st.waitingForRider && me.ready) {
          st.waitingForRider = false;
          st.nextRaceAt = Date.now() + 30_000;
        }
        return this.changed();
      case "kick": {
        if (!isHost) return this.error(ws, "not_host", "Only the host can remove players.");
        const target = st.members.find((x) => x.id === m.member);
        if (!target || target.id === me.id) return this.error(ws, "bad_value", "No such player.");
        const tws = this.sockets.get(target.id);
        await this.leave(target.id);
        if (tws) this.close(tws, "kicked", "The host removed you from the room.");
        return;
      }
      case "start":
        if (!isHost) return this.error(ws, "not_host", "Only the host can start the race.");
        return this.onStart(ws, st);
      case "finish":
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
        this.retire(st, me.id, "gave_up");
        if (st.race && !this.anyRacing(st)) await this.closeRace(st, false);
        return this.changed();
      case "race_go":
        return this.onRaceGo(st, me, m);
      case "end":
        if (!isHost) return this.error(ws, "not_host", "Only the host can end the race.");
        if (!st.race) return;
        await this.closeRace(st, true);
        return this.changed();
      case "leave":
        await this.leave(me.id);
        try {
          ws.close(1000, "leave");
        } catch {}
        return;
      default:
        return this.refuse(ws, "bad_message", `Unknown message type "${m.type}".`);
    }
  }

  async webSocketClose(ws: WebSocket, code: number, _reason?: string, wasClean?: boolean): Promise<void> {
    if (this.restarting) return;  // the restart closed it: the member waits for its rejoin
    // 1006: the connection ended without a close frame (the game's network dropped, or it gave a
    // silent connection up): it may come back. A game that leaves sends "leave" first.
    await this.dropSocket(ws, code === 1006 || wasClean === false);
    try {
      ws.close(1000);
    } catch {}
  }

  async webSocketError(ws: WebSocket): Promise<void> {
    if (this.restarting) return;
    await this.dropSocket(ws, true);
  }

  /** A cleanup pass runs (retryCleanup): the alarm starts no second one meanwhile. */
  private cleaning = false;

  async alarm(): Promise<void> {
    const now = Date.now();
    // In the background (finding 175): the alarm awaited the pass, which waits for the SFU queue
    // and makes its own API calls (5.4 s each for a gone session's close), so the setup checks
    // below ran a minute late (room SHCF93: "setup after 68271 ms").
    if (!this.cleaning) {
      this.cleaning = true;
      this.ctx.waitUntil(this.retryCleanup(now).finally(() => { this.cleaning = false; }));
    }
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
            await this.leave(x.id);
            try {
              ws.close(1001, "silent");
            } catch {}
          }
        } else if (now > (x.awaySince != null ? x.awaySince + REJOIN_GRACE_MS : this.startedAt + HELLO_TIMEOUT_MS)) {
          // Not back in time: after a drop (awaySince), or after a restart (a deploy drops every
          // socket and no close event comes: members without a socket at the start are ghosts).
          await this.leave(x.id);
        }
      }
    const st = this.state;
    if (!st) {
      // An ended room still wakes for sockets waiting for hello and for its pending cleanup.
      const at = Math.min(pending ? now + PING_INTERVAL_MS : Infinity, ...this.cleanup.map(x => Math.min(x.at, x.until)));
      if (at !== Infinity) await this.ctx.storage.setAlarm(Math.max(now, at));
      return;
    }
    const race = st.race;
    // A pass that an eviction cut short, or a pair still missing for the coming or running race.
    if (this.sfuWork()) void this.subscribePass();
    // Sessions with dead stream ids (finding 187), once replacing them costs no race anything.
    await this.replacePoisoned();
    // A racer whose session was replaced in its race and isn't back in time (finding 187).
    if (race && st.race === race) {
      const overdue = st.members.filter(x => this.inRace(st, x.id) && x.renewedRace === race.startAt && x.renewBy != null && now >= x.renewBy);
      for (const x of overdue) console.warn(`room ${st.code}: member ${x.id}'s new session isn't up ${this.sfuRenewMs()} ms after it was asked for`);
      if (overdue.length) await this.retireSfu(overdue.map(x => x.id), race.startAt, false);
    }
    // A racer whose own connection isn't up (SFU_CONNECT_MS) leaves alone; the others then release.
    if (race && st.race === race && !race.sfuReleased && race.sfuConnectBy != null && now >= race.sfuConnectBy) {
      race.sfuConnectBy = undefined;
      const racers = st.members.filter(x => this.inRace(st, x.id));
      const lost = racers.filter(x => !(x.sfuConnected || x.sfu?.phase === "ready") && !this.renewing(x, race, now));
      console.log(`room ${st.code}: Realtime setup after ${now - race.startAt + START_LEAD_MS} ms: ` + racers.map(x =>
        `${x.id}${x.region ? "/" + x.region : ""}:${!x.sfu ? "no-session" : x.sfuReadyRace === race.startAt ? "ready" :
          x.sfuConnected || x.sfu.phase === "ready" ? "connected" : x.turnAttempt === x.sfuAttempt ? "turn" : "offered"}`).join(" "));
      if (lost.length) await this.retireSfu(lost.map(x => x.id), race.startAt, false);
      else await this.save();  // the check is done: not again after an eviction (finding 175)
    }
    // Every racer ready but no release yet (a reconcile that never asked again, finding 175):
    // asked again each tick; releaseSfu does nothing until every racer is ready.
    if (race && st.race === race && !race.sfuReleased) this.releaseSfu(st);
    // Realtime setup not done in time (SFU_SETUP_MS): those racers are out, the rest go on; a
    // connected one's channels never opened, so its game starts a new session.
    if (race && st.race === race && !race.sfuReleased && race.sfuBy != null && now >= race.sfuBy) {
      const late = st.members.filter(x => this.inRace(st, x.id) &&
        !(x.sfuReadyRace === race.startAt && x.sfu?.phase === "ready") && !this.renewing(x, race, now)).map(x => x.id);
      if (late.length) await this.retireSfu(late, race.startAt, true);
    }
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
    // The countdown's end. Alarms fire at or after their time; a little early still counts.
    if (st.countdownEndsAt != null && now >= st.countdownEndsAt - 20 && !st.race) {
      if (st.grid?.length) await this.startRace(st);
      else st.countdownEndsAt = null;
    }
    if (st.members.length === 0) {
      if (pending === 0 && now - st.createdAt > UNUSED_ROOM_MS) return this.destroy();
    } else {
      const ping = JSON.stringify({ type: "ping", s: now });
      for (const ws of this.sockets.values()) this.sendRaw(ws, ping);
      this.broadcastRoom();
      if (now - this.listedAt > LISTING_REFRESH_MS) this.updateListing(true);
    }
    await this.ctx.storage.setAlarm(this.nextAlarm(st, now));
    await this.updateAtSafeMoment(st, now, pending);
  }

  /** The next ping, or a deadline (the countdown's end, the intermission's, the finish window's, a
      Realtime setup's) if that is sooner. */
  private nextAlarm(st: RoomState, now: number): number {
    const ping = now + PING_INTERVAL_MS;
    const sfuBy = st.race && !st.race.sfuReleased ? Math.min(st.race.sfuBy ?? Infinity, st.race.sfuConnectBy ?? Infinity) : null;
    const race = st.race;
    const renewBy = race ? st.members.filter(x => x.renewedRace === race.startAt && x.renewBy != null && this.inRace(st, x.id))
      .map(x => x.renewBy!) : [];
    return Math.min(ping, ...[st.countdownEndsAt, st.nextRaceAt, st.race?.finishEndsAt, sfuBy, ...renewBy]
      .filter((x): x is number => x != null).map(x => Math.max(now, x)));
  }

  /** `member`'s session is being replaced in `race` (sfuRenew) and its time isn't up: the race's
      setup checks wait for it. */
  private renewing(member: Member, race: Race, now: number): boolean {
    return member.renewedRace === race.startAt && member.renewBy != null && now < member.renewBy;
  }

  /** Usage stats: one Workers Analytics Engine point (dataset dhd_online; wrangler.jsonc STATS).
      Joins carry the release, the country and a hash of the name; races the player count, track
      and laps; a join refused for a full room the member count and the limit. No addresses or names.
      A server without the binding keeps none. */
  private stat(blobs: string[], doubles: number[]): void {
    try {
      this.env.STATS?.writeDataPoint({ indexes: [blobs[0]], blobs, doubles });
    } catch {}
  }

  /** A member's binary PING, answered to it alone: the same bytes with the type changed to PONG,
      then the server's clock (f64 ms, little-endian; protocol 4), on which the race's snapshots
      are stamped (finding 169). Any other binary message is not for the server and is dropped. */
  private pong(ws: WebSocket, data: ArrayBuffer): void {
    const info = ws.deserializeAttachment() as SocketInfo | null;
    const n = data.byteLength;
    if (info?.memberId == null || n < 2 || n > 2 + PING_MAX_PAYLOAD) return;
    const b = new Uint8Array(data);
    if (b[0] !== FRAME_PING) return;
    const pong = new Uint8Array(n + 8);
    pong.set(b);
    pong[0] = FRAME_PONG;
    new DataView(pong.buffer).setFloat64(n, Date.now(), true);
    try {
      ws.send(pong);
    } catch {}
  }

  /** Sends to every racer of the running race still in it. */
  private sendRacers(race: Race, msg: object): void {
    race.slots.forEach((id, slot) => {
      const ws = race.left.includes(slot) ? undefined : this.sockets.get(id);
      if (ws) this.send(ws, msg);
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
    // when they speak an older protocol.
    const minVersion = minVersionOf(this.env.MIN_VERSION);
    const clientVersion = clientVersionOf(m);
    if (isOutdated(clientVersion, minVersion))
      return this.refuse(ws, "client_outdated", OUTDATED_MESSAGE, { minVersion, clientVersion });
    if (m.protocol !== PROTOCOL)
      return this.refuse(ws, "protocol_mismatch", `This server speaks protocol ${PROTOCOL}; update the game.`);
    const version = m.version;
    const iso = m.isoSha256;
    const name = typeof m.name === "string" ? sessionName(m.name).trim() : "";
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
      st.session = true;
      st.rules = {finishWindow: 60, nextRace: "host", then: "next"};
      st.rotation = [];
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
      token: rejoinToken(),
    };
    member.region = info.region ?? "";
    member.raceRejoin = features.includes(FEATURE_RACE_REJOIN);
    // Stored only when set: a room of 0.7.13 games keeps the shape older code wrote (rollout.test.ts).
    if (features.includes(FEATURE_RACE_GO)) member.raceGo = true;
    else delete member.raceGo;
    if (!ghost) st.members.push(member);
    if (asHost) st.hostId = member.id;
    // A racer back in its race (finding 175): it raced on over the SFU meanwhile.
    const backInRace = !!ghost && this.inRace(st, ghost.id) && !!ghost.raceRejoin;
    if (ghost) {
      delete ghost.awaySince;
      // The old socket may not close for minutes (a dead path the room hasn't seen end yet):
      // forget it, so its late close doesn't remove the member, and close it.
      const old = this.sockets.get(ghost.id);
      if (old && old !== ws) {
        old.serializeAttachment({ ...(old.deserializeAttachment() as SocketInfo), memberId: null, gone: true });
        try {
          old.close(REPLACED_CLOSE, "replaced");
        } catch {}
      }
      this.rtts.delete(ghost.id);
      this.firstRtts.delete(ghost.id);
      // A game without race_rejoin never rejoins during its race (it races on alone): a member
      // still in the race is out of it now. A game with it gets its race back (catchUp below).
      if (this.inRace(st, ghost.id) && !backInRace) this.retire(st, ghost.id, "left");
    }
    info.memberId = member.id;
    ws.serializeAttachment(info);
    this.sockets.set(member.id, ws);
    if (ghost) this.noteRejoin(st);
    if (ghost) {
      if (st.race && !this.anyRacing(st)) await this.closeRace(st, false);
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
      // The finer name ("EU WEST"...) and whether the creator picked the region or Auto did
      // (finding 181; older clients ignore them).
      roomArea: this.area(st),
      roomChosen: st.origin?.how === "chosen",
      // Only this game gets it; its rejoin hello sends it back (finding 137).
      rejoinToken: member.token,
      // Whether the room holds a Realtime session for this member (finding 177): "ready", its game
      // keeps its association if it has one (a rejoin), else offers a new session; "setup", none
      // here (a new member, or its session ended): its game offers one.
      sfu: member.sfu && member.sfu.phase !== "failed" ? "ready" : "setup",
      // What this server takes beyond protocol 4 (finding 187; older games ignore it).
      features: SERVER_FEATURES,
      // Back in a running race (finding 175): which one. A game that missed the race's start (its
      // hello names another race or none) gets the start again.
      ...(backInRace ? { race: { startAt: st.race!.startAt } } : {}),
    });
    if (ghost) this.catchUp(ws, st, member, backInRace, m.raceStartAt);
    this.probeColo();
    // First ping right away so the lobby shows a delay quickly.
    this.send(ws, { type: "ping", s: Date.now() });
    await this.changed();
    this.broadcastWhere();
  }

  /** One Realtime signaling message from a member (docs/online-protocol.md). Returns true when its
      session failed for good: the caller ends it (sfuFailed). */
  private async onSfu(ws: WebSocket, me: Member, m: Msg): Promise<boolean> {
    if (this.sockets.get(me.id) !== ws || !this.state?.members.includes(me)) return false;
    const st = this.state;
    const api = this.realtime(true);
    let attempt = me.sfuAttempt;
    const original = me.sfu;
    const current = () => me.sfuAttempt === attempt && !!this.state?.members.includes(me) &&
      (m.type !== "sfu_connected" || me.sfu === original);
    try {
      if (m.type === "sfu_offer") {
        // A member's session for its stay in the room (finding 177); a new offer replaces the old
        // one (its game lost it, or renews its TURN relay), within SFU_OFFERS a window.
        const now = Date.now();
        me.sfuOffers = (me.sfuOffers ?? []).filter((t) => now - t < SFU_OFFER_WINDOW_MS);
        if (me.sfuOffers.length >= SFU_OFFERS) {
          this.error(ws, "bad_value", "Too many Realtime sessions: wait a few minutes.");
          return false;
        }
        me.sfuOffers.push(now);
        attempt = (me.sfuAttempt ?? 0) + 1;
        me.sfuAttempt = attempt;
        if (me.sfu || me.turnUser) await this.clearSfu(me);
        await this.save();
        const opened = await api.open(m.sdp);
        // Replaced meanwhile, or gone: a session nobody connects ends by itself after 30 s.
        if (me.sfuAttempt !== attempt || !this.state?.members.includes(me)) return false;
        me.sfu = opened.session;
        await this.save();
        this.send(ws, opened.message);
      } else if (m.type === "sfu_connected") {
        const session = me.sfu;
        if (!session || session.phase !== "connect" || me.sfuConnected) return false;
        // Admit once before the external await: repeats while publish is in flight must not
        // allocate a second pair (or make every subscriber follow duplicate publications).
        me.sfuConnected = true;
        const channels = await api.publish(me.id, session);
        if (me.sfu !== session || !this.state?.members.includes(me)) {
          // Replaced or cleared during the call: what it allocated closes with that session.
          await api.close(session).catch(() => undefined);
          return false;
        }
        this.send(ws, { type: "sfu_channels", channels: clientChannels(channels) });
        await this.save();
        // Subscriptions run in one coalesced pass (subscribePass): a newcomer to the coming or
        // running race gets its missing pairs; anyone else waits for its next grid.
        this.kickSubscribe();
      } else if (m.type === "sfu_turn") {
        // UDP to the SFU did not connect for this member: TURN over TLS (finding 158, round 4).
        const attempt = me.sfuAttempt;
        if (!me.sfu) { this.error(ws, "bad_value", "TURN requires a Realtime session."); return false; }
        if (me.turnAttempt === attempt) { this.error(ws, "bad_value", "One TURN credential per session."); return false; }
        me.turnAttempt = attempt;
        await this.save();
        await this.clearTurn(me);
        let minted: Awaited<ReturnType<Turn["mint"]>>;
        try {
          minted = await this.turn().mint(`room-${st.code}-m${me.id}`);
        } catch (error) {
          // No relay: the member's UDP attempt goes on (its session stays usable); nothing sent.
          console.warn("TURN credentials unavailable", error instanceof Error ? error.message.slice(0, 80) : "unknown");
          return false;
        }
        const until = Date.now() + TURN_TTL_S * 1000;
        if (me.sfuAttempt !== attempt || !this.state?.members.includes(me) || this.sockets.get(me.id) !== ws) {
          // The mint belongs to the old attempt. Never overwrite the replacement's credential
          // record, even temporarily: revoke this result using a detached owner instead.
          await this.clearTurn({ ...me, turnUser: minted.username, turnUntil: until });
          return false;
        }
        me.turnUser = minted.username;
        me.turnUntil = until;
        await this.save();
        this.send(ws, minted.message);
      } else if (m.type === "sfu_ready") {
        // Its channels for this race are open (an older race's acknowledgement counts for nothing).
        const race = st.race;
        if (!race || !this.inRace(st, me.id) || me.sfu?.phase !== "ready") return false;
        if (m.race !== undefined && m.race !== race.startAt) return false;
        if (me.sfuReadyRace === race.startAt) return false;
        me.sfuReadyRace = race.startAt;
        // A session replaced before the release is back: the setup deadline covers it again.
        if (!race.sfuReleased) delete me.renewBy;
        this.releaseSfu(st);
        await this.save();
      } else if (m.type === "sfu_silent") {
        // Its race traffic was released but nothing came from any other racer (finding 187): its
        // session's stream ids can be dead on Cloudflare's side with every call answered 200 and
        // every channel listed active, and only a new session clears that. Once per race; a game
        // still silent on its new session is out of the race (the race can't reach it).
        const race = st.race;
        if (!race || !this.inRace(st, me.id) || m.race !== race.startAt || me.releasedRace !== race.startAt ||
            me.sfu?.phase !== "ready") return false;
        if (me.renewedRace === race.startAt) {
          console.warn(`room ${st.code}: member ${me.id}'s race link is still silent on its new session`);
          await this.retireSfu([me.id], race.startAt, true);
          return false;
        }
        this.sfuRenew(me, race, "hears nothing from the other racers (sfu_silent)");
        await this.save();
      } else this.error(ws, "bad_value", "Unknown signaling message.");
      return false;
    } catch (error) {
      let detail = error instanceof Error ? error.message : "unknown";
      const secrets = this.env as Env & SfuSecrets;
      for (const value of [secrets.REALTIME_APP_ID, secrets.REALTIME_APP_TOKEN, secrets.TURN_KEY_ID, secrets.TURN_KEY_API_TOKEN])
        if (value) detail = detail.replaceAll(value, "<redacted>");
      console.warn("Realtime setup failed", detail.slice(0, 200));
      // An old call may fail after a newer offer replaced its session. Only the operation's
      // own generation may fail the member; otherwise a healthy replacement is torn down.
      if (!current()) return false;
      // No retry of the call (a partly made allocation must not be doubled): the session ends.
      if (me.sfu) me.sfu.phase = "failed";
      await this.save();
      return current();
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
    // Ready means "count me in for the next race", not "I agree to these settings": a new track or
    // computer riders on or off keep it. Cheats on or off make another kind of race, so everyone
    // outside the running race readies again for it (finding 128).
    if (m.laps !== undefined && (((m.laps as number) ^ st.laps) & LAPS_CHEATS))
      for (const x of st.members) if (!this.inRace(st, x.id)) x.ready = false;
    const newTrack = m.track !== undefined && m.track !== st.track;
    if (m.track !== undefined) st.track = m.track as number;
    // A new track during the start countdown: the grid is locked again and the countdown runs
    // again in full, so the ready players see the change; the new track's limit is checked.
    if (newTrack && !st.race && st.countdownEndsAt != null && st.grid) this.lockGrid(st);
    if (m.laps !== undefined) st.laps = m.laps as number;
    if (m.seed !== undefined) st.seed = m.seed as number | null;
    if (m.public !== undefined) st.public = m.public as boolean;
    if (m.roomName !== undefined) st.roomName = sessionName(m.roomName as string).trim();
    if (m.maxPlayers !== undefined) st.maxPlayers = m.maxPlayers as number;
    await this.changed();
  }

  private async onPick(ws: WebSocket, st: RoomState, me: Member, isHost: boolean, m: Msg): Promise<void> {
    const target = m.member === undefined ? me : st.members.find((x) => x.id === m.member);
    if (target && this.inRace(st, target.id)) return this.error(ws, "room_started", "The rider is racing.");
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
    if (!st.members.some(x => x.ready && !x.choosing)) return this.error(ws, "not_ready", "Nobody is ready.");
    if (st.countdownEndsAt == null) this.lockGrid(st);
    return this.changed();
  }

  /** A racer's game named the race's start moment (finding 187): the moment every game holds its
      countdown for, sent as the host's START event over the race link too. The room keeps the first
      one, from the racer whose game names it (the race's host, or once it left, the lowest slot still
      racing: port/hle/online_start.h start_namer), and sends it to every racer that takes it from the
      room, so a game whose race link carries nothing still starts with the others. */
  private async onRaceGo(st: RoomState, me: Member, m: Msg): Promise<void> {
    const race = st.race;
    if (!race || race.goAt != null || m.race !== race.startAt || !this.inRace(st, me.id)) return;
    const namer = race.slots.findIndex((_, slot) => !race.left.includes(slot));
    const at = m.at;
    if (race.slots[namer] !== me.id || typeof at !== "number" || !Number.isFinite(at) ||
        at < race.startAt || at > race.startAt + RACE_GO_MAX_MS) return;
    race.goAt = at;
    console.log(`room ${st.code}: the race's start moment, from member ${me.id}: ${Math.round(at - race.startAt)} ms after startAt`);
    this.sendRaceGo(race);
    await this.save();
  }

  /** The race's start moment to its racers whose games take it from the room (Member.raceGo). */
  private sendRaceGo(race: Race, only?: Member): void {
    if (race.goAt == null) return;
    const msg = JSON.stringify({ type: "race_go", race: race.startAt, at: race.goAt });
    for (const x of only ? [only] : this.state?.members ?? []) {
      const ws = this.sockets.get(x.id);
      if (ws && x.raceGo && this.state && this.inRace(this.state, x.id)) this.sendRaw(ws, msg);
    }
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

  private sfuConnectMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).SFU_CONNECT_MS);
    return Number.isFinite(v) && v > 0 ? v : SFU_CONNECT_MS;
  }

  private sfuSetupMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).SFU_SETUP_MS);
    return Number.isFinite(v) && v > 0 ? v : SFU_SETUP_MS;
  }

  private sfuRenewMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).SFU_RENEW_MS);
    return Number.isFinite(v) && v > 0 ? v : SFU_RENEW_MS;
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

  private async startRace(st: RoomState): Promise<void> {
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
    const racers = st.members.filter(x => this.sockets.has(x.id) && st.grid?.includes(x.id)).sort((a, b) => Number(b.id === st.hostId) - Number(a.id === st.hostId));
    const now = Date.now();
    st.race = {
      rules: {...st.gridRules!},
      results: racers.map(x => ({id:x.id, name:x.name, rider:x.rider, place:0})),
      slots: racers.map((x) => x.id),
      left: [],
      track: st.track,
      laps: st.laps,
      seed: st.seed ?? randomSeed(),
      startAt: now + START_LEAD_MS,
      sfuBy: now + this.sfuSetupMs(),
      sfuConnectBy: now + this.sfuConnectMs(),
      players: racers.map((x, slot) => ({ slot, memberId: x.id, name: x.name, rider: x.rider, bike: x.bike })),
    };
    st.grid = undefined; st.gridRules = undefined;
    await this.save();
    const race = st.race;
    this.stat(["race", "", ""], [racers.length, race.track, race.laps]);
    racers.forEach((x, slot) => {
      const s = this.sockets.get(x.id);
      if (s) this.send(s, this.startMessage(race, slot, now));
    });
    this.broadcastRoom();
    this.updateListing();
    // Racers whose sessions were up at the grid's lock are subscribed already (finding 177): their
    // games acknowledge at once. This pass adds what is still missing (a racer that connected
    // since) and closes the subscriptions of members sitting this race out.
    this.kickSubscribe(true);
  }

  /** The race's start for the racer in `slot`. */
  private startMessage(race: Race, slot: number, now: number): Msg {
    const players = race.players ?? race.slots.map((id, s) => {
      const x = this.state?.members.find((m) => m.id === id);
      return { slot: s, memberId: id, name: x?.name ?? "", rider: x?.rider ?? 0, bike: x?.bike ?? 0 };
    });
    return { type: "start", yourSlot: slot, track: race.track, laps: race.laps, seed: race.seed, players,
      serverTime: now, startAt: race.startAt };
  }

  /** What a member back with its token missed while its socket was down, after the welcome. In
      its race (finding 175): the start, if its hello names another race (`known`, the game's
      raceStartAt) or none, and the racers who left meanwhile. Then, in the race or not, its
      channels (all of them: the game keeps those it has and drops the others, finding 177) and,
      once the race's traffic flows and it had acknowledged, sfu_ready. The race data itself never
      waited: it goes over the SFU. */
  private catchUp(ws: WebSocket, st: RoomState, me: Member, inRace: boolean, known: unknown): void {
    const race = st.race;
    if (race && inRace) {
      if (known !== race.startAt) this.send(ws, this.startMessage(race, race.slots.indexOf(me.id), Date.now()));
      for (const left of race.left) {
        const id = race.slots[left], r = race.results?.find((x) => x.id === id);
        this.send(ws, { type: "left", slot: left, memberId: id, reason: r?.ticks !== undefined ? "finished" : r?.dnfReason ?? "left" });
      }
      this.sendRaceGo(race, me);
    }
    if (me.sfu?.phase === "ready") {
      // Not those still closing: the game was told they closed (finding 179).
      this.send(ws, { type: "sfu_channels", full: true, channels: clientChannels(me.sfu.channels.filter((c) => c.closeAt == null)) });
      if (race && inRace && race.sfuReleased && me.sfuReadyRace === race.startAt) {
        this.send(ws, { type: "sfu_ready", race: race.startAt });
        me.releasedRace = race.startAt;
        delete me.renewBy;
      }
    }
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
    // In its race only a game that races on over the SFU (race_rejoin, finding 175) is waited
    // for: it stays in the race, its rider moving on the others' screens, until it is back or the
    // grace is up (the alarm then retires it). An older game's rider would stand still meanwhile.
    const racing = !!st && this.inRace(st, id!);
    if (!dropped || !st || !member || (racing && !member.raceRejoin) || this.sockets.get(id) !== ws)
      return this.leave(id);
    member.awaySince = Date.now();
    this.sockets.delete(id);
    this.rtts.delete(id);
    this.firstRtts.delete(id);
    ws.serializeAttachment({ ...info, memberId: null, gone: true });
    if (racing)
      console.log(`room ${st.code}: member ${id} lost its connection in the race; kept ${REJOIN_GRACE_MS / 1000} s for its rejoin`);
    await this.changed();
  }

  /** Whom subscriptions are for now (finding 177): the running race's racers, or, during the
      countdown before a race, the locked grid's members still in the room, so that the race's
      start finds them subscribed (the pre-setup). Null between races. */
  private sfuGroup(): Member[] | null {
    const st = this.state;
    if (!st) return null;
    if (st.race) return st.members.filter((x) => this.inRace(st, x.id));
    if (st.grid?.length) return st.members.filter((x) => st.grid!.includes(x.id) && this.sockets.has(x.id));
    // A room with the old rules has no grid: its countdown runs only with everyone ready.
    return null;
  }

  /** `sub` holds a live subscription to both channels of `pub`'s current publication: made to its
      present session (a subscription to an earlier session of `pub` is stale, subscribePass closes
      it). Both: an adopted half (finding 182) still needs its other channel. */
  private subscribed(sub: Member, pub: Member): boolean {
    const live = (name: string) => !!sub.sfu?.channels.some((c) => !c.local && c.memberId === pub.id && c.session === pub.sfu?.id && c.name === name && c.closeAt == null);
    return live("state") && live("events");
  }

  /** A subscriber and a publisher of `group` with no subscription between them yet, among those
      whose publication is up. `now`: only a pair a pass can make now, not one whose subscriber
      waits for an unknown outcome (finding 182: asking for a pass for such a pair made the pass
      run again at once, without a call, until the object ran out of CPU time). */
  private missingSubscription(group: Member[], now?: number): [Member, Member] | null {
    const published = group.filter((x) => x.sfu?.phase === "ready");
    for (const a of published) {
      if (now != null && (this.unsure(a.sfu!, now) || a.sfu!.poisoned)) continue;
      for (const b of published) if (a !== b && !this.subscribed(a, b)) return [a, b];
    }
    return null;
  }

  /** Remote channels of `sub` that no longer lead anywhere: to a member gone, or to an earlier
      session of theirs. */
  private staleChannels(sub: Member): Allocation[] {
    const st = this.state;
    return (sub.sfu?.channels ?? []).filter((c) => !c.local && c.closeAt == null && c.session !== st?.members.find((x) => x.id === c.memberId)?.sfu?.id);
  }

  /** `sub`'s subscriptions whose close failed and is due again (finding 179): those to try again
      (CLOSE_RETRIES), those given up that lead to no live publication (forgotten), and whether one
      given up still forwards a live one while `sub` is outside the group (its session is reset;
      in the group it waits: a racer is not reset for a duplicate of a pair it receives anyway). */
  private failedCloses(sub: Member, group: Member[] | null, now: number): { retry: Allocation[]; forget: Allocation[]; reset: boolean } {
    const st = this.state;
    const retry: Allocation[] = [], forget: Allocation[] = [];
    let reset = false;
    for (const c of sub.sfu?.channels ?? []) {
      if (c.closeAt == null || c.closeAt > now) continue;
      if ((c.closeTries ?? 0) <= CLOSE_RETRIES) retry.push(c);
      else if (!st?.members.some((m) => m.id === c.memberId && m.sfu?.id === c.session)) forget.push(c);
      else if (!group?.includes(sub)) reset = true;
    }
    return { retry, forget, reset };
  }

  /** `session` waits for the outcome of an earlier call (finding 182): no change on it before then. */
  private unsure(session: SfuSession, now: number): boolean {
    return session.unsure != null && session.unsure > now;
  }

  /** The subscription pass has work: a pair missing in the group, a stale channel to close, a failed
      close due again, or an inspection due (finding 182). */
  private sfuWork(): boolean {
    const group = this.sfuGroup(), now = Date.now();
    return (!!group && !!this.missingSubscription(group, now)) ||
      !!this.state?.members.some((x) => {
        const s = x.sfu;
        if (s?.phase !== "ready") return false;
        if ((s.unsure != null && s.unsure <= now) || (s.recheckAt != null && s.recheckAt <= now)) return true;
        const failed = this.failedCloses(x, group, now);
        return this.staleChannels(x).length || failed.retry.length || failed.forget.length || failed.reset;
      });
  }

  /** Runs `change` after the changes already queued on session `id`: Cloudflare asks for the
      mutations of one session one at a time (Realtime SFU, "Serialize mutations per session"); a
      departure's closes and a pass's calls on one subscriber came at once (finding 182). */
  private sessionTail = new Map<string, Promise<unknown>>();
  private serial<T>(id: string, change: () => Promise<T>): Promise<T> {
    const run = (this.sessionTail.get(id) ?? Promise.resolve()).then(change, change);
    const tail = run.catch(() => {});
    this.sessionTail.set(id, tail);
    void tail.then(() => { if (this.sessionTail.get(id) === tail) this.sessionTail.delete(id); });
    return run;
  }

  /** Closes subscriptions `channels` of `session`. Those that don't close stay on it marked
      closing, tried again by the subscription pass (failedCloses), stored with the room so an
      eviction keeps them (finding 179): forgotten, a member sitting races out kept receiving the
      racers' frames until its session ended. */
  private async closeSubscriptions(api: Realtime, session: SfuSession, channels: Allocation[]): Promise<void> {
    try {
      await this.serial(session.id, () => api.close(session, channels));
    } catch (error) {
      const now = Date.now();
      const left = channels.filter((c) => session.channels.includes(c));
      for (const c of left) {
        c.closeTries = (c.closeTries ?? 0) + 1;
        c.closeAt = now + CLEANUP_FIRST_MS * 2 ** (c.closeTries - 1);
      }
      console.warn(`Realtime close failed; ${left.length} subscriptions tried again later`, error instanceof Error ? error.message.slice(0, 80) : "unknown");
    }
  }

  /** Brings `sub`'s record in line with its session as Cloudflare lists it after an unknown outcome
      (finding 182): a subscription to a member of the group that the record lacks is adopted (the
      aborted call made it; its subscriber hears it in sfu_channels), one it already has, or one to
      nobody in the group, is closed (an aborted call made it after a retry, or for a pair no longer
      wanted). Channels the record knows (live or closing) are left alone. */
  private async adopt(api: Realtime, sub: Member, session: SfuSession, listed: ListedChannel[], group: Member[] | null,
                      counts: PassCounts): Promise<void> {
    const st = this.state;
    const adopted: Allocation[] = [], extra: Allocation[] = [];
    for (const c of listed) {
      if (c.local || c.status === "inactive" || session.channels.some((a) => a.channelId === c.id)) continue;
      const pub = st?.members.find((m) => m !== sub && m.sfu?.phase === "ready" && m.sfu.id === c.session);
      const wanted = !!pub && !!group?.includes(pub) && !!group.includes(sub) && (c.name === "state" || c.name === "events") &&
        !session.channels.some((a) => !a.local && a.memberId === pub.id && a.session === c.session && a.name === c.name && a.closeAt == null);
      const allocation: Allocation = { memberId: pub?.id ?? -1, name: c.name, channelId: c.id, local: false, ...(c.session ? { session: c.session } : {}) };
      if (wanted) {
        session.channels.push(allocation);
        adopted.push(allocation);
      } else extra.push(allocation);
    }
    counts.adopted += adopted.length;
    const peer = this.sockets.get(sub.id);
    if (adopted.length && peer && sub.sfu === session) this.send(peer, { type: "sfu_channels", channels: clientChannels(adopted) });
    if (!extra.length) return;
    // Never on the record, so nobody hears of them; one that doesn't close is tried at the next
    // inspection, then left to the session's end. Their game never had them, so the stream ids
    // they free stay dead on Cloudflare's side (finding 187): the session gets no new subscription
    // and is replaced (replacePoisoned).
    if (!session.poisoned)
      console.warn(`room ${st?.code ?? ""}: member ${sub.id}: ${extra.length} subscriptions its game never had are closed; its session is replaced when that costs no race`);
    session.poisoned = true;
    try {
      await this.serial(session.id, () => api.close({ ...session, channels: [...extra] }, extra));
      counts.extra += extra.length;
    } catch (error) {
      console.warn(`Realtime close of ${extra.length} unrecorded subscriptions failed`, error instanceof Error ? error.message.slice(0, 80) : "unknown");
      if (session.recheckAt == null) session.recheckAt = Date.now() + RECHECK_MS;
    }
  }

  /** An inspection due on `sub`'s session (after an unknown outcome, or its recheck): lists the
      session and adopts or closes what the record doesn't know. False while the outcome stays
      unknown (the inspection itself got no answer) or the session failed. */
  private async inspect(api: Realtime, sub: Member, session: SfuSession, group: Member[] | null, counts: PassCounts,
                        failed: { id: number; session: SfuSession }[]): Promise<boolean> {
    const now = Date.now();
    const due = session.unsure != null && session.unsure <= now, recheck = session.recheckAt != null && session.recheckAt <= now;
    if (!due && !recheck) return true;
    let listed: ListedChannel[];
    try {
      listed = await this.serial(session.id, () => api.inspect(session));
    } catch (error) {
      if (sub.sfu !== session) return false;
      if (unknownOutcome(error)) {
        this.unsureOf(session);
        counts.unknown++;
        return false;
      }
      console.warn("Realtime inspection failed", error instanceof Error ? error.message.slice(0, 80) : "unknown");
      session.phase = "failed";
      if (!failed.some(f => f.session === session)) failed.push({ id: sub.id, session });
      return false;
    }
    if (sub.sfu !== session || session.phase !== "ready") return false;
    // The recheck comes once after an inspection that found the outcome: a call Cloudflare carried
    // out after that GET (an empty snapshot doesn't settle a pending request) shows by then.
    session.unsure = undefined;
    session.recheckAt = due ? Date.now() + RECHECK_MS : undefined;
    counts.inspected++;
    await this.adopt(api, sub, session, listed, group, counts);
    return true;
  }

  /** Marks `session` as waiting for an unknown outcome: inspected after the backoff. */
  private unsureOf(session: SfuSession): void {
    session.unknowns = (session.unknowns ?? 0) + 1;
    session.unsure = Date.now() + Math.min(UNSURE_MAX_MS, UNSURE_FIRST_MS * 2 ** (session.unknowns - 1));
  }

  /** Subscribes every published member of the group to every publication it lacks, one call per
      subscriber (finding 182), each endpoint told only its own allocations, and first closes stale
      channels (and, at a race's start, the subscriptions of members outside it: a member who sits a
      race out receives nothing) and tries failed closes again (failedCloses). A subscriber whose
      last call's outcome is unknown is left alone until its inspection is due, then inspected
      first. All calls go through the room's gate, the calls on one session one at a time (serial).
      Leaves and kicks are not queued with this, so the group is read again before every call: a
      departure during one can't make it skip anyone (release review round 2, finding 158). Rounds
      repeat until nothing is missing. Returns the subscribers whose call failed for certain (an
      item error, a 4xx), or whose subscriptions don't close: their session is failed (no second
      attempt), the caller ends it. An unknown outcome fails nobody. */
  private async subscribeGroup(api: Realtime, prune: boolean, counts: PassCounts): Promise<{ id: number; session: SfuSession }[]> {
    const failed: { id: number; session: SfuSession }[] = [];
    const st = this.state;
    if (!st) return failed;
    // Close what leads nowhere, and what a member outside the race holds; try failed closes again.
    const group = this.sfuGroup(), now = Date.now();
    await Promise.all(st.members.filter((x) => x.sfu?.phase === "ready").map(async (sub) => {
      const session = sub.sfu!;
      if (this.unsure(session, now) || !await this.inspect(api, sub, session, group, counts, failed)) return;
      const outside = prune && st.race && group && !group.includes(sub);
      const fresh = outside ? session.channels.filter((c) => !c.local && c.closeAt == null) : this.staleChannels(sub);
      const { retry, forget, reset } = this.failedCloses(sub, group, now);
      if (forget.length) {
        console.warn(`Realtime close given up for ${forget.length} subscriptions to no live publication`);
        session.channels = session.channels.filter((c) => !forget.includes(c));
      }
      if (reset) {
        console.warn(`room ${st.code}: member ${sub.id}'s subscriptions don't close; its session is reset`);
        session.phase = "failed";
        if (!failed.some(f => f.session === session)) failed.push({ id: sub.id, session });
        return;
      }
      const close = [...fresh, ...retry];
      if (!close.length) return;
      await this.closeSubscriptions(api, session, close);
      // Told either way, once: the room took them away.
      const peer = this.sockets.get(sub.id);
      if (fresh.length && peer && sub.sfu === session) this.send(peer, { type: "sfu_closed", channelIds: fresh.map((c) => c.channelId) });
    }));
    for (let round = 0; round <= MAX_MEMBERS; round++) {
      const group = this.sfuGroup();
      if (!group) break;
      const published = group.filter((x) => x.sfu?.phase === "ready");
      const now = Date.now();
      const work = new Map<Member, Member[]>();
      // Not on a poisoned session (finding 187): Cloudflare would give the pair a dead id.
      for (const a of published) for (const b of published)
        if (a !== b && !this.subscribed(a, b) && !this.unsure(a.sfu!, now) && !a.sfu!.poisoned) work.set(a, [...(work.get(a) ?? []), b]);
      if (!work.size) break;
      let progress = false;
      await Promise.all([...work].map(async ([sub, pubs]) => {
        const session = sub.sfu!;
        // An inspection due first (the round may come after the backoff of an earlier round's).
        if (!await this.inspect(api, sub, session, group, counts, failed)) return;
        try {
          const channels = await this.serial(session.id, async () => {
            // Read again when the call starts: who left, who is published, what an earlier change
            // or the inspection gave it meanwhile.
            const now = this.sfuGroup() ?? [];
            if (!now.includes(sub) || sub.sfu !== session || session.phase !== "ready" || session.poisoned ||
                this.unsure(session, Date.now())) return null;
            return api.subscribeAll(session, pubs.filter((p) => now.includes(p) && p.sfu?.phase === "ready" && !this.subscribed(sub, p))
              .map((p) => ({ memberId: p.id, session: p.sfu! })));
          });
          if (!channels) return;
          session.unknowns = 0;
          if (channels.length) progress = true;
          counts.ok += new Set(channels.map((c) => c.memberId)).size;
          const peer = this.sockets.get(sub.id);
          if (channels.length && peer && sub.sfu === session && this.state?.members.includes(sub))
            this.send(peer, { type: "sfu_channels", channels: clientChannels(channels) });
        } catch (error) {
          if (sub.sfu !== session || !this.state?.members.includes(sub)) return;
          if (unknownOutcome(error)) {
            // Not a failure (finding 182): the session is inspected after the backoff, what the
            // call made is adopted, the rest made in a later pass.
            console.warn("Realtime subscription outcome unknown; inspected later", error instanceof Error ? error.message.slice(0, 80) : "unknown");
            this.unsureOf(session);
            counts.unknown += pubs.length;
            return;
          }
          console.warn("Realtime subscription failed", error instanceof Error ? error.message.slice(0, 80) : "unknown");
          counts.failed += pubs.length;
          session.phase = "failed";
          if (!failed.some(f => f.session === session)) failed.push({ id: sub.id, session });
        }
      }));
      if (!progress) break;
    }
    return failed;
  }

  /** One subscription pass at a time, coalesced (2026-11-07): a pass asked for while one runs runs
      once more after it, for the room as it is then (finding 175: a pass must never end with an
      older race). It runs at a grid's lock (the pre-setup, finding 177), at a race's start (pruning
      those outside it), at every publication, and from the alarm when work is left (an inspection
      due included). After each pass the release is checked again. */
  private subscribing = false;
  private subscribeAgain = false;
  private pruneDue = false;
  private kickSubscribe(prune = false): void {
    if (prune) this.pruneDue = true;
    void this.subscribePass();
  }
  private async subscribePass(): Promise<void> {
    if (this.subscribing) { this.subscribeAgain = true; return; }
    this.subscribing = true;
    let called = false;
    try {
      do {
        this.subscribeAgain = false;
        const prune = this.pruneDue;
        this.pruneDue = false;
        const started = Date.now(), race = this.state?.race ?? null;
        const before = this.calls;
        const counts: PassCounts = { ok: 0, unknown: 0, failed: 0, inspected: 0, adopted: 0, extra: 0 };
        const failed = await this.subscribeGroup(this.realtime(false, this.countedFetch, this.subscribeTimeoutMs()), prune, counts);
        // Pairs subscribed, unknown (inspected later) and failed for certain; inspections, channels
        // they adopted and unrecorded ones they closed (finding 182).
        if (this.calls > before)
          console.log(`room ${this.state?.code ?? ""}: Realtime subscription pass (${race ? "race" : "grid"}): ${this.calls - before} calls in ${Date.now() - started} ms; ` +
            `pairs ok ${counts.ok} unknown ${counts.unknown} failed ${counts.failed}; inspected ${counts.inspected} adopted ${counts.adopted} closed ${counts.extra}`);
        await this.save();
        // Saving yields too: a replacement completed since the failing call is healthy.
        const currentFailures = failed.filter(f => this.state?.members.find(m => m.id === f.id)?.sfu === f.session);
        if (currentFailures.length) await this.sfuFailed(currentFailures.map(f => f.id));
        await this.replacePoisoned();
        const st = this.state;
        if (st?.race && !st.race.sfuReleased) this.releaseSfu(st);
        called = this.calls > before;
        // Again only after a pass that made a call: one that made none (every missing pair waits
        // for an inspection) would run again at once, without yielding (finding 182, the test
        // Worker: the object ran out of CPU time). The alarm asks for the next one (sfuWork).
      } while (this.subscribeAgain && called);
    } catch (error) {
      console.warn("Realtime subscription pass failed", error instanceof Error ? error.message.slice(0, 80) : "unknown");
    } finally {
      this.subscribing = false;
    }
  }
  /** The room's call gate (finding 182) and its API clients: `urgent` for a member's own session,
      publication and TURN credentials. */
  private gate = new CallGate(ROOM_CALLS, BACKGROUND_CALLS);
  private realtime(urgent: boolean, request?: typeof fetch, timeoutMs?: number): Realtime {
    return new Realtime(this.env as Env & SfuSecrets, request, { gate: this.gate, urgent, timeoutMs });
  }
  private turn(): Turn {
    return new Turn(this.env as Env & SfuSecrets, undefined, this.gate);
  }
  private subscribeTimeoutMs(): number {
    const v = Number((this.env as unknown as Record<string, unknown>).SFU_SUBSCRIBE_TIMEOUT_MS);
    return Number.isFinite(v) && v > 0 ? v : SUBSCRIBE_TIMEOUT_MS;
  }
  /** API calls the subscription passes made (their log line). */
  private calls = 0;
  /** A test switch for a test server, never set in production: SFU_TEST_LOST_ANSWERS=N makes the
      room's first N subscription calls lose their answer (Cloudflare carries the call out; the room
      waits for its timeout and sees no answer), the slow-call case of finding 182. */
  private lostAnswers?: number;
  private countedFetch: typeof fetch = async (input, init) => {
    this.calls++;
    this.lostAnswers ??= Math.max(0, Math.floor(Number((this.env as unknown as Record<string, unknown>).SFU_TEST_LOST_ANSWERS) || 0));
    const lose = this.lostAnswers > 0 && String(input).endsWith("/datachannels/new") && String(init?.body ?? "").includes("\"remote\"");
    if (lose) this.lostAnswers--;  // before the await: calls run at once
    const answer = await fetch(input, init);
    if (!lose) return answer;
    await answer.body?.cancel();
    console.log(`room ${this.state?.code ?? ""}: test: a subscription call's answer is dropped (${this.lostAnswers} left)`);
    return new Promise<Response>((_, reject) => {
      const signal = init?.signal;
      if (!signal) return reject(new Error("no answer"));
      if (signal.aborted) return reject(signal.reason);
      signal.addEventListener("abort", () => reject(signal.reason), { once: true });
    });
  };

  /** Lets race traffic flow once every racer acknowledged its channels for this race and every pair
      of racers is subscribed both ways. */
  private releaseSfu(st: RoomState): void {
    const race = st.race;
    if (!race) return;
    const racers = st.members.filter(m => this.inRace(st, m.id));
    if (!racers.length || !racers.every(m => m.sfuReadyRace === race.startAt && m.sfu?.phase === "ready")) return;
    if (this.missingSubscription(racers)) {
      // A pass for a pair it can make now; one waiting for its inspection gets the alarm's.
      if (this.missingSubscription(racers, Date.now())) this.kickSubscribe();
      return;
    }
    // Once per race and racer, and stored (finding 175): the release was never saved, so after the
    // object was evicted and loaded again the race looked unreleased; the setup checks ran again
    // ("setup after 66255 ms" for a race released 3 s after its start, room 3G5MXD) and every
    // later retire sent sfu_ready and logged "released" again. A racer that started a new session
    // after the release gets it once more.
    const first = !race.sfuReleased;
    race.sfuReleased = true;
    if (first) console.log(`room ${st.code}: Realtime released ${Date.now() - race.startAt + START_LEAD_MS} ms after the start, ${racers.length} racers`);
    let sent = false;
    for (const member of racers) {
      if (member.releasedRace === race.startAt) continue;
      const peer = this.sockets.get(member.id);
      if (!peer) continue;  // away: its rejoin's catch-up sends it
      this.send(peer, { type: "sfu_ready", race: race.startAt });
      member.releasedRace = race.startAt;
      delete member.renewBy;  // a replaced session is back in its race (finding 187)
      sent = true;
    }
    if (first || sent) this.ctx.waitUntil(this.save());
  }
  /** Revokes a member's TURN credentials. A revoke that fails goes to the pending cleanup (until
      the credentials' TTL ends them), so the username outlives the member. Never throws. */
  private async clearTurn(member: Member): Promise<void> {
    const username = member.turnUser;
    if (!username) return;
    const until = member.turnUntil ?? Date.now() + TURN_TTL_S * 1000;
    try {
      await this.turn().revoke(username);
    } catch (error) {
      console.warn("TURN revoke failed; tried again later", error instanceof Error ? error.message.slice(0, 80) : "unknown");
      await this.keepCleanup({ turnUser: username, until });
    }
    if (member.turnUser === username) {
      delete member.turnUser;
      delete member.turnUntil;
    }
  }

  /** Closes a member's SFU session and the others' subscriptions to it, and revokes its TURN
      credentials. Never throws, and one failure doesn't stop the rest (release review round 2):
      a session that doesn't close goes to the pending cleanup, retried by the alarm after the
      member is gone; a subscription that doesn't close stays marked closing on its subscriber's
      session and is tried again by the subscription pass (finding 179). Its subscribers hear
      sfu_closed. */
  private async clearSfu(member: Member): Promise<void> {
    const api = this.realtime(false);
    const session = member.sfu;
    delete member.sfu;
    delete member.sfuConnected; delete member.sfuReadyRace; delete member.releasedRace;
    await this.clearTurn(member);
    if (session) {
      try {
        await this.serial(session.id, () => api.close(session));
      } catch (error) {
        console.warn("Realtime close failed; tried again later", error instanceof Error ? error.message.slice(0, 80) : "unknown");
        if (session.channels.length)
          await this.keepCleanup({ session: session.id, channels: session.channels.map(c => c.channelId), until: Date.now() + SFU_CLEANUP_MS });
      }
    }
    await Promise.all([...(this.state?.members ?? [])].map(async (other) => {
      const sub = other.sfu;
      if (other.id === member.id || !sub) return;
      const channels = sub.channels.filter(c => !c.local && c.memberId === member.id && c.closeAt == null);
      if (!channels.length) return;
      await this.closeSubscriptions(api, sub, channels);
      // Told either way: the publication is gone for it (the room said so).
      const peer = this.sockets.get(other.id);
      if (peer && other.sfu === sub) this.send(peer, { type: "sfu_closed", channelIds: channels.map(c => c.channelId) });
    }));
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
      const api = this.realtime(false), turn = this.turn();
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

  /** Racers whose Realtime setup failed for good or ran out of time (SFU_CONNECT_MS, SFU_SETUP_MS)
      leave the race `startAt` (release review round 2, finding 158): the others' traffic would
      otherwise wait for them forever. They stay in the room. As for any racer who leaves, the
      others hear `left` and the host's AI takes the rider; the racer hears the error, and the
      room's next snapshot (not in the race) takes its game back to the room. `reset`: its session
      ends too (sfuReset), else it is kept for the next race (a racer still connecting). */
  private async retireSfu(ids: number[], startAt: number, reset: boolean): Promise<void> {
    const st = this.state;
    let out = 0;
    for (const id of ids) {
      const race = st?.race;
      if (!st || !race || race.startAt !== startAt || !this.inRace(st, id)) continue;
      out++;
      const member = st.members.find((x) => x.id === id);
      console.warn(`room ${st.code}: member ${id} out of the race: no Realtime connection`);
      const ws = this.sockets.get(id);
      if (ws) this.error(ws, "bad_value", SFU_FAILED_MESSAGE);
      this.retire(st, id, "left");
      if (member) delete member.renewBy;
      if (member && (reset || member.sfu?.phase === "failed")) this.sfuReset(member);
    }
    if (!out || !st) return;
    this.releaseSfu(st);
    if (st.race && !this.anyRacing(st)) await this.closeRace(st, false);
    await this.changed();
  }

  /** Members whose session failed (an API call on it): it ends (sfuReset), and a racer among them
      is out of the running race, as before finding 177. */
  private async sfuFailed(ids: number[]): Promise<void> {
    const st = this.state;
    if (!st) return;
    const race = st.race;
    const racers = race ? ids.filter((id) => this.inRace(st, id)) : [];
    if (race && racers.length) await this.retireSfu(racers, race.startAt, true);
    for (const id of ids.filter((id) => !racers.includes(id))) {
      const member = st.members.find((x) => x.id === id);
      if (member) this.sfuReset(member);
    }
  }

  /** Ends a member's session the room gave up on: closed after the queued work (queueClear), and
      its game told to offer a new one (`sfu_reset`; an away game hears "setup" in its rejoin's
      welcome). */
  private sfuReset(member: Member): void {
    if (member.sfu) member.sfu.phase = "failed";
    if (member.sfu || member.turnUser) this.queueClear(member);
    const ws = this.sockets.get(member.id);
    if (ws) this.send(ws, { type: "sfu_reset" });
  }

  /** Replaces `member`'s session during `race` (finding 187): sfuReset, and the race waits up to
      SFU_RENEW_MS for its new one (its setup checks spare it meanwhile; the alarm takes it out of the
      race after that). The others keep racing: their subscriptions to it close and come back with
      its new publication. Once per member and race. */
  private sfuRenew(member: Member, race: Race, why: string): void {
    console.warn(`room ${this.state?.code ?? ""}: member ${member.id} ${why}; a new session for its race`);
    member.renewedRace = race.startAt;
    member.renewBy = Date.now() + this.sfuRenewMs();
    this.sfuReset(member);
  }

  /** Replaces the sessions with dead stream ids (SfuSession.poisoned, finding 187) once that costs
      no race anything: at once for a member outside a running race (between races, sitting one
      out, in the countdown), and for a racer only when it needs a pair its session can't take (its
      working pairs are left alone; it is replaced once its race is over). A racer whose session was
      replaced in this race already is out of it instead. */
  private async replacePoisoned(): Promise<void> {
    const st = this.state;
    if (!st) return;
    const group = this.sfuGroup(), race = st.race, out: number[] = [];
    for (const m of [...st.members]) {
      if (!m.sfu?.poisoned || m.sfu.phase !== "ready") continue;
      if (!race || !this.inRace(st, m.id)) {
        console.warn(`room ${st.code}: member ${m.id}'s session has dead stream ids; a new session`);
        this.sfuReset(m);
        continue;
      }
      const lacks = !!group?.includes(m) && group.some((p) => p !== m && p.sfu?.phase === "ready" && !this.subscribed(m, p));
      if (!lacks) continue;
      if (m.renewedRace === race.startAt) out.push(m.id);
      else this.sfuRenew(m, race, "needs a pair its session can't take (dead stream ids)");
    }
    if (out.length && race) await this.retireSfu(out, race.startAt, true);
  }

  /** Clears a member's SFU session and TURN credentials after the queued work. */
  private queueClear(member: Member): void {
    const session = member.sfu;
    const cleanup = this.sfuQueue.then(() => (member.sfu === session ? this.clearSfu(member) : undefined));
    this.sfuQueue = cleanup.catch(() => {});
    this.sfuCalls++;
    this.ctx.waitUntil(cleanup.catch(() => {}).then(() => this.save()).finally(() => { this.sfuCalls--; }));
  }
  /** Removes a member (idempotent). A racer's rider goes to the host's AI (the game decides). */
  private async leave(memberId: number): Promise<void> {
    const st = this.state;
    if (!st) return;
    const i = st.members.findIndex((x) => x.id === memberId);
    if (i < 0) return;
    const departed = st.members[i];
    if (departed.sfu || departed.turnUser) this.queueClear(departed);
    this.retire(st, memberId, "left");
    st.members.splice(i, 1);
    this.releaseSfu(st);
    const ws = this.sockets.get(memberId);
    this.sockets.delete(memberId);
    this.rtts.delete(memberId);
    this.firstRtts.delete(memberId);
    if (ws) {
      // Forget the socket so a late close event doesn't count twice.
      ws.serializeAttachment({ ...(ws.deserializeAttachment() as SocketInfo), memberId: null, gone: true });
    }
    if (st.hostId === memberId) st.hostId = st.members[0]?.id ?? null;
    if (st.members.length === 0) return this.destroy();
    if (st.race && !this.anyRacing(st)) await this.closeRace(st, false);
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
    // The pre-setup (finding 177): the grid's missing subscriptions are made during the countdown,
    // so the start message finds nothing left to do for members already connected.
    this.kickSubscribe();
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
    this.sendRacers(st.race, {type:"left", slot, memberId:id, reason:r.ticks !== undefined ? "finished" : reason});
    // After `left`: the others hear who is gone before their traffic is released (sent once now,
    // finding 175; the repeat used to come after `left` by chance).
    this.releaseSfu(st);
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
    await this.save();
    this.broadcast({type:"ended", reason:voided ? "void" : "done"});
  }

  private async destroy(): Promise<void> {
    if (this.listedKey !== null) this.updateListing(false, true);
    this.state = null;
    await this.ctx.storage.deleteAlarm();
    await this.ctx.storage.deleteAll();
    // What didn't close outlives the room (Cleanup): stored again, with its alarm.
    if (this.cleanup.length) await this.saveCleanup();
    // Sockets still waiting for hello get room_not_found then; keep their timeout running.
    if (this.ctx.getWebSockets().length) await this.ctx.storage.setAlarm(Date.now() + PING_INTERVAL_MS);
  }

  // ---- Code updates at a safe moment (src/rollout.ts, finding 184) ----

  /** The newest Worker version heard on this instance's requests (WORKER_VERSION_HEADER). */
  private heardVersion: CodeVersion | null = null;
  /** The last restart for new code (storage key "update"), and when this instance logs its
      members' rejoins. */
  private lastRestart: RestartRecord | null = null;
  private rejoinsSince: number | null = null;
  /** Saved and closing for a restart: nothing else is handled or stored. */
  private restarting = false;
  /** Realtime signaling calls and queued closes in flight (setup work a restart would cut). */
  private sfuCalls = 0;

  private ownVersion(): CodeVersion | null {
    return versionOf((this.env as unknown as Record<string, unknown>).CF_VERSION_METADATA);
  }

  /** The constructor after a restart for new code: one log line, and the members' rejoins timed. */
  private backFromRestart(): void {
    const last = this.lastRestart, st = this.state;
    if (!last || !st || this.startedAt - last.at > 60_000) return;
    const own = this.ownVersion();
    const adopted = !!own && own.id !== last.from;
    console.log(`room ${st.code}: back ${this.startedAt - last.at} ms after its restart for new code, ` +
      (adopted ? `now on ${own!.id}` : `still on ${last.from} (try ${last.tries})`));
    this.rejoinsSince = last.at;
  }

  /** A member back by token: once every member is back after a restart, how long it took. */
  private noteRejoin(st: RoomState): void {
    if (this.rejoinsSince == null || !st.members.every((x) => this.sockets.has(x.id))) return;
    console.log(`room ${st.code}: all ${st.members.length} members back ${Date.now() - this.rejoinsSince} ms after the restart for new code`);
    this.rejoinsSince = null;
  }

  /** The alarm's last step: restarts the room for new code when one is deployed and nothing is
      under way (safeMoment). `pending`: sockets that haven't said hello yet. */
  private async updateAtSafeMoment(st: RoomState, now: number, pending: number): Promise<void> {
    if (this.restarting || st !== this.state) return;
    const moment = {
      race: !!st.race, grid: !!st.grid?.length, countdown: st.countdownEndsAt != null,
      setup: this.subscribing || this.cleaning || this.sfuCalls > 0, hello: pending > 0,
      nextRaceInMs: st.nextRaceAt != null ? st.nextRaceAt - now : null, members: st.members.length,
    };
    if (!safeMoment(moment)) return;
    const own = this.ownVersion();
    const { rollout } = await this.online();
    const target = restartTarget(own, rollout, this.heardVersion, this.lastRestart, now);
    // The store's answer may have taken a moment: still nothing under way?
    if (target === null || !own || st !== this.state || this.restarting || this.subscribing || this.sfuCalls > 0 ||
        !safeMoment({ ...moment, race: !!st.race, grid: !!st.grid?.length, countdown: st.countdownEndsAt != null })) return;
    await this.restartForUpdate(st, own, target, rollout !== undefined && rollout !== null);
  }

  /** Saves, tells every member (UPDATE_NOTICE), closes their sockets (RESTART_CLOSE) and resets the
      object, which comes back on the deployed code. The games rejoin by token (finding 137) and keep
      their seats; their Realtime sessions are in the stored state (finding 177). */
  private async restartForUpdate(st: RoomState, own: CodeVersion, target: string, byRollout: boolean): Promise<void> {
    const now = Date.now();
    const record = nextRecord(own, target, byRollout, this.lastRestart, now, st.members.length);
    this.lastRestart = record;
    await this.ctx.storage.put("room", st);
    await this.ctx.storage.put("update", record);
    await this.ctx.storage.sync();
    this.restarting = true;
    console.log(`room ${st.code}: restarting for new code at a safe moment: ${own.id} -> ${target}, ` +
      `${st.members.length} members, try ${record.tries}`);
    const notice = JSON.stringify(UPDATE_NOTICE);
    for (const ws of this.ctx.getWebSockets()) {
      const info = ws.deserializeAttachment() as SocketInfo | null;
      if (info?.memberId != null) this.sendRaw(ws, notice);
      try {
        ws.close(RESTART_CLOSE, RESTART_REASON);
      } catch {}
    }
    // The frames go out before the reset.
    await new Promise((resolve) => setTimeout(resolve, 250));
    this.ctx.abort("code update at a safe moment (finding 184)", { retryAlarm: false });
  }

  // ---- Helpers ----

  private rttOf(memberId: number): number | null {
    return measuredRtt(this.firstRtts.get(memberId) ?? null, this.rtts.get(memberId) ?? []);
  }

  private async changed(): Promise<void> {
    const off = !(await this.online()).on;
    const st = this.state;
    if (st) {
      this.updateCountdown(st, off);
      await this.ctx.storage.setAlarm(this.nextAlarm(st, Date.now()));
    }
    await this.save();
    this.broadcastRoom();
    this.updateListing();
  }

  private async save(): Promise<void> {
    if (this.state && !this.restarting) await this.ctx.storage.put("room", this.state);
  }

  private snapshot(st: RoomState) {
    return {
      phase: this.phase(st), raceStartedAt: st.race?.startAt ?? null,
      finishEndsAt: st.race?.finishEndsAt ?? null, nextRaceAt: st.nextRaceAt ?? null,
      waitingForRider: st.waitingForRider ?? false, lastEvent: st.lastEvent ?? null,
      lastRace: st.lastRace ?? null, rules: st.rules, rotation: st.rotation,
      rulesPending: !!((st.race?.rules || st.gridRules) &&
        JSON.stringify(st.race?.rules || st.gridRules) !== JSON.stringify(st.rules)),
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
        ...this.memberRace(st, x.id),
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
      area: this.area(st),
      colo: st.colo ?? "",
      members: st.members.map((x) => ({ id: x.id, region: x.region ?? "" })),
    });
  }

  /** Keeps this room's entry in the public lobby list current. */
  private updateListing(refresh = false, forceRemove = false): void {
    const st = this.state;
    let entry: LobbyEntry | null = null;
    if (!forceRemove && st && st.public && st.hostId !== null) {
      entry = {
        phase: this.phase(st), raceStartedAt: st.race?.startAt ?? null,
        finishEndsAt: st.race?.finishEndsAt ?? null, nextRaceAt: st.nextRaceAt ?? null,
        full: st.members.length >= st.maxPlayers,
        code: st.code,
        roomName: st.roomName,
        version: st.version ?? "",
        track: st.track,
        players: st.members.length,
        maxPlayers: st.maxPlayers,
        hostName: st.members.find((x) => x.id === st.hostId)?.name ?? "",
        region: this.region(st),
        area: this.area(st),
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
