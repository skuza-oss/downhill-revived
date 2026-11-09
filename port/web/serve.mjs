// Serves the browser build on localhost with the cross-origin isolation headers the page needs
// (SharedArrayBuffer): node port/web/serve.mjs [--port 8177] [--iso <path>]
// The page is port/web/www, the modules build/web (python port/web/build.py ...; the Rust renderer
// under build/web/renderer). --iso serves that file as /test-iso for headless checks (?iso=test),
// to this machine only; players pick their own ISO in the page instead and it is read where it is.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2), option = (name, fallback) => { const i = args.indexOf(name); return i < 0 ? fallback : args[i + 1]; };
const port = Number(option('--port', 8177)), iso = option('--iso', null);
const routes = [['/build/', path.join(root, 'build/web')], ['/', path.join(root, 'port/web/www')]];
// /build/renderer/: the Rust renderer's two files (port/web/build_renderer.py).
// gqueue.wasm, vu.wasm: the threaded build's core side of the graphics queue and its VU worker (build.py --threads).
// dhd_settings.wasm: dhd.ini's rules for settings.js, on the page and in the game worker (build.py).
const allowed = { '/build/': /^(dhd_core\.wasm|graphics\.wasm|gqueue\.wasm|vu\.wasm|dhd_settings\.wasm|imports\.json|renderer\/dhd_render\.js|renderer\/dhd_render_bg\.wasm|star\/dhd_star\.js|star\/dhd_star_bg\.wasm)$/, '/': /^[\w.-]+$/ };
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
  '.json': 'application/json' };
const server = http.createServer((req, res) => {
  const headers = { 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'same-origin', 'Cache-Control': 'no-store' };
  const url = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  let file = null;
  if (url === '/test-iso' && iso) file = iso;
  else for (const [prefix, dir] of routes) if (url.startsWith(prefix)) {
    const name = url.slice(prefix.length) || 'index.html';
    if (allowed[prefix].test(name)) file = path.join(dir, name);
    break;
  }
  let stat;
  try { stat = file && fs.statSync(file); } catch { stat = null; }
  if (!stat?.isFile()) { res.writeHead(404, headers); res.end('not found'); return; }
  headers['Content-Type'] = types[path.extname(file)] || 'application/octet-stream';
  headers['Accept-Ranges'] = 'bytes';
  const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
  if (range) {
    const start = Number(range[1]), end = Math.min(range[2] ? Number(range[2]) : stat.size - 1, stat.size - 1);
    headers['Content-Range'] = `bytes ${start}-${end}/${stat.size}`; headers['Content-Length'] = end - start + 1;
    res.writeHead(206, headers);
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file, { start, end }).pipe(res);
  } else {
    headers['Content-Length'] = stat.size; res.writeHead(200, headers);
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file).pipe(res);
  }
});
server.listen(port, '127.0.0.1', () => console.log(`Downhill Revived (browser): http://localhost:${port}/`));
