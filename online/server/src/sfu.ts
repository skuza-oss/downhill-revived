// Trusted control plane. Client SDP is consumed here, never copied into a client message.
export interface SfuSecrets { REALTIME_APP_ID?: string; REALTIME_APP_TOKEN?: string; }
export interface SfuSession { id: string; phase: "connect" | "ready" | "failed"; channels: Allocation[]; }
export interface Allocation { memberId: number; name: string; channelId: number; local: boolean; }
type Json = Record<string, unknown>;
const policies = [{ dataChannelName: "state", ordered: false, maxRetransmits: 0 },
                  { dataChannelName: "events", ordered: true }];

export class Realtime {
  constructor(private secrets: SfuSecrets, private request: typeof fetch = (input, init) => fetch(input, init)) {}
  private async call(path: string, body?: unknown, method = "POST"): Promise<Json> {
    const { REALTIME_APP_ID: app, REALTIME_APP_TOKEN: token } = this.secrets;
    if (!app || !token) throw new Error("Realtime is not configured");
    const request = this.request;
    const r = await request(`https://rtc.live.cloudflare.com/v1/apps/${encodeURIComponent(app)}/${path}`, {
      method, headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body), signal: AbortSignal.timeout(15000),
    });
    if (!r.ok) {
      console.warn("Realtime API operation failed", path.split("/").at(-1), r.status);
      throw new Error("Realtime request failed");
    }
    const data = await r.json() as Json;
    if (data.errorCode) {
      const code = typeof data.errorCode === "string" && /^[a-z_]{1,64}$/.test(data.errorCode) ? data.errorCode : "unknown";
      console.warn("Realtime API error", path.split("/").at(-1), code);
      throw new Error("Realtime operation failed");
    }
    return data;
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
    const r = await this.call(`sessions/${session.id}/datachannels/close`, {
      dataChannels: channels.map(c => ({ id: c.channelId })),
    }, "PUT");
    const results = r.dataChannels as Json[];
    if (!Array.isArray(results) || channels.some(c => !results.some(x => x.id === c.channelId &&
        (!x.errorCode || x.errorCode === "close_track_error")))) throw new Error("Channel close incomplete");
    session.channels = session.channels.filter(c => !channels.includes(c));
  }
  async publish(memberId: number, session: SfuSession): Promise<Allocation[]> {
    if (session.phase !== "connect") throw new Error("Transport not connected");
    const allocations = await this.allocate(session, memberId, true);
    session.phase = "ready";
    return allocations;
  }
  async subscribe(session: SfuSession, memberId: number, publisher: SfuSession): Promise<Allocation[]> {
    if (session.phase !== "ready" || publisher.phase !== "ready") return [];
    if (session.channels.some(x => !x.local && x.memberId === memberId)) return [];
    const allocations = await this.allocate(session, memberId, false, publisher.id);
    return allocations;
  }
  private async allocate(session: SfuSession, memberId: number, local: boolean, publisher?: string): Promise<Allocation[]> {
    const r = await this.call(`sessions/${session.id}/datachannels/new`, { dataChannels: policies.map(p => ({
      ...p, location: local ? "local" : "remote", ...(publisher ? { sessionId: publisher } : {}),
    })) });
    const items = r.dataChannels as Json[];
    if (!Array.isArray(items)) throw new Error("Missing channels");
    const allocations: Allocation[] = [];
    let failed = false;
    for (const policy of policies) {
      const item = items.find(x => x.dataChannelName === policy.dataChannelName);
      if (!item || item.errorCode || !Number.isInteger(item.id) || Number(item.id) < 0 || Number(item.id) > 65534) {
        failed = true;
        continue;
      }
      const allocation = { memberId, name: policy.dataChannelName, channelId: Number(item.id), local };
      allocations.push(allocation);
      // Keep successful partial allocations for teardown, even if another item failed.
      session.channels.push(allocation);
    }
    if (failed) throw new Error("Channel allocation failed");
    return allocations;
  }
}
