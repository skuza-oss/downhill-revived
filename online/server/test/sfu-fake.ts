import { vi } from "vitest";
import type { SfuSession } from "../src/sfu";

/** A Realtime and TURN API stand-in on the global fetch (the room's default), recording each call.
    A call `hold` picks waits (in the room's own context: a promise from the test's can't be
    awaited there) until `release()`; `held` counts those waiting. `fail` makes a call answer 500. */
export function fakeRealtime() {
  let channel = 100;
  const fake = {
    calls: [] as { path: string; body: any }[],
    hold: null as null | ((path: string, body: any) => boolean),
    held: 0,
    released: false,
    release: () => { fake.released = true; },
    fail: null as null | ((path: string, body: any) => boolean),
    restore: () => spy.mockRestore(),
  };
  const spy = vi.spyOn(globalThis, "fetch").mockImplementation(async (input, init) => {
    const path = String(input), body = init?.body ? JSON.parse(String(init.body)) : null;
    fake.calls.push({ path, body });
    if (fake.hold?.(path, body)) {
      fake.held++;
      while (!fake.released) await new Promise((r) => setTimeout(r, 5));
    }
    if (fake.fail?.(path, body)) return new Response("no", { status: 500 });
    if (path.endsWith("/revoke")) return new Response(null, { status: 204 });
    if (path.endsWith("sessions/new")) return Response.json({ sessionId: `session-${channel++}`, sessionDescription: { type: "answer", sdp: "SFU" } });
    if (path.endsWith("datachannels/close")) return Response.json({ dataChannels: body.dataChannels });
    if (path.endsWith("datachannels/new")) return Response.json({ dataChannels: body.dataChannels.map((x: any) => ({ ...x, id: channel++ })) });
    return Response.json({});
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
    channels.push({ memberId: id, name: "state", channelId: 200 + 10 * memberId + id, local: false },
                  { memberId: id, name: "events", channelId: 300 + 10 * memberId + id, local: false });
  return { id: `session-${memberId}`, phase: "ready", channels };
}
