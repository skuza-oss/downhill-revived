// The launcher page's own logic without a window (finding 225): the string table's helpers
// (ui/i18n.js), the phones' bridge protocol (ui/bridge.js) and the disc import's states
// (ui/disc_flow.js); and that the page's scripts share one scope without a name declared twice.
//     node port/launcher/test_launcher_ui.js
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const assert = require('assert');

const ui = (f) => fs.readFileSync(path.join(__dirname, 'ui', f), 'utf8');
function load(file, extra = {}) {
  const sandbox = { module: { exports: {} }, console, ...extra };
  vm.runInNewContext(ui(file), sandbox);
  return sandbox.module.exports;
}

// ---- the page's scripts, in index.html's order, as one script: a name declared twice fails here
const order = [...ui('index.html').matchAll(/<script src="([^"]+)"><\/script>/g)].map((m) => m[1]);
assert.deepStrictEqual(order, ['i18n.js', 'bridge.js', 'disc_flow.js', 'stick.js', 'controls.js', 'app.js', 'pad.js']);
new vm.Script(order.map(ui).join('\n;\n'), { filename: 'page.js' });

// ---- i18n
const i = load('i18n.js');
const en = JSON.parse(ui('lang/en.json'));
assert.ok(i.LANGUAGES.includes('en'));
// the language: saved, else the system's (exact, base, a sibling), else English
assert.strictEqual(i.pickLanguage('', ['de-DE', 'en'], ['en', 'de']), 'de');
assert.strictEqual(i.pickLanguage('', ['pt-BR'], ['en', 'pt-BR']), 'pt-BR');
assert.strictEqual(i.pickLanguage('', ['pt-PT'], ['en', 'pt-BR']), 'pt-BR', 'a sibling of the same language');
assert.strictEqual(i.pickLanguage('', ['zh-Hans-CN', 'fr'], ['en', 'zh-Hans', 'fr']), 'zh-Hans');
assert.strictEqual(i.pickLanguage('', ['zh-TW', 'fr'], ['en', 'zh-Hans', 'fr']), 'fr', 'no Simplified Chinese for Traditional');
assert.strictEqual(i.pickLanguage('', ['ja'], ['en']), 'en');
assert.strictEqual(i.pickLanguage('fr', ['de'], ['en', 'fr', 'de']), 'fr', 'dhd.ini language first');
assert.strictEqual(i.pickLanguage('xx', ['de'], ['en', 'de']), 'de', 'a saved language without a file: the system');
// every shipped language is shown under its own name, as its speakers write it, whichever is active
assert.strictEqual(JSON.stringify(Object.keys(i.LANGUAGE_NAMES)), JSON.stringify(i.LANGUAGES), 'a name for each language, in the row\'s order');
assert.strictEqual(JSON.stringify(i.LANGUAGES.map((c) => i.languageName(c))),
  JSON.stringify(['English', 'Português (Brasil)', 'Español', 'Français', 'Deutsch', 'Italiano', '日本語', '简体中文']));
assert.strictEqual(i.languageName('xx'), 'xx', 'no name: the code');
// the whole list: Chinese by script and region, Portuguese, and the other languages by their base
const all = (...wanted) => i.pickLanguage('', wanted);
for (const w of ['zh', 'zh-CN', 'zh-SG', 'zh-Hans', 'zh-Hans-CN', 'zh-Hans-SG', 'zh-cn', 'zh_CN']) assert.strictEqual(all(w), 'zh-Hans', w);
for (const w of ['zh-TW', 'zh-HK', 'zh-MO', 'zh-Hant', 'zh-Hant-TW', 'zh-Hant-HK']) assert.strictEqual(all(w), 'en', `${w}: no Simplified for Traditional`);
assert.strictEqual(all('zh-TW', 'de'), 'de', 'Traditional passes on to the next language');
assert.strictEqual(i.pickLanguage('', ['zh-Hant-TW'], ['en', 'zh-Hans', 'zh-Hant']), 'zh-Hant', 'a Traditional file, when one exists');
assert.strictEqual(i.pickLanguage('', ['zh-TW'], ['en', 'zh-Hans', 'zh-Hant']), 'zh-Hant');
for (const w of ['pt', 'pt-PT', 'pt-BR', 'pt-AO']) assert.strictEqual(all(w), 'pt-BR', w);
for (const [w, code] of [['es', 'es'], ['es-ES', 'es'], ['es-MX', 'es'], ['es-419', 'es'], ['fr-FR', 'fr'], ['fr-CA', 'fr'], ['de-AT', 'de'],
  ['de-CH', 'de'], ['it-IT', 'it'], ['it-CH', 'it'], ['ja', 'ja'], ['ja-JP', 'ja'], ['en-GB', 'en'], ['en-US', 'en'], ['ko-KR', 'en']]) {
  assert.strictEqual(all(w), code, w);
}
assert.strictEqual(all('ko', 'es-MX', 'de'), 'es', 'the first language with a file');
assert.strictEqual(all('en-GB', 'de'), 'en', 'English first stays English');
assert.strictEqual(i.pickLanguage('de-AT', ['fr']), 'de', 'dhd.ini may name a region too');
assert.strictEqual(i.pickLanguage('zh-CN', ['fr']), 'zh-Hans');
assert.strictEqual(i.pickLanguage('', []), 'en');
assert.strictEqual(i.pickLanguage('', undefined), 'en');
// words: placeholders, English for a key a language lacks, the key itself when none has it
i.lang.english = en;
i.lang.words = { 'home.version': 'Versión {version}' };
i.lang.code = 'es';
assert.strictEqual(i.t('home.version', { version: '1.2.3' }), 'Versión 1.2.3');
assert.strictEqual(i.t('footer.play'), 'Play');
assert.strictEqual(i.t('no.such.key'), 'no.such.key');
assert.strictEqual(i.t('home.newIn', { version: '0.9.0' }), 'New in 0.9.0', 'bold markers are dropped in plain text');
assert.strictEqual(i.fill('{a} and {b}', { a: 1 }), '1 and {b}');
i.lang.words = en;
i.lang.code = 'en';
assert.strictEqual(i.tn('server.races', 1), '1 public race');
assert.strictEqual(i.tn('server.races', 3), '3 public races');
// every source string: whole, named placeholders only, bold spans paired
for (const [k, v] of Object.entries(en)) {
  assert.strictEqual(typeof v, 'string', k);
  assert.ok(!/\{\s*\}|\{\d+\}|%s|%d/.test(v), `${k}: placeholders have names`);
  assert.strictEqual((v.match(/\*\*/g) || []).length % 2, 0, `${k}: bold spans paired`);
}

// ---- the phones' bridge: calls answered by id, events, messages before the port queued
const listeners = [];
const fakeWindow = { addEventListener: (type, f) => listeners.push(f) };
const b = load('bridge.js').messageBridge(fakeWindow);
const sent = [];
const first = b.call('snapshot');               // before the port: queued
let event = null;
b.on('disc-import', (p) => { event = p; });
const port = { postMessage: (m) => sent.push(JSON.parse(m)), start() {} };
listeners.forEach((f) => f({ data: 'something else', ports: [port] }));  // not the bridge's message
assert.strictEqual(sent.length, 0);
listeners.forEach((f) => f({ data: 'dhd-bridge', ports: [port] }));
assert.deepStrictEqual(sent[0], { id: 1, call: 'snapshot', args: {} });
const second = b.call('save_setting', { key: 'fps', value: '120' });
assert.deepStrictEqual(sent[1], { id: 2, call: 'save_setting', args: { key: 'fps', value: '120' } });
(async () => {
  port.onmessage({ data: JSON.stringify({ id: 2, ok: false, error: { code: 'noRoom', params: { need: '3 GB' } } }) });
  port.onmessage({ data: JSON.stringify({ id: 1, ok: true, value: { version: '0.8.0' } }) });
  port.onmessage({ data: JSON.stringify({ event: 'disc-import', payload: { job: 1, stage: 'checking' } }) });
  port.onmessage({ data: 'not json' });
  assert.strictEqual(JSON.stringify(await first), '{"version":"0.8.0"}');  // (another realm's objects)
  await second.then(() => assert.fail('an error answer rejects'), (e) => assert.strictEqual(e.code, 'noRoom'));
  assert.strictEqual(JSON.stringify(event), '{"job":1,"stage":"checking"}');
  await b.ready;

  // ---- the disc import's states
  const d = load('disc_flow.js');
  let s = d.discIdle();
  assert.ok(!d.discBusy(s));
  s = d.discFlow(s, { job: 1, stage: 'checking' });
  assert.ok(d.discBusy(s));
  s = d.discFlow(s, { job: 1, stage: 'copying', done: 100, total: 1000 });
  assert.deepStrictEqual({ ...s }, { stage: 'copying', job: 1, done: 100, total: 1000 });
  assert.strictEqual(d.discFlow(s, { job: 1, stage: 'copying', done: 50, total: 1000 }), s, 'never backwards');
  // a second pick replaces the first: the first's late events count no more
  s = d.discFlow(s, { job: 2, stage: 'checking' });
  assert.strictEqual(d.discFlow(s, { job: 1, stage: 'copying', done: 900, total: 1000 }), s);
  assert.strictEqual(d.discFlow(s, { job: 1, stage: 'problem', problem: 'x' }), s);
  s = d.discFlow(s, { job: 2, stage: 'done', deletable: true, size: 2600000000 });
  assert.ok(!d.discBusy(s) && s.deletable && s.size === 2600000000);
  assert.strictEqual(d.discFlow(s, { job: 2, stage: 'copying', done: 1, total: 2 }), s, 'a finished job stays finished');
  s = d.discFlow(s, { job: 2, stage: 'kept' });
  assert.strictEqual(s.stage, 'kept');
  // a problem, then a new pick
  s = d.discFlow(s, { job: 3, stage: 'problem', problem: { code: 'noRoom', params: {} } });
  assert.strictEqual(s.problem.code, 'noRoom');
  assert.strictEqual(d.discFlow(s, { job: 3, stage: 'kept' }), s, 'kept only answers a done job');
  s = d.discFlow(s, { job: 4, stage: 'checking' });
  assert.strictEqual(s.stage, 'checking');
  // nonsense is ignored
  assert.strictEqual(d.discFlow(s, null), s);
  assert.strictEqual(d.discFlow(s, { job: 4, stage: 'flying' }), s);
  assert.strictEqual(d.discFlow(s, { stage: 'done' }), s);
  console.log('test_launcher_ui: ok');
})().catch((e) => { console.error(e); process.exit(1); });
