// The iOS bridge's page side (port/ios/app/bridge_ios.js, finding 227) with the page's own
// bridge.js: the app's script posts the port, the page's calls reach the app's message handler as
// {id, call, args} text, and the app's answers and events (dhdDeliver) settle them.
//     node port/ios/tests/test_bridge_ios.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const root = path.join(__dirname, '..', '..', '..');
const listeners = [];
const toApp = [];
// one window both worlds share (the page's and the app's script's); postMessage as a browser does
const window = {
  addEventListener: (type, f) => { if (type === 'message') listeners.push(f); },
  postMessage: (data, origin, ports) => setImmediate(() => listeners.forEach((f) => f({ data, ports }))),
  webkit: { messageHandlers: { dhd: { postMessage: (text) => toApp.push(JSON.parse(text)) } } },
};
const page = { window, module: { exports: {} }, console, setTimeout, setImmediate };
vm.runInNewContext(fs.readFileSync(path.join(root, 'port/launcher/ui/bridge.js'), 'utf8'), page);
const bridge = page.module.exports.messageBridge(window);
const appWorld = { window, MessageChannel, console };
const first = bridge.call('snapshot');   // before the app's script ran: queued
vm.runInNewContext(fs.readFileSync(path.join(root, 'port/ios/app/bridge_ios.js'), 'utf8'), appWorld);
assert.strictEqual(typeof appWorld.dhdDeliver, 'function');

// without the handler (another page, a test browser) the script does nothing
const bare = { window: { postMessage: () => assert.fail('no port without the app'), addEventListener() {} }, MessageChannel };
vm.runInNewContext(fs.readFileSync(path.join(root, 'port/ios/app/bridge_ios.js'), 'utf8'), bare);
assert.strictEqual(bare.dhdDeliver, null);

const wait = () => new Promise((r) => setTimeout(r, 20));
(async () => {
  await bridge.ready;
  await wait();
  assert.deepStrictEqual(toApp[0], { id: 1, call: 'snapshot', args: {} });
  const saved = bridge.call('save_setting', { key: 'perf_counter', value: '1' });
  let events = [];
  bridge.on('disc-import', (p) => events.push(p));
  await wait();
  assert.deepStrictEqual(toApp[1], { id: 2, call: 'save_setting', args: { key: 'perf_counter', value: '1' } });
  appWorld.dhdDeliver(JSON.stringify({ id: 2, ok: true, value: null }));
  appWorld.dhdDeliver(JSON.stringify({ id: 1, ok: true, value: { version: '0.8.0', caps: ['disc-import'] } }));
  appWorld.dhdDeliver(JSON.stringify({ event: 'disc-import', payload: { job: 1, stage: 'copying', done: 1, total: 2 } }));
  assert.strictEqual((await first).version, '0.8.0');
  assert.strictEqual(await saved, null);
  await wait();
  assert.strictEqual(events.length, 1);
  assert.strictEqual(events[0].stage, 'copying');
  const failed = bridge.call('open_link', { url: 'https://example.com/' });
  await wait();
  appWorld.dhdDeliver(JSON.stringify({ id: 3, ok: false, error: 'not opening https://example.com/' }));
  await failed.then(() => assert.fail('an error rejects'), (e) => assert.match(e, /not opening/));
  // PLAY right after saves the page didn't wait for: they reach the app in the order sent, which is
  // the order the app's settings tickets follow (launcher_calls.c), so the game starts after them
  // (the page itself holds PLAY back until the saves have answered: finding 228's bridge.js)
  const before = toApp.length;
  bridge.call('save_setting', { key: 'scale', value: '3' });
  bridge.call('save_setting', { key: 'fps', value: '120' });
  bridge.call('play', {});
  await wait();
  const sent = () => toApp.slice(before).map((m) => m.call + (m.args.key ? ':' + m.args.key : ''));
  assert.deepStrictEqual(sent(), ['save_setting:scale', 'save_setting:fps']);   // PLAY waits for the answers
  for (const m of toApp.slice(before)) appWorld.dhdDeliver(JSON.stringify({ id: m.id, ok: true, value: null }));
  await wait();
  assert.deepStrictEqual(sent(), ['save_setting:scale', 'save_setting:fps', 'play']);
  console.log('bridge_ios: the page and the app talk over the posted port');
  process.exit(0);   // the channel's ports would keep node waiting
})().catch((e) => { console.error(e); process.exit(1); });
