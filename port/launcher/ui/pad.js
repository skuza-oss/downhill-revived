// Controller support for the launcher (the browser's Gamepad API, standard mapping): the d-pad or
// the left stick moves the focus to the nearest control that way, A (Cross) presses it, B (Circle)
// goes back to PLAY, LB/RB change the settings tab and Start presses the footer's primary button.
// The first pad press shows the focus ring and the button hints (html.pad); a mouse click hides
// them again. Uses app.js's show, renderTabs, renderRows and state.
'use strict';

const PAD = { A: 0, B: 1, LB: 4, RB: 5, START: 9, UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15 };
const REPEAT_AFTER = 380, REPEAT_EVERY = 110, STICK = 0.55;
const held = new Map();   // button or direction -> time of its next repeat
let polling = false;

// The controls a pad can reach (not the window's own buttons), each with its part of the window.
function focusable() {
  const sel = 'button:not(:disabled), select:not(:disabled), input:not([hidden]), [tabindex="0"]';
  return [...document.querySelectorAll(sel)].filter((e) => {
    if (e.tabIndex < 0 || e.closest('[hidden], .titlebar')) return false;
    const r = e.getBoundingClientRect();
    return r.width > 0 && r.height > 0;
  });
}
const area = (e) => e.closest('.rail, .main, .foot');
const rowsOf = () => [$('tabs'), ...$('rows').querySelectorAll('.row:not(.off):not(.sticks-row):not(.bind-note)')];

// A settings row's control: the chosen choice of a segmented one, else the first.
function rowControl(row) {
  if (row.id === 'tabs') return row.querySelector('.on');
  return row.querySelector('.seg .on') || row.querySelector('.seg button, select, .sw, input, .btn');
}

// Up and down in Settings go row by row (the tabs count as the first row).
function moveRow(from, dy) {
  const rows = rowsOf();
  const i = rows.findIndex((r) => r.contains(from));
  if (i < 0) return false;
  const next = rows[i + dy];
  if (next) rowControl(next).focus();
  else if (dy > 0) $('primary').focus();
  else if (!$('reset-tab').hidden) $('reset-tab').focus();
  return true;
}

// Else the nearest control that way: the distance along it plus three times the sideways gap
// between the two (0 when they line up). Up and down stay in the sidebar, the page or the footer
// while that has a control that way.
function move(dx, dy) {
  const from = document.activeElement && document.activeElement !== document.body ? document.activeElement : null;
  if (!from) return $('primary').focus();
  if (dy && !$('page-settings').hidden && moveRow(from, dy)) return;
  if (dy > 0 && from.id === 'reset-tab') return rowControl($('tabs')).focus();
  const a = from.getBoundingClientRect();
  const ax = a.left + a.width / 2, ay = a.top + a.height / 2;
  let best = null, bestScore = Infinity;
  for (const e of focusable()) {
    if (e === from) continue;
    const r = e.getBoundingClientRect();
    const along = ((r.left + r.width / 2) - ax) * dx + ((r.top + r.height / 2) - ay) * dy;
    if (along <= 4) continue;
    const gap = dx ? Math.max(0, r.top - a.bottom, a.top - r.bottom) : Math.max(0, r.left - a.right, a.left - r.right);
    const score = along + gap * 3 + (dy && area(e) !== area(from) ? 10000 : 0);
    if (score < bestScore) { bestScore = score; best = e; }
  }
  if (best) best.focus();
}

function press() {
  const e = document.activeElement;
  if (!e || e === document.body) return $('primary').focus();
  if (e.tagName === 'SELECT') {
    e.selectedIndex = (e.selectedIndex + 1) % e.options.length;
    e.dispatchEvent(new Event('change'));
  } else if (e.tagName !== 'INPUT') {
    e.click();
  }
  if (document.activeElement === document.body) $('primary').focus();
}

function back() {
  const e = document.activeElement;
  if (e && e.tagName === 'INPUT') return e.blur();
  show('play');
  $('primary').focus();
}

function changeTab(step) {
  if ($('page-settings').hidden) return;
  state.tab = (state.tab + step + TABS.length) % TABS.length;
  renderTabs();
  renderRows();
  document.querySelector('#tabs .on').focus();
}

const ACTIONS = {
  [PAD.UP]: () => move(0, -1), [PAD.DOWN]: () => move(0, 1), [PAD.LEFT]: () => move(-1, 0), [PAD.RIGHT]: () => move(1, 0),
  [PAD.A]: press, [PAD.B]: back, [PAD.LB]: () => changeTab(-1), [PAD.RB]: () => changeTab(1),
  [PAD.START]: () => { if (!$('primary').disabled) $('primary').click(); },
};
const REPEATS = new Set([PAD.UP, PAD.DOWN, PAD.LEFT, PAD.RIGHT]);

// PlayStation pads name their buttons by shape; the rest get Xbox letters.
function padLabels(id) {
  const sony = /054c|playstation|dualsense|dualshock|wireless controller/i.test(id);
  $('pad-a').textContent = sony ? '✕' : 'A';
  $('pad-b').textContent = sony ? '○' : 'B';
  $('pad-a').classList.toggle('sony', sony);
  $('pad-b').classList.toggle('sony', sony);
  $('pad-lb').textContent = sony ? 'L1' : 'LB';
  $('pad-rb').textContent = sony ? 'R1' : 'RB';
}

function poll(now) {
  const pads = [...navigator.getGamepads()].filter(Boolean);
  if (!pads.length) { polling = false; return; }
  const down = new Set();
  // while the stick picture is shown (stick.js), the stick only moves it; the d-pad still moves
  const sticksShown = !!document.querySelector('canvas.sticks');
  for (const p of pads) {
    p.buttons.forEach((b, i) => { if (b.pressed) down.add(i); });
    const [x = 0, y = 0] = sticksShown ? [] : p.axes;
    if (y < -STICK) down.add(PAD.UP);
    if (y > STICK) down.add(PAD.DOWN);
    if (x < -STICK) down.add(PAD.LEFT);
    if (x > STICK) down.add(PAD.RIGHT);
  }
  for (const k of held.keys()) if (!down.has(k)) held.delete(k);
  // a binding waits for a button (controls.js): the press is its own, and stays held until let go
  if (bindCapturing()) for (const k of down) held.set(k, Infinity);
  if (document.hasFocus() && !bindCapturing()) {
    for (const k of down) {
      const act = ACTIONS[k];
      if (!act) continue;
      const next = held.get(k);
      if (next === undefined) {
        held.set(k, now + REPEAT_AFTER);
      } else if (REPEATS.has(k) && now >= next) {
        held.set(k, now + REPEAT_EVERY);
      } else {
        continue;
      }
      if (!document.documentElement.classList.contains('pad')) padLabels(pads[0].id);
      document.documentElement.classList.add('kbd', 'pad');
      act();
    }
  }
  requestAnimationFrame(poll);
}

function startPolling() {
  if (!polling) { polling = true; requestAnimationFrame(poll); }
}

addEventListener('gamepadconnected', startPolling);
addEventListener('pointerdown', () => document.documentElement.classList.remove('pad'), true);
if ([...navigator.getGamepads()].some(Boolean)) startPolling();
