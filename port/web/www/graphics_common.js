// Shared by worker.js (the game's worker) and graphics.js (the threaded build's graphics worker):
// the Rust renderer's start on the core's memory, the reactor's dhd_render imports, and the layout
// of the threaded build's graphics queue (port/web/web_queue.h).

// The Rust renderer: its data, stack and bounded heap (finding 085, G1 decision 5), and the
// dhdr_web_init flags: the raster key gpu_rust_web.c passes to dhdr_exact_create (DHDR_RASTER_*,
// all three on; its pipelines are built and prewarmed in the init), prewarm on, errors-only log.
const RUST_WINDOW = [0x60000000, 0x69000000];
const RUST_INIT_FLAGS = 7;

// The Rust module, on the core's memory; dhdr_web_init on the canvas. Returns the glue and the
// module's exports. env: the page's environment, for what Rust can't read itself here
// (DHD_TEST_NO_MESHES, Native VU's refusal test, finding 194).
export async function startRenderer(memory, canvas, say, env = {}) {
  const words = new Uint32Array(memory.buffer, RUST_WINDOW[0], (RUST_WINDOW[1] - RUST_WINDOW[0]) / 4);
  let bits = 0;
  for (let i = 0; i < words.length; i++) bits |= words[i];
  if (bits) throw Error(`GS: the Rust renderer's memory window is in use (nonzero at 0x${(RUST_WINDOW[0] + 4 * words.findIndex(v => v)).toString(16)})`);
  const R = await import('./build/renderer/dhd_render.js');
  const module = await WebAssembly.compileStreaming(fetch('build/renderer/dhd_render_bg.wasm'));
  const rx = await R.default({ module_or_path: module, memory });
  if (rx.memory !== memory) throw Error('GS: the Rust renderer does not use the core memory');
  // Rust logs through the console (its wbg bridge); the page's log shows those lines too.
  for (const k of ['log', 'info', 'warn', 'error']) {
    const f = console[k].bind(console);
    console[k] = (...a) => { say(a.map(String).join(' ')); f(...a); };
  }
  const noMeshes = (env.DHD_TEST_NO_MESHES ?? '').trim();
  if (/^\d+$/.test(noMeshes)) R.dhdr_web_test_no_meshes(Number(noMeshes));
  if (!await R.dhdr_web_init(canvas, RUST_INIT_FLAGS)) throw Error('GS: the Rust renderer did not start (dhdr_web_init)');
  const text = p => { const b = new Uint8Array(memory.buffer, p >>> 0); return new TextDecoder().decode(b.subarray(0, b.indexOf(0))); };
  return { R, rx, adapter: text(rx.dhdr_adapter_name()) || 'WebGPU', version: text(rx.dhdr_version()) };
}

// The reactor's dhd_render imports: the Rust module's own wasm exports, so each device op goes wasm
// to wasm (a JS function here would put a JS frame between the two modules).
export function renderImports(graphicsModule, rx) {
  const out = {};
  for (const i of WebAssembly.Module.imports(graphicsModule)) if (i.module === 'dhd_render') {
    const f = rx[i.name];
    if (typeof f !== 'function' || !/\[native code\]/.test(Function.prototype.toString.call(f)))
      throw Error(`GS: the Rust renderer has no wasm export ${i.name}`);
    out[i.name] = f;
  }
  return out;
}

// ---- the threaded build (build.py --threads) ----

// web_queue.h's control blocks (WEB_Q_BLOCK: the core's queue to the VU worker, vu.js;
// WEB_Q_BLOCK2: the VU worker's to the GS worker, graphics.js) and their fields as Int32Array indices.
export const Q = 0x07A00000, Q2 = 0x07A00800;
export const QI = { written: 0, consumed: 16, sleeping: 32, waiting: 48, ring: 64, failed: 65, display: 67,
  env_text: 70, env_bytes: 71, stats: 72 };
// The status line's numbers graphics.js leaves in the first block's stats for worker.js.
export const STAT = { shown: 0, size: 1, draws: 2, dispatches: 3, reads: 4, submits: 5, scale: 6 };
// Where worker.js puts an environment line for web_q_env (it is copied at once).
export const ENV_SCRATCH = Q + 0x1000;

// True when the module defines a shared memory (the threaded build's core).
export function definesSharedMemory(bytes) {
  const b = new Uint8Array(bytes);
  const leb = i => { let n = 0, s = 0, x; do { x = b[i++]; n += (x & 127) * 2 ** s; s += 7; } while (x & 128); return [n, i]; };
  for (let i = 8; i < b.length;) {
    const id = b[i]; const [size, at] = leb(i + 1);
    if (id === 5) { const [count, j] = leb(at); return count > 0 && (b[j] & 2) !== 0; }
    i = at + size;
  }
  return false;
}

// TextDecoder refuses views of a SharedArrayBuffer: decode a copy (the threaded build's memory is
// shared; wasm-bindgen's glue and these workers decode strings straight from it).
export function shareSafeDecoding() {
  const decode = TextDecoder.prototype.decode;
  if (decode.shareSafe) return;
  const safe = function (input, options) {
    if (typeof SharedArrayBuffer !== 'undefined') {
      if (input instanceof SharedArrayBuffer) input = new Uint8Array(input).slice();
      else if (input?.buffer instanceof SharedArrayBuffer) input = new Uint8Array(input.buffer, input.byteOffset, input.byteLength).slice();
    }
    return decode.call(this, input, options);
  };
  safe.shareSafe = true;
  TextDecoder.prototype.decode = safe;
}
