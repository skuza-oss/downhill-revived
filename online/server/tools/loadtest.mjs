// Simulates a state-sync race against a running server (wrangler dev or deployed) and reports
// relay latency. Node 22+ (global WebSocket), no dependencies.
//
//   node tools/loadtest.mjs [--url ws://127.0.0.1:8787] [--players 4] [--seconds 10] [--bytes 600]
//
// Each player creates/joins one room, readies up, and after "start" sends a STATE frame of
// --bytes 20 times a second, as the game does with its riders' snapshots (port/hle/online.c).
// Every player checks that each other racer's frames arrive whole, once and in order.

const args = Object.fromEntries(
  process.argv.slice(2).map((a, i, all) => (a.startsWith("--") ? [a.slice(2), all[i + 1]?.startsWith("--") ? true : (all[i + 1] ?? true)] : null)).filter(Boolean),
);
const BASE = (args.url ?? "ws://127.0.0.1:8787").replace(/\/$/, "");
const PLAYERS = Number(args.players ?? 4);
// The server starts a race only with two or more members (room.ts onStart, finding 118).
if (!Number.isInteger(PLAYERS) || PLAYERS < 2) {
  console.error("--players must be 2 or more: the server starts a race only with two members");
  process.exit(2);
}
const SECONDS = Number(args.seconds ?? 10);
const BYTES = Math.max(8, Math.min(3072, Number(args.bytes ?? 600)));
const RATE = 20; // STATE frames a second, as the game sends

const VERSION = "dhd-port loadtest";
const ISO = "c".repeat(64);

/** A STATE frame: type 5, slot (the server sets it), the frame number, a filler that depends on both. */
function stateFrame(slot, seq) {
  const b = new Uint8Array(BYTES);
  b[0] = 5;
  b[1] = slot;
  new DataView(b.buffer).setUint32(2, seq, true);
  for (let i = 6; i < BYTES; i++) b[i] = (seq * 31 + slot * 7 + i) & 0xff;
  return b;
}

class Player {
  constructor(name) {
    this.name = name;
    this.queue = [];
    this.waiters = [];
    this.got = new Map(); // slot -> frames received
    this.last = new Map(); // slot -> last frame number
    this.latencies = []; // one-way relay ms (sender and receiver share this process's clock)
    this.bad = 0; // wrong length, filler or order
    this.pingRtts = [];
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
    if (b[0] !== 5) return;
    const slot = b[1];
    const seq = v.getUint32(2, true);
    const want = stateFrame(slot, seq);
    if (b.length !== BYTES || want.some((x, i) => i > 1 && x !== b[i]) || seq !== (this.last.get(slot) ?? -1) + 1) this.bad++;
    this.last.set(slot, seq);
    this.got.set(slot, (this.got.get(slot) ?? 0) + 1);
    const sent = sendTimes.get(`${slot}:${seq}`);
    if (sent !== undefined) this.latencies.push(now - sent);
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

/** When each (slot, frame) was sent, for one-way latency. */
const sendTimes = new Map();

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
  const hello = (name) => ({ type: "hello", protocol: health.protocol ?? 2, version: VERSION, clientVersion: health.minVersion, isoSha256: ISO, name });

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
  const frames = Math.round(SECONDS * RATE);
  const t0 = performance.now() + 200;
  await Promise.all(
    players.map(async (p) => {
      for (let seq = 0; seq < frames; seq++) {
        const wait = t0 + (seq * 1000) / RATE - performance.now();
        if (wait > 1) await new Promise((r) => setTimeout(r, wait));
        sendTimes.set(`${p.slot}:${seq}`, performance.now());
        p.ws.send(stateFrame(p.slot, seq));
        if (seq % 10 === 0) {
          const b = new Uint8Array(10);
          b[0] = 3;
          new DataView(b.buffer).setFloat64(2, performance.now(), true);
          p.ws.send(b);
        }
      }
    }),
  );
  await new Promise((r) => setTimeout(r, 1000));

  const lat = players.flatMap((p) => p.latencies);
  const rtt = players.flatMap((p) => p.pingRtts);
  const expected = frames * (PLAYERS - 1);
  console.log(`\n${frames} STATE frames of ${BYTES} bytes from each player in ${SECONDS} s`);
  for (const p of players) {
    const total = [...p.got.values()].reduce((a, b) => a + b, 0);
    console.log(`  slot ${p.slot}: ${total}/${expected} frames, ${p.bad} wrong or out of order`);
  }
  console.log(`relay one-way (sender -> server -> receiver), ${lat.length} frames:`);
  console.log(`  min ${ms(Math.min(...lat))}  median ${ms(pct(lat, 50))}  p95 ${ms(pct(lat, 95))}  p99 ${ms(pct(lat, 99))}  max ${ms(Math.max(...lat))}`);
  console.log(`binary PING round-trip, ${rtt.length} samples: median ${ms(pct(rtt, 50))}  p99 ${ms(pct(rtt, 99))}`);

  for (const p of players) p.ws.close(1000);
  const ok = players.every((p) => [...p.got.values()].reduce((a, b) => a + b, 0) === expected && p.bad === 0);
  console.log(ok ? "\nOK" : "\nFAILED");
  process.exit(ok ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
