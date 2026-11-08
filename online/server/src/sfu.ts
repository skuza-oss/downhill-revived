// Trusted control plane. Client SDP is consumed here, never copied into a client message.
export interface SfuSecrets {
  REALTIME_APP_ID?: string; REALTIME_APP_TOKEN?: string;
  /** The TURN key (Realtime > TURN) for the fallback over TLS (finding 158, round 4). */
  TURN_KEY_ID?: string; TURN_KEY_API_TOKEN?: string;
}
export interface SfuSession {
  id: string; phase: "connect" | "ready" | "failed"; channels: Allocation[];
  /** A subscription call on this session ended with no answer (finding 182: a timeout, a network
      error, a 5xx, 409 or 429): what it made is unknown, so no other change is made on the session
      until this time, when it is inspected first (GET) and what exists is adopted. */
  unsure?: number;
  /** Such outcomes in a row (the backoff doubles with each); a call that answers resets it. */
  unknowns?: number;
  /** One more inspection then, after an unsure one: an aborted call that Cloudflare still carried
      out after that inspection left a second subscription, closed then (finding 182). */
  recheckAt?: number;
}
/** One channel on a session: its own publication (local) or a subscription to `memberId`'s
    publication on that member's session `session` (remote). The session id never goes to a client
    (clientChannels). A subscription whose close failed stays on its session with `closeAt` (its
    next try) and `closeTries` (the failed ones so far) until a close goes through (finding 179);
    its endpoint was told it closed. */
export interface Allocation { memberId: number; name: string; channelId: number; local: boolean; session?: string; closeAt?: number; closeTries?: number; }
/** Allocations as their own endpoint gets them: no session ids. */
export function clientChannels(channels: Allocation[]): Allocation[] {
  return channels.map(({ memberId, name, channelId, local }) => ({ memberId, name, channelId, local }));
}
type Json = Record<string, unknown>;
const policies = [{ dataChannelName: "state", ordered: false, maxRetransmits: 0 },
                  { dataChannelName: "events", ordered: true }];

/**
 * TURN credentials last this long (seconds). They are minted once per member's Realtime session,
 * only for a member whose UDP to the SFU did not connect, and revoked when that session ends (the
 * member leaves, its session fails or a new one replaces it); the TTL also bounds a credential
 * whose revoke failed. A session now lasts the whole room stay (finding 177) and a relay can't take
 * new credentials, so between races the game replaces a session whose credentials are 35 minutes
 * old (port/render/net/src/sfu.rs TURN_RENEW): no race meets an expired credential.
 */
export const TURN_TTL_S = 3600;

/** Cloudflare's TURN credential API (https://developers.cloudflare.com/realtime/turn/). */
export class Turn {
  constructor(private secrets: SfuSecrets, private request: typeof fetch = (input, init) => fetch(input, init), private gate?: CallGate) {}
  private async call(path: string, body?: unknown): Promise<Response> {
    const { TURN_KEY_ID: key, TURN_KEY_API_TOKEN: token } = this.secrets;
    if (!key || !token) throw new Error("TURN is not configured");
    const request = this.request;
    // Waiting for headers is what the runtime's connection limit counts: the gate holds that part.
    const send = () => request(`https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(key)}/${path}`, {
      method: "POST", headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(CALL_TIMEOUT_MS),
    });
    return this.gate ? this.gate.run(true, send) : send();
  }
  /** New credentials for one racer; `tag` (no personal data) shows in Cloudflare's TURN analytics.
      The message goes to that racer only: the TURN-over-TLS urls, the username and credential. */
  async mint(tag: string): Promise<{ username: string; message: Json }> {
    const r = await this.call("credentials/generate-ice-servers", { ttl: TURN_TTL_S, customIdentifier: tag });
    if (!r.ok) {
      console.warn("TURN credentials failed", r.status);
      throw new Error("TURN request failed");
    }
    const data = await r.json() as { iceServers?: { urls?: unknown; username?: unknown; credential?: unknown }[] };
    const server = data.iceServers?.find(s => typeof s.username === "string" && typeof s.credential === "string");
    const urls = (Array.isArray(server?.urls) ? server!.urls : [server?.urls])
      .filter((u): u is string => typeof u === "string" && /^turns:[a-z0-9.-]+:\d{1,5}(\?transport=tcp)?$/.test(u));
    const username = server?.username as string, credential = server?.credential as string;
    if (!urls.length || !/^[\x21-\x7e]{1,256}$/.test(username ?? "") || !/^[\x21-\x7e]{1,256}$/.test(credential ?? ""))
      throw new Error("Invalid TURN credentials");
    return { username, message: { type: "sfu_turn", urls, username, credential } };
  }
  /** Ends a racer's credentials before their TTL (204 from the API). */
  async revoke(username: string): Promise<void> {
    const r = await this.call(`credentials/${encodeURIComponent(username)}/revoke`);
    if (!r.ok && r.status !== 404) {
      console.warn("TURN revoke failed", r.status);
      throw new Error("TURN revoke failed");
    }
  }
}

/** A Realtime API answer that wasn't OK, with its HTTP status (410: the session is gone). */
export class RealtimeError extends Error {
  constructor(message: string, readonly status: number) { super(message); }
}
/** A Realtime call whose outcome is unknown (finding 182): no answer in time, a network error, or an
    answer cut short. Cloudflare may have carried it out: "a lost response leaves the request's
    outcome unknown" (Realtime SFU, "Retry and reconnect"); the session is inspected before
    anything is made again. */
export class RealtimeUnknown extends Error {}
/** Whether a failed call's outcome is unknown (inspect, then retry) rather than a definite failure
    (an item error, a 4xx other than 409 and 429, an answer that makes no sense). */
export function unknownOutcome(error: unknown): boolean {
  if (error instanceof RealtimeUnknown) return true;
  if (error instanceof RealtimeError) return error.status >= 500 || error.status === 409 || error.status === 429;
  return false;
}

/**
 * The room's Realtime and TURN calls, at most `limit` at once (finding 182). A Durable Object has
 * only 6 outgoing connections waiting for their response headers; a 7th fetch waits in the
 * runtime's queue, where its timeout runs too (16 subscription calls at once: 7 timed out, mostly
 * queued). Here a call waits before it starts, and its timeout starts with it. `urgent` calls (a
 * member's own session, publication and TURN credentials: the connect check waits for them) go
 * first and may take every slot; the others (subscriptions, closes, inspections) at most
 * `background`, so a slot is always left for an urgent call.
 */
export class CallGate {
  private active = 0;
  private waiting: { urgent: boolean; go: () => void }[] = [];
  constructor(readonly limit: number, readonly background: number) {}
  private fits(urgent: boolean): boolean { return this.active < (urgent ? this.limit : this.background); }
  async run<T>(urgent: boolean, call: () => Promise<T>): Promise<T> {
    // Nobody overtakes a waiting call of its own class or an urgent one.
    if (this.fits(urgent) && !this.waiting.some((w) => w.urgent || !urgent)) this.active++;
    else await new Promise<void>((go) => this.waiting.push({ urgent, go }));  // next() counted it
    try {
      return await call();
    } finally {
      this.active--;
      this.next();
    }
  }
  private next(): void {
    for (;;) {
      const urgent = this.waiting.findIndex((w) => w.urgent);
      const i = urgent >= 0 && this.fits(true) ? urgent : this.fits(false) ? this.waiting.findIndex((w) => !w.urgent) : -1;
      if (i < 0) return;
      const [w] = this.waiting.splice(i, 1);
      this.active++;
      w.go();
    }
  }
  /** Calls running and waiting. */
  get load(): { active: number; waiting: number } { return { active: this.active, waiting: this.waiting.length }; }
}
/** How a Realtime instance runs its calls: the room's gate, whether they are urgent, and their own
    timeout (ms). */
export interface CallOptions { gate?: CallGate; urgent?: boolean; timeoutMs?: number; }
/** A call's own time, from its start (after the gate) to its answer: 15 s for a member's own calls,
    as before; the subscription pass sets its own (SUBSCRIBE_TIMEOUT_MS in room.ts). */
export const CALL_TIMEOUT_MS = 15_000;
/** A channel on a session as Cloudflare lists it (GET sessions/{id}); `session` is a subscription's
    publisher session. */
export interface ListedChannel { id: number; name: string; local: boolean; session?: string; status: string; }

export class Realtime {
  constructor(private secrets: SfuSecrets, private request: typeof fetch = (input, init) => fetch(input, init), private options: CallOptions = {}) {}
  private async call(path: string, body?: unknown, method = "POST"): Promise<Json> {
    const { REALTIME_APP_ID: app, REALTIME_APP_TOKEN: token } = this.secrets;
    if (!app || !token) throw new Error("Realtime is not configured");
    const request = this.request, queued = Date.now(), timeout = this.options.timeoutMs ?? CALL_TIMEOUT_MS;
    // The log names the operation, never a session id.
    const op = method === "GET" ? "sessions/get" : `${path.includes("datachannels") ? "datachannels/" : "sessions/"}${path.split("/").at(-1)}`;
    const waited = (started: number) => started - queued > 50 ? ` (waited ${started - queued} ms)` : "";
    const send = async (): Promise<{ r: Response; started: number; data?: Json }> => {
      const started = Date.now();
      let r: Response;
      try {
        r = await request(`https://rtc.live.cloudflare.com/v1/apps/${encodeURIComponent(app)}/${path}`, {
          method, headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
          body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(timeout),
        });
      } catch (error) {
        // No answer (a timeout, a network error): Cloudflare may have carried the call out.
        console.log(`Realtime ${op} no answer ${Date.now() - started} ms${waited(started)}`);
        throw new RealtimeUnknown(error instanceof Error ? error.message.slice(0, 80) : "no answer");
      }
      if (!r.ok) {
        await r.body?.cancel();
        return { r, started };
      }
      try {
        return { r, started, data: await r.json() as Json };
      } catch {
        throw new RealtimeUnknown("Realtime answer cut short");
      }
    };
    const { r, started, data } = this.options.gate ? await this.options.gate.run(!!this.options.urgent, send) : await send();
    // Every call's time (2026-11-07: rooms' setups ran past SFU_SETUP_MS; this says where), and
    // its wait for the room's gate (finding 182).
    console.log(`Realtime ${op} ${r.status} ${Date.now() - started} ms${waited(started)}`);
    if (!r.ok) {
      console.warn("Realtime API operation failed", op, r.status);
      throw new RealtimeError("Realtime request failed", r.status);
    }
    if (data!.errorCode) {
      const code = typeof data!.errorCode === "string" && /^[a-z_]{1,64}$/.test(data!.errorCode) ? data!.errorCode : "unknown";
      console.warn("Realtime API error", op, code);
      throw new Error("Realtime operation failed");
    }
    return data!;
  }
  async open(sdp: unknown): Promise<{ session: SfuSession; message: Json }> {
    if (typeof sdp !== "string" || sdp.length > 32768) throw new Error("Invalid offer");
    const r = await this.call("sessions/new", { sessionDescription: { type: "offer", sdp } });
    if (typeof r.sessionId !== "string" || !/^[a-zA-Z0-9_-]{1,128}$/.test(r.sessionId)) throw new Error("Invalid session");
    const session: SfuSession = { id: r.sessionId, phase: "connect", channels: [] };
    const description = r.sessionDescription as Json | undefined;
    if (description?.type !== "answer" || typeof description.sdp !== "string") throw new Error("Invalid SFU answer");
    // Only this session's SFU answer escapes. Never echo the supplied client offer.
    return { session, message: { type: "sfu_answer", sdp: description.sdp } };
  }
  async close(session: SfuSession, channels = session.channels): Promise<void> {
    if (!channels.length) return;
    let r: Json;
    try {
      r = await this.call(`sessions/${session.id}/datachannels/close`, {
        dataChannels: channels.map(c => ({ id: c.channelId })),
      }, "PUT");
    } catch (error) {
      // 410 (404): the session itself is gone, its channels with it (a racer's session that
      // ended with its connection). Retrying that close forever, 5.4 s a call on the room's SFU
      // queue, held room SHCF93's release for minutes (finding 175).
      if (error instanceof RealtimeError && (error.status === 410 || error.status === 404)) {
        session.channels = session.channels.filter(c => !channels.includes(c));
        return;
      }
      throw error;
    }
    // close_track_error: absent or closed already. Channels that closed are forgotten even when
    // others did not, so a retry asks only for the rest.
    const results = r.dataChannels;
    const closed = Array.isArray(results) ? channels.filter(c => (results as Json[]).some(x => x.id === c.channelId &&
        (!x.errorCode || x.errorCode === "close_track_error"))) : [];
    session.channels = session.channels.filter(c => !closed.includes(c));
    if (closed.length < channels.length) throw new Error("Channel close incomplete");
  }
  async publish(memberId: number, session: SfuSession): Promise<Allocation[]> {
    if (session.phase !== "connect") throw new Error("Transport not connected");
    const allocations = await this.allocate(session, memberId, true);
    session.phase = "ready";
    return allocations;
  }
  /** Subscribes `session` to `publisher`'s publication (both channels): subscribeAll for one. */
  async subscribe(session: SfuSession, memberId: number, publisher: SfuSession): Promise<Allocation[]> {
    return this.subscribeAll(session, [{ memberId, session: publisher }]);
  }
  /** Subscribes `session` to every channel of `publishers` it lacks, in one call (finding 182: a
      remote batch may name several publishing sessions, so a pass makes one call per subscriber,
      not one per pair). Allocations made are kept on the session even when another item fails;
      then this throws (a definite failure). */
  async subscribeAll(session: SfuSession, publishers: { memberId: number; session: SfuSession }[]): Promise<Allocation[]> {
    if (session.phase !== "ready") return [];
    const wanted: { memberId: number; publisher: string; policy: typeof policies[number] }[] = [];
    for (const p of publishers) {
      if (p.session.phase !== "ready") continue;
      for (const policy of policies)
        if (!session.channels.some(x => !x.local && x.memberId === p.memberId && x.session === p.session.id && x.name === policy.dataChannelName && x.closeAt == null))
          wanted.push({ memberId: p.memberId, publisher: p.session.id, policy });
    }
    if (!wanted.length) return [];
    const r = await this.call(`sessions/${session.id}/datachannels/new`, { dataChannels: wanted.map(w => ({
      ...w.policy, location: "remote", sessionId: w.publisher,
    })) });
    const items = r.dataChannels as Json[];
    if (!Array.isArray(items)) throw new Error("Missing channels");
    // An item by its publisher and name when the answer names the publisher, else by its place.
    const itemFor = (w: typeof wanted[number], i: number): Json | undefined => {
      const named = items.find(x => x.sessionId === w.publisher && x.dataChannelName === w.policy.dataChannelName);
      if (named) return named;
      const placed = items.length === wanted.length ? items[i] : undefined;
      return placed && placed.sessionId === undefined && placed.dataChannelName === w.policy.dataChannelName ? placed : undefined;
    };
    return this.keep(session, wanted.map((w, i) => ({ memberId: w.memberId, name: w.policy.dataChannelName, publisher: w.publisher, item: itemFor(w, i) })), false);
  }
  /** The session's channels as Cloudflare lists them (GET sessions/{id}). Not a change: it runs
      while an earlier call's outcome is unknown. */
  async inspect(session: SfuSession): Promise<ListedChannel[]> {
    const r = await this.call(`sessions/${session.id}`, undefined, "GET");
    const items = Array.isArray(r.dataChannels) ? r.dataChannels as Json[] : [];
    return items.filter(x => Number.isInteger(x.id) && Number(x.id) >= 0 && Number(x.id) <= 65534 && typeof x.dataChannelName === "string")
      .map(x => ({ id: Number(x.id), name: x.dataChannelName as string, local: x.location === "local",
        ...(typeof x.sessionId === "string" ? { session: x.sessionId } : {}), status: typeof x.status === "string" ? x.status : "active" }));
  }
  private async allocate(session: SfuSession, memberId: number, local: boolean, publisher?: string): Promise<Allocation[]> {
    const r = await this.call(`sessions/${session.id}/datachannels/new`, { dataChannels: policies.map(p => ({
      ...p, location: local ? "local" : "remote", ...(publisher ? { sessionId: publisher } : {}),
    })) });
    const items = r.dataChannels as Json[];
    if (!Array.isArray(items)) throw new Error("Missing channels");
    return this.keep(session, policies.map(p => ({ memberId, name: p.dataChannelName, publisher, item: items.find(x => x.dataChannelName === p.dataChannelName) })), local);
  }
  /** Records an answer's allocations on the session, and throws if an item failed (after keeping
      the others for teardown). */
  private keep(session: SfuSession, results: { memberId: number; name: string; publisher?: string; item?: Json }[], local: boolean): Allocation[] {
    const allocations: Allocation[] = [];
    let failed = false;
    for (const { memberId, name, publisher, item } of results) {
      if (!item || item.errorCode || !Number.isInteger(item.id) || Number(item.id) < 0 || Number(item.id) > 65534) {
        failed = true;
        continue;
      }
      const allocation: Allocation = { memberId, name, channelId: Number(item.id), local, ...(publisher ? { session: publisher } : {}) };
      allocations.push(allocation);
      // Cloudflare gives an id only once its last channel closed: a subscription still marked
      // closing on that id is gone, and must never be "closed again" over its successor.
      session.channels = session.channels.filter(c => !(c.closeAt != null && c.channelId === allocation.channelId));
      // Keep successful partial allocations for teardown, even if another item failed.
      session.channels.push(allocation);
    }
    if (failed) throw new Error("Channel allocation failed");
    return allocations;
  }
}
