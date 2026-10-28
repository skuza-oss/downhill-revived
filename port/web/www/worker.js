// The game in a Web Worker: port/web/run_headless.mjs's host (JSPI fibers, guest memory folds,
// the GS reactor sharing the core's memory) with the browser's devices: the player's ISO read
// locally in slices (File + FileReaderSync, never uploaded), WebGPU for the GS, sound to an
// AudioWorklet and pads/keys from the page through SharedArrayBuffers.
//
// The GS's device ops are the Rust renderer module's (port/render, build/renderer/dhd_render*),
// loaded in the plan's order (finding 085): the core and its memory, then the Rust module on that
// memory (its window 0x60000000-0x69000000 checked untouched first), then the async dhdr_web_init
// (device, canvas surface, exact pipelines, prewarm dispatch), then the reactor, whose dhd_render
// imports are the Rust module's own wasm exports (wasm to wasm, no JS frame) and whose readback
// waits in env.web_gpu_await, a Suspending import on dhdr_web_pending()'s promise. Each vsync
// shows the scanout through the reactor's web_present.
//
// The threaded build (build.py --threads; findings 087 BS2, 090 SP4; the core module then defines a
// shared memory): the graphics run in two more workers, as natively on a vu and a GS thread:
// www/vu.js (VIF1/VU1, build/vu.wasm) and www/graphics.js (the GS reactor and the Rust renderer),
// which this one starts and hands the memory, the compiled modules and the canvas. The core's
// graphics imports link to build/gqueue.wasm instead (port/web/web_queue.c's core side): it copies
// the VIF1/GIF data into a queue in the shared memory and waits only where the native EE waits for
// its graphics threads (a graphics wait, or a call that returns GS state). The VU worker runs that
// queue and passes the GS's part on to the GS worker. Each vsync queues the picture; graphics.js
// shows it and leaves its numbers for the status line here.
import { createVfs } from './vfs.js';
import { thunks } from './thunks.js';
import { createSettings, BROWSER_PC_SETTINGS, RENDERER_LIVE } from './settings.js';
import { startRenderer, renderImports, definesSharedMemory, shareSafeDecoding, Q, QI, STAT, ENV_SCRATCH } from './graphics_common.js';

const post = (type, data) => postMessage({ type, ...data });
const say = text => post('log', { text });
onmessage = e => { if (e.data.type === 'start') start(e.data).catch(err => { post('error', { text: String(err?.stack || err) }); }); };

// ---- the disc: 1 MiB blocks, a few kept ----
function discSource(m) {
  const BLOCK = 1 << 20, cache = new Map();
  let size, readBlock;
  if (m.file) {
    const reader = new FileReaderSync(), file = m.file; size = file.size;
    readBlock = (at, n) => new Uint8Array(reader.readAsArrayBuffer(file.slice(at, at + n)));
  } else {
    // Test hook (serve.mjs --iso): the same reads as HTTP Range requests to localhost.
    const head = new XMLHttpRequest(); head.open('HEAD', m.isoUrl, false); head.send();
    size = Number(head.getResponseHeader('Content-Length'));
    readBlock = (at, n) => { const x = new XMLHttpRequest(); x.open('GET', m.isoUrl, false);
      x.responseType = 'arraybuffer'; x.setRequestHeader('Range', `bytes=${at}-${at + n - 1}`); x.send();
      if (x.status !== 206) throw Error(`disc read ${x.status}`); return new Uint8Array(x.response); };
  }
  function block(i) {
    let b = cache.get(i);
    if (b) { cache.delete(i); cache.set(i, b); return b; }
    b = readBlock(i * BLOCK, Math.min(BLOCK, size - i * BLOCK));
    cache.set(i, b); if (cache.size > 48) cache.delete(cache.keys().next().value);
    return b;
  }
  return { size, read(pos, out) { let done = 0;
    while (done < out.length && pos + done < size) {
      const at = pos + done, b = block(Math.floor(at / BLOCK)), off = at % BLOCK;
      const n = Math.min(out.length - done, b.length - off); out.set(b.subarray(off, off + n), done); done += n;
    } return done; } };
}

// keyboard.c's table: [VK code, button in menus, button while riding, stick axis, direction]
const P = { SELECT: 1, L3: 2, R3: 4, START: 8, UP: 16, RIGHT: 32, DOWN: 64, LEFT: 128, L2: 256, R2: 512, L1: 1024,
  R1: 2048, TRIANGLE: 4096, CIRCLE: 8192, CROSS: 16384, SQUARE: 32768 };
const KEYS = [[0x25, P.LEFT, 0, 1, -1], [0x27, P.RIGHT, 0, 1, 1], [0x26, P.UP, 0, 2, -1], [0x28, P.DOWN, 0, 2, 1],
  [0x20, P.CROSS, P.CROSS], [88, P.CROSS, P.CROSS], [83, P.TRIANGLE, P.TRIANGLE], [0x08, P.TRIANGLE, P.TRIANGLE],
  [0x1b, P.TRIANGLE, P.START], [0x0d, P.START, P.START], [0x09, P.SELECT, P.SELECT], [90, P.SQUARE, P.SQUARE],
  [67, P.CIRCLE, P.CIRCLE], [65, P.L1, P.L1], [68, P.R1, P.R1], [81, P.L2, P.L2], [69, P.R2, P.R2], [87, P.R3, P.R3], [70, P.L3, P.L3]];
const PRESSURE = [P.RIGHT, P.LEFT, P.UP, P.DOWN, P.TRIANGLE, P.CIRCLE, P.CROSS, P.SQUARE, P.L1, P.R1, P.L2, P.R2];

async function start(m) {
  if (typeof WebAssembly.Suspending !== 'function') throw Error('this browser has no JSPI (WebAssembly.Suspending); use Chrome or Edge 137 or later');
  const t0 = performance.now();
  const fetchBytes = async url => { const r = await fetch(url); if (!r.ok) throw Error(`${url}: ${r.status}`); return r.arrayBuffer(); };
  const [coreBytes, graphicsBytes, metadata] = await Promise.all([
    fetchBytes('build/dhd_core.wasm'), fetchBytes('build/graphics.wasm'), fetch('build/imports.json').then(r => r.json())]);
  // The threaded build's core defines a shared memory; its graphics imports link to gqueue.wasm.
  const threaded = definesSharedMemory(coreBytes);
  if (threaded) shareSafeDecoding();
  const [coreModule, graphicsModule, queueModule, vuModule] = await Promise.all([WebAssembly.compile(coreBytes), WebAssembly.compile(graphicsBytes),
    threaded ? WebAssembly.compileStreaming(fetch('build/gqueue.wasm')) : null, threaded ? WebAssembly.compileStreaming(fetch('build/vu.wasm')) : null]);
  const disc = discSource(m);
  say(`disc: ${(disc.size / 2 ** 30).toFixed(2)} GiB`);
  const keys = new Uint8Array(m.input, 0, 256), pads = new Int32Array(m.input, 256, 32);  // main.js: players 1-4
  const mouse = new Int32Array(m.input, 384, 16);  // main.js: HostMouse, then [12] riding
  const ring = { head: new Int32Array(m.audio, 0, 2), data: new Int16Array(m.audio, 8), frames: (m.audio.byteLength - 8) / 4 };

  const vfs = await createVfs(say);
  say(`memory card: ${vfs.count()} saved files in this browser`);
  // host_api.h's HostDirEntry: name[260], is_dir, size, created and modified (6 x u16 each).
  function entry(n, out) { const o = span(out, 292); o.fill(0); o.set(new TextEncoder().encode(n.name).subarray(0, 259));
    dv.setInt32(fold(out) + 260, n.dir ? 1 : 0, true); dv.setUint32(fold(out) + 264, n.data?.length || 0, true);
    const d = new Date(n.time || 0), t = [d.getFullYear(), d.getMonth() + 1, d.getDate(), d.getHours(), d.getMinutes(), d.getSeconds()];
    t.forEach((v, i) => { dv.setUint16(fold(out) + 268 + 2 * i, v, true); dv.setUint16(fold(out) + 280 + 2 * i, v, true); }); }
  // DHD_SETTINGS: the stored dhd.ini's name (settings.js); the page's ?env.NAME go over everything.
  // BROWSER_PC_SETTINGS: the PC SETTINGS rows the browser shows and applies (settings.js).
  // DHD_RENDER=native: the Rust renderer draws in its native (upscaled) mode, DHD_SCALE times the
  // PS2's resolution (2 unless the settings say otherwise); ?env.DHD_RENDER=exact shows the exact
  // renderer's picture instead (checks).
  // DHD_GRAPHICS_ORDERED (threaded; port/hle/hle.h): the core skips the waits that only order queued
  // graphics work, which the workers run in order anyway; ?env.DHD_GRAPHICS_ORDERED= keeps them.
  // DHD_FPS_WALL=1 (port/hle/kernel.c): above 60 the game's step follows real time, as on x64, so a
  // PC that can't keep 144 runs fewer, longer updates at the game's own speed, not slow motion;
  // host_wall_seconds is then the real clock (performance.now). ?env.DHD_FPS_WALL= turns it off
  // (the virtual clock, field / 60, as run_headless.mjs's deterministic runs keep).
  const env = { DHD_ISO: '/disc.iso', DHD_HEADLESS: '1', DHD_FRAME_CLOCK: '1', DHD_FPS_WALL: '1', DHD_SKIP_MOVIES: '1',
    DHD_RENDER: 'native', DHD_SETTINGS: 'dhd.ini', ...(threaded ? { DHD_GRAPHICS_ORDERED: '1' } : {}),
    ...BROWSER_PC_SETTINGS, ...(m.env || {}) };
  const settings = await createSettings(env, say);
  let core, graphics, queue, memory, bytes, dv, arena = 0x07000000, current = 65, csr = 0x1f80, field = 0;
  const strings = new Map(), files = new Map(), fibers = new Map(); let nextFile = 1, nextDir = 1; const dirs = new Map();
  const refresh = () => { bytes = new Uint8Array(memory.buffer); dv = new DataView(memory.buffer); };
  function fold(a) { a >>>= 0; const low = a & 0x1fffffff;
    if (a >= 0x38000000 && a < 0x3a000000) return a - 0x30000000;
    if (a >= 0x30400000 && a < 0x32000000) return a - 0x28000000;
    if ((a & 0xe0000000) === 0 || (a & 0xe0000000) === 0x20000000) {
      if (low >= 0x08000000 && low < 0x0a000000) return low;
      if (low >= 0x00400000 && low < 0x02000000) return low + 0x08000000;
    } return a; }
  const span = (a, n) => { if (bytes.buffer !== memory.buffer) refresh(); a = fold(a); n >>>= 0;
    if (a + n > bytes.length) throw Error(`guest span OOB ${a.toString(16)}+${n.toString(16)}`); return bytes.subarray(a, a + n); };
  const str = a => { if (bytes.buffer !== memory.buffer) refresh(); a = fold(a); const end = bytes.indexOf(0, a); return new TextDecoder().decode(bytes.subarray(a, end)); };
  const intern = s => { if (strings.has(s)) return strings.get(s); const b = new TextEncoder().encode(s + '\0'); const a = arena; arena += b.length;
    if (arena > 0x07800000) throw Error('environment arena exhausted'); span(a, b.length).set(b); strings.set(s, a); return a; };
  class Exit extends Error { constructor(code) { super(`host_exit(${code})`); this.code = code; } }
  let fail; const failure = new Promise((_, reject) => { fail = reject; });
  const save = () => { const f = fibers.get(current); f.sp = core.__stack_pointer.value; f.csr = getCsr(); };
  // The FP control word lives in memory at 0x07800000 (the float helpers read it there); the core's
  // host_fp_csr/host_fp_set_csr are linked straight to the reactor's, which load and store it.
  const setCsr = x => { csr = x; if (memory) { if (bytes.buffer !== memory.buffer) refresh(); dv.setUint32(0x07800000, x, true); } };
  const getCsr = () => { if (!memory) return csr; if (bytes.buffer !== memory.buffer) refresh(); return dv.getUint32(0x07800000, true); };
  function activate(id) { const f = fibers.get(id); if (!f) throw Error(`invalid fiber ${id}`);
    current = id; core.__stack_pointer.value = f.sp; setCsr(f.csr);
    if (f.resume) { const resume = f.resume; f.resume = null; resume(); }
    else if (!f.started) { f.started = true; WebAssembly.promising(core.__indirect_function_table.get(f.fn))(f.ctx).then(() => fail(Error(`fiber ${id} returned`)), fail); }
    else throw Error(`fiber ${id} has no continuation`); }

  // Pacing: one field each period at most, the core's port_frame_period (host_pacing_config at each
  // vsync): 1/59.94 s, or 1/DHD_FPS above 60 (PC SETTINGS > Frame Rate; Monitor is the display's
  // refresh, which the page measured: m.displayHz). Every field yields to the event loop so the
  // canvas shows the picture (OffscreenCanvas commits when the worker's task ends). The core runs
  // on its frame clock here; at 60 a PC that can't keep up runs the game slower, above 60 the
  // update's step grows with real time (DHD_FPS_WALL), as on x64.
  const channel = new MessageChannel(); let wake = null; channel.port1.onmessage = () => { const w = wake; wake = null; w?.(); };
  const yieldTask = () => new Promise(r => { wake = r; channel.port2.postMessage(0); });
  let period = 1000 / 59.94, due = performance.now(), shown = 0, lastReport = performance.now(), lastField = 0;
  let riding = 0, held = new Int8Array(KEYS.length);
  // host_pad_device: player `port` (0-3; played by hand the game opens four pads on a multitap,
  // pad.c) as hle.h's HostPad. connected: a gamepad for that player, player 1 always (the keyboard).
  function pad(port, out) {
    const o = span(out, 19); o.fill(0); o[2] = o[3] = o[4] = o[5] = 128;
    let buttons = 0; const p = pads.subarray(port * 8, port * 8 + 8);
    const connected = p.length > 0 && Atomics.load(p, 0) !== 0;
    if (connected) { buttons = Atomics.load(p, 1); for (let i = 0; i < 4; i++) o[2 + i] = Atomics.load(p, 2 + i); }
    o[18] = connected || port === 0 ? 1 : 0;
    if (port === 0) {
      riding = dv.getInt32(0x0829E290, true) !== 0;
      Atomics.store(mouse, 12, riding ? 1 : 0);  // the page hides the cursor while riding
      const dir = [0, 0, 0];
      KEYS.forEach((k, i) => { if (!Atomics.load(keys, k[0])) { held[i] = 0; return; }
        if (!held[i]) held[i] = riding ? 2 : 1;
        if (held[i] === 2 && k[3]) dir[k[3]] += k[4]; else buttons |= held[i] === 2 ? k[2] : k[1]; });
      if (dir[1]) o[4] = dir[1] < 0 ? 0 : 255;  // left stick x, full deflection while riding
      if (dir[2]) o[5] = dir[2] < 0 ? 0 : 255;
    }
    o[0] = buttons & 255; o[1] = buttons >> 8;
    PRESSURE.forEach((b, i) => { o[6 + i] = buttons & b ? 255 : 0; });
  }
  // The core's environment; the threaded build's graphics worker gets each change in its queue,
  // in order with the graphics calls (the reactor reads DHD_RENDER, DHD_SCALE, ... from its own).
  // A variable the renderer follows while the game runs (RENDERER_LIVE: PC SETTINGS > Resolution,
  // Scaling) tells the reactor, here or in graphics.js, which applies it at the next vsync.
  function setEnv(name, value) {
    env[name] = value;
    if (!threaded && graphics && RENDERER_LIVE.has(name)) graphics.web_env_changed();
    if (!threaded || !queue) return;
    const b = new TextEncoder().encode(`${name}=${value}`);
    if (b.length > 4000) throw Error(`environment line too long: ${name}`);
    span(ENV_SCRATCH, b.length).set(b); queue.web_q_env(ENV_SCRATCH, b.length);
  }
  // Threaded: the GS worker (graphics.js) on this memory, with the canvas, then the VU worker
  // (vu.js), which needs the GS worker's queue; their log lines and errors come to the page
  // through here.
  function startWorker(script, name, start, transfer) {
    const w = new Worker(script, { type: 'module' });
    const ready = new Promise((resolve, reject) => {
      w.onmessage = e => { const d = e.data;
        if (d.type === 'ready') resolve(d);
        else if (d.type === 'error') { postMessage(d); const err = Error(d.text); reject(err); fail(err); }
        else postMessage(d); };
      w.onerror = e => { const err = Error(name + ' worker: ' + e.message); reject(err); fail(err); };
    });
    w.postMessage({ type: 'start', memory, env: { ...env }, abi: queue.web_q_abi(), ...start }, transfer);
    return ready;
  }
  async function startGraphicsWorker() {
    await startWorker('graphics.js', 'graphics', { module: graphicsModule, canvas: m.canvas }, [m.canvas]);
    await startWorker('vu.js', 'VU', { module: vuModule }, []);
  }
  let soundFrames = 0;  // stereo frames the game wrote (the status line's seconds of sound)
  const impl = {
    host_getenv: a => env[str(a)] ? intern(env[str(a)]) : 0,
    host_putenv_s: (a, b) => { setEnv(str(a), str(b)); },
    host_putenv: a => { const s = str(a), i = s.indexOf('='); setEnv(s.slice(0, i), s.slice(i + 1)); },
    host_last_error: () => 0, host_exit: code => { throw new Exit(code); },
    host_error_box: (a, b) => { throw Error(`${str(a)}: ${str(b)}`); },
    host_exe_path: (a, n) => { const b = new TextEncoder().encode('/dhd.js\0'); span(a, n).set(b.subarray(0, n)); return Math.min(n - 1, b.length - 1); },
    host_display_hz: () => m.displayHz || 60, host_monitor_aspect: () => 16 / 9,
    host_wall_seconds: env.DHD_FPS_WALL === '1' ? () => performance.now() / 1000 : () => field / 60, host_cycles: () => 0n,
    host_fp_csr: getCsr, host_fp_set_csr: setCsr, host_fp_mode: x => { setCsr(x ? 0xffc0 : 0x1f80); },
    host_frame_context: f => { field = Number(f); if (threaded) queue.web_q_frame(f); else graphics.web_frame(f); },
    host_fiber_main: () => { if (!fibers.has(65)) fibers.set(65, { sp: core.__stack_pointer.value, csr: getCsr(), started: true }); return 65; },
    host_fiber_current: () => current,
    host_callback_fiber: (fn, ctx) => { if (!core.__indirect_function_table.get(fn)) throw Error('null callback');
      for (let id = 1; id <= 64; id++) if (!fibers.has(id)) {
        const sp = 0x06000000 + id * 0x40000; span(sp - 0x40000 + 0x1000, 0x3f000).fill(0);
        fibers.set(id, { fn, ctx, sp, csr: 0x1f80, started: false }); return id;
      } return 0; },
    host_fiber_switch: new WebAssembly.Suspending(id => {
      if (id === current) return;
      save(); const f = fibers.get(current);
      return new Promise(resolve => { f.resume = resolve; queueMicrotask(() => { try { activate(id); } catch (e) { fail(e); } }); });
    }),
    host_fiber_delete: id => { if (id === current || id === 65) throw Error('delete active/root fiber'); fibers.delete(id); },
    host_stack_ranges_copy: (out, max) => { let n = 0; for (const id of fibers.keys()) if (id !== 65) {
      if (n < max) { dv.setUint32(out + 8 * n, 0x06000000 + (id - 1) * 0x40000, true); dv.setUint32(out + 8 * n + 4, 0x06000000 + id * 0x40000, true); } n++;
    } return Math.min(n, max); },
    host_ram_create: n => +(n === 0x02000000),
    host_ram_view: (at, offset, n) => +((offset === 0 && n === 0x02000000 && [0x08000000, 0x28000000, 0x38000000].includes(at >>> 0)) ||
      (offset === 0x00400000 && n === 0x01c00000 && [0x00400000, 0x20400000, 0x30400000].includes(at >>> 0))),
    host_mem_commit: (a, n) => +((a >>> 0) + n <= bytes.length), host_mem_reserved: (a, n) => +((a >>> 0) + n <= bytes.length),
    host_mem_protect: (_a, _n, protection) => +(protection === 2), host_callback_fault_guard: () => 0, host_fault_guard_here: () => 0,
    // The disc (read only) and the memory card's files (vfs.js); other paths don't exist.
    host_file_open: (a, mode) => { const name = str(a);
      if (mode & 8) { const id = nextFile++; files.set(id, { sink: true }); return id; }
      if (name === env.DHD_ISO) { if (mode & 2) return 0; const id = nextFile++; files.set(id, { disc: true, pos: 0 }); return id; }
      const f = vfs.open(name, mode); if (!f) return 0; const id = nextFile++; files.set(id, f); return id; },
    host_file_close: id => { const f = files.get(id); if (f && !f.sink && !f.disc) vfs.flush(f); files.delete(id); },
    host_file_read: (id, a, n) => { const f = files.get(id); if (!f || f.sink) return -1n;
      const got = f.disc ? disc.read(f.pos, span(a, n)) : vfs.read(f, span(a, n)); if (f.disc) f.pos += got; return BigInt(got); },
    host_file_write: (id, a, n) => { const f = files.get(id); if (!f || f.disc) return 0; return f.sink ? n : vfs.write(f, span(a, n)); },
    host_file_seek: (id, off) => { const f = files.get(id); if (!f || f.sink) return -1; f.pos = Number(BigInt.asUintN(64, off)); return 0; },
    host_file_size: id => { const f = files.get(id); return !f || f.sink ? 0n : BigInt(f.disc ? disc.size : vfs.size(f)); },
    host_file_flush: id => { const f = files.get(id); if (f && !f.sink && !f.disc) vfs.flush(f); return 0; },
    host_console_write: (a, n) => { say(new TextDecoder().decode(span(a, n)).trimEnd()); },
    host_path_exists: a => +(str(a) === env.DHD_ISO || vfs.exists(str(a))), host_path_is_dir: a => +vfs.isDir(str(a)),
    host_dir_create: a => +vfs.mkdir(str(a)),
    host_dir_open: (a, out) => { const list = vfs.list(str(a)); if (!list?.length) return 0;
      const id = nextDir++; dirs.set(id, { list, at: 0 }); entry(list[0], out); return id; },
    host_dir_next: (id, out) => { const d = dirs.get(id); if (!d || ++d.at >= d.list.length) return 0; entry(d.list[d.at], out); return 1; },
    host_dir_close: id => { dirs.delete(id); },
    host_service_host_disc_ready: () => 0, host_service_host_skip_movies: () => 1,
    host_service_host_pad_device: (port, a) => pad(port, a),
    // host_api.h's HostMouse, 36 bytes, from the page (main.js)
    host_service_mouse_state: out => { const o = span(out, 36);
      for (let i = 0; i < 9; i++) { const v = Atomics.load(mouse, i); o[4 * i] = v; o[4 * i + 1] = v >> 8; o[4 * i + 2] = v >> 16; o[4 * i + 3] = v >> 24; } },
    // PC settings (settings.js): dhd.ini's lines in this browser.
    host_service_host_game_options_read: (a, n) => { const v = settings.gameOptions(); if (v === undefined || !n) return 0;
      const b = new TextEncoder().encode(v).subarray(0, n - 1), o = span(a, b.length + 1); o.set(b); o[b.length] = 0; return 1; },
    host_service_host_setting_locked: a => settings.locked(str(a)),
    host_service_host_setting_save: (k, v) => settings.save(str(k), str(v)),
    host_service_movie_update: () => 0, host_service_movie_playing: () => 0,
    host_service_host_poll_dump: () => 0,
    host_service_graphics_threaded: () => 1, host_service_texpack_available: () => 0,
    host_pacing_period: () => 1 / 60,
    host_pacing_config: p => { period = p * 1000; },
    host_sound_write: (a, frames) => {
      soundFrames += frames;
      const src = new Int16Array(span(a, frames * 4).slice().buffer), h = ring.head, cap = ring.frames;
      const w = Atomics.load(h, 0), r = Atomics.load(h, 1), free = cap - 1 - ((w - r + cap) % cap);
      const n = Math.min(frames, free);
      for (let i = 0; i < n; i++) { const at = ((w + i) % cap) * 2; ring.data[at] = src[i * 2]; ring.data[at + 1] = src[i * 2 + 1]; }
      Atomics.store(h, 0, (w + n) % cap);
    },
    // The VBLANK's picture, then pacing; the only place the game thread lets the browser in.
    // web_present draws the scanout onto the canvas surface (it never suspends).
    // Threaded: the vsync is queued (with the GS register pages as they are now); graphics.js draws
    // and shows it while this worker goes on, and leaves the status line's numbers in the queue's block.
    host_service_gs_vsync: new WebAssembly.Suspending(async () => {
      let size, presented = 0, stats = null;
      if (threaded) {
        queue.web_q_vsync();
        const s = new Int32Array(memory.buffer, Q + 4 * QI.stats, 16);
        size = s[STAT.size] >>> 0; shown = s[STAT.shown];
        stats = which => s[[STAT.draws, STAT.dispatches, STAT.reads, STAT.submits, STAT.scale][which]] >>> 0;
      } else {
        const saved = getCsr(); setCsr(0x1f80);
        try {
          size = graphics.web_display();
          if (size) presented = graphics.web_present(m.canvas.width, m.canvas.height);
        } finally { setCsr(saved); }
        if (presented === 1) shown++;
        stats = which => graphics.web_gpu_stats(which);
      }
      const now = performance.now();
      if (now - lastReport >= 1000) {
        post('status', { field, fps: (field - lastField) * 1000 / (now - lastReport), shown, size: size ? `${size >>> 16}x${size & 0xffff}` : '-',
          draws: stats(0), dispatches: stats(1), reads: stats(2), riding, sound: +(soundFrames / 48000).toFixed(1),
          renderer: 'rust', submits: stats(3), scale: stats(4), workers: threaded ? 3 : 1 });
        lastReport = now; lastField = field;
      }
      due = Math.max(due + period, now - 4 * period);
      if (due > now + 1) await new Promise(r => setTimeout(r, due - now)); else await yieldTask();
    }),
  };
  const noops = `host_sleep_ms host_wait_wall host_dpi_aware host_callbacks_drain host_frame_registers host_frame_video host_capture_name
host_pacing_wait host_pacing_handoff host_pacing_input host_callback_heartbeat
host_sound_open host_sound_reset host_sound_tick host_sound_log_stack snd_host_bench
host_service_host_init host_service_host_console_init host_service_host_settings_load host_service_host_pad_vibrate
host_service_movie_prepare host_service_movie_start host_service_movie_audio host_service_graphics_wait host_service_graphics_shutdown
host_service_gs_pump_messages host_service_gs_window_title host_service_gs_native_view
host_service_gs_window_apply host_service_gs_present_options_apply host_service_gs_window_close
host_service_text_input_begin host_service_text_input_end host_service_host_backtrace host_service_presence`.split(/\s+/);
  for (const n of noops) impl[n] ??= () => {};
  for (const n of ['host_service_net_racing', 'host_service_net_seconds', 'host_service_net_server_ms', 'host_heartbeat_eip', 'host_service_launcher_open']) impl[n] = () => 0;

  const graphicsExports = new Set(WebAssembly.Module.exports(graphicsModule).map(e => e.name));
  const linkedExports = threaded ? new Set(WebAssembly.Module.exports(queueModule).map(e => e.name)) : graphicsExports;
  const coreImports = WebAssembly.Module.imports(coreModule).map(e => e.name);
  const direct = coreImports.filter(n => linkedExports.has('web_x_' + n));
  // Threaded: every call the one-worker build gives the reactor goes through the queue.
  const unqueued = threaded ? coreImports.filter(n => graphicsExports.has('web_x_' + n) && !linkedExports.has('web_x_' + n)) : [];
  if (unqueued.length) throw Error(`gqueue.wasm does not queue ${unqueued.join(', ')}`);
  const table = new WebAssembly.Table({ element: 'anyfunc', initial: direct.length });
  const thunk = (await WebAssembly.instantiate(thunks(direct, metadata), { env: { t: table } })).instance.exports;
  const imports = { env: {} };
  for (const entry of WebAssembly.Module.imports(coreModule)) {
    const name = entry.name;
    if (entry.module !== 'env' || !metadata[name]) throw Error(`unchecked import ${entry.module}.${name}`);
    if (direct.includes(name)) { imports.env[name] = thunk[name]; continue; }
    if (impl[name] instanceof WebAssembly.Suspending) { imports.env[name] = impl[name]; continue; }
    imports.env[name] = impl[name] || ((...a) => { throw Error(`unsupported service ${name}(${a})`); });
  }
  core = (await WebAssembly.instantiate(coreModule, imports)).exports; memory = core.memory; refresh();
  if (threaded) {
    if (!(memory.buffer instanceof SharedArrayBuffer)) throw Error('the threaded core has no shared memory');
    queue = (await WebAssembly.instantiate(queueModule, { env: { memory } })).exports;
    direct.forEach((n, i) => table.set(i, queue['web_x_' + n]));
    await startGraphicsWorker();
    say(`loaded in ${((performance.now() - t0) / 1000).toFixed(1)} s; ${direct.length} GS calls queued to the VU and GS workers; renderer rust`);
    post('running', {});
    let status = 0;
    try { await Promise.race([WebAssembly.promising(core.__main_argc_argv)(0, 0), failure]); }
    catch (e) { if (e instanceof Exit) status = e.code; else throw e; }
    post('exit', { status });
    return;
  }
  const renderer = await startRenderer(memory, m.canvas, say);
  say(`WebGPU: ${renderer.adapter} (${renderer.version})`);
  // The reactor's WASI needs: its environment, stderr, nothing else. The environment is the core's
  // as it is now (the reactor reads it again when it makes the native renderer, after the core's
  // settings load: DHD_SCALE).
  const enc = new TextEncoder(), envList = () => Object.entries(env).map(([k, v]) => enc.encode(`${k}=${v}\0`));
  let gx;
  const wasi = {
    environ_sizes_get: (count, size) => { const l = envList(); dv.setUint32(count, l.length, true); dv.setUint32(size, l.reduce((s, b) => s + b.length, 0), true); return 0; },
    environ_get: (ptrs, buf) => { if (bytes.buffer !== memory.buffer) refresh(); for (const b of envList()) { dv.setUint32(ptrs, buf, true); ptrs += 4; bytes.set(b, buf); buf += b.length; } return 0; },
    fd_write: (fd, iovs, n, written) => { let out = ''; let total = 0;
      for (let i = 0; i < n; i++) { const p = dv.getUint32(iovs + 8 * i, true), l = dv.getUint32(iovs + 8 * i + 4, true); out += new TextDecoder().decode(bytes.subarray(p, p + l)); total += l; }
      dv.setUint32(written, total, true); if (out.trim()) say(out.trimEnd()); return 0; },
    fd_close: () => 0, fd_fdstat_get: () => 8, fd_fdstat_set_flags: () => 8, fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8,
    // clock_time_get: libc's clock (the graphics reactor's C may ask for it); nanoseconds of the page's clock
    clock_time_get: (id, precision, out) => { if (bytes.buffer !== memory.buffer) refresh(); dv.setBigUint64(out, BigInt(Math.round(performance.now() * 1e6)), true); return 0; },
    fd_read: () => 8, fd_seek: () => 8, path_open: () => 44, proc_exit: code => { throw new Exit(code); },
  };
  const reactorImports = { env: { memory, host_wall_seconds: impl.host_wall_seconds,
    host_fp_csr: impl.host_fp_csr, host_fp_set_csr: impl.host_fp_set_csr,
    // A readback's wait: the reactor suspends on the promise of the map in flight (1 mapped, 0 failed).
    web_gpu_await: new WebAssembly.Suspending(() => renderer.R.dhdr_web_pending()) },
    dhd_render: renderImports(graphicsModule, renderer.rx), wasi_snapshot_preview1: wasi };
  const gi = await WebAssembly.instantiate(graphicsModule, reactorImports);
  graphics = gx = gi.exports;
  gx._initialize?.();
  direct.forEach((n, i) => table.set(i, gx['web_x_' + n]));
  if (!gx.web_gpu_enable()) throw Error('GS: the Rust renderer did not start');
  say(`loaded in ${((performance.now() - t0) / 1000).toFixed(1)} s; ${direct.length} GS calls linked directly; renderer rust`);
  post('running', {});
  let status = 0;
  try { await Promise.race([WebAssembly.promising(core.__main_argc_argv)(0, 0), failure]); }
  catch (e) { if (e instanceof Exit) status = e.code; else throw e; }
  post('exit', { status });
}
