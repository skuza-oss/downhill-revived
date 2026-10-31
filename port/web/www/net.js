// Online in the browser (finding 123): the native game's lobby client (port/net: client.c, json.c,
// net.c), built for the worker as build/dhd_net.wasm with its own memory (port/web/build_net.py),
// under the worker's WebSocket and fetch in place of ws.h's Rust client. The game's
// host_service_net_* services arrive here with the game's addresses; each copies its strings and
// buffers into the client's memory and its answers back, byte for byte (both modules are wasm32).
// The worker hands its thread to the browser at every vsync, so socket events and the 10 ms tick
// run between fields, never inside a call into the client.
const enc = new TextEncoder(), dec = new TextDecoder();

export async function createNet(bytes, guest, say) {
  let x, view;  // the client's exports and its memory, as bytes
  const mem = () => (view?.buffer === x.memory.buffer ? view : (view = new Uint8Array(x.memory.buffer)));
  const cstr = at => { const m = mem(); return dec.decode(m.subarray(at, m.indexOf(0, at))); };
  const words = (at, n) => new Uint32Array(x.memory.buffer, at, n);
  const call = (index, ...args) => x.__indirect_function_table.get(index)(...args);
  let line = '';  // the client's stderr, by line
  const wasi = {
    fd_write: (fd, iovs, n, written) => { let total = 0;
      for (let i = 0; i < n; i++) { const [p, l] = words(iovs + 8 * i, 2); line += dec.decode(mem().subarray(p, p + l)); total += l; }
      for (let i; (i = line.indexOf('\n')) >= 0; line = line.slice(i + 1)) say(line.slice(0, i));
      words(written, 1)[0] = total; return 0; },
    environ_sizes_get: (count, size) => { words(count, 1)[0] = 0; words(size, 1)[0] = 0; return 0; },
    environ_get: () => 0,
    clock_time_get: (_id, _precision, out) => { new BigUint64Array(x.memory.buffer, out, 1)[0] = BigInt(Math.round(performance.now() * 1e6)); return 0; },
    fd_close: () => 0, fd_seek: () => 70, proc_exit: code => { throw Error(`dhd_net exited (${code})`); },
  };

  // ---- ws.h over WebSocket ----
  // A connection: its handler's words {ctx, on_open, on_message, on_close, on_tick}, copied at
  // ws_start as the native client copies them (net.c's handler is on its stack).
  const conns = new Map(); let nextConn = 1;
  const BACKOFF = [1000, 2000, 4000, 8000];
  function open(c, url) {
    let ws;
    try { ws = new WebSocket(url); } catch (e) { say(`online: ${e.message}`); return closed(c, -1, ''); }
    ws.binaryType = 'arraybuffer'; c.ws = ws; c.opened = false; c.silent = false;
    ws.onopen = () => { if (c.ws !== ws) return; c.opened = true; c.attempt = 0; call(c.h[1], c.h[0], c.id); };
    ws.onmessage = e => { if (c.ws !== ws) return;
      const data = typeof e.data === 'string' ? enc.encode(e.data) : new Uint8Array(e.data), at = x.malloc(data.length + 1);
      mem().set(data, at); mem()[at + data.length] = 0;
      try { call(c.h[2], c.h[0], typeof e.data === 'string' ? 0 : 1, at, data.length); } finally { x.free(at); } };
    // the tick's drop of a silent server is a lost connection (1006), not the server's refusal (4000s)
    ws.onclose = e => { if (c.ws === ws) closed(c, !c.opened ? -1 : c.silent ? 1006 : e.code, e.reason); };
  }
  // on_close says where to go next (a lobby drop rejoins the room by its code) after a backoff.
  function closed(c, code, reason) {
    c.ws = null;
    if (!c.opened) c.attempt++;
    const r = enc.encode(reason + '\0'), at = x.malloc(r.length); mem().set(r, at);
    let next = 0;
    try { next = call(c.h[3], c.h[0], code, at, c.attempt); } finally { x.free(at); }
    if (!next || c.stopped) return;
    const url = cstr(next);
    c.retry = setTimeout(() => { if (!c.stopped) open(c, url); }, BACKOFF[Math.min(c.attempt, BACKOFF.length - 1)]);
  }
  const env = {
    web_now_ns: () => BigInt(Math.round(performance.now() * 1e6)),
    ws_start: (url, h) => {
      const c = { id: nextConn++, h: [...words(h, 5)], attempt: 0, stopped: false };
      conns.set(c.id, c);
      if (c.h[4]) c.tick = setInterval(() => { if (c.ws?.readyState === 1 && call(c.h[4], c.h[0])) { c.silent = true; c.ws.close(); } }, 10);
      open(c, cstr(url));
      return c.id;
    },
    ws_send: (id, binary, data, n) => { const ws = conns.get(id)?.ws; if (ws?.readyState !== 1) return;
      const bytes = mem().slice(data, data + n); ws.send(binary ? bytes : dec.decode(bytes)); },
    // Sends nothing more (WebSocket sends in order: what was queued goes before the close), closes,
    // and calls on_close for that close at once, as ws_stop's receive thread does before it returns,
    // so the client is told before net.c sets up the next room.
    ws_stop: id => { const c = conns.get(id); if (!c) return; conns.delete(id);
      c.stopped = true; clearInterval(c.tick); clearTimeout(c.retry);
      const ws = c.ws; if (ws) { c.ws = null; try { ws.close(1000); } catch {} closed(c, 1000, ''); } },
    // The room list: the worker fetches it first (listStart), and the client reads that body here.
    http_get: (_url, out, cap) => { if (body === null) return -1;
      const b = enc.encode(body).subarray(0, cap - 1); mem().set(b, out); mem()[out + b.length] = 0; return b.length; },
  };
  let body = null;

  x = (await WebAssembly.instantiate(bytes, { env, wasi_snapshot_preview1: wasi })).instance.exports;
  x._initialize?.();
  const LOBBY = x.web_net_lobby_size(), INFO = x.web_net_info_size(), CHAT = x.web_net_chat_size(), FRAME_MAX = 3072, LIST_MAX = 64;
  const scratch = x.malloc(Math.max(LOBBY, INFO * LIST_MAX, CHAT, FRAME_MAX, 64));
  const copyIn = (at, n) => { const p = x.malloc(n); mem().set(guest.bytes(at, n), p); return p; };
  const stringIn = at => { const s = guest.bytes(at, guest.strlen(at) + 1); const p = x.malloc(s.length); mem().set(s, p); return p; };
  const withStrings = (addrs, f) => { const ps = addrs.map(a => a ? stringIn(a) : 0); try { return f(...ps); } finally { ps.forEach(p => p && x.free(p)); } };
  let base = '', list = { state: 'idle', text: null };

  return {
    host_service_net_init: (a, b, c, d) => {
      base = a ? dec.decode(guest.bytes(a, guest.strlen(a))).replace(/\/+$/, '') : '';
      withStrings([a, b, c, d], x.host_service_net_init);
    },
    host_service_net_create_room: () => x.host_service_net_create_room(),
    host_service_net_join_room: a => withStrings([a], x.host_service_net_join_room),
    host_service_net_leave: () => x.host_service_net_leave(),
    host_service_net_get_lobby: out => { x.host_service_net_get_lobby(scratch); guest.bytes(out, LOBBY).set(mem().subarray(scratch, scratch + LOBBY)); },
    // Text chat (finding 125): a line out, and the last lines (a NetChat) copied back.
    host_service_net_send_chat: a => withStrings([a], x.host_service_net_send_chat),
    host_service_net_get_chat: out => { x.host_service_net_get_chat(scratch); guest.bytes(out, CHAT).set(mem().subarray(scratch, scratch + CHAT)); },
    host_service_net_pick: (m, r, b) => x.host_service_net_pick(m, r, b),
    host_service_net_set_ready: r => x.host_service_net_set_ready(r),
    host_service_net_start_race: () => x.host_service_net_start_race(),
    host_service_net_racing: () => x.host_service_net_racing(),
    host_service_net_seconds: () => x.host_service_net_seconds(),
    host_service_net_server_ms: () => x.host_service_net_server_ms(),
    host_service_net_send_frame: (type, at, n) => { if (n > FRAME_MAX) return 0;
      mem().set(guest.bytes(at, n), scratch); return x.host_service_net_send_frame(type, scratch, n); },
    host_service_net_receive_frame: (out, max) => { const n = x.host_service_net_receive_frame(scratch, Math.min(max, FRAME_MAX));
      if (n > 0) guest.bytes(out, n).set(mem().subarray(scratch, scratch + n)); return n; },
    // NetRoomSettings as the game passes it (net.c's host_service_net_set_room): 7 words, the
    // last its room name's address (0: the name stays).
    host_service_net_set_room: at => { const p = copyIn(at, 28), w = words(p, 7), name = w[6] ? stringIn(w[6]) : 0;
      w[6] = name; try { x.host_service_net_set_room(p); } finally { x.free(p); if (name) x.free(name); } },
    // host_sdl.c's background fetch of the public list: start 1/0; poll -2 working, -1 failed,
    // else the count, the entries copied to the game's buffer.
    host_service_net_list_lobbies_start: max => {
      if (max <= 0 || max > LIST_MAX || list.state === 'working' || !base) return 0;
      list = { state: 'working', text: null };
      fetch(base.replace(/^ws/, 'http') + '/v1/lobbies').then(r => r.ok ? r.text() : Promise.reject(Error(`${r.status}`)))
        .then(t => { list = { state: 'done', text: t }; }, e => { say(`online: room list: ${e.message}`); list = { state: 'failed' }; });
      return 1;
    },
    host_service_net_list_lobbies_poll: (out, max) => {
      if (list.state === 'working') return -2;
      if (list.state !== 'done') { list = { state: 'idle' }; return -1; }
      body = list.text; list = { state: 'idle' };
      let n;
      try { n = x.host_service_net_list_lobbies(scratch, Math.min(max, LIST_MAX)); } finally { body = null; }
      if (n > 0) guest.bytes(out, n * INFO).set(mem().subarray(scratch, scratch + n * INFO));
      return n;
    },
    // DHD_HASH_LOG's state hashes (online.c): a debugging log, none in the browser.
    host_service_net_hash_ranges: () => 0n,
  };
}
