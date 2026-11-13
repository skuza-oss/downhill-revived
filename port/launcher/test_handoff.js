// The launcher's handoff gate (port/launcher/ui/bridge.js, finding 228) and the page's own handlers
// that use it, without a window:
// - the gate itself (orderedCalls): a setting operation with several writes is one unit, PLAY waits
//   for it and for every pending write, looking again after each change; a second handoff during
//   the transition is the same one; nothing new is written once PLAY went out; a save that never
//   answers cancels the handoff after the deadline instead of starting the game over it;
// - the same with slow saves over each transport the page runs on: the desktop's Tauri invoke,
//   Android's posted MessagePort, iOS's injected bridge_ios.js: the native side sees every write
//   before `play`, and one `play` only;
// - app.js itself (its resetTab and play, run with stand-ins for the drawing): a reset of a tab with
//   Performance mode followed at once by PLAY; PLAY over a save that never answers: cancelled with
//   its message and a retry.
//     node port/launcher/test_handoff.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const ui = (f) => fs.readFileSync(path.join(__dirname, 'ui', f), 'utf8');
const root = path.join(__dirname, '..', '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const timers = { setTimeout, clearTimeout, setImmediate };

// ---- the gate on its own, with a fake native side that answers when told
function fakeNative() {
  const seen = [], open = [];
  const call = (name, args) => new Promise((resolve, reject) => { seen.push(name); open.push({ name, args, resolve, reject }); });
  return { seen, open, call };
}

async function gateAlone() {
  const { orderedCalls } = (() => { const s = { module: { exports: {} }, console, ...timers }; vm.runInNewContext(ui('bridge.js'), s); return s.module.exports; })();
  // a multi-write operation (Performance mode's three keys), then PLAY at once
  {
    const n = fakeNative();
    const call = orderedCalls(n.call, 2000);
    const op = call.operation(async () => {
      for (const k of ['shadows', 'lens_flare', 'particles']) await call('save_setting', { key: k, value: '0' });
    });
    const played = call('play', {});
    const editor = call('play', { edit_touch_layout: true });   // the editor row during the transition
    assert.strictEqual(editor, played, 'a second handoff is the same one');
    for (let i = 0; i < 3; i++) {
      await sleep(5);
      assert.ok(!n.seen.includes('play'), `play waits: write ${i + 1} of 3 still to come`);
      n.open[i].resolve(null);
    }
    await op;
    await sleep(5);
    assert.deepStrictEqual(n.seen, ['save_setting', 'save_setting', 'save_setting', 'play']);
    // once PLAY went out: no new operation, no write
    await call.operation(async () => {}).then(() => assert.fail('no operation after the handoff began'), (e) => assert.strictEqual(e.code, 'footer.handoffBusy'));
    await call('save_setting', { key: 'fps', value: '60' }).then(() => assert.fail('no write after play went out'), (e) => assert.strictEqual(e.code, 'footer.handoffBusy'));
    n.open[3].resolve(null);
    assert.strictEqual(await played, null);
    assert.deepStrictEqual(n.seen, ['save_setting', 'save_setting', 'save_setting', 'play'], 'one play');
    // after the handoff (the desktop's launcher stays): settings can be changed again
    const later = call('save_setting', { key: 'fps', value: '60' });
    n.open[4].resolve(null);
    await later;
  }
  // a failed write counts as answered; a read is never held back
  {
    const n = fakeNative();
    const call = orderedCalls(n.call, 2000);
    call('save_setting', { key: 'fps', value: '120' }).catch(() => {});
    const played = call('play', {});
    call('snapshot');
    await sleep(5);
    assert.deepStrictEqual(n.seen, ['save_setting', 'snapshot']);
    n.open[0].reject({ code: 'x' });
    await sleep(5);
    assert.deepStrictEqual(n.seen, ['save_setting', 'snapshot', 'play']);
    n.open[2].resolve(null);
    await played;
  }
  // a save that never answers: the handoff is cancelled after the deadline, not performed; the save
  // stays pending, and the next PLAY waits for it again
  {
    const n = fakeNative();
    const call = orderedCalls(n.call, 120);
    call('save_setting', { key: 'fps', value: '120' });
    const t0 = Date.now();
    await call('play', {}).then(() => assert.fail('no game over an unfinished write'), (e) => assert.strictEqual(e.code, 'footer.handoffStuck'));
    assert.ok(Date.now() - t0 >= 110, 'cancelled at the deadline');
    assert.deepStrictEqual(n.seen, ['save_setting'], 'play never went out');
    const again = call('play', {});
    await sleep(20);
    assert.deepStrictEqual(n.seen, ['save_setting'], 'the retry waits for the same save');
    n.open[0].resolve(null);
    await sleep(5);
    assert.deepStrictEqual(n.seen, ['save_setting', 'play']);
    n.open[1].resolve(null);
    await again;
    // an operation stuck between two writes is waited for the same way
    const stuck = call.operation(() => new Promise(() => {}));
    await call('install_update', {}).then(() => assert.fail(), (e) => assert.strictEqual(e.code, 'footer.handoffStuck'));
    assert.ok(stuck);
  }
}

// ---- each transport with slow saves: a three-write operation, then PLAY and the editor row at once
const SLOW = 25;
async function transportCase(name, bridge, seen) {
  const op = bridge.operation(async () => {
    for (const k of ['shadows', 'lens_flare', 'particles']) await bridge.call('save_setting', { key: k, value: '0' });
  });
  const played = bridge.call('play', {});
  bridge.call('play', { edit_touch_layout: true });
  await op;
  await played;
  await sleep(SLOW * 2);
  assert.deepStrictEqual(seen.filter((c) => c !== 'snapshot'), ['save_setting', 'save_setting', 'save_setting', 'play'], `${name}: every write before play, one play`);
}

async function tauri() {
  const seen = [];
  const page = {
    window: { __TAURI__: { core: { invoke: async (name) => { seen.push(name); await sleep(SLOW); return null; } }, event: { listen: async () => () => {} } } },
    module: { exports: {} }, console, ...timers,
  };
  vm.createContext(page);
  vm.runInContext(ui('bridge.js'), page);
  const bridge = vm.runInContext('bridge', page);
  assert.strictEqual(bridge.kind, 'tauri');
  await transportCase('desktop (Tauri)', bridge, seen);
}

async function android() {
  const seen = [];
  const listeners = [];
  const window = { addEventListener: (type, f) => listeners.push(f) };
  const page = { window, module: { exports: {} }, console, ...timers };
  vm.runInNewContext(ui('bridge.js'), page);
  const bridge = page.module.exports.messageBridge(window);
  // LauncherBridge: answers each call after SLOW ms, in arrival order (its settings queue)
  const port = {
    start() {},
    postMessage(text) {
      const m = JSON.parse(text);
      seen.push(m.call);
      setTimeout(() => port.onmessage({ data: JSON.stringify({ id: m.id, ok: true, value: null }) }), SLOW);
    },
  };
  listeners.forEach((f) => f({ data: 'dhd-bridge', ports: [port] }));
  await bridge.ready;
  await transportCase('Android (MessagePort)', bridge, seen);
}

async function ios() {
  const seen = [];
  const listeners = [];
  let deliver = null;
  const window = {
    addEventListener: (type, f) => { if (type === 'message') listeners.push(f); },
    postMessage: (data, origin, ports) => setImmediate(() => listeners.forEach((f) => f({ data, ports }))),
    webkit: { messageHandlers: { dhd: { postMessage: (text) => {
      const m = JSON.parse(text);
      seen.push(m.call);
      setTimeout(() => deliver(JSON.stringify({ id: m.id, ok: true, value: null })), SLOW);
    } } } },
  };
  const page = { window, module: { exports: {} }, console, ...timers };
  vm.runInNewContext(ui('bridge.js'), page);
  const bridge = page.module.exports.messageBridge(window);
  const appWorld = { window, MessageChannel, console };
  vm.runInNewContext(fs.readFileSync(path.join(root, 'port/ios/app/bridge_ios.js'), 'utf8'), appWorld);
  deliver = appWorld.dhdDeliver;
  await bridge.ready;
  await transportCase('iOS (bridge_ios.js)', bridge, seen);
}

// ---- app.js's own handlers, the page's scripts loaded as index.html loads them, with stand-ins for
// the drawing (every DOM call answers a do-nothing element) and a Tauri stand-in as the native side
function pageWithApp(invoke) {
  const nothing = new Proxy(function () {}, {
    get: (target, key) => (key === Symbol.toPrimitive ? () => '' : key === Symbol.iterator ? function* () {} : key === 'then' ? undefined : nothing),
    apply: () => nothing,
    set: () => true,
  });
  const window = {
    __TAURI__: { core: { invoke }, event: { listen: async () => () => {} } },
    addEventListener() {}, matchMedia: () => nothing,
  };
  const page = {
    window, document: nothing, navigator: { languages: ['en'] }, addEventListener() {}, getComputedStyle: () => nothing,
    requestAnimationFrame: () => 0, console: { ...console, warn() {} }, Intl, ...timers,
  };
  vm.createContext(page);
  const order = [...ui('index.html').matchAll(/<script src="([^"]+)"><\/script>/g)].map((m) => m[1]).filter((f) => f !== 'pad.js');
  for (const f of order) vm.runInContext(ui(f), page, { filename: f });
  // the drawing and the footer's words are stand-ins; the logic under test is app.js's
  vm.runInContext(`renderAll = renderRows = renderFooter = renderTabs = () => {};
    globalThis.flashed = []; globalThis.failed = [];
    flash = (text) => flashed.push(text); fail = (text, fix) => failed.push({ text, fix });
    t = (key) => key;`, page);
  return page;
}

async function appHandlers() {
  // Reset of a tab whose rows are a setting and Performance mode (three keys), then PLAY at once
  {
    const seen = [];
    const page = pageWithApp(async (name) => {
      if (name === 'snapshot') return new Promise(() => {});   // the page's own start waits for ever
      seen.push(name);
      await sleep(SLOW);
      return null;
    });
    vm.runInContext(`tabs = [['graphics', [{ key: 'fps', type: 'select' }, { key: 'shadows', type: 'performance' }]]];
      state.tab = 0;
      state.schema = { fps: { default: '60' }, shadows: { default: '1' }, lens_flare: { default: '1' }, particles: { default: '1' } };
      caps = new Set(['touch-settings']);`, page);
    const reset = vm.runInContext('resetTab()', page);
    const played = vm.runInContext('play()', page);
    vm.runInContext('play({ editTouchLayout: true })', page);
    await reset;
    await played;
    await sleep(SLOW * 2);
    assert.deepStrictEqual(seen, ['save_setting', 'save_setting', 'save_setting', 'save_setting', 'play'], 'resetTab: its four writes, then one play');
    assert.strictEqual(vm.runInContext('JSON.stringify(failed)', page), '[]');
  }
  // a save that never answers: PLAY is cancelled at the gate's deadline with its message and a
  // retry; the retry, once the save has answered, starts the game
  {
    const seen = [];
    let answerSave = null;
    const page = pageWithApp((name) => {
      if (name === 'snapshot') return new Promise(() => {});
      seen.push(name);
      if (name === 'save_setting') return new Promise((resolve) => { answerSave = resolve; });
      return Promise.resolve(null);
    });
    vm.runInContext(`caps = new Set(['touch-settings']); save('fps', '120', 'x');`, page);
    await vm.runInContext('play()', page);
    assert.deepStrictEqual(seen, ['save_setting'], 'no play over an unfinished write');
    const failure = vm.runInContext('failed[0]', page);
    assert.strictEqual(failure.text, 'footer.handoffStuck', 'the message is shown');
    assert.strictEqual(failure.fix.label, 'footer.tryAgain', 'with a retry');
    answerSave(null);
    await failure.fix.onclick();
    assert.deepStrictEqual(seen, ['save_setting', 'play']);
  }
}

(async () => {
  await gateAlone();
  await tauri();
  await android();
  await ios();
  await appHandlers();
  console.log('test_handoff: ok');
  process.exit(0);   // iOS's MessageChannel ports would keep node waiting
})().catch((e) => { console.error(e); process.exit(1); });
