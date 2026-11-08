// Simulates the WebSocket side of a race against a running server (wrangler dev or deployed) and
// reports the room's binary PING round trip. Node 22+ (global WebSocket), no dependencies.
//
//   node tools/loadtest.mjs [--url ws://127.0.0.1:8787] [--players 4] [--seconds 10] [--rate 2]
//
// Each player creates/joins one room, readies up, and after "start" sends a binary PING --rate
// times a second (the game: 2, port/net/client.c PING_RACE_MS) and answers the room's text pings.
// Since protocol 3 race frames travel over the Realtime SFU (finding 158), not this socket; the
// SFU path is measured by port/render/net/src/probe.rs. Every PING must come back as its PONG, and
// no player may be warned or closed by the caps (src/guard.ts). These players never set up the SFU,
// so the room takes them out of the race 25 s after its start (src/room.ts SFU_SETUP_MS): keep
// --seconds below that, or run wrangler dev with a larger SFU_SETUP_MS (--var SFU_SETUP_MS:...).

const args = Object.fromEntries(
  process.argv.slice(2).map((a, i, all) => (a.startsWith("--") ? [a.slice(2), all[i + 1]?.startsWith("--") ? true : (all[i + 1] ?? true)] : null)).filter(Boolean),
);
const BASE = (args.url ?? "ws://127.0.0.1:8787").replace(/\/$/, "");
const PLAYERS = Number(args.players ?? 4);
const SECONDS = Number(args.seconds ?? 10);
const RATE = Math.max(0.1, Number(args.rate ?? 2)); // binary PINGs a second

const VERSION = "dhd-port loadtest";
const ISO = "c".repeat(64);

class Player {
  constructor(name) {
    this.name = name;
    this.queue = [];
    this.waiters = [];
    this.pingRtts = [];
    this.errors = []; // error codes the room sent (too_fast would be a cap hit)
  }

  open(path) {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(BASE + path);
      this.ws.binaryType = "arraybuffer";
      this.ws.onopen = () => resolve();
      this.ws.onerror = (e) => reject(new Error(`${this.name}: ${e.message ?? "socket error"}`));
      this.ws.onclose = (e) => {
        this.closed = e.code;
        for (const w of this.waiters.splice(0)) w();
      };
      this.ws.onmessage = (e) => this.onMessage(e.data);
    });
  }

  onMessage(data) {
    if (typeof data !== "string") return this.onBinary(new Uint8Array(data));
    const m = JSON.parse(data);
    if (m.type === "ping") this.send({ type: "pong", s: m.s });
    if (m.type === "error") this.errors.push(m.code);
    this.queue.push(m);
    for (const w of this.waiters.splice(0)) w();
  }

  onBinary(b) {
    const now = performance.now();
    const v = new DataView(b.buffer);
    if (b[0] === 4) {
      this.pingRtts.push(now - v.getFloat64(2, true));
      return;
    }
  }

  send(obj) {
    this.ws.send(JSON.stringify(obj));
  }

  async next(type) {
    for (;;) {
      const i = this.queue.findIndex((m) => m.type === type);
      if (i >= 0) return this.queue.splice(i, 1)[0];
      if (this.closed !== undefined) throw new Error(`${this.name}: closed ${this.closed}`);
      await new Promise((r) => this.waiters.push(r));
    }
  }
}

function pct(values, p) {
  if (!values.length) return NaN;
  const s = [...values].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor((p / 100) * s.length))];
}
const ms = (x) => `${x.toFixed(2)} ms`;

async function main() {
  const health = await (await fetch(BASE.replace(/^ws/, "http") + "/v1/health")).json();
  console.log(`server ${BASE}: protocol ${health.protocol}, minimum version ${health.minVersion}`);
  // The version gate: play as the oldest release the server allows.
  const hello = (name) => ({ type: "hello", protocol: health.protocol, version: VERSION, clientVersion: health.minVersion, isoSha256: ISO, name });

  const players = [];
  const host = new Player("P0");
  await host.open("/v1/rooms/new");
  host.send(hello("P0"));
  const { code } = await host.next("welcome");
  host.send({ type: "settings", maxPlayers: Math.max(2, PLAYERS), track: 1 });
  players.push(host);
  for (let i = 1; i < PLAYERS; i++) {
    const p = new Player(`P${i}`);
    await p.open(`/v1/rooms/${code}`);
    p.send(hello(p.name));
    await p.next("welcome");
    players.push(p);
  }
  console.log(`room ${code}: ${PLAYERS} players joined`);

  for (const p of players) p.send({ type: "ready", ready: true });
  for (;;) {
    const room = await host.next("room");
    if (room.members.length === PLAYERS && room.members.every((m) => m.ready)) break;
  }
  host.send({ type: "start" });
  const starts = await Promise.all(players.map((p) => p.next("start")));
  starts.forEach((s, i) => (players[i].slot = s.yourSlot));
  console.log(`started: seed ${starts[0].seed}`);

  // One sender per player, all on this process's event loop.
  const pings = Math.round(SECONDS * RATE);
  const t0 = performance.now() + 200;
  await Promise.all(
    players.map(async (p) => {
      for (let seq = 0; seq < pings; seq++) {
        const wait = t0 + (seq * 1000) / RATE - performance.now();
        if (wait > 1) await new Promise((r) => setTimeout(r, wait));
        const b = new Uint8Array(10);
        b[0] = 3;
        new DataView(b.buffer).setFloat64(2, performance.now(), true);
        p.ws.send(b);
      }
    }),
  );
  await new Promise((r) => setTimeout(r, 1000));

  const rtt = players.flatMap((p) => p.pingRtts);
  console.log(`\n${pings} binary PINGs from each player in ${SECONDS} s`);
  for (const p of players)
    console.log(`  slot ${p.slot}: ${p.pingRtts.length}/${pings} PONGs${p.errors.length ? `, errors ${p.errors.join(" ")}` : ""}${p.closed !== undefined ? `, closed ${p.closed}` : ""}`);
  console.log(`binary PING round-trip, ${rtt.length} samples:`);
  console.log(`  min ${ms(Math.min(...rtt))}  median ${ms(pct(rtt, 50))}  p95 ${ms(pct(rtt, 95))}  p99 ${ms(pct(rtt, 99))}  max ${ms(Math.max(...rtt))}`);

  const ok = players.every((p) => p.pingRtts.length === pings && !p.errors.length && p.closed === undefined);
  for (const p of players) p.ws.close(1000);
  console.log(ok ? "\nOK" : "\nFAILED");
  process.exit(ok ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
