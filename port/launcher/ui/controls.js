// The Controls tab's bindings (finding 157): which key or controller button stands for each of the
// PS2 pad's buttons, for the keyboard (player 1's) and each player's controller. The rules and the
// dhd.ini lines (keyboard, pad1, pad2) are dhd_settings' (port/render/settings/src/controls.rs):
// the Rust side (src-tauri/src/controls.rs) makes every change and saves it; this file draws the
// layouts and waits for the key or button pressed. Uses app.js's el, icon, flash and state.
'use strict';

const bind = {
  schema: null,     // controls_schema: targets, keys, pads, devices, buttons
  layouts: null,    // controls_state: per device { inputs: [[main, alternate]...], swap_sticks, invert }
  device: 0,        // the layout shown: 0 keyboard, 1 Controller 1, 2 Controller 2
  capture: null,    // waiting: { device, target, slot, since, held: Set of pad buttons down at the start }
  taken: new Set(), // the pad buttons down when a binding took one: pad.js ignores them until let go
  note: '',         // what the last change did (moved from, or why not)
  bad: false,
};
const CAPTURE_MS = 5000;
const INVERTS = [[1, 'Invert left stick X'], [2, 'Invert left stick Y'], [4, 'Invert right stick X'], [8, 'Invert right stick Y']];

// A controller's kind by its name, as the game's PAD_KIND_* (1 PlayStation, 2 Xbox, 3 Nintendo): the
// words its buttons print.
function padKind(id) {
  if (/xbox|045e/i.test(id || '')) return 2;
  if (/054c|playstation|dualsense|dualshock|^wireless controller/i.test(id || '')) return 1;  // a DualShock 4: "Wireless Controller"
  if (/057e|nintendo|pro controller|joy-con/i.test(id || '')) return 3;
  return 2;
}

function connectedKind() {
  const p = [...(navigator.getGamepads?.() ?? [])].find(Boolean);
  return p ? padKind(p.id) : 2;
}

// The word for an input of a device's layout ('' none): a key's label, a controller button's by kind.
function inputWord(schema, device, code, kind) {
  if (!code) return '';
  if (device === 0) return schema.keys.find((k) => k.vk === code)?.label ?? '?';
  const p = schema.pads.find((q) => q.id === code);
  return p ? p.labels[kind === 1 ? 1 : kind === 3 ? 2 : 0] : '?';
}

// The key a KeyboardEvent.code names, as the layouts keep it (its vk), or null (not one the game uses).
function keyOfCode(schema, code) {
  if (code === 'NumpadEnter') code = 'Enter';  // the game takes both Enters as one key
  return schema.keys.find((k) => k.code === code)?.vk ?? null;
}

// The standard-mapping buttons pressed now (a trigger past half way counts).
function padButtonsDown(pads) {
  const down = new Set();
  for (const p of pads) p.buttons.forEach((b, i) => { if (b.pressed || b.value > 0.5) down.add(i); });
  return down;
}

// The controller input newly pressed (down now, not when the wait began), or null.
function padInputPressed(schema, held, down) {
  for (const i of down) {
    if (held.has(i)) continue;
    const p = schema.pads.find((q) => q.web === i);
    if (p) return p.id;
  }
  return null;
}

// The rows a device shows: each target it can bind, with its words.
function bindingRows(schema, layout, device, kind) {
  const count = device === 0 ? schema.targets.length : schema.buttons;
  return schema.targets.slice(0, count).map((t, i) => ({
    target: i, label: t.label, race: t.race,
    words: layout.inputs[i].map((code) => inputWord(schema, device, code, kind)),
  }));
}

async function loadBindings() {
  if (!bind.schema) bind.schema = await invoke('controls_schema');
  bind.layouts = (await invoke('controls_state')).layouts;
}

// A change through the launcher's Rust side (it saves dhd.ini); the note says what happened.
async function bindEdit(device, edit) {
  try {
    const r = await invoke('controls_edit', { device, edit: { target: 0, slot: 0, input: 0, on: false, ...edit }, kind: connectedKind() });
    bind.layouts = r.layouts;
    bind.note = r.message;
    bind.bad = r.outcome === 'refused';
    if (r.outcome === 'changed') flash(`Saved. ${bind.schema.devices[device].label}${r.message ? `: ${r.message}` : ''}`);
  } catch (e) {
    bind.note = String(e);
    bind.bad = true;
  }
  renderRows();
}

function endCapture() {
  bind.capture = null;
  renderRows();
}

// A wait counts only while its prompt is on screen (the settings page's Controls tab, the device
// it began on) and for five seconds: leaving it gives up, so a key typed elsewhere is never taken.
function captureLive(c) {
  return c === bind.capture && state.tab === CONTROLS_TAB && !$('page-settings').hidden
    && bind.device === c.device && performance.now() - c.since <= CAPTURE_MS;
}

function startCapture(device, target, slot) {
  const pads = [...(navigator.getGamepads?.() ?? [])].filter(Boolean);
  const c = { device, target, slot, since: performance.now(), held: padButtonsDown(pads) };
  bind.capture = c;
  bind.note = '';
  renderRows();
  if (device > 0) requestAnimationFrame(pollCapture);
  else setTimeout(() => { if (bind.capture === c) endCapture(); }, CAPTURE_MS + 50);
}

// A controller's wait: the first button pressed after it began; five seconds without one give up.
function pollCapture() {
  const c = bind.capture;
  if (!c || c.device === 0) return;
  if (!captureLive(c)) return endCapture();
  const down = padButtonsDown([...(navigator.getGamepads?.() ?? [])].filter(Boolean));
  for (const i of c.held) if (!down.has(i)) c.held.delete(i);
  const input = padInputPressed(bind.schema, c.held, down);
  if (input) {
    bind.taken = down;  // the press is the binding's, not a Start, Back or tab change as well
    bind.capture = null;
    return bindEdit(c.device, { edit: 'assign', target: c.target, slot: c.slot, input });
  }
  requestAnimationFrame(pollCapture);
}

// The keyboard's wait (any device: Esc gives up).
addEventListener('keydown', (e) => {
  const c = bind.capture;
  if (!c) return;
  if (!captureLive(c)) { endCapture(); return; }  // the key goes where it was typed
  e.preventDefault();
  e.stopPropagation();
  if (e.code === 'Escape') return endCapture();
  if (c.device > 0 || e.repeat) return;
  const input = keyOfCode(bind.schema, e.code);
  if (input === null) {
    bind.note = 'That key can\'t be used: Esc, Alt, Ctrl and the Windows keys stay the game\'s and the system\'s';
    bind.bad = true;
    return renderRows();
  }
  bind.capture = null;
  bindEdit(0, { edit: 'assign', target: c.target, slot: c.slot, input });
}, true);

// pad.js: no navigation while a controller button is awaited, nor from the buttons that answered
// it until they are let go
function bindCapturing() {
  if (bind.capture) return true;
  if (!bind.taken.size) return false;
  const down = padButtonsDown([...(navigator.getGamepads?.() ?? [])].filter(Boolean));
  for (const i of bind.taken) if (!down.has(i)) bind.taken.delete(i);
  return bind.taken.size > 0;
}

// The tab's bindings block: the device, its rows, a controller's stick options and its Reset.
function bindingsBlock() {
  if (!bind.layouts) {
    loadBindings().then(renderRows).catch((e) => { bind.note = String(e); bind.bad = true; });
    return [];
  }
  const s = bind.schema, device = bind.device, layout = bind.layouts[device], kind = connectedKind();
  const out = [el('div', { class: 'row' },
    el('div', { class: 'l' }, 'Bindings', el('small', {}, device === 0
      ? 'Keys by their place on the keyboard. Esc always goes back and pauses.'
      : 'By the button\'s place on the controller. Every controller of this player uses this layout.')),
    el('div', { class: 'seg', role: 'radiogroup', 'aria-label': 'Bindings' }, s.devices.map((d, i) => el('button', {
      class: i === device ? 'on' : '', role: 'radio', 'aria-checked': String(i === device),
      onclick: () => { bind.device = i; bind.note = ''; bind.capture = null; renderRows(); },
    }, d.label))))];
  if (bind.note) out.push(el('div', { class: `row bind-note${bind.bad ? ' bad' : ''}` }, bind.note));
  for (const r of bindingRows(s, layout, device, kind)) {
    const slotButton = (slot) => {
      const waiting = bind.capture && bind.capture.target === r.target && bind.capture.slot === slot;
      const word = waiting ? (device === 0 ? 'Press a key…' : 'Press a button…') : r.words[slot] || '—';
      return el('button', { class: `btn slot${waiting ? ' waiting' : ''}${r.words[slot] ? '' : ' empty'}`,
        'aria-label': `${r.label} ${slot ? 'alternate' : 'main'}: ${r.words[slot] || 'none'}`,
        onclick: () => (waiting ? endCapture() : startCapture(device, r.target, slot)),
        oncontextmenu: (e) => { e.preventDefault(); bindEdit(device, { edit: 'clear', target: r.target, slot }); },
      }, word);
    };
    const waitingHere = bind.capture && bind.capture.target === r.target;
    out.push(el('div', { class: 'row bind' },
      el('div', { class: 'l' }, r.label, r.race ? el('small', {}, r.race) : null),
      waitingHere ? el('button', { class: 'link quiet', onclick: () => { const { slot } = bind.capture; bind.capture = null;
        bindEdit(device, { edit: 'clear', target: r.target, slot }); } }, 'Clear') : null,
      slotButton(0), slotButton(1)));
  }
  if (device > 0) {
    const option = (label, on, edit) => el('div', { class: 'row' }, el('div', { class: 'l' }, label),
      el('button', { class: 'sw', role: 'switch', 'aria-checked': String(on), 'aria-label': label,
        onclick: () => bindEdit(device, { ...edit, on: !on }) }));
    out.push(option('Swap sticks', layout.swap_sticks, { edit: 'swap' }));
    for (const [bit, label] of INVERTS) out.push(option(label, !!(layout.invert & bit), { edit: 'invert', input: bit }));
  }
  out.push(el('div', { class: 'row' }, el('div', { class: 'l' }, el('small', {},
    'Click a key or button to change it, right-click to clear it. A key or button another one had moves here.')),
    el('button', { class: 'btn', onclick: () => bindEdit(device, { edit: 'reset' }) }, icon('i-undo'), `Reset ${s.devices[device].label}`)));
  return out;
}

// The tab's Reset: every layout back to the defaults.
async function resetBindings() {
  for (let d = 0; d < 3; d++) await bindEdit(d, { edit: 'reset' });
  bind.note = '';
}

if (typeof module !== 'undefined') module.exports = { padKind, inputWord, keyOfCode, padInputPressed, bindingRows };
