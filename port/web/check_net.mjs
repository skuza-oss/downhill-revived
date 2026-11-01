// The browser's online client (www/net.js over build/web/dhd_net.wasm, finding 123) in Node, which
// has the browser's WebSocket and fetch: two players against a relay (by default a local
// `wrangler dev` in online/server), the way the game drives net.js. The host creates a room, the
// guest joins it by its code, both ready, the host starts, and each sends a STATE frame the other
// must receive; then the public list is read and both leave. In the room the guest says a chat line
// both must get. In the race the host's microphone (the page's ring, here a 440 Hz tone, open mic)
// must be heard in the guest's mix, and the guest must see the host's slot talking (voice chat,
// finding 131: EVENT_VOICE frames, which any server relays).
//   node port/web/check_net.mjs [ws://127.0.0.1:8787]
import { readFileSync } from 'node:fs';
import { createNet } from './www/net.js';

const server = process.argv[2] || 'ws://127.0.0.1:8787';
const wasm = readFileSync(new URL('../../build/web/dhd_net.wasm', import.meta.url));
const sleep = ms => new Promise(r => setTimeout(r, ms));
const fail = text => { console.log('FAIL ' + text); process.exit(1); };

// A stand-in for the game's memory: the services take its addresses, as the core passes them.
async function player(name, host = {}) {
  const ram = new Uint8Array(1 << 16); let top = 16;
  const guest = { bytes: (a, n) => ram.subarray(a, a + n), strlen: a => ram.indexOf(0, a) - a };
  const put = s => { const b = new TextEncoder().encode(s + '\0'), a = top; ram.set(b, a); top += b.length + 7 & ~7; return a; };
  const net = await createNet(wasm, guest, t => console.log(`  ${name}: ${t}`), host);
  const lobbyAt = 0x4000, dv = new DataView(ram.buffer);
  const lobby = () => { net.host_service_net_get_lobby(lobbyAt); const at = lobbyAt;
    return { state: dv.getInt32(at, true), code: new TextDecoder().decode(ram.subarray(at + 8, ram.indexOf(0, at + 8))),
             isHost: dv.getInt32(at + 24, true), members: dv.getInt32(at + 160, true) }; };
  net.host_service_net_init(put(server), put(name), put('dhd-port test-browser 0000000000000000'), put('c'.repeat(64)));
  return { net, put, ram, lobby };
}
async function until(what, test, ms = 8000) {
  for (const end = Date.now() + ms; Date.now() < end; await sleep(50)) { const v = test(); if (v) return v; }
  fail('timed out: ' + what);
}

// The host's microphone ring (main.js's layout: [write, read] then 48 kHz mono samples), fed a tone
// at the microphone's pace once the race is on.
const mic = new SharedArrayBuffer(8 + 48000 * 2), micHead = new Int32Array(mic, 0, 2), micData = new Int16Array(mic, 8);
let micWanted = false, phase = 0;
const host = await player('HostB', { env: { DHD_VOICE_MODE: 'open' }, mic, wantMic: on => { micWanted = on; } });
const guest = await player('GuestB', { env: {} });
const tone = setInterval(() => { if (!micWanted) return;
  let w = Atomics.load(micHead, 0); const cap = micData.length;
  for (let i = 0; i < 960; i++, phase += 2 * Math.PI * 440 / 48000) { micData[w] = Math.round(8000 * Math.sin(phase)); w = (w + 1) % cap; }
  Atomics.store(micHead, 0, w); }, 20);
if (host.net.host_service_net_create_room() !== 0) fail('create_room');
const room = await until('the host in a room', () => { const l = host.lobby(); return l.state === 2 && l.code ? l : null; });
console.log(`host in room ${room.code} (host ${room.isHost})`);
if (guest.net.host_service_net_join_room(guest.put(room.code)) !== 0) fail('join_room');
await until('the guest in the room', () => guest.lobby().state === 2 && guest.lobby().members === 2);
await until('the host sees two', () => host.lobby().members === 2);
console.log('both in the room: 2 members');
// Chat (finding 125): a line from the guest reaches both, the sender too, as net.h's NetChat
// (u32 seq, i32 count, then lines of u32 seq, i32 memberId, name[32], text[128], f64 at).
const chatAt = 0xA000, readChat = p => { p.net.host_service_net_get_chat(chatAt); const dv = new DataView(p.ram.buffer), n = dv.getInt32(chatAt + 4, true);
  const text = at => new TextDecoder().decode(p.ram.subarray(at, p.ram.indexOf(0, at)));
  return [...Array(n)].map((_, i) => { const at = chatAt + 8 + 176 * i; return { member: dv.getInt32(at + 4, true), name: text(at + 8), text: text(at + 40) }; }); };
// A server without the room's chat message (the official one as deployed) gets none: the line
// stays on the sender's screen with a notice, and nobody is put out of the room.
if (guest.net.host_service_net_send_chat(guest.put('  READY?  ')) !== 1) {
  const lines = readChat(guest);
  if (lines.length !== 1 || lines[0].member !== -1 || !/isn't on this server/.test(lines[0].text)) fail(`no-chat notice ${JSON.stringify(lines)}`);
  console.log(`room chat: this server has none: the guest sees "${lines[0].text}"`);
} else for (const [p, label] of [[host, 'host'], [guest, 'guest']]) {
  const lines = await until(`the chat line for the ${label}`, () => { const l = readChat(p); return l.length ? l : null; });
  if (lines[0].name !== 'GuestB' || lines[0].text !== 'READY?') fail(`${label} chat ${JSON.stringify(lines)}`);
  console.log(`${label} chat: ${lines[0].name}: ${lines[0].text}`);
}
host.net.host_service_net_pick(-1, 1, 0); guest.net.host_service_net_pick(-1, 3, 0);
host.net.host_service_net_set_ready(1); guest.net.host_service_net_set_ready(1);
await sleep(500);
host.net.host_service_net_start_race();
await until('racing', () => host.net.host_service_net_racing() && guest.net.host_service_net_racing());
console.log('both racing');
for (const p of [host, guest]) if (!p.net.host_service_net_send_frame(5, p.put('\x07\x08\x09'), 3)) fail('send_frame');
for (const [p, label] of [[host, 'host'], [guest, 'guest']]) {
  const out = 0x8000;
  const n = await until(`a frame for the ${label}`, () => { const n = p.net.host_service_net_receive_frame(out, 3072); return n > 0 ? n : 0; });
  const got = [...p.ram.subarray(out, out + n)];
  if (got[0] !== 5 || got.slice(2).join() !== '7,8,9') fail(`${label} frame ${got}`);
  console.log(`${label} received STATE from slot ${got[1]}: ${got.slice(2)}`);
}
// Voice: the guest hears the host. The game places every slot (online.c voice_step); here all at
// full level in the middle.
for (let slot = 0; slot < 10; slot++) guest.net.host_service_voice_place(slot, 1000, 0);
const VOICE_STATE_TALKING = 4, VOICE_STATE_TRANSMITTING = 1;
await until('the host transmitting', () => micWanted && host.net.host_service_voice_state(VOICE_STATE_TRANSMITTING, 0));
const heard = await until("the host heard in the guest's mix", () => {
  const pcm = new Int16Array(960 * 2); guest.net.voiceMix(pcm);
  let sum = 0; for (const v of pcm) sum += v * v; const rms = Math.sqrt(sum / pcm.length);
  return rms > 500 ? { rms, talking: [...Array(10).keys()].filter(s => guest.net.host_service_voice_state(VOICE_STATE_TALKING, s)) } : null;
});
if (!heard.talking.length) fail('no slot shows as talking on the guest');
console.log(`guest hears the host: RMS ${heard.rms.toFixed(0)}, talking slot(s) ${heard.talking}`);
clearInterval(tone);
if (!host.net.host_service_net_list_lobbies_start(16)) fail('list start');
const listed = await until('the public list', () => { const n = host.net.host_service_net_list_lobbies_poll(0xC000, 16); return n === -2 ? 0 : { n }; });
if (listed.n < 0) fail('list poll ' + listed.n);
console.log(`public list: ${listed.n} room(s)`);
host.net.host_service_net_leave(); guest.net.host_service_net_leave();
// leaving mid-race ends as NET_FAILED (client_on_closed checks racing first), else NET_IDLE
const gone = s => s === 0 || s === 5;
await until('both gone', () => gone(host.lobby().state) && gone(guest.lobby().state), 4000);
console.log('PASS');
process.exit(0);
