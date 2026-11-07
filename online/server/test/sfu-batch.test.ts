import { describe, expect, it } from "vitest";
import { Realtime, type SfuSession } from "../src/sfu";

/** Realtime.subscribeAll: one datachannels/new call per subscriber for all its publishers. */
const secrets = { REALTIME_APP_ID: "app", REALTIME_APP_TOKEN: "token" };
const ready = (id: string): SfuSession => ({ id, phase: "ready", channels: [] });

function api(answer: (entries: any[]) => any[]) {
  const calls: any[] = [];
  const request = (async (input: any, init: any) => {
    const body = JSON.parse(String(init.body));
    calls.push({ path: String(input), body });
    return Response.json({ dataChannels: answer(body.dataChannels) });
  }) as typeof fetch;
  return { realtime: new Realtime(secrets, request), calls };
}

describe("Realtime.subscribeAll", () => {
  it("subscribes to every publisher in one call and maps the results by position", async () => {
    let id = 100;
    const { realtime, calls } = api((entries) => entries.map((e) => ({ ...e, id: id++ })));
    const sub = ready("s0");
    const got = await realtime.subscribeAll(sub, [1, 2, 3, 4].map((m) => ({ memberId: m, session: ready(`s${m}`) })));
    expect(calls.length).toBe(1);
    expect(calls[0].body.dataChannels.map((e: any) => [e.sessionId, e.dataChannelName, e.location])).toEqual(
      [1, 2, 3, 4].flatMap((m) => [[`s${m}`, "state", "remote"], [`s${m}`, "events", "remote"]]));
    expect(got.map((a) => [a.memberId, a.name, a.channelId])).toEqual(
      [1, 2, 3, 4].flatMap((m, i) => [[m, "state", 100 + 2 * i], [m, "events", 101 + 2 * i]]));
    expect(sub.channels.length).toBe(8);
    // Nothing missing: no call at all.
    expect(await realtime.subscribeAll(sub, [{ memberId: 1, session: ready("s1") }])).toEqual([]);
    expect(calls.length).toBe(1);
  });

  it("finds a result by publisher when the answer comes in another order", async () => {
    const { realtime } = api((entries) => entries.map((e, i) => ({ ...e, id: 10 + i })).reverse());
    const got = await realtime.subscribeAll(ready("s0"), [1, 2].map((m) => ({ memberId: m, session: ready(`s${m}`) })));
    expect(got.map((a) => [a.memberId, a.name, a.channelId])).toEqual([[1, "state", 10], [1, "events", 11], [2, "state", 12], [2, "events", 13]]);
  });

  it("keeps the successful allocations and throws when one item fails", async () => {
    const { realtime } = api((entries) => entries.map((e, i) => i === 3 ? { ...e, errorCode: "x" } : { ...e, id: 50 + i }));
    const sub = ready("s0");
    await expect(realtime.subscribeAll(sub, [1, 2].map((m) => ({ memberId: m, session: ready(`s${m}`) })))).rejects.toThrow();
    expect(sub.channels.map((c) => c.channelId)).toEqual([50, 51, 52]);
  });

  it("skips publishers not ready yet", async () => {
    const { realtime, calls } = api((entries) => entries.map((e, i) => ({ ...e, id: i })));
    const late: SfuSession = { id: "s2", phase: "connect", channels: [] };
    await realtime.subscribeAll(ready("s0"), [{ memberId: 1, session: ready("s1") }, { memberId: 2, session: late }]);
    expect(calls[0].body.dataChannels.every((e: any) => e.sessionId === "s1")).toBe(true);
  });
});
