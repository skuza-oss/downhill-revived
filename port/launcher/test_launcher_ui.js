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
assert.deepStrictEqual(order, ['boot.js', 'i18n.js', 'bridge.js', 'disc_flow.js', 'steps.js', 'stick.js', 'controls.js', 'app.js', 'pad.js']);
new vm.Script(order.map(ui).join('\n;\n'), { filename: 'page.js' });
assert.ok(ui('index.html').indexOf('<script src="boot.js">') < ui('index.html').indexOf('<body'), 'boot.js runs before the body');

// ---- the oldest WebView the page runs on (finding 273): Chromium 80 (Android System WebView 80).
// A denylist, no parser: each construct newer than 80 the page could plausibly use, with the first
// Chromium that has it. A match fails the test: rewrite the place (no transpiler).
const MIN_CHROMIUM = 80;
const code = (text) => text.split('\n').filter((l) => !/^\s*(\/\/|\*|\/\*)/.test(l)).map((l) => l.replace(/\s\/\/\s.*$/, '')).join('\n');
const tooNewJs = [
  [/\.replaceChildren\(/, 'Element.replaceChildren', 86], [/\.at\(/, 'Array/String.at', 92],
  [/\.roundRect\(/, 'canvas roundRect', 99], [/\.replaceAll\(/, 'String.replaceAll', 85],
  [/\?\?=|\|\|=|&&=/, 'logical assignment', 85], [/\bstructuredClone\b/, 'structuredClone', 98],
  [/\bObject\.hasOwn\b/, 'Object.hasOwn', 93], [/\.findLast(Index)?\(/, 'Array.findLast', 97],
  [/\.(toSorted|toReversed|toSpliced)\(/, 'change-array-by-copy', 110], [/\bPromise\.any\b|\bAggregateError\b/, 'Promise.any', 85],
  [/\bWeakRef\b|\bFinalizationRegistry\b/, 'WeakRef', 84], [/\bthis\.#|^\s*(static\s+)?#[A-Za-z_]\w*\s*[(=;]/m, 'private class members', 84],
  [/\bstatic\s*\{/, 'class static block', 94], [/^await\b/m, 'top-level await', 89],
  [/\bIntl\.(DisplayNames|Segmenter)\b/, 'Intl.DisplayNames / Segmenter', 81], [/\bAbortSignal\.(timeout|any)\b/, 'AbortSignal.timeout', 103],
  [/\.checkVisibility\(|\.showPopover\(|\.inert\b/, 'checkVisibility / popover / inert', 102],
  [/\b(Object|Map)\.groupBy\b|\bPromise\.withResolvers\b|\bArray\.fromAsync\b/, 'groupBy / withResolvers / fromAsync', 117],
];
for (const f of order.filter((f) => f !== 'boot.js')) {
  const text = code(ui(f));
  for (const [re, what, since] of tooNewJs) assert.ok(!re.test(text), `${f}: ${what} needs Chromium ${since}, the page's floor is ${MIN_CHROMIUM}`);
}
const css = ui('style.css').replace(/\/\*[\s\S]*?\*\//g, '');
for (const [re, what, since] of [
  [/(^|[;{\s])inset\s*:/, 'inset', 87], [/(^|[;{\s])(translate|scale|rotate)\s*:/, 'individual transform properties', 104],
  [/:(is|where)\(/, ':is() / :where()', 88], [/:has\(/, ':has()', 105], [/aspect-ratio\s*:/, 'aspect-ratio', 88],
  [/\d(dvh|svh|lvh|dvw|svw|lvw)\b/, 'dynamic viewport units', 108], [/color-mix\(/, 'color-mix()', 111],
  [/@container|@layer/, '@container / @layer', 99], [/accent-color\s*:/, 'accent-color', 93], [/text-wrap\s*:/, 'text-wrap', 114],
  [/overflow(-[xy])?\s*:\s*clip/, 'overflow: clip', 90], [/(^|\s)&[\s.:#[]/m, 'CSS nesting', 112],
]) assert.ok(!re.test(css), `style.css: ${what} needs Chromium ${since}, the page's floor is ${MIN_CHROMIUM}`);
for (const rule of css.split('}')) {
  assert.ok(!/(^|[;{\s])appearance\s*:/.test(rule) || /-webkit-appearance\s*:/.test(rule), 'style.css: appearance needs -webkit-appearance beside it (Chromium 84)');
}
// boot.js is the safety net for older WebViews: ES5 only, and its floor is the page's
{
  const boot = code(ui('boot.js')).replace(/'[^'\n]*'/g, "''");
  for (const [re, what] of [[/=>/, 'arrow function'], [/\b(let|const|class)\s/, 'let / const / class'], [/`/, 'template string'],
    [/\.\.\./, 'spread'], [/\basync\b|\bawait\b/, 'async'], [/\?\.|\?\?/, '?. / ??'], [/\bfor\s*\([^)]*\bof\b/, 'for...of']]) {
    assert.ok(!re.test(boot), `boot.js must stay ES5: ${what}`);
  }
  assert.strictEqual(load('boot.js').minChromium, MIN_CHROMIUM, "boot.js's MIN_CHROMIUM is the test's floor");
}
// boot.js in a page: an old Android WebView gets the message and the Play Store button at once; a
// new one nothing until a start fails; nothing after the start
function bootPage(ua) {
  const listeners = {};
  const node = (tag) => ({ tag, kids: [], className: '', on: {}, appendChild(k) { this.kids.push(k); }, addEventListener(e, f) { this.on[e] = f; } });
  const body = node('body');
  const page = {
    navigator: { userAgent: ua, maxTouchPoints: 5 },
    document: { documentElement: { className: '' }, body, createElement: node, createTextNode: (text) => ({ text }), addEventListener() {} },
    window: { addEventListener: (e, f) => { listeners[e] = f; } },
  };
  vm.createContext(page);
  vm.runInContext(ui('boot.js'), page);
  const words = (n) => (n.text || '') + (n.kids || []).map(words).join(' ');
  return { page, listeners, body, words };
}
{
  const old = bootPage('Mozilla/5.0 (Linux; Android 10; Redmi Note 8; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/74.0.3729.186 Mobile Safari/537.36');
  assert.ok(/\btouch\b/.test(old.page.document.documentElement.className), 'a phone layout before the page starts');
  assert.strictEqual(old.body.kids.length, 1);
  assert.ok(/your version: 74, needed: 80/.test(old.words(old.body.kids[0])), old.words(old.body.kids[0]));
  const sent = [];
  old.body.kids[0].kids[2].on.click();   // before the bridge's port: queued
  old.listeners.message({ data: 'dhd-bridge', ports: [{ postMessage: (t) => sent.push(JSON.parse(t)) }] });
  assert.deepStrictEqual(sent, [{ id: 0, call: 'open_link', args: { url: 'webview' } }]);
  old.listeners.error({ error: new Error('later') });
  assert.strictEqual(old.body.kids.length, 1, 'one message');

  const fresh = bootPage('Mozilla/5.0 (Linux; Android 14; Pixel 8; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/131.0.0.0 Mobile Safari/537.36');
  assert.strictEqual(fresh.body.kids.length, 0);
  fresh.listeners.unhandledrejection({ reason: new Error('snapshot broke') });
  assert.ok(/could not start.*snapshot broke/.test(fresh.words(fresh.body.kids[0])), fresh.words(fresh.body.kids[0]));

  const desk = bootPage('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0');
  assert.ok(!/touch/.test(desk.page.document.documentElement.className), 'the desktop keeps its layout');
  vm.runInContext('dhdBoot.started = true', desk.page);
  desk.listeners.error({ error: new Error('after the start') });
  assert.strictEqual(desk.body.kids.length, 0, 'an error after the start is not the safety net\'s');
}

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
// the launcher names a setting as the game's own SETTINGS page does (port/hle/pc_settings.c)
const gamePage = fs.readFileSync(path.join(__dirname, '..', 'hle', 'pc_settings.c'), 'utf8');
for (const [key, label] of [['row.scale', 'Resolution'], ['row.window', 'Display'], ['row.size', 'Window Size'], ['row.aspect', 'Picture'],
  ['row.scaling', 'Scaling'], ['row.widescreen', 'Widescreen'], ['row.brightness', 'Brightness'], ['row.contrast', 'Contrast'],
  ['row.saturation', 'Saturation'], ['row.gamma', 'Gamma']]) {
  assert.ok(gamePage.includes(`{"${label}:"`), `${label} is a row of the game's page`);
  assert.strictEqual(en[key].toLowerCase(), label.toLowerCase(), `${key} reads as the game's ${label} (case aside)`);
}
assert.ok(en['row.scaling.hint'], 'the Scaling row says what it does');
// every source string: whole, named placeholders only, bold spans paired
for (const [k, v] of Object.entries(en)) {
  assert.strictEqual(typeof v, 'string', k);
  assert.ok(!/\{\s*\}|\{\d+\}|%s|%d/.test(v), `${k}: placeholders have names`);
  assert.strictEqual((v.match(/\*\*/g) || []).length % 2, 0, `${k}: bold spans paired`);
}

// ---- the picture settings (finding 275): Graphics' four stepped rows and Reset Picture, the game's
// left/right as - and +: one choice, stopping at the ends
const app = ui('app.js');
for (const key of ['brightness', 'contrast', 'saturation', 'gamma'])
  assert.ok(app.includes(`{ key: '${key}', label: 'row.${key}', hint: 'row.${key}.hint', type: 'step' }`), `${key}: a step row`);
assert.ok(app.includes("label: 'row.resetPicture'") && gamePage.includes('"Reset Picture"'), 'Reset Picture in both');
for (const k of ['row.resetPicture', 'row.resetPicture.button', 'row.resetPicture.done', 'settings.less', 'settings.more']) assert.ok(en[k], k);
const { stepChoice } = load('steps.js');
const percents = ['50', '60', '70', '80', '90', '100', '110', '120', '130', '140', '150'];
assert.strictEqual(stepChoice(percents, '100', 1), '110');
assert.strictEqual(stepChoice(percents, '100', -1), '90');
assert.strictEqual(stepChoice(percents, '150', 1), null, 'stops at the top');
assert.strictEqual(stepChoice(percents, '50', -1), null, 'stops at the bottom');
assert.strictEqual(stepChoice(percents, '125', 1), '130', 'a number between choices: the next one up');
assert.strictEqual(stepChoice(percents, '125', -1), '120');
assert.strictEqual(stepChoice(percents, '400', 1), null);
assert.strictEqual(stepChoice(percents, '400', -1), '150');
assert.strictEqual(stepChoice(percents, 'bright', 1), '110', 'no number: from the middle (neutral)');
assert.strictEqual(stepChoice(['0.6', '0.7', '0.8', '0.9', '1.0', '1.1', '1.2', '1.3', '1.4'], '1', 1), '1.1', 'gamma 1 is 1.0');
assert.strictEqual(stepChoice([], '1', 1), null);

// ---- the Controls tab's stick captions: one line when they fit, else split after the colon
const stick = load('stick.js');
const width = (x) => x.length * 5;   // a stand-in for the measured width
assert.strictEqual(JSON.stringify(stick.stickLabelLines(width, 'Left stick: steer, lean')), '["Left stick: steer, lean"]');
assert.strictEqual(JSON.stringify(stick.stickLabelLines(width, 'Linker Stick: lenken, lehnen und noch mehr')),
  '["Linker Stick:","lenken, lehnen und noch mehr"]');
assert.strictEqual(JSON.stringify(stick.stickLabelLines(width, '左スティック：ステアリング・傾き・もっと長い説明文です')), '["左スティック：","ステアリング・傾き・もっと長い説明文です"]');
assert.strictEqual(JSON.stringify(stick.stickLabelLines(width, 'Eine Beschriftung ohne Doppelpunkt aber sehr lang')), '["Eine Beschriftung ohne Doppelpunkt aber sehr lang"]');

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

  // PLAY's wait for the settings writes (bridge.js's handoff gate): test_handoff.js

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
  // "Delete it" (finding 228): the question waits for the native answer; deleted ends it, a refusal
  // asks again and says so, and the original is never reported deleted when it was not
  {
    let q = d.discFlow(d.discIdle(), { job: 7, stage: 'done', deletable: true, size: 2600000000 });
    q = d.discDeleteAsked(q);
    assert.strictEqual(q.stage, 'deleting');
    assert.ok(d.discBusy(q), 'no other pick or PLAY while it deletes');
    assert.strictEqual(d.discFlow(q, { job: 7, stage: 'copying', done: 1, total: 2 }), q, 'late events do nothing');
    assert.strictEqual(d.discFlow(q, { job: 7, stage: 'deleting' }), q, 'never an event');
    const refused = d.discDeleteAnswer(q, false);
    assert.deepStrictEqual({ ...refused }, { stage: 'done', job: 7, deletable: true, size: 2600000000, deleteFailed: true });
    assert.strictEqual(d.discDeleteAnswer(q, undefined).stage, 'done', 'a failed call is a refusal');
    const again = d.discDeleteAsked(refused);
    assert.strictEqual(again.stage, 'deleting', 'the choice is offered again');
    assert.deepStrictEqual({ ...d.discDeleteAnswer(again, true) }, { stage: 'kept', job: 7 });
    assert.strictEqual(d.discDeleteAnswer(d.discIdle(), true).stage, 'idle', 'an answer without a question');
    assert.strictEqual(d.discDeleteAsked(d.discFlow(d.discIdle(), { job: 8, stage: 'done', deletable: false })).stage, 'done',
      'nothing to delete when the provider does not allow it');
  }

  // nonsense is ignored
  assert.strictEqual(d.discFlow(s, null), s);
  assert.strictEqual(d.discFlow(s, { job: 4, stage: 'flying' }), s);
  assert.strictEqual(d.discFlow(s, { stage: 'done' }), s);
  console.log('test_launcher_ui: ok');
})().catch((e) => { console.error(e); process.exit(1); });
