import { vi } from "vitest";
import type { SfuSession } from "../src/sfu";

/** A Realtime and TURN API stand-in on the global fetch (the room's default), recording each call.
    A call `hold` picks waits (in the room's own context: a promise from the test's can't be
    awaited there) until `release()`, or until its own timeout aborts it; `held` counts those
    waiting. An aborted call still allocates when `allocateOnAbort` (Cloudflare carried it out, its
    answer was lost: finding 182). `fail` makes a call answer `failStatus` (500). The fake keeps
    each session's channels: `GET sessions/{id}` lists them as Cloudflare does, with each
    subscription's publisher session; a close removes them. `inFlight` / `maxInFlight`: calls
    running at once. */
export function fakeRealtime() {
  let channel = 100;
  const fake = {
    calls: [] as { path: string; body: any; method: string }[],
    hold: null as null | ((path: string, body: any) => boolean),
    held: 0,
    released: false,
    release: () => { fake.released = true; },
    allocateOnAbort: false,
    fail: null as null | ((path: string, body: any) => boolean),
    failStatus: 500,
    /** Calls whose every item answers an errorCode (HTTP 200): a definite refusal. */
    itemError: null as null | ((path: string, body: any) => boolean),
    /** Channels per session id, as the API would list them. */
    sessions: new Map<string, { id: number; dataChannelName: string; location: string; sessionId?: string; status: string }[]>(),
    inFlight: 0,
    maxInFlight: 0,
    restore: () => spy.mockRestore(),
  };
  const sessionOf = (path: string) => path.split("/sessions/")[1]?.split("/")[0] ?? "";
  const allocate = (path: string, body: any) => {
    const list = fake.sessions.get(sessionOf(path)) ?? [];
    fake.sessions.set(sessionOf(path), list);
    return body.dataChannels.map((x: any) => {
      const item = { ...x, id: channel++ };
      list.push({ id: item.id, dataChannelName: x.dataChannelName, location: x.location, ...(x.sessionId ? { sessionId: x.sessionId } : {}), status: "active" });
      return item;
    });
  };
  const spy = vi.spyOn(globalThis, "fetch").mockImplementation(async (input, init) => {
    const path = String(input), body = init?.body ? JSON.parse(String(init.body)) : null, method = init?.method ?? "GET";
    fake.calls.push({ path, body, method });
    fake.inFlight++;
    fake.maxInFlight = Math.max(fake.maxInFlight, fake.inFlight);
    try {
      if (fake.hold?.(path, body)) {
        fake.held++;
        const signal = init?.signal;
        while (!fake.released && !signal?.aborted) await new Promise((r) => setTimeout(r, 5));
        if (signal?.aborted) {
          if (fake.allocateOnAbort && path.endsWith("datachannels/new")) allocate(path, body);
          throw signal.reason;
        }
      }
      if (fake.fail?.(path, body)) return new Response("no", { status: fake.failStatus });
      if (path.endsWith("/revoke")) return new Response(null, { status: 204 });
      if (path.endsWith("generate-ice-servers")) return Response.json({ iceServers: [{ urls: ["turns:turn.cloudflare.com:443?transport=tcp"], username: "u".repeat(32), credential: "c".repeat(32) }] });
      if (path.endsWith("sessions/new")) return Response.json({ sessionId: `session-${channel++}`, sessionDescription: { type: "answer", sdp: "SFU" } });
      if (path.endsWith("datachannels/close")) {
        const ids = body.dataChannels.map((x: any) => x.id);
        const list = fake.sessions.get(sessionOf(path));
        if (list) fake.sessions.set(sessionOf(path), list.filter((c) => !ids.includes(c.id)));
        return Response.json({ dataChannels: body.dataChannels });
      }
      if (path.endsWith("datachannels/new") && fake.itemError?.(path, body))
        return Response.json({ dataChannels: body.dataChannels.map((x: any) => ({ ...x, errorCode: "invalid_request", errorDescription: "no" })) });
      if (path.endsWith("datachannels/new")) return Response.json({ dataChannels: allocate(path, body) });
      if (method === "GET" && /\/sessions\/[^/]+$/.test(path)) return Response.json({ tracks: [], dataChannels: fake.sessions.get(sessionOf(path)) ?? [] });
      return Response.json({});
    } finally {
      fake.inFlight--;
    }
  });
  return fake;
}

/** A racer's session with its own channels published and subscriptions to `subscribed`. */
export function publishedSession(memberId: number, subscribed: number[]): SfuSession {
  const channels = [
    { memberId, name: "state", channelId: 10 * memberId + 1, local: true },
    { memberId, name: "events", channelId: 10 * memberId + 2, local: true },
  ];
  for (const id of subscribed)
    channels.push({ memberId: id, name: "state", channelId: 200 + 10 * memberId + id, local: false, session: `session-${id}` } as any,
                  { memberId: id, name: "events", channelId: 300 + 10 * memberId + id, local: false, session: `session-${id}` } as any);
  return { id: `session-${memberId}`, phase: "ready", channels };
}
