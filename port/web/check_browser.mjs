// One headless check of the browser build: starts serve.mjs with --iso, opens ?iso=test in
// headless Chrome (no window), waits until the game reaches --field (or --seconds pass), saves a
// screenshot through the DevTools protocol and prints the page's last status.
//   node port/web/check_browser.mjs --iso <path> [--field 600] [--seconds 600] [--out build/web/browser.png]
// --user-data <dir>: Chrome's profile (default build/web/chrome-profile; it keeps the saves and
// settings between runs). --then-query Q [--then-field F]: once --field is reached, load the page
// again with ?iso=test&Q (a reload: what the first load saved must still be there) and run to F.
// --eval EXPR: an expression evaluated in the page at the end, its value printed.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { startChrome, connect } from './cdp.mjs';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2), option = (n, f) => { const i = args.indexOf(n); return i < 0 ? f : args[i + 1]; };
const iso = option('--iso', process.env.DHD_ISO || process.env.DHD_TEST_ISO || '');
const field = Number(option('--field', 600)), seconds = Number(option('--seconds', 600));
const out = path.resolve(option('--out', path.join(root, 'build/web/browser.png'))), port = Number(option('--port', 8193));
const query = option('--query', '');
// --profile F S: a CPU profile of the game worker for S seconds from field F (build/web/browser.cpuprofile).
const profileAt = Number(option('--profile', 0)), profileFor = Number(args[args.indexOf('--profile') + 2] || 20);
const chrome = option('--chrome', 'C:/Program Files/Google/Chrome/Application/chrome.exe');
const profile = path.resolve(option('--user-data', path.join(root, 'build/web/chrome-profile')));
const thenQuery = option('--then-query', null), thenField = Number(option('--then-field', field));
const server = spawn(process.execPath, [path.join(root, 'port/web/serve.mjs'), '--port', String(port), '--iso', iso], { stdio: ['ignore', 'pipe', 'inherit'] });
await new Promise(r => server.stdout.once('data', r));
const { browser, wsUrl } = await startChrome({ chrome, profile,
  flags: ['--enable-unsafe-webgpu', '--autoplay-policy=no-user-gesture-required', '--window-size=1400,1150'] });
console.error(`server pid ${server.pid}, chrome pid ${browser.pid}`);
const { ws, send, evaluate: evaluateIn, events } = await connect(wsUrl);
const { result: { targetId } } = await send('Target.createTarget', { url: 'about:blank' });
const { result: { sessionId } } = await send('Target.attachToTarget', { targetId, flatten: true });
await send('Page.enable', {}, sessionId); await send('Runtime.enable', {}, sessionId);
let workerSession = null;
if (profileAt) {
  ws.addEventListener('message', e => { const m = JSON.parse(e.data);
    if (m.method === 'Target.attachedToTarget' && m.params.targetInfo.type === 'worker') workerSession = m.params.sessionId; });
  await send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sessionId);
}
let profiled = false;
const t0 = Date.now(); let last = null;
const evaluate = expr => evaluateIn(expr, { sessionId });
for (const [q, until] of [[query, field], ...(thenQuery !== null ? [[thenQuery, thenField]] : [])]) {
if (last) { const s = JSON.parse(last); console.log(JSON.stringify({ stage: 'first', seconds: (Date.now() - t0) / 1000, status: s.text, last: s.s, display: s.display })); console.log(s.log); }
await send('Page.navigate', { url: `http://localhost:${port}/?iso=test${q ? '&' + q : ''}` }, sessionId);
const tStage = Date.now();
while ((Date.now() - tStage) / 1000 < seconds) {
  await new Promise(r => setTimeout(r, 2000));
  last = await evaluate('JSON.stringify({h: (window.dhdHistory||[]).slice(-400), s: window.dhdStatus, display: window.dhdDisplay, text: document.getElementById("status").textContent, log: document.getElementById("log").textContent.slice(-1500)})');
  const s = JSON.parse(last || '{}');
  if (/stopped|exited/.test(s.text || '') || (s.s && s.s.field >= until)) break;
  if (profileAt && !profiled && workerSession && s.s && s.s.field >= profileAt) {
    profiled = true;
    await send('Profiler.enable', {}, workerSession); await send('Profiler.setSamplingInterval', { interval: 500 }, workerSession);
    await send('Profiler.start', {}, workerSession);
    await new Promise(r => setTimeout(r, profileFor * 1000));
    const prof = await send('Profiler.stop', {}, workerSession);
    fs.writeFileSync(path.join(root, 'build/web/browser.cpuprofile'), JSON.stringify(prof.result.profile));
    console.error('profile written at field', s.s.field);
  }
}
}
// --eval EXPR: printed at the end (awaited), e.g. the page's stored settings.
const extra = option('--eval', null);
if (extra) console.log('eval', JSON.stringify(await evaluateIn(extra, { sessionId, awaitPromise: true })));
const shot = await send('Page.captureScreenshot', { format: 'png' }, sessionId);
fs.writeFileSync(out, Buffer.from(shot.result.data, 'base64'));
const s = JSON.parse(last || '{}');
console.log(JSON.stringify({ seconds: (Date.now() - t0) / 1000, status: s.text, last: s.s, display: s.display, screenshot: out }, null, 1));
console.log('history', JSON.stringify(s.h));
console.log(s.log);
for (const e of events) if (e.method === 'Runtime.exceptionThrown') console.log('exception', JSON.stringify(e.params.exceptionDetails).slice(0, 800));
ws.close(); browser.kill(); server.kill();
