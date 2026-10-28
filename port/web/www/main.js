// The page: picks the ISO, hands the canvas to the game worker, and feeds it keys, gamepads, the
// mouse (or touch) and sound through SharedArrayBuffers (the worker never returns to its event loop mid-field, so
// messages would wait). Needs a cross-origin isolated page: serve.mjs sends COOP/COEP.
import { startSettings } from './settings.js';
const $ = id => document.getElementById(id);
const status = $('status'), log = $('log'), canvas = $('screen');
const params = new URLSearchParams(location.search);
const addLog = text => { log.textContent += text + '\n'; log.scrollTop = log.scrollHeight; };

if (!crossOriginIsolated) status.textContent = 'This page needs cross-origin isolation: start it with port/web/serve.mjs.';
const input = new SharedArrayBuffer(256 + 32 * 4 + 16 * 4);
// pads: players 1-4, 8 ints each (played by hand the game opens four pads on a multitap, pad.c).
const keys = new Uint8Array(input, 0, 256), pads = new Int32Array(input, 256, 32);
// The mouse: host_api.h's HostMouse (x, y as 65536ths of the picture, buttons held, seen, moves,
// presses of the left, right and middle buttons, wheel steps), then [12] riding (the worker's).
const mouse = new Int32Array(input, 384, 16);
const audio = new SharedArrayBuffer(8 + 48000 / 4 * 4);  // 0.25 s of 48 kHz stereo

// KeyboardEvent.code -> the Windows virtual-key code keyboard.c's table uses (US places).
function vk(code) {
  const named = { ArrowLeft: 0x25, ArrowUp: 0x26, ArrowRight: 0x27, ArrowDown: 0x28, Space: 0x20, Enter: 0x0d,
    NumpadEnter: 0x0d, Escape: 0x1b, Tab: 0x09, Backspace: 0x08 };
  if (named[code]) return named[code];
  const m = /^Key([A-Z])$/.exec(code); return m ? m[1].charCodeAt(0) : 0;
}
let running = false;
addEventListener('keydown', e => { const k = vk(e.code); if (!k || !running || e.altKey || e.ctrlKey || e.metaKey) return;
  Atomics.store(keys, k, 1); e.preventDefault(); });
addEventListener('keyup', e => { const k = vk(e.code); if (k) Atomics.store(keys, k, 0); });
addEventListener('blur', () => keys.fill(0));

// Standard-mapping gamepads -> PS2 buttons (libpad bits) and sticks (0..255, 128 centred).
const GP = [16384, 8192, 32768, 4096, 1024, 2048, 256, 512, 1, 8, 2, 4, 16, 64, 128, 32];
function pollPads() {
  const list = [...(navigator.getGamepads?.() || [])].filter(p => p && p.mapping === 'standard').slice(0, 4);
  for (let port = 0; port < 4; port++) {
    const p = list[port], o = port * 8;
    if (!p) { Atomics.store(pads, o, 0); continue; }
    let b = 0; p.buttons.forEach((x, i) => { if (x.pressed && GP[i]) b |= GP[i]; });
    const axis = v => Math.max(0, Math.min(255, Math.round(128 + (Math.abs(v) < 0.12 ? 0 : v) * 127.5)));
    Atomics.store(pads, o + 1, b);
    Atomics.store(pads, o + 2, axis(p.axes[2] ?? 0)); Atomics.store(pads, o + 3, axis(p.axes[3] ?? 0));
    Atomics.store(pads, o + 4, axis(p.axes[0] ?? 0)); Atomics.store(pads, o + 5, axis(p.axes[1] ?? 0));
    Atomics.store(pads, o, 1);
  }
  cursor();
  requestAnimationFrame(pollPads);
}

// The mouse and touch over the picture (the menus' pointer, finding 092): where it is as a
// fraction of the picture (the canvas keeps the picture's shape; fullscreen letterboxes it), and
// counters the worker reads once a field. The cursor hides after 3 idle seconds and while riding.
let lastMove = -1e9, wheelRest = 0;
function point(e) {
  const r = canvas.getBoundingClientRect(), shape = canvas.width / canvas.height;
  let w = r.width, h = r.height;
  if (w / h > shape) w = h * shape; else h = w / shape;
  Atomics.store(mouse, 0, Math.round((e.clientX - r.left - (r.width - w) / 2) / w * 65536));
  Atomics.store(mouse, 1, Math.round((e.clientY - r.top - (r.height - h) / 2) / h * 65536));
  Atomics.store(mouse, 3, 1); lastMove = performance.now(); canvas.style.cursor = '';
}
const button = e => [0, 2, 1][e.button];  // left, middle, right -> HostMouse's left, right, middle
canvas.style.touchAction = 'none';
canvas.addEventListener('pointermove', e => { point(e); Atomics.add(mouse, 4, 1); });
canvas.addEventListener('pointerdown', e => { const i = button(e); point(e); if (i === undefined) return;
  Atomics.or(mouse, 2, 1 << i); Atomics.add(mouse, 5 + i, 1); });
addEventListener('pointerup', e => { const i = button(e); if (i !== undefined) Atomics.and(mouse, 2, ~(1 << i)); });
canvas.addEventListener('contextmenu', e => e.preventDefault());
canvas.addEventListener('wheel', e => { point(e); e.preventDefault();
  wheelRest -= e.deltaMode ? e.deltaY : e.deltaY / 100;  // away from the player: up, positive
  const steps = Math.trunc(wheelRest); wheelRest -= steps; Atomics.add(mouse, 8, steps); }, { passive: false });
function cursor() {
  if (canvas.style.cursor !== 'none' && (performance.now() - lastMove > 3000 || Atomics.load(mouse, 12))) canvas.style.cursor = 'none';
}

// Fullscreen: a double click on the picture or the Fullscreen button (PC SETTINGS has no Display
// row in the browser).
const fullscreen = () => { if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
  else canvas.requestFullscreen?.().catch(e => addLog(`fullscreen: ${e.message}`)); };
canvas.ondblclick = fullscreen;
$('fullscreen').onclick = () => { fullscreen(); canvas.focus(); };

// The Controls and Log buttons under the picture open their panels; a click elsewhere closes them.
const panels = { controlsButton: $('controls'), logButton: $('logPanel') };
for (const [id, panel] of Object.entries(panels))
  $(id).onclick = e => { e.stopPropagation(); const open = panel.hidden;
    Object.values(panels).forEach(p => { p.hidden = true; }); panel.hidden = !open; };
Object.values(panels).forEach(p => p.addEventListener('pointerdown', e => e.stopPropagation()));
addEventListener('pointerdown', () => Object.values(panels).forEach(p => { p.hidden = true; }));

// The display's refresh rate (PC SETTINGS > Frame Rate > Monitor: the worker's host_display_hz):
// the median of 30 frame intervals; 60 when the page gets no frames within a second (hidden).
function refreshHz() {
  return new Promise(done => {
    const gaps = []; let last;
    const timeout = setTimeout(() => done(60), 1000);
    const frame = now => {
      if (last !== undefined) gaps.push(now - last);
      last = now;
      if (gaps.length < 30) { requestAnimationFrame(frame); return; }
      clearTimeout(timeout); gaps.sort((a, b) => a - b);
      done(Math.round(1000 / gaps[gaps.length >> 1]) || 60);
    };
    requestAnimationFrame(frame);
  });
}

// The canvas's shape (PC SETTINGS > Widescreen, from the stored settings): 16:9 with widescreen
// (the picture's "wide", host_monitor_aspect's 16:9), else the PS2's 4:3: DHD_WIDESCREEN as the game
// will have it (?env., else the stored settings', else the player's default; settings.js).
// Also at page load, so the frame around the picture has its shape before Start.
async function shape(env) {
  const start = await startSettings(env).catch(() => null);
  start?.db?.close();
  const wide = (parseInt(env.DHD_WIDESCREEN ?? start?.vars.DHD_WIDESCREEN ?? '0') || 0) !== 0;
  document.documentElement.style.setProperty('--ar', wide ? '16 / 9' : '4 / 3');  // index.html's stage
  return wide;
}

async function start(source) {
  $('start').disabled = true; $('iso').disabled = true; running = true;
  document.body.classList.add('running'); status.textContent = 'starting';
  const hz = refreshHz();
  try {
    const context = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
    await context.audioWorklet.addModule('audio.js');
    new AudioWorkletNode(context, 'dhd-ring', { outputChannelCount: [2], processorOptions: { ring: audio } }).connect(context.destination);
    context.resume();
  } catch (e) { addLog(`no sound: ${e}`); }
  // After the sound, which needs the Start click; read again at each start, as the game reads it.
  if (await shape(source.env ?? {})) { canvas.width = 1708; canvas.height = 960; }
  const worker = new Worker('worker.js', { type: 'module' });
  const offscreen = canvas.transferControlToOffscreen();
  worker.onmessage = e => { const m = e.data;
    if (m.type === 'log') addLog(m.text);
    else if (m.type === 'error') { addLog('ERROR ' + m.text); status.textContent = 'stopped: ' + m.text.split('\n')[0];
      $('logPanel').hidden = false; }
    else if (m.type === 'running') status.textContent = 'running';
    else if (m.type === 'exit') status.textContent = `game exited (${m.status})`;
    else if (m.type === 'status') { status.textContent = `field ${m.field}  ${m.fps.toFixed(1)} fields/s  picture ${m.size}  draws ${m.draws}  batches ${m.dispatches}  readbacks ${m.reads}  sound ${m.sound} s${m.riding ? '  riding' : ''}`;
      document.title = `DHD ${m.fps.toFixed(1)} fps (field ${m.field})`; window.dhdStatus = m; (window.dhdHistory ??= []).push([m.field, +m.fps.toFixed(2), m.draws]); }
  };
  worker.onerror = e => addLog('worker error: ' + e.message);
  worker.postMessage({ type: 'start', ...source, canvas: offscreen, input, audio, displayHz: await hz }, [offscreen]);
  canvas.focus();
  pollPads();
}

$('iso').onchange = () => { const file = $('iso').files[0];
  $('start').disabled = !file; $('disc').textContent = file ? `${file.name} (${(file.size / 2 ** 30).toFixed(2)} GB)` : ''; };
// A disc dropped anywhere on the page before Start.
addEventListener('dragover', e => { if (running) return; e.preventDefault(); document.body.classList.add('dragging'); });
addEventListener('dragleave', e => { if (!e.relatedTarget) document.body.classList.remove('dragging'); });
addEventListener('drop', e => { e.preventDefault(); document.body.classList.remove('dragging');
  if (running || !e.dataTransfer?.files.length) return;
  $('iso').files = e.dataTransfer.files; $('iso').onchange(); });
$('start').onclick = () => start({ file: $('iso').files[0], env });
// Test hook: serve.mjs --iso <path> serves that file as /test-iso to localhost only, and
// ?iso=test starts with it at once (headless checks; the ISO never leaves the machine).
// ?env.NAME=value sets a DHD_* variable for the game (tests: DHD_NAV, DHD_RACE_PAD scripts).
const env = Object.fromEntries([...params].filter(([k]) => k.startsWith('env.')).map(([k, v]) => [k.slice(4), v]));
// Tests read and save no settings unless they name them (?env.DHD_SETTINGS=<name>), as the tools'
// DHD_SETTINGS=none natively.
if (params.get('iso') === 'test') start({ isoUrl: '/test-iso', env: { DHD_SETTINGS: 'none', ...env } });
else shape(env);
