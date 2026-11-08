// The Controls tab's page logic (ui/controls.js, finding 157) without a window: a pad's kind, the
// words for a layout's inputs, a key and a controller button pressed while a binding waits.
//     node port/launcher/test_controls.js [schema.json]
// schema.json: the launcher's controls_schema and the default layouts, as its test writes them
// (DHD_CONTROLS_SCHEMA=schema.json cargo test schema_for_the_page in port/launcher/src-tauri);
// without it the test uses a small schema of the same shape.
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const sandbox = { module: { exports: {} }, addEventListener: () => {}, navigator: {}, performance: { now: () => 0 }, console };
vm.runInNewContext(fs.readFileSync(path.join(__dirname, 'ui', 'controls.js'), 'utf8'), sandbox);
const c = sandbox.module.exports;

const given = process.argv[2] ? JSON.parse(fs.readFileSync(process.argv[2], 'utf8')) : null;
const schema = {  // a small one for the rules below; the launcher's own is checked at the end
  targets: [{ label: 'Cross', race: 'Pedal' }, { label: 'Circle', race: 'Kick' }, { label: 'Left Stick Up', race: 'Lean' }],
  buttons: 2,
  keys: [{ vk: 0x20, label: 'Space', code: 'Space' }, { vk: 0x58, label: 'X', code: 'KeyX' }, { vk: 0x0D, label: 'Enter', code: 'Enter' }],
  pads: [{ id: 1, labels: ['A', 'Cross', 'B'], web: 0 }, { id: 2, labels: ['B', 'Circle', 'A'], web: 1 },
         { id: 21, labels: ['LT', 'L2', 'ZL'], web: 6 }, { id: 15, labels: ['Share', 'Mic', 'Capture'], web: -1 }],
  devices: [{ label: 'Keyboard' }, { label: 'Controller 1' }, { label: 'Controller 2' }],
};

// the kind by the browser's pad name: the words the pad prints
assert.strictEqual(c.padKind('DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)'), 1);
assert.strictEqual(c.padKind('Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)'), 2);
assert.strictEqual(c.padKind('Pro Controller (STANDARD GAMEPAD Vendor: 057e Product: 2009)'), 3);
assert.strictEqual(c.padKind(''), 2);

// words: a key's label, a controller button's by kind, '' for nothing
assert.strictEqual(c.inputWord(schema, 0, 0x20, 2), 'Space');
assert.strictEqual(c.inputWord(schema, 1, 1, 1), 'Cross');
assert.strictEqual(c.inputWord(schema, 2, 1, 3), 'B');
assert.strictEqual(c.inputWord(schema, 2, 21, 2), 'LT');
assert.strictEqual(c.inputWord(schema, 0, 0, 2), '');

// a key by KeyboardEvent.code (the place), the keypad's Enter as Enter; Esc and others: none
assert.strictEqual(c.keyOfCode(schema, 'KeyX'), 0x58);
assert.strictEqual(c.keyOfCode(schema, 'NumpadEnter'), 0x0D);
assert.strictEqual(c.keyOfCode(schema, 'Escape'), null);
assert.strictEqual(c.keyOfCode(schema, 'ControlLeft'), null);

// a controller button: the first one pressed since the wait began (one held then doesn't count)
assert.strictEqual(c.padInputPressed(schema, new Set([0]), new Set([0])), null);
assert.strictEqual(c.padInputPressed(schema, new Set([0]), new Set([0, 1])), 2);
assert.strictEqual(c.padInputPressed(schema, new Set(), new Set([6])), 21);
assert.strictEqual(c.padInputPressed(schema, new Set(), new Set([16])), null, 'Guide is not bound');

// the rows: a controller has the buttons only, the keyboard the stick directions too
const layout = { inputs: [[0x20, 0x58], [0, 0], [0x0D, 0]] };
const keys = c.bindingRows(schema, layout, 0, 2);
assert.strictEqual(keys.length, 3);
assert.deepStrictEqual([...keys[0].words], ['Space', 'X']);
assert.deepStrictEqual([...keys[1].words], ['', '']);
const pad = c.bindingRows(schema, { inputs: [[2, 0], [1, 21], [0, 0]] }, 1, 1);
assert.strictEqual(pad.length, 2);
assert.deepStrictEqual([...pad[1].words], ['Cross', 'L2']);

// the launcher's own tables: the default layouts read as the game's old ones (docs/PLAYING.md)
if (given) {
  const [keyboard, pad1] = given.state.layouts;
  const kb = c.bindingRows(given.schema, keyboard, 0, 2), p1 = c.bindingRows(given.schema, pad1, 1, 1);
  assert.strictEqual(kb.length, 24);
  assert.strictEqual(p1.length, 16);
  assert.deepStrictEqual([...kb[0].words], ['Space', 'X']);
  assert.deepStrictEqual([...kb[3].words], ['S', 'Backspace']);
  assert.deepStrictEqual([...kb[15].words], ['Right', '']);
  assert.deepStrictEqual([...p1[0].words], ['Cross', '']);
  assert.deepStrictEqual([...c.bindingRows(given.schema, pad1, 1, 2)[6].words], ['LT', '']);
  assert.strictEqual(c.keyOfCode(given.schema, 'Numpad5'), 0x65);
  assert.strictEqual(c.keyOfCode(given.schema, 'Escape'), null);
  assert.strictEqual(c.padInputPressed(given.schema, new Set(), new Set([7])), 22, 'RT');
}
console.log(`test_controls: ok${given ? " (the launcher's schema)" : ''}`);
