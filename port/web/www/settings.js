// PC settings in the browser: the bytes of a dhd.ini kept in this browser's IndexedDB, under the name
// DHD_SETTINGS gives ("dhd.ini" for players; "none" or "0": no settings read or saved, the tools'
// rule, finding 056). What they mean (the keys, the variables they set, the defaults, the
// environment's precedence, how a save rewrites one setting's line and keeps every other byte) is
// dhd_settings (port/render/settings), the crate the game and the launcher use: its wasm module,
// build/dhd_settings.wasm (its own memory, apart from the game's; finding 102), loaded on the page
// and in the game worker. This file keeps the storage and what the browser can't offer.
// The worker reads the text before the game starts and sets the DHD_* variables it stands for, as
// host_settings_load does natively (a variable already set wins: the page's ?env.NAME and the
// browser's own fixed ones); each host_setting_save stores the crate's new text. The disc is never
// stored: it stays the player's own file, picked again at each start.
const DB = 'downhill-revived-settings', STORE = 'ini';

// The browser's OPTIONS > PC SETTINGS (pc_settings.c's DHD_PC_SETTINGS_* variables):
// - ROWS, the rows shown: Resolution (DHD_SCALE: 1x to DHD_SCALE_MAX, the reactor's NATIVE_SCALE_MAX,
//   or Window, the canvas's match), Scaling, Widescreen, Font, Frame Rate and FPS & Ping. Display, window size,
//   picture shape (the canvas has one shape: 16:9 with widescreen, else 4:3, main.js), texture
//   packs, skip movies (the browser never plays them) and vibration (no rumble here) are left out.
// - LIVE, the rows that apply at once here besides the core's live ones: Resolution (the reactor's
//   native_rescale, through web_env_changed). Scaling is live everywhere.
// - RESTART, the footer of the others (Widescreen, Font, Frame Rate): the page loaded again.
// - FPS_MAX: the browser's Frame Rate list and Monitor stop at 144 (the core's own cap is 240).
export const BROWSER_PC_SETTINGS = {
  DHD_PC_SETTINGS_ROWS: 'DHD_SCALE,DHD_PRESENT_FILTER,DHD_WIDESCREEN,DHD_FONT,DHD_FPS,DHD_PERF_COUNTER',
  DHD_PC_SETTINGS_LIVE: 'DHD_SCALE',
  DHD_PC_SETTINGS_RESTART: '* Applies when the page is loaded again',
  DHD_SCALE_MAX: '8',
  DHD_FPS_MAX: '144',
};
// The variables neither a dhd.ini line nor a default sets here, since the browser has no such thing:
// the canvas is the window (no display mode, window size or monitor: widescreen.c's shape stays
// the page's) and has one shape (DHD_ASPECT: 16:9 with widescreen, else 4:3, main.js); the page's
// frames pace it (no V-Sync switch); no rumble, texture packs, Discord or updater.
export const BROWSER_WITHOUT = new Set(['DHD_WINDOW', 'DHD_WINDOW_WIDTH', 'DHD_WINDOW_HEIGHT', 'DHD_MONITOR', 'DHD_ASPECT',
  'DHD_VSYNC', 'DHD_VIBRATION', 'DHD_TEXTURES', 'DHD_TEXTURE_PACK', 'DHD_TEXTURE_CACHE_MB', 'DHD_DISCORD', 'DHD_UPDATE_URL']);
// The variables the reactor follows while the game runs (gpu_rust_web.c's web_env_changed).
export const RENDERER_LIVE = new Set(['DHD_SCALE', 'DHD_PRESENT_FILTER', 'DHD_ASPECT', 'DHD_PERF_COUNTER']);

// ---- the crate (port/render/settings/src/web.rs: arguments in buffers it allocates, each result
// in its memory until the next call)
let loading = null;
const crate = () => loading ??= WebAssembly.instantiateStreaming(fetch(new URL('build/dhd_settings.wasm', import.meta.url)))
  .then(r => r.instance.exports);
function call(x, fn, ...args) {
  const held = [], flat = [];
  for (const a of args) {
    if (typeof a === 'number') { flat.push(a); continue; }
    const b = typeof a === 'string' ? new TextEncoder().encode(a) : a, p = x.dhd_settings_alloc(b.length);
    new Uint8Array(x.memory.buffer, p, b.length).set(b); held.push([p, b.length]); flat.push(p, b.length);
  }
  try {
    const ok = x[fn](...flat);
    return [ok, new Uint8Array(x.memory.buffer, x.dhd_settings_out(), x.dhd_settings_out_len()).slice()];
  } finally { for (const [p, n] of held) x.dhd_settings_free(p, n); }
}
const utf8 = b => new TextDecoder().decode(b);

// ---- storage
function open() {
  return new Promise((resolve, reject) => {
    const r = indexedDB.open(DB, 1);
    r.onupgradeneeded = () => r.result.createObjectStore(STORE);
    r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error);
  });
}
const request = r => new Promise((resolve, reject) => { r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error); });
// The stored bytes, or null when there are none (texts stored as strings before finding 102: UTF-8).
async function stored(db, name) {
  const v = await request(db.transaction(STORE).objectStore(STORE).get(name));
  return v === undefined ? null : typeof v === 'string' ? new TextEncoder().encode(v) : new Uint8Array(v);
}
function put(db, name, bytes) {
  const t = db.transaction(STORE, 'readwrite'); t.objectStore(STORE).put(bytes, name);
  return new Promise((resolve, reject) => { t.oncomplete = resolve; t.onerror = () => reject(t.error); });
}

// DHD_SETTINGS's name: whether it names settings at all (none, 0: no; an empty name names no entry).
export async function enabled(name) { return !!name && !call(await crate(), 'dhd_settings_disabled', name)[0]; }

// What the game's start makes of the stored settings: { on, text, vars, unknown, x }. vars: the DHD_*
// variables (name -> value) not in env already, without BROWSER_WITHOUT's; text: the stored bytes or
// null. With settings on the crate's player defaults apply, stored text or not (the launcher's
// dhd.ini, which players always have). Also for the page, which shapes the canvas before Start.
export async function startSettings(env, log = () => {}) {
  const name = env.DHD_SETTINGS ?? 'dhd.ini', x = await crate(), on = await enabled(name);
  let text = null, db = null;
  if (on) try { db = await open(); text = await stored(db, name); } catch (e) { log(`settings: kept for this session only (${e})`); }
  const set = Object.keys(env).filter(k => env[k] !== undefined).join('\n');
  const [, out] = call(x, 'dhd_settings_environment', text ?? new Uint8Array(), on ? 1 : 0, set);
  const { vars, unknown } = JSON.parse(utf8(out));
  return { name, on, db, text, unknown, x, vars: Object.fromEntries(vars.filter(([k]) => !BROWSER_WITHOUT.has(k))) };
}

// The worker's settings: name's variables into env (the ones not set yet), then serves the game.
export async function createSettings(env, log) {
  const start = await startSettings(env, log), { name, on, db, x, unknown, vars } = start;
  let text = start.text;
  for (const [k, v] of Object.entries(vars)) env[k] ??= v;
  for (const key of unknown) log(`settings: unknown key ${key}`);
  if (on) log(`settings: ${name} in this browser${text ? '' : ' (none stored yet: the defaults)'}`);
  let writing = Promise.resolve();
  return {
    // host_setting_save: nonzero when kept (the write finishes while the game goes on).
    save(key, value) {
      if (!on) return 0;
      if (/^(iso|DHD_ISO)$/i.test(key)) return 1;  // never the disc
      const [ok, out] = call(x, 'dhd_settings_save', text ?? new Uint8Array(), text ? 1 : 0, key, value);
      if (!ok) { log(`settings: ${key} not saved (${utf8(out)})`); return 0; }
      text = out;
      if (db) writing = writing.then(() => put(db, name, text)).catch(e => log(`settings: not saved (${e})`));
      return 1;
    },
    // host_setting_locked: the settings' variables set before dhd.ini (DHD_ENV_OVERRIDES, the crate's).
    locked: variable => +(env.DHD_ENV_OVERRIDES ?? '').includes(`,${variable},`),
    gameOptions: () => { if (!on || !text) return undefined;
      const [ok, out] = call(x, 'dhd_settings_written', text, 'game_options'); return ok ? utf8(out) : undefined; },
    // the stored text once the saves so far are written (checks)
    written: () => writing.then(() => text),
  };
}
