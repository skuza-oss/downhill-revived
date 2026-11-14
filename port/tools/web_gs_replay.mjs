// Replay a legacy GS capture on the browser's GS backend and write its VRAM hashes (finding 084).
//
//   node port/tools/web_gs_replay.mjs CAPTURE.gs PREFIX [--web TREE (default: this tree)]
//
// Builds port/gs/replay_web.c with gs.c, memory.c and the web tree's port/web/gpu_rust_web.c into a
// wasm (wasi-sdk, DHD_DEVCACHES/wasi-sdk), then loads it in headless Chromium (port/web/cdp.mjs's
// runPage) with the Rust renderer module (port/render). Writes
// PREFIX-web-vram.txt in replay.c's DHD_GS_REPLAY_VRAM format, to compare with replay_gs.py's
// PREFIX-wgpu-vram.txt, and PREFIX-web.png, a screenshot of the last field's picture on the canvas. Any other option
// is refused (the JS renderer and its --renderer selector are gone: cleanup CL5, finding 088).
// The module is built first by the web tree's port/web/build_renderer.py (rustgs' cache makes a
// rebuild cheap) into its build/web/renderer.
// The page wires the two modules as worker.js does (plan 3.3): one memory (made by the page, imported
// by both: the replay module's first heap ends below the GS MMIO page and grows above the Rust
// window), the Rust module on it, dhdr_web_init on the canvas (as an OffscreenCanvas), then the
// replay module with dhd_render = the Rust module's raw exports (wasm to wasm) and
// env.web_gpu_await = Suspending(() => R.dhdr_web_pending()). The last field is shown through
// web_present. The shaders are the ones compiled into the module.
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { homedir } from 'node:os';
import { resolve, dirname } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import { runPage } from '../web/cdp.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = resolve(process.env.DHD_DEVCACHES || resolve(homedir(), 'DevCaches'), 'wasi-sdk/wasi-sdk-34.0-x86_64-windows');
const args = process.argv.slice(2);
const webAt = args.indexOf('--web');
const WEB = resolve(webAt >= 0 ? args.splice(webAt, 2)[1] : ROOT);
const [capture, prefix] = args;
const unknown = args.find((a) => a.startsWith('--'));
if (unknown || !capture || !prefix || args.length !== 2) {
  if (unknown) console.error(`web_gs_replay.mjs: unknown option ${unknown}`);
  console.error('usage: web_gs_replay.mjs CAPTURE.gs PREFIX [--web TREE]'); process.exit(2);
}

const OUT = resolve(ROOT, 'build/web-gs-replay');
mkdirSync(OUT, { recursive: true });
// An earlier run's outputs go first, so a failed run leaves no hashes to compare (gate G0).
for (const old of [`${prefix}-web-vram.txt`, `${prefix}-web.png`]) rmSync(old, { force: true });
// GS MMIO (0x12000000, replay_display) has to be inside the memory.
// The memory is imported (the page makes it, RUST_MEMORY_PAGES, and the Rust module shares it).
// The declared initial size 0x12000000 is wasi-libc's __heap_end, so the first heap ends below the
// MMIO page; malloc grows with memory.grow, above the Rust window (0x60000000-0x69000000).
const memoryFlags = ['-Wl,--import-memory', '-Wl,--initial-memory=301989888', '-Wl,--max-memory=2147483648'];
const build = spawnSync(`${WASI}/bin/clang.exe`, ['-O2', '-msimd128', '-fno-strict-aliasing', '-fwrapv',
  '-ffp-contract=off', '-mexec-model=reactor', '-I' + resolve(ROOT, 'port/gs'),
  '-I' + resolve(WEB, 'port/render/include'),
  ...memoryFlags, '-Wl,-z,stack-size=1048576', '-Wl,--allow-undefined',
  resolve(ROOT, 'port/gs/replay_web.c'), resolve(ROOT, 'port/gs/gs.c'), resolve(ROOT, 'port/gs/memory.c'),
  resolve(WEB, 'port/web/gpu_rust_web.c'), '-o', resolve(OUT, 'replay.wasm')],
  { encoding: 'utf8', env: { ...process.env, TEMP: OUT, TMP: OUT } });
if (build.status) { console.error(build.stdout + build.stderr); process.exit(1); }
// The renderer module, in the web tree's build/web/renderer (its build output only on failure).
const RENDERER_DIR = resolve(WEB, 'build/web/renderer');
{
  const r = spawnSync(process.env.PYTHON || 'python', [resolve(WEB, 'port/web/build_renderer.py')],
    { cwd: WEB, encoding: 'utf8' });
  if (r.status) { console.error(r.stdout + r.stderr); console.error('web_gs_replay: build_renderer.py failed'); process.exit(1); }
}

// The Rust page: worker.js's init order (plan 3.3), on the page's own thread.
const RUST_MEMORY_PAGES = 0x69000000 / 65536;   // the Rust module's declared initial memory
const rustPage = `<!doctype html><canvas id=c width=640 height=448></canvas><script type=module>
const log = [];
for (const k of ['log', 'info', 'warn', 'error']) {
  const f = console[k].bind(console);
  console[k] = (...a) => { log.push(a.map(String).join(' ')); f(...a); };
}
try {
  if (typeof WebAssembly.Suspending !== 'function') throw Error('no JSPI');
  const memory = new WebAssembly.Memory({ initial: ${RUST_MEMORY_PAGES}, maximum: 32768 });
  const R = await import('./dhd_render.js');
  const renderModule = await WebAssembly.compileStreaming(fetch('dhd_render_bg.wasm'));
  const rx = await R.default({ module_or_path: renderModule, memory });
  if (rx.memory !== memory) throw Error('the renderer does not use the page memory');
  const canvas = document.getElementById('c').transferControlToOffscreen();
  if (!await R.dhdr_web_init(canvas, 7)) throw Error('dhdr_web_init failed: ' + log.join(' | '));
  const cstr = p => { const b = new Uint8Array(memory.buffer, p); return new TextDecoder().decode(b.slice(0, b.indexOf(0))); };
  // Anything else the module imports (WASI, unused host calls) is a stub that records its name.
  const stub = module => new Proxy({}, { get: (_, name) => (...a) => { log.push(module + '.' + String(name)); return 0; } });
  const replayModule = await WebAssembly.compileStreaming(fetch('replay.wasm'));
  const render = {};
  for (const i of WebAssembly.Module.imports(replayModule)) if (i.module === 'dhd_render') {
    if (typeof rx[i.name] !== 'function') throw Error('the renderer has no export ' + i.name);
    render[i.name] = rx[i.name];
  }
  const own = { memory, web_gpu_await: new WebAssembly.Suspending(() => R.dhdr_web_pending()) };
  const env = new Proxy(own, { get: (t, name) => name in t ? t[name] : stub('env')[name] });
  const instance = await WebAssembly.instantiate(replayModule, { env, dhd_render: render, wasi_snapshot_preview1: stub('wasi') });
  const x = instance.exports;
  x._initialize?.();
  const data = new Uint8Array(await (await fetch('capture.gs')).arrayBuffer());
  const at = x.replay_alloc(data.length), hashes = x.replay_alloc(8 * 4096);
  new Uint8Array(memory.buffer).set(data, at);
  const t = performance.now();
  const fields = await WebAssembly.promising(x.replay_web)(at, data.length, hashes, 4096);
  const ms = performance.now() - t;
  const h = new BigUint64Array(memory.buffer, hashes, Math.max(fields, 0));
  const size = await WebAssembly.promising(x.replay_display)();
  const presented = x.web_present(canvas.width, canvas.height);
  await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
  window.result = { fields, ms, size, presented, adapter: cstr(rx.dhdr_adapter_name()) || 'WebGPU', version: cstr(rx.dhdr_version()),
    stats: { submits: x.web_gpu_stats(3), reads: x.web_gpu_stats(2) }, log: log.slice(0, 20),
    hashes: [...h].map((v, i) => i + ' ' + v.toString(16).toUpperCase().padStart(16, '0')) };
} catch (e) { window.result = { error: String(e && e.stack || e), log: log.slice(0, 20) }; }
</script>`;
const files = {
  '/': [rustPage, 'text/html'],
  '/dhd_render.js': [resolve(RENDERER_DIR, 'dhd_render.js'), 'text/javascript'],
  '/dhd_render_bg.wasm': [resolve(RENDERER_DIR, 'dhd_render_bg.wasm'), 'application/wasm'],
  '/replay.wasm': [resolve(OUT, 'replay.wasm'), 'application/wasm'],
  '/capture.gs': [resolve(capture), 'application/octet-stream'] };
await runPage((req, res) => {
  const f = files[req.url];
  if (!f) { res.statusCode = 404; res.end(); return; }
  res.setHeader('content-type', f[1]);
  res.end(f[0] === rustPage ? f[0] : readFileSync(f[0]));
}, async ({ url, send: call, evaluate }) => {
  let status = 1;
  await call('Page.navigate', { url });
  let v;
  for (let i = 0; i < 6000 && !v; i++) {
    await sleep(100);
    v = await evaluate('window.result');
  }
  if (!v || v.error || v.fields < 0) console.log('failed:', JSON.stringify(v || 'no result'));
  else {
    writeFileSync(`${prefix}-web-vram.txt`, v.hashes.map(l => l + '\n').join(''));
    const shot = await call('Page.captureScreenshot', { format: 'png' });
    if (shot.result?.data) writeFileSync(`${prefix}-web.png`, Buffer.from(shot.result.data, 'base64'));
    console.log(`${v.adapter}: replayed ${v.fields} fields in ${v.ms.toFixed(1)} ms, picture ${v.size >>> 16}x${v.size & 0xffff}, ` +
      `${v.stats.submits} submits, ${v.stats.reads} reads; ${prefix}-web-vram.txt`);
    console.log(`renderer ${v.version}; last field ${v.presented === 1 ? 'presented' : 'not presented (' + v.presented + ')'}`);
    if (v.log.length) console.log('log:', v.log.join(' | '));
    status = 0;
  }
  return status;
});
