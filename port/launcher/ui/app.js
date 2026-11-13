// Downhill Revived launcher front end, one page for the desktop, Android and iOS (findings 093,
// 225). Files, network and processes are the native side's (bridge.js: the desktop's Tauri
// commands, a phone's bridge); this file draws the state it reports. What a platform offers comes
// from the snapshot's caps (port/launcher/BRIDGE.md); the words from the string table (i18n.js).
'use strict';

const call = bridge.call;
const $ = (id) => document.getElementById(id);
let caps = new Set();
const has = (cap) => caps.has(cap);

// The settings pages: the rows of OPTIONS > REVIVED SETTINGS plus the dhd.ini lines players change. Each
// setting's choices (value and label) and default are the game's (dhd_settings, port/render/settings,
// through settings_schema); a switch's are off then on. A row's `needs` is the capability it takes
// (pc: the PC settings, touch: a touch device's); none, every platform. A row with `when` is
// greyed out, with `offHint`, while that doesn't hold. A hint may be a function (drawn with the
// schema); a select's `option` gives a choice's words (value, label) and `allow` the choices offered.
// Labels and hints are string table keys.
const TABS = [
  ['graphics', [
    { key: 'scale', label: 'row.scale', hint: () => scaleHint(), type: 'select', option: (v, t) => scaleOption(v, t), allow: (v) => scaleOffered(v) },
    { key: 'DHD_ASPECT', label: 'row.aspect', hint: () => (has('touch-settings') ? 'row.aspect.hintTouch' : 'row.aspect.hint'), type: 'seg' },
    { key: 'DHD_PRESENT_FILTER', label: 'row.scaling', hint: 'row.scaling.hint', type: 'seg', needs: 'pc-settings' },
    { key: 'widescreen', label: 'row.widescreen', hint: 'row.widescreen.hint', type: 'switch', needs: 'touch-settings' },
    { key: 'fps', label: 'row.fps', hint: 'row.fps.hint', type: 'select' },
    { key: 'perf_counter', label: 'row.perfCounter', hint: 'row.perfCounter.hint', type: 'switch' },
    { key: 'vsync', label: 'row.vsync', hint: 'row.vsync.hint', type: 'switch', needs: 'pc-settings' },
    { key: 'textures', label: 'row.textures', hint: 'row.textures.hint', type: 'switch', needs: 'pc-settings',
      when: () => state.texturePack, offHint: 'row.textures.none' },
    { key: 'font', label: 'row.font', type: 'seg', needs: 'pc-settings' },
    { key: 'native_vu', label: 'row.nativeVu', hint: 'row.nativeVu.hint', type: 'switch' },
    { key: 'shadows', label: 'row.performance', hint: () => perfHint(), type: 'performance', needs: 'touch-settings' },
  ]],
  ['display', [
    { key: 'window', label: 'row.window', type: 'seg', needs: 'pc-settings' },
    { key: 'size', label: 'row.size', hint: 'row.size.hint', type: 'select', needs: 'pc-settings',
      when: () => get('window') === 'windowed', offHint: 'row.size.off' },
    { key: 'widescreen', label: 'row.widescreen', hint: 'row.widescreen.hint', type: 'switch', needs: 'pc-settings' },
  ]],
  ['touch', [
    { key: 'touch_preset', label: 'row.touchPreset', hint: 'row.touchPreset.hint', type: 'seg', needs: 'touch-settings' },
    { key: 'touch_hand', label: 'row.touchHand', hint: 'row.touchHand.hint', type: 'seg', needs: 'touch-settings' },
    { key: 'touch_steer', label: 'row.touchSteer', hint: 'row.touchSteer.hint', type: 'seg', needs: 'touch-settings' },
    { key: 'touch_size', label: 'row.touchSize', hint: 'row.touchSize.hint', type: 'select', needs: 'touch-settings' },
    { label: 'row.touchEdit', hint: 'row.touchEdit.hint', type: 'action', button: 'row.touchEdit.button', icon: 'i-pencil',
      run: () => play({ editTouchLayout: true }), needs: 'touch-editor' },
  ]],
  ['game', [
    { key: 'iso', label: 'row.disc', type: 'disc' },
    { key: 'skip_movies', label: 'row.skipMovies', type: 'switch' },
    { key: 'unlock_all', label: 'row.unlockAll', hint: 'row.unlockAll.hint', type: 'switch' },
    { key: 'auto_update', label: 'row.updates', hint: () => (has('install-update') ? 'row.updates.hint' : 'row.updates.hintPhone'), type: 'seg',
      allow: (v) => has('install-update') || v !== 'on' },
    { key: 'launcher', label: 'row.launcher', hint: 'row.launcher.hint', type: 'switch', needs: 'direct-start' },
    { key: 'language', label: 'row.language', hint: 'row.language.hint', type: 'language', when: () => LANGUAGES.length > 1, hideOff: true },
  ]],
  ['controls', [
    { key: 'stick_dead_zone', label: 'row.deadZone', hint: 'row.deadZone.hint', type: 'select', needs: 'bindings' },
    { type: 'sticks', needs: 'bindings' },
    { key: 'prompts', label: 'row.prompts', hint: 'row.prompts.hint', type: 'seg', needs: 'bindings' },
    { key: 'vibration', label: 'row.vibration', type: 'switch', needs: 'bindings' },
    { type: 'bindings', needs: 'bindings' },
  ]],
  ['online', [
    { key: 'names', label: 'row.names', hint: 'row.names.hint', type: 'seg', needs: 'pc-settings' },
    { key: 'name', label: 'row.name', hint: () => t('row.name.hint', { rule: state.schema.name.rule.hint }), type: 'name' },
    { key: 'server', label: 'row.server', hint: 'row.server.hint', type: 'text', placeholder: 'wss://dhd.skuza.app', needs: 'pc-settings' },
    { key: 'discord', label: 'row.discord', hint: 'row.discord.hint', type: 'switch', needs: 'pc-settings' },
  ]],
];
// The tabs this platform shows: each with the rows its capabilities allow, empty ones left out.
let tabs = [];
const tabIndex = (key) => tabs.findIndex(([k]) => k === key);
const EFFECTS = ['shadows', 'lens_flare', 'particles'];

const state = {
  version: '', settings: {}, schema: {}, gameFound: true, running: false, tab: 0, update: null, check: 'idle', checkError: '',
  discProblem: null, texturePack: false,
  scale: null,        // the last run's resolution scale (dhd.log): { used, asked, limit, reason }; null: no run said yet
  scaleMax: 0,        // the most a platform offers (a phone's renderer's limit), 0: no such limit
  online: null,       // the disc's online check: { path, checking, done, total }, { path, usa } or { path, error }
  server: null,       // null: not asked yet; 'down'; or { races, riders, min_version }
  error: null,        // { text, fix: { label, icon, onclick } }: stays in the footer until dismissed
  confirmClose: false,
  disc: discIdle(),   // a phone's disc import (disc_flow.js)
  discSize: 0, freeSpace: 0,
  lastExit: null,     // the run before closed unexpectedly: { why }
  starting: false,    // PLAY pressed on a platform where the game takes the launcher's place
  licences: null,
};
let flashTimer = 0;

// A setting's value (the snapshot's: dhd.ini's, else the default), its choices as [value, label].
const get = (key) => state.settings[key] ?? '';
const choices = (key) => state.schema[key]?.choices ?? [];

// What the native side's error says: a coded one ({ code: a string table key, params }) in this
// language, else its own words.
function errorText(e) {
  if (e && typeof e === 'object' && e.code) return t(e.code, e.params || {});
  return String(e);
}

async function write(key, value) {
  await call('save_setting', { key, value });  // "size" is width and height (dhd_settings)
  state.settings[key] = value !== '' ? value : state.schema[key]?.default ?? '';
}

async function save(key, value, label) {
  try {
    await write(key, value);
    flash(t('settings.saved', { what: label }));
  } catch (e) {
    fail(errorText(e));
  }
}

const needsDisc = () => !get('iso') || !!state.discProblem;

// ---------- pages ----------

function show(page) {
  for (const p of ['play', 'settings', 'notes', 'help']) $(`page-${p}`).hidden = p !== page;
  document.querySelectorAll('.nav button').forEach((b) => b.classList.toggle('on', b.dataset.page === page));
  if (page === 'help') renderHelp();
}

function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k != null) e.append(k);
  return e;
}

function icon(id, cls = '') {
  const ns = 'http://www.w3.org/2000/svg';
  const s = document.createElementNS(ns, 'svg');
  s.setAttribute('class', `i ${cls}`.trim());
  const u = document.createElementNS(ns, 'use');
  u.setAttribute('href', `#${id}`);
  s.append(u);
  return s;
}

const gb = (bytes) => `${(bytes / 1073741824).toFixed(1)}\u00A0GB`;

// ---------- play page ----------

const baseName = (p) => p.split(/[\\/]/).pop();

// The notes' sections as { title, text }: each bold line and the first sentence of its first
// bullet; notes without bold lines give their first bullets as titles.
function newsItems(notes) {
  const plain = (s) => s.replace(/:?\s*<https?:[^>\s]+>/g, '').replace(/\*\*|`/g, '').replace(/\[([^\]]+)\]\([^)]+\)/g, '$1').trim();
  const items = [];
  for (const line of notes.split('\n')) {
    const title = line.match(/^\*\*([^*]+)\*\*$/)?.[1];
    const bullet = line.match(/^\s*[-*]\s+(.*)/)?.[1];
    if (title) items.push({ title: plain(title), text: '' });
    else if (bullet && items.length && !items.at(-1).text) items.at(-1).text = plain(bullet).split(/(?<=[.!?])\s/)[0];
  }
  if (items.length) return items;
  return notes.split('\n').map((l) => l.match(/^\s*[-*]\s+(.*)/)?.[1]).filter(Boolean).map((b) => ({ title: plain(b), text: '' }));
}

function released(iso) {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return '';
  const opts = { month: 'short', day: 'numeric' };
  if (d.getFullYear() !== new Date().getFullYear()) opts.year = 'numeric';
  return t('home.released', { date: d.toLocaleDateString(lang.code === 'en' ? 'en-US' : lang.code, opts) });
}

function renderHome() {
  const first = needsDisc();
  $('home-ready').hidden = first;
  $('home-first').hidden = !first;
  if (first) {
    $('first-title').textContent = t(state.discProblem ? 'home.first.again' : 'home.first.title');
    $('first-lead').replaceChildren(...tNodes(has('disc-import') ? 'home.first.leadImport' : 'home.first.lead'));
    const step = $('first-step');
    const hint = has('disc-import') ? t('home.first.step1HintImport', { free: gb(state.freeSpace) }) : t('home.first.step1Hint');
    step.replaceChildren(...withLink(state.discProblem || hint));
    step.classList.toggle('bad', !!state.discProblem);
  } else {
    const r = state.update?.release;
    const notes = r?.notes ? playerNotes(r.notes) : '';
    const items = notes ? newsItems(notes).slice(0, 4) : [];
    if (items.length) {
      $('home-title').replaceChildren(...tNodes('home.newIn', { version: r.version }));
      const date = released(r.published);
      $('home-meta').replaceChildren(...(date ? [date, el('span', { class: 'sep' }, '·')] : []),
        el('button', { class: 'link', onclick: () => show('notes') }, t('home.readNotes')));
      $('news').replaceChildren(...items.map((i) => el('li', {}, el('b', {}, i.title), i.text)));
    } else {
      $('home-title').textContent = t('home.ready');
      $('home-meta').replaceChildren(t('home.version', { version: state.version }));
    }
    $('news').hidden = !items.length;
  }
  const iso = get('iso');
  $('fact-disc').hidden = first || !iso;
  $('fact-disc').title = iso;
  $('fact-disc-name').textContent = baseName(iso);
  renderCrash();
  renderOnline();
  renderServer();
  renderPlayed();
}

// After a run that closed unexpectedly (a phone's record, finding 152): what happened and Share log.
function renderCrash() {
  const box = $('crash-note');
  const x = state.lastExit;
  box.hidden = !x;
  if (!x) return;
  box.replaceChildren(el('div', { class: 'note' }, icon('i-alert'), el('div', {},
    el('b', {}, t('crash.title')), el('div', {}, t('crash.body', { why: x.why })),
    el('div', { class: 'note-actions' },
      el('button', { class: 'btn', onclick: shareLog }, icon('i-share'), t('crash.share')),
      el('button', { class: 'btn text', onclick: () => { state.lastExit = null; renderCrash(); } }, t('crash.dismiss'))))));
}

// Time played in all (saves/playtime.txt, counted by the launcher while the game runs).
function renderPlayed() {
  const s = state.played || 0;
  $('fact-played').hidden = s < 60;
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  $('fact-played-time').textContent = h ? t('home.played.hours', { hours: h, minutes: m }) : t('home.played.minutes', { minutes: m });
}

// A disc problem's "More: https://..." (the other releases, finding 100) as a link.
function withLink(text) {
  const m = text.match(/^(.*?)\s*More: (https:\S+)$/s);
  if (!m) return [text];
  const open = (e) => { e.stopPropagation(); call('open_link', { url: m[2] }).catch((err) => fail(errorText(err))); };
  return [`${m[1]} `, el('button', { class: 'link inline', onclick: open }, t('home.otherReleases'))];
}

// What the online check says, for the Play page and the Online tab: nothing for the USA disc's files.
function onlineNote() {
  const o = state.online;
  if (!o || o.path !== get('iso') || needsDisc()) return null;
  if (o.checking) {
    if (!o.total) return null; // answered from iso-check.txt before any progress
    const pct = Math.min(100, Math.floor((o.done / o.total) * 100));
    return el('div', { class: 'note' }, el('span', { class: 'dot busy' }),
      el('div', {}, t('online.checking', { percent: pct }), el('div', { class: 'bar' }, el('i', { style: `width:${pct}%` }))));
  }
  if (o.error) return el('div', { class: 'note' }, icon('i-alert'), el('div', {}, o.error));
  if (o.usa === false) return el('div', { class: 'note' }, icon('i-alert'), el('div', {}, t('online.offlineOnly')));
  return null;
}

function renderOnline() {
  const note = onlineNote();
  $('online-note').hidden = !note;
  $('online-note').replaceChildren(...(note ? [note] : []));
  if (tabs[state.tab]?.[0] === 'online' && !$('page-settings').hidden) renderRows();
}

function renderServer() {
  const f = $('fact-server');
  const s = state.server;
  f.hidden = !s;
  if (!s) return;
  if (s === 'down') return f.replaceChildren(el('span', { class: 'dot' }), t('server.down'));
  if (newer(s.min_version, state.version))
    return f.replaceChildren(el('span', { class: 'dot warn' }), t('server.needsVersion', { version: s.min_version }));
  const open = s.races ? t('server.up', { races: tn('server.races', s.races), riders: tn('server.riders', s.riders) }) : t('server.upEmpty');
  f.replaceChildren(el('span', { class: 'dot ok' }), open);
}

function newer(a, b) {
  const p = (v) => (v || '').split('.').map(Number);
  const [x, y] = [p(a), p(b)];
  for (let i = 0; i < 3; i++) if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) > (y[i] || 0);
  return false;
}

// ---------- settings ----------

// Resolution's hint and choices with what the last run found (state.scale, src-tauri/src/scale.rs):
// the scales over this GPU's limit stay, marked, and the hint says what the chosen one runs at. A
// platform with a fixed limit (a phone's renderer: state.scaleMax) offers only the scales up to it.
function scaleOffered(value) {
  return !state.scaleMax || !(Number(value) > state.scaleMax);
}

function scaleHint() {
  const r = state.scale, value = get('scale'), asked = Number(value);
  const base = state.scaleMax ? 'row.scale.hintPhone' : 'row.scale.hint';
  if (!r) return base;
  const why = r.reason === 'memory' ? 'memory' : r.reason === 'texture' ? 'texture' : 'gpu';
  if (asked > r.limit) return t(`row.scale.capped.${why}`, { limit: r.limit, asked });
  if (value === 'window' && r.asked === 'window')
    return r.limit < 8 ? t(`row.scale.window.${why}`, { used: r.used, limit: r.limit }) : t('row.scale.window', { used: r.used });
  return r.limit < 8 && !state.scaleMax ? t(`row.scale.limit.${why}`, { limit: r.limit }) : base;
}

function scaleOption(value, label) {
  const r = state.scale;
  return r && Number(value) > r.limit ? t('row.scale.option', { label, limit: r.limit }) : label;
}

// Performance mode (the game's Performance Mode row, finding 183): on is the three effects off.
const effectsOff = () => EFFECTS.filter((k) => get(k) === '0').length;
function perfHint() {
  const off = effectsOff();
  return off > 0 && off < EFFECTS.length ? 'row.performance.custom' : 'row.performance.hint';
}

// A hint or label: a table key, else words already.
const wordsOf = (k) => (typeof k === 'function' ? wordsOf(k()) : k && lang.english[k] !== undefined ? t(k) : k);

function control(row, off) {
  const value = get(row.key);
  const name = t(row.label);
  const label = (text) => t('settings.savedValue', { setting: name, value: text });
  const tab = off ? -1 : null;
  const offered = () => choices(row.key).filter(([v]) => !row.allow || row.allow(v));
  switch (row.type) {
    case 'seg':
      return el('div', { class: 'seg', role: 'radiogroup', 'aria-label': name },
        offered().map(([v, text]) => el('button', {
          class: v === value ? 'on' : '', role: 'radio', 'aria-checked': String(v === value), tabindex: tab,
          onclick: () => save(row.key, v, label(text)).then(renderRows),
        }, text)));
    case 'select': {
      const show = row.option || ((v, text) => text);
      const s = el('select', { 'aria-label': name, disabled: off, onchange: (e) => {
        const opt = choices(row.key).find(([v]) => v === e.target.value);
        save(row.key, e.target.value, label(opt[1])).then(() => { if (typeof row.hint === 'function') renderRows(); });
      } }, offered().map(([v, text]) => el('option', { value: v, selected: v === value }, show(v, text))));
      return el('div', { class: 'select' }, s, icon('i-down'));
    }
    case 'switch': {
      const [[offValue], [onValue]] = choices(row.key);
      const on = !off && value === onValue;
      return el('button', { class: 'sw', role: 'switch', 'aria-checked': String(on), 'aria-label': name, disabled: off,
        onclick: () => save(row.key, on ? offValue : onValue, label(t(on ? 'settings.off' : 'settings.on'))).then(renderRows) });
    }
    case 'performance': {
      const on = effectsOff() === EFFECTS.length;
      return el('button', { class: 'sw', role: 'switch', 'aria-checked': String(on), 'aria-label': name,
        onclick: async () => {
          try {
            for (const k of EFFECTS) await write(k, on ? '1' : '0');
            flash(t('settings.saved', { what: label(t(on ? 'settings.off' : 'settings.on')) }));
          } catch (e) { fail(errorText(e)); }
          renderRows();
        } });
    }
    case 'language': {
      // "System" (what the device's languages give) first, then every language under its own name
      const options = [['', t('row.language.system', { language: languageName(systemLanguage()) })],
        ...LANGUAGES.map((c) => [c, languageName(c)])];
      const s = el('select', { 'aria-label': name, onchange: async (e) => {
        await save('language', e.target.value, label(e.target.selectedOptions[0].textContent));
        await loadLanguage(e.target.value);
        renderStatic();
      } }, options.map(([v, text]) => el('option', { value: v, selected: v === value }, text)));
      return el('div', { class: 'select' }, s, icon('i-down'));
    }
    case 'text':
      return el('input', { class: 'text-in', value, placeholder: row.placeholder, spellcheck: 'false', 'aria-label': name,
        onchange: (e) => save(row.key, e.target.value.trim(), label(e.target.value.trim() || t('settings.official'))) });
    case 'name':
      return el('input', { class: 'text-in upper', value, maxlength: String(state.schema.name.rule.max), spellcheck: 'false', 'aria-label': name,
        autocapitalize: 'characters', autocomplete: 'off',
        oninput: (e) => { e.target.value = cleanName(e.target.value); },
        onchange: (e) => setName(e.target.value) });
    case 'disc':
      return el('button', { class: 'btn', onclick: chooseDisc, disabled: discBusy(state.disc) }, icon('i-disc'),
        t(!value ? 'row.disc.choose' : has('disc-import') ? 'row.disc.replace' : 'row.disc.change'));
    case 'action':
      return el('button', { class: 'btn', onclick: row.run, disabled: needsDisc() }, row.icon ? icon(row.icon) : null, t(row.button));
  }
}

// Redraws a part of the page and puts the focus back on the control in the same place, so a key or
// pad press on a choice or a tab keeps its place.
function keepFocus(box, draw) {
  const controls = () => [...box.querySelectorAll('button, select, input')];
  const at = controls().indexOf(document.activeElement);
  draw();
  if (at >= 0) controls()[at]?.focus();
}

function renderTabs() {
  keepFocus($('tabs'), () => $('tabs').replaceChildren(...tabs.map(([key], i) => el('button', {
    class: i === state.tab ? 'on' : '', role: 'tab', 'aria-selected': String(i === state.tab),
    onclick: () => { state.tab = i; renderTabs(); renderRows(); },
  }, t(`tab.${key}`)))));
}

function discHint() {
  if (state.discProblem) return state.discProblem;
  const iso = get('iso');
  if (!iso) return t('row.disc.notChosen');
  if (has('disc-import') && state.discSize) return t('row.disc.inApp', { size: gb(state.discSize) });
  return shortPath(iso);
}

function renderRows() {
  if (!tabs[state.tab]) return;
  const rows = tabs[state.tab][1].flatMap((row) => {
    if (row.type === 'sticks') {  // stick.js: the dead zone, live from the controller
      const { canvas, note } = stickIndicator(() => get('stick_dead_zone'));
      return el('div', { class: 'row sticks-row' }, el('div', { class: 'l' }, note), canvas);
    }
    if (row.type === 'bindings') return bindingsBlock();  // controls.js: keys and buttons
    const off = row.when ? !row.when() : false;
    if (off && row.hideOff) return [];
    let hint = off ? wordsOf(row.offHint) : wordsOf(row.hint);
    let title = null;
    let bad = false;
    if (row.type === 'disc') {
      title = get('iso');
      hint = discHint();
      bad = !!state.discProblem;
    }
    return el('div', { class: off ? 'row off' : 'row' },
      el('div', { class: 'l' }, t(row.label), hint ? el('small', { title, class: bad ? 'bad' : null }, withLink(hint)) : null),
      control(row, off));
  });
  const note = tabs[state.tab][0] === 'online' || (tabs[state.tab][0] === 'game' && has('disc-import')) ? onlineNote() : null;
  keepFocus($('rows'), () => $('rows').replaceChildren(...(note ? [note] : []), ...rows));
}

// Back to the game's player defaults; the disc image, the rider name and the language stay.
async function resetTab() {
  const [key, rows] = tabs[state.tab];
  try {
    // a text row is emptied (its default, the official server, stays the game's to name)
    for (const row of rows) {
      if (!row.key || ['disc', 'name', 'language', 'action'].includes(row.type)) continue;
      if (row.type === 'performance') { for (const k of EFFECTS) await write(k, state.schema[k].default); continue; }
      await write(row.key, row.type === 'text' ? '' : state.schema[row.key].default);
    }
    if (key === 'controls') await resetBindings();  // the keyboard's and both controllers' layouts
    flash(t('settings.tabReset', { tab: t(`tab.${key}`) }));
  } catch (e) {
    fail(errorText(e));
  }
  renderRows();
}

// A long path keeps its drive and its file name: "D:\Games\…\Downhill Domination (USA).iso".
function shortPath(p) {
  return p.length <= 64 ? p : `${p.slice(0, 16)}…${p.slice(-46)}`;
}

async function chooseDisc() {
  try {
    const picked = await call('pick_disc');
    if (!picked) return;
    if (has('disc-import')) {  // a phone copies it: disc-import events follow (disc_flow.js)
      state.disc = discFlow(state.disc, { job: picked.job, stage: 'checking' });
      state.error = null;
      renderAll();
      return;
    }
    state.settings.iso = picked;
    state.discProblem = null;
    state.error = null;
    state.online = null;
    renderAll();
    flash(t('disc.checked'));
    checkDisc();
  } catch (e) {
    fail(errorText(e));
  }
}

// A phone's import moved on (a disc-import event).
async function discEvent(e) {
  const before = state.disc;
  state.disc = discFlow(state.disc, e);
  if (state.disc === before) return;
  if (state.disc.stage === 'done' || state.disc.stage === 'kept') {
    await refresh();
    state.online = null;
    flash(t('disc.copied'));
    checkDisc();
  }
  if (state.disc.stage === 'problem') state.error = null;
  renderAll();
}

async function discAnswer(remove) {
  const job = state.disc.job;
  state.disc = discFlow(state.disc, { job, stage: 'kept' });
  renderAll();
  if (!remove) return;
  try {
    await call('delete_original', { job });
    flash(t('disc.deleted'));
  } catch (e) {
    fail(errorText(e));
  }
}

// Online races need the USA disc's game files (finding 098): the disc image's files are hashed once
// (a few seconds on an SSD, about half a minute on a hard disk), then remembered in iso-check.txt
// for the game too.
async function checkDisc() {
  const path = get('iso');
  if (!path || state.discProblem) return;
  state.online = { path, checking: true };
  try {
    const r = await call('check_disc');
    if (r.path !== get('iso')) return;
    state.online = { path: r.path, usa: r.usa };
  } catch (e) {
    if (String(e).startsWith('replaced') || state.online?.path !== path) return;
    state.online = { path, error: errorText(e) };
  }
  renderOnline();
}

// ---------- rider name ----------

// The rider name's rule is dhd_settings' (settings_schema's rule for name): what a field keeps while
// the player types; rider_name gives the name saved.
function cleanName(s) {
  const { letters, max } = state.schema.name.rule;
  return [...s.replace(/[a-z]/g, (c) => c.toUpperCase())].filter((c) => letters.includes(c)).join('').slice(0, max);
}

function renderRider() {
  const name = get('name');
  $('rider').classList.toggle('unset', !name);
  $('rider-name').textContent = name || t('rider.unset');
  $('rider-hint').textContent = t(name ? 'rider.label' : 'rider.notSet');
  $('rider-initial').textContent = name.trim()[0] || '?';
}

async function setName(raw) {
  const name = await call('rider_name', { text: raw });
  if (!name || name === get('name')) return;
  await save('name', name, t('rider.saved', { name }));
  renderRider();
}

function editName(on) {
  const input = $('rider-input');
  $('rider-name').hidden = on;
  input.hidden = !on;
  if (on) {
    $('rider-hint').textContent = t('rider.enterToSave');
    input.value = get('name');
    input.focus();
    input.select();
  } else {
    renderRider();
  }
}

// ---------- patch notes ----------

function escapeHtml(s) { return s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }

function inline(s) {
  return escapeHtml(s.replace(/<(https?:[^>\s]+)>/g, '$1'))
    .replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>')
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1');
}

// The release notes for players: what's above "## Commits" (the list of commits is for the repo).
function playerNotes(text) {
  return text.replace(/\r/g, '').split(/^##\s+Commits\s*$/m)[0].replace(/^##\s+What's new\s*$/m, '').trim();
}

// A small markdown subset (headings, bold-only lines as headings, bullets, paragraphs): enough
// for GitHub release notes.
function markdown(text) {
  const out = [];
  let list = false;
  for (const raw of text.split('\n')) {
    const line = raw.trimEnd();
    const bullet = line.match(/^\s*[-*]\s+(.*)/);
    const heading = line.match(/^#{1,6}\s+(.*)/) || line.match(/^\*\*([^*]+)\*\*$/);
    if (!bullet && list) { out.push('</ul>'); list = false; }
    if (bullet) { if (!list) { out.push('<ul>'); list = true; } out.push(`<li>${inline(bullet[1])}</li>`); }
    else if (heading) out.push(`<h3>${inline(heading[1])}</h3>`);
    else if (line.trim()) out.push(`<p>${inline(line)}</p>`);
  }
  if (list) out.push('</ul>');
  return out.join('');
}

function renderNotes() {
  const r = state.update?.release;
  if (!r) return;
  $('notes-title').textContent = t('notes.title', { version: r.version });
  if (r.notes) {
    $('notes-body').innerHTML = markdown(playerNotes(r.notes));
  } else {
    // the answer came from the backup server, which has no notes (finding 138)
    $('notes-body').replaceChildren(el('p', { class: 'muted' }, t('notes.missing', { version: r.version })));
  }
}

// ---------- help (a phone's) ----------

function helpRow(label, hint, ...controls) {
  return el('div', { class: 'row' }, el('div', { class: 'l' }, label, hint ? el('small', {}, hint) : null), ...controls);
}

function renderHelp() {
  const link = (url, label, iconId, cls = 'btn') => el('button', { class: cls, onclick: () => call('open_link', { url }).catch((e) => fail(errorText(e))) },
    icon(iconId, iconId === 'i-discord' ? 'fill' : ''), label);
  const rows = [];
  if (has('share-log')) rows.push(helpRow(t('help.log'), t('help.log.hint'), el('button', { class: 'btn', onclick: shareLog }, icon('i-share'), t('help.log'))));
  rows.push(helpRow(t('help.version'), t('help.version.value', { version: state.version })));
  rows.push(helpRow(t('help.links'), null, el('div', { class: 'btns' },
    link('https://github.com/skuza-oss/downhill-revived', t('help.github'), 'i-external'),
    link('discord', t('footer.discord'), 'i-discord', 'btn discord'),
    link('kofi', t('footer.support'), 'i-heart'))));
  rows.push(helpRow(t('help.licences'), t('help.licences.hint'),
    el('button', { class: 'btn', onclick: toggleLicences }, icon('i-notes'), t('help.licences.button'))));
  const out = [...rows];
  if (state.licences) out.push(el('pre', { class: 'licences' }, state.licences));
  keepFocus($('help-rows'), () => $('help-rows').replaceChildren(...out));
}

async function toggleLicences() {
  try {
    state.licences = state.licences ? null : (await call('licences')) || ' ';
  } catch (e) {
    fail(errorText(e));
  }
  renderHelp();
}

async function shareLog() {
  try {
    await call('share_log');
  } catch (e) {
    fail(errorText(e));
  }
}

// ---------- footer ----------

function status(kind, iconId, ...parts) {
  const lead = iconId ? icon(iconId, kind) : el('span', { class: `dot ${kind}` });
  $('status').replaceChildren(lead, el('span', {}, ...parts));
}

// A confirmation fades back to the footer's state; an error stays (fail).
function flash(text) {
  clearTimeout(flashTimer);
  if (state.error || state.confirmClose) return;
  status('good', 'i-check', text);
  flashTimer = setTimeout(renderFooter, 3500);
}

function fail(text, fix = null) {
  state.error = { text, fix };
  renderFooter();
}

function setPrimary({ text, icon: iconId = null, disabled = false, onclick }) {
  const b = $('primary');
  b.disabled = disabled;
  b.replaceChildren(...(iconId ? [icon(iconId, iconId === 'i-play' ? 'fill' : '')] : []), el('span', {}, text));
  b.onclick = onclick;
}

// The button beside the primary one: a quiet text button, or a solid one for a fix.
function setSecondary(text, onclick, solid = false) {
  const b = $('secondary');
  b.hidden = !text;
  b.className = solid ? 'btn' : 'btn text';
  if (text) { b.replaceChildren(...(typeof text === 'string' ? [text] : text)); b.onclick = onclick; }
}

// A phone's disc import in the footer: its progress, its question or its problem. True when drawn.
function renderDiscFooter() {
  const d = state.disc;
  if (d.stage === 'checking') {
    status('busy', null, t('disc.importing'), el('small', { class: 'sub-line' }, t('disc.importingCloud')));
    setPrimary({ text: t('footer.play'), icon: 'i-play', disabled: true });
    return true;
  }
  if (d.stage === 'copying') {
    const pct = d.total ? Math.min(100, (d.done / d.total) * 100) : 0;
    const line = d.total ? t('disc.copying', { done: gb(d.done), total: gb(d.total) }) : t('disc.copyingUnknown', { done: gb(d.done) });
    $('status').replaceChildren(el('div', {}, line, el('div', { class: 'bar' }, el('i', { style: `width:${pct}%` })),
      el('small', { class: 'sub-line' }, t('disc.keepOpen'))));
    setPrimary({ text: t('footer.play'), icon: 'i-play', disabled: true });
    return true;
  }
  if (d.stage === 'done' && d.deletable) {
    status('good', 'i-check', t('disc.deleteAsk', { size: gb(d.size) }));
    setSecondary(t('disc.keepIt'), () => discAnswer(false));
    $('danger').hidden = false;
    $('danger').textContent = t('disc.deleteIt');
    $('danger').onclick = () => discAnswer(true);
    setPrimary({ text: t('footer.play'), icon: 'i-play', onclick: () => discAnswer(false).then(() => play()) });
    return true;
  }
  if (d.stage === 'problem') {
    status('bad', 'i-alert', errorText(d.problem));
    // not enough space: the same file again once there is room; else another file (the primary button)
    if (d.problem?.code === 'disc.problem.noRoom')
      setSecondary([icon('i-retry'), t('disc.tryAgain')],
        () => call('retry_import', { job: d.job }).then((r) => r && discEvent({ job: r.job, stage: 'checking' })), true);
    setPrimary({ text: t('disc.chooseAnother'), icon: 'i-disc', onclick: chooseDisc });
    return true;
  }
  return false;
}

function renderFooter() {
  clearTimeout(flashTimer);
  const playNow = { text: t('footer.play'), icon: 'i-play', onclick: () => play() };
  setSecondary(null);
  $('danger').hidden = true;
  $('dismiss').hidden = true;
  if (state.running) {
    setPrimary({ text: t('footer.running'), disabled: true });
    if (state.confirmClose) {
      status('warn', null, t('footer.closeAsk'));
      setSecondary(t('footer.keepPlaying'), () => { state.confirmClose = false; renderFooter(); });
      $('danger').hidden = false;
      $('danger').textContent = t('footer.closeGame');
      $('danger').onclick = () => { state.confirmClose = false; call('stop_game'); renderFooter(); };
      return;
    }
    status('ok', null, t('footer.isRunning'));
    return setSecondary(t('footer.closeGame'), () => { state.confirmClose = true; renderFooter(); });
  }
  if (state.starting) {
    status('busy', null, t('footer.starting'));
    return setPrimary({ ...playNow, disabled: true });
  }
  if (!state.gameFound) {
    status('bad', 'i-alert', t('footer.noGame'));
    setSecondary([icon('i-folder'), t('footer.openFolder')], () => call('open_folder').catch((e) => fail(errorText(e))), true);
    return setPrimary({ ...playNow, disabled: true });
  }
  if (renderDiscFooter()) return;
  if (needsDisc() && state.check !== 'installing') {
    if (state.discProblem) status('bad', 'i-alert', t('footer.discProblem'));
    else status('warn', null, t('footer.noDisc'));
    setPrimary({ text: t('footer.chooseDisc'), icon: 'i-disc', onclick: chooseDisc });
  } else {
    switch (state.check) {
      case 'checking':
        status('busy', null, t('footer.checking'));
        setPrimary(playNow);
        break;
      case 'off':
        status('', null, ...tNodes('footer.checksOff', { version: state.version }));
        setPrimary(playNow);
        break;
      case 'unavailable':
        status('', null, ...tNodes('footer.unavailable', { version: state.version }));
        setPrimary(playNow);
        break;
      case 'failed':
        // the launcher's line says what went wrong (update.rs check_failed_message; launcher.log has the rest)
        status('bad', 'i-alert', state.checkError || t('footer.checkFailed', { version: state.version }));
        setSecondary([icon('i-retry'), t('footer.retry')], checkUpdate);
        setPrimary(playNow);
        break;
      case 'available': {
        const r = state.update.release;
        if (!has('install-update')) {  // a phone: the release page, where the player installs it (finding 143)
          status('warn', null, ...tNodes('footer.out', { version: r.version }));
          setSecondary([icon('i-external'), t('footer.openPage')], () => call('open_link', { url: r.page }).catch((e) => fail(errorText(e))));
          setPrimary(playNow);
          break;
        }
        status('warn', null, ...(r.size ? tNodes('footer.readySize', { version: r.version, size: Math.round(r.size / 1048576) })
          : tNodes('footer.ready', { version: r.version })));
        setSecondary(t('footer.playVersion', { version: state.version }), () => play());
        setPrimary({ text: t('footer.update'), icon: 'i-download', onclick: installUpdate });
        break;
      }
      case 'installing':
        return; // drawn by the progress events
      case 'install-failed':
        // "Update failed: ..." from the launcher (update.rs download_failed_message for the network)
        status('bad', 'i-alert', state.checkError || t('footer.updateFailed'));
        setSecondary([icon('i-retry'), t('footer.tryAgain')], installUpdate);
        setPrimary(playNow);
        break;
      default:
        status('ok', null, ...tNodes('footer.upToDate', { version: state.version }));
        setPrimary(playNow);
    }
  }
  if (state.error) {
    const { text, fix } = state.error;
    status('bad', 'i-alert', ...withLink(text));
    setSecondary(fix && [icon(fix.icon), fix.label], fix?.onclick, true);
    $('dismiss').hidden = false;
  }
}

function renderProgress(done, total) {
  const pct = total ? Math.min(100, (done / total) * 100) : 0;
  const mb = (n) => (n / 1048576).toFixed(0);
  const bar = el('div', { class: 'bar' }, el('i', { style: `width:${pct}%` }));
  const version = state.update.release.version;
  $('status').replaceChildren(el('div', {},
    ...(total ? tNodes('footer.downloading', { version, done: mb(done), total: mb(total) }) : tNodes('footer.downloadingSize', { version, done: mb(done) })), bar));
}

// The words in index.html and those drawn once at start, again after a language change.
function renderStatic() {
  translatePage(document);
  renderTabs();
  renderAll();
  if (!$('page-help').hidden) renderHelp();
  if (state.update?.release) renderNotes();
}

function renderAll() {
  renderRider();
  renderHome();
  renderRows();
  renderFooter();
}

// ---------- actions ----------

async function checkUpdate() {
  if (get('auto_update') === 'off') { state.check = 'off'; return renderFooter(); }
  state.check = 'checking';
  renderFooter();
  try {
    state.update = await call('check_update');
    renderNotes();
    renderHome();
    state.check = !state.update.release ? 'unavailable' : state.update.newer ? 'available' : 'current';
    renderFooter();
    if (state.update.newer && get('auto_update') === 'on' && has('install-update')) installUpdate();
  } catch (e) {
    state.check = 'failed';
    state.checkError = errorText(e);
    renderFooter();
  }
}

async function checkServer() {
  if (!has('server')) return;
  try {
    state.server = await call('server_status');
  } catch {
    state.server = 'down';
  }
  renderServer();
}

async function installUpdate() {
  state.check = 'installing';
  state.error = null;
  setSecondary(null);
  $('dismiss').hidden = true;
  setPrimary({ text: t('footer.updating'), icon: 'i-download', disabled: true });
  renderProgress(0, state.update.release.size);
  try {
    await call('install_update', { release: state.update.release }); // restarts the launcher
  } catch (e) {
    state.check = 'install-failed';
    state.checkError = errorText(e);
    renderFooter();
  }
}

// PLAY. Where the game takes the launcher's place (a phone: caps without close-game) the page
// only says it is starting; the desktop's launcher waits for game-exited.
async function play(options) {
  try {
    await call('play', options && options.editTouchLayout ? { edit_touch_layout: true } : {});
    if (has('close-game')) state.running = true;
    else state.starting = true;
    state.error = null;
    renderFooter();
  } catch (e) {
    fail(errorText(e));
  }
}

// The snapshot again (after an import, or the game's run on the desktop).
async function refresh() {
  apply(await call('snapshot'));
}

function apply(snap) {
  caps = new Set(snap.caps || []);
  state.version = snap.version;
  state.settings = snap.settings;
  state.gameFound = snap.game_found;
  state.running = snap.running;
  state.discProblem = snap.disc_problem;
  state.texturePack = snap.texture_pack;
  state.played = snap.played;
  state.scale = snap.scale;
  state.scaleMax = snap.scale_max || 0;
  state.discSize = snap.disc_size || 0;
  state.freeSpace = snap.free_space || 0;
  if (snap.last_exit) state.lastExit = snap.last_exit;  // said once; the note stays until dismissed
}

// ---------- start ----------

async function start() {
  await bridge.ready;
  const snap = await call('snapshot');
  apply(snap);
  await loadLanguage(get('language'));
  // macOS supplies real window buttons over the title bar's drag region; a phone has no title bar.
  document.querySelector('.titlebar').hidden = !has('window');
  document.querySelector('.win-ctl').hidden = snap.native_window_controls;
  document.documentElement.classList.toggle('touch', has('touch-settings'));
  $('nav-notes').hidden = !has('patch-notes');
  $('nav-help').hidden = !has('help');
  $('kofi').hidden = $('discord').hidden = has('help');  // a phone has them on its Help page
  $('settings-sub').textContent = t(has('touch-settings') ? 'settings.subTouch' : 'settings.sub');
  $('settings-sub').dataset.i18n = has('touch-settings') ? 'settings.subTouch' : 'settings.sub';
  state.schema = Object.fromEntries((await call('settings_schema')).map((s) => [s.key, s]));
  tabs = TABS.map(([key, rows]) => [key, rows.filter((r) => !r.needs || has(r.needs))]).filter(([, rows]) => rows.length);
  $('rider-input').maxLength = state.schema.name.rule.max;

  document.querySelectorAll('[data-page]').forEach((b) => b.addEventListener('click', () => show(b.dataset.page)));
  if (has('window')) {
    const appWindow = window.__TAURI__.window.getCurrentWindow();
    $('win-min').onclick = () => appWindow.minimize();
    $('win-close').onclick = () => appWindow.close();
  }
  $('discord').onclick = () => call('open_link', { url: 'discord' });
  $('kofi').onclick = () => call('open_link', { url: 'kofi' });
  $('dismiss').onclick = () => { state.error = null; renderFooter(); };
  $('reset-tab').onclick = resetTab;
  $('fact-disc').onclick = () => { state.tab = Math.max(0, tabIndex('game')); renderTabs(); renderRows(); show('settings'); };
  $('notes-page').onclick = () => call('open_link', { url: state.update?.release?.page || 'https://github.com/skuza-oss/downhill-revived/releases' });

  // the whole rider card edits the name; a click outside the input while editing saves it
  const input = $('rider-input');
  const rider = $('rider');
  rider.tabIndex = 0;
  rider.setAttribute('role', 'button');
  rider.setAttribute('aria-label', t('rider.change'));
  $('rider-edit').tabIndex = -1;
  rider.addEventListener('mousedown', (e) => { if (e.target !== input && !input.hidden) e.preventDefault(); });
  rider.addEventListener('click', (e) => { if (e.target !== input) (input.hidden ? editName(true) : input.blur()); });
  rider.addEventListener('keydown', (e) => { if (e.target === rider && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); editName(true); } });
  input.addEventListener('input', () => { input.value = cleanName(input.value); });
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') input.value = get('name');
    if (e.key === 'Enter' || e.key === 'Escape') input.blur();
  });
  input.addEventListener('blur', () => {
    setName(input.value);
    editName(false);
  });

  // the focus ring is for keys and pads only (style.css)
  addEventListener('keydown', () => document.documentElement.classList.add('kbd'), true);
  addEventListener('pointerdown', () => document.documentElement.classList.remove('kbd'), true);
  document.addEventListener('contextmenu', (e) => { if (!e.target.closest('input, .md')) e.preventDefault(); });

  await bridge.on('update-progress', (e) => renderProgress(e.done, e.total));
  await bridge.on('disc-progress', (e) => {
    const { path, done, total } = e;
    if (!state.online?.checking || state.online.path !== path) return;
    state.online = { path, checking: true, done, total };
    renderOnline();
  });
  await bridge.on('disc-import', discEvent);
  // the app came back to the front (a phone: the share sheet, the release page): the snapshot may
  // have changed (SETTINGS in the game)
  await bridge.on('resumed', () => { state.starting = false; refresh().then(renderAll).catch(() => {}); });
  await bridge.on('game-exited', (e) => {
    state.running = false;
    state.confirmClose = false;
    state.played = e.played;
    renderPlayed();
    // what this run's dhd.log says about the resolution scale (the GPU may have changed), and the
    // bindings REVIVED SETTINGS > Controls may have changed (controls.js reads them again)
    bind.layouts = null;
    call('snapshot').then((s) => { state.scale = s.scale; if (!$('page-settings').hidden) renderRows(); });
    renderFooter();
    checkServer();
    if (e.wants_update) checkUpdate().then(() => { if (state.check === 'available') installUpdate(); });
  });

  renderTabs();
  renderAll();
  $('primary').focus();
  checkUpdate();
  checkServer();
  checkDisc();
}

start();
