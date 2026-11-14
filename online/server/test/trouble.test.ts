// A game's report of a failed path to the SFU (sfu_trouble, finding 248): one log line of fixed
// words per report, a few per member, nothing sent back; a report with an unknown word is dropped.
import { describe, it, expect, vi } from "vitest";
import { Client } from "./helpers";
import { troubleOf } from "../src/protocol";

const report = (extra: object = {}) => ({ type: "sfu_trouble", path: "turn", step: "allocate", kind: "turn_400", version: "0.8.5", ...extra });

async function logged(run: () => Promise<void>): Promise<string[]> {
  const lines: string[] = [];
  const spies = (["log", "warn", "error"] as const).map((k) =>
    vi.spyOn(console, k).mockImplementation((...a: unknown[]) => { lines.push(a.map(String).join(" ")); }));
  try {
    await run();
  } finally {
    for (const s of spies) s.mockRestore();
  }
  return lines.filter((l) => l.startsWith("Realtime trouble:"));
}

describe("sfu_trouble (finding 248)", () => {
  it("takes fixed words and a plain version only", () => {
    expect(troubleOf(report())).toEqual({ path: "turn", step: "allocate", kind: "turn_400", version: "0.8.5" });
    expect(troubleOf(report({ path: "tcp" }))).toBeNull();
    expect(troubleOf(report({ step: "1.2.3.4:3478" }))).toBeNull();
    expect(troubleOf(report({ kind: "turn_400 user=x" }))).toBeNull();
    expect(troubleOf(report({ version: "0.8.5 at 192.0.2.7" }))).toBeNull();
    expect(troubleOf(report({ version: 85 }))).toBeNull();
  });

  it("logs one line per report, as the room's other lines name it, and answers nothing", async () => {
    let code = "";
    const lines = await logged(async () => {
      const room = await Client.host();
      code = room.code;
      const guest = await Client.join(code, "Guest");
      guest.sendJson(report({ path: "udp", step: "send", kind: "denied" }));
      guest.sendJson(report());
      guest.sendJson(report({ kind: "made_up" }));
      // The connection stays open and no error comes back: the next ping is answered.
      guest.sendJson({ type: "ping", t: 7 });
      expect((await guest.json("pong")).t).toBe(7);
      expect(guest.seen.map((f) => JSON.parse(f).type)).not.toContain("error");
    });
    expect(lines).toHaveLength(2);
    expect(lines[0]).toMatch(/^Realtime trouble: udp\/send\/denied 0\.8\.5 .*\(room \w+, member 1\)$/);
    expect(lines[0]).toContain(`(room ${code}, member 1)`);
    expect(lines[1]).toMatch(/^Realtime trouble: turn\/allocate\/turn_400 0\.8\.5 /);
  });

  it("an unknown sfu_ message (what a server without sfu_trouble does with it) is an error, not a closed connection", async () => {
    const { host } = await Client.host();
    host.sendJson({ type: "sfu_made_up" });
    expect((await host.json("error")).message).toBe("Unknown signaling message.");
    host.sendJson({ type: "ping", t: 2 });
    expect((await host.json("pong")).t).toBe(2);
  });

  it("logs at most twelve reports per member in five minutes", async () => {
    const lines = await logged(async () => {
      const { host } = await Client.host();
      for (let i = 0; i < 20; i++) host.sendJson(report());
      host.sendJson({ type: "ping", t: 1 });
      await host.json("pong");
    });
    expect(lines).toHaveLength(12);
  });
});
