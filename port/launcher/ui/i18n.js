// The launcher's words (finding 225): every string the page shows comes from lang/<code>.json, a
// flat table of stable keys. lang/en.json is the source; a language is one more file of the same
// shape (lang/README.md). The language: dhd.ini's "language" when it names one of LANGUAGES, else
// the system's first language that has a file, else English. A key missing from a language falls
// back to English, so a half-done translation still shows every line.
//
// A string may hold {name} placeholders (filled by t's params) and **bold** spans (tNodes draws
// them as <b>); a count has its forms as key.one, key.other (and key.zero, .two, .few, .many where a
// language needs them; Intl.PluralRules picks one).
'use strict';

// The languages shipped: their codes (the file names) in the Language row's order, and each one's own
// name as its speakers write it (the row shows every language under its own name, whichever is
// active). A translator adds the code and the name here with the file.
const LANGUAGES = ['en', 'pt-BR', 'es', 'fr', 'de', 'it', 'ja', 'zh-Hans'];
const LANGUAGE_NAMES = {
  'en': 'English',
  'pt-BR': 'Português (Brasil)',
  'es': 'Español',
  'fr': 'Français',
  'de': 'Deutsch',
  'it': 'Italiano',
  'ja': '日本語',
  'zh-Hans': '简体中文',
};
const PLURAL_FORMS = ['zero', 'one', 'two', 'few', 'many', 'other'];

const lang = { code: 'en', words: {}, english: {} };

// Chinese comes in two scripts and a device names one by script (zh-Hans, zh-Hant) or by region
// (zh-CN, zh-TW); a region alone says the script.
const CHINESE_SCRIPT_OF_REGION = { cn: 'hans', sg: 'hans', my: 'hans', tw: 'hant', hk: 'hant', mo: 'hant' };

// The file for one language tag (a device's or dhd.ini's), or '' when there is none: an exact code
// (pt-BR), then Chinese by script and region (zh, zh-CN, zh-SG and zh-Hans-* take zh-Hans; zh-TW,
// zh-HK and zh-Hant-* take only a Traditional file, never the Simplified one), then for any other
// language the longest file code the tag starts with (de for de-AT), the base (pt), and last another
// file of the same language (pt-BR for pt-PT and plain pt: the only Portuguese there is).
function languageFile(tag, available) {
  const low = (c) => String(c).toLowerCase().replace(/_/g, '-');
  const has = (c) => available.find((a) => low(a) === low(c)) || '';
  const parts = low(tag).split('-');
  const base = parts[0];
  if (!base) return '';
  if (has(tag)) return has(tag);
  if (base === 'zh') {
    const script = parts.slice(1).find((p) => p.length === 4) || CHINESE_SCRIPT_OF_REGION[parts.slice(1).find((p) => p.length === 2)] || 'hans';
    return has(`zh-${script}`);
  }
  const prefix = available.filter((a) => low(tag).startsWith(`${low(a)}-`)).sort((x, y) => y.length - x.length)[0];
  return prefix || has(base) || available.find((a) => low(a).split('-')[0] === base) || '';
}

// The language to use: saved (dhd.ini's language, '' = the system's), else the first of wanted
// (navigator.languages) with a file, else English.
function pickLanguage(saved, wanted, available = LANGUAGES) {
  const own = saved && languageFile(saved, available);
  if (own) return own;
  for (const w of wanted || []) {
    const file = languageFile(w, available);
    if (file) return file;
  }
  return 'en';
}

// One string with its {placeholders} filled ('' when the key has no words anywhere: the key itself,
// so a missing one shows in the page and in the checks).
function fill(text, params) {
  return text.replace(/\{(\w+)\}/g, (m, name) => (params && name in params ? String(params[name]) : m));
}

function words(key) {
  return lang.words[key] ?? lang.english[key];
}

function t(key, params) {
  const w = words(key);
  return w === undefined ? key : fill(w, params).replace(/\*\*/g, '');
}

// A count's form: key.<form> by the language's plural rules, {count} filled in.
function tn(key, count, params = {}) {
  let form = 'other';
  try { form = new Intl.PluralRules(lang.code).select(count); } catch { /* an unknown code: other */ }
  const k = words(`${key}.${form}`) !== undefined ? `${key}.${form}` : `${key}.other`;
  return t(k, { count, ...params });
}

// A string as nodes: **bold** spans as <b>, and a placeholder whose param is a Node put in as it is.
function tNodes(key, params = {}) {
  const w = words(key);
  if (w === undefined) return [key];
  const out = [];
  w.split(/(\*\*[^*]+\*\*)/).forEach((part, i) => {
    const bold = i % 2 === 1;
    const text = bold ? part.slice(2, -2) : part;
    const pieces = [];
    text.split(/(\{\w+\})/).forEach((p, j) => {
      if (j % 2 === 0) { if (p) pieces.push(p); return; }
      const v = params[p.slice(1, -1)];
      pieces.push(v === undefined ? p : typeof v === 'object' ? v : String(v));
    });
    if (!bold) out.push(...pieces);
    else {
      const b = document.createElement('b');
      b.append(...pieces);
      out.push(b);
    }
  });
  return out;
}

// A language's own name for the Language row (LANGUAGE_NAMES; the code when it has none).
function languageName(code) {
  return LANGUAGE_NAMES[code] || code;
}

// The language the device alone would give (no dhd.ini choice): the Language row's "System" entry.
function systemLanguage() {
  return pickLanguage('', typeof navigator !== 'undefined' ? navigator.languages : []);
}

// The language file at lang/<code>.json (from the page's own folder).
async function loadTable(code) {
  const r = await fetch(`lang/${code}.json`);
  if (!r.ok) throw new Error(`lang/${code}.json: ${r.status}`);
  return r.json();
}

// Loads English and the language chosen; the page's static words (data-i18n attributes) follow.
async function loadLanguage(saved) {
  lang.english = await loadTable('en');
  lang.code = pickLanguage(saved, typeof navigator !== 'undefined' ? navigator.languages : []);
  lang.words = lang.code === 'en' ? lang.english : await loadTable(lang.code).catch(() => lang.english);
  if (typeof document !== 'undefined') {
    document.documentElement.lang = lang.code;
    translatePage(document);
  }
}

// data-i18n="key" sets an element's words (with **bold**), data-i18n-aria its aria-label,
// data-i18n-title its title.
function translatePage(root) {
  root.querySelectorAll('[data-i18n]').forEach((e) => e.replaceChildren(...tNodes(e.dataset.i18n)));
  root.querySelectorAll('[data-i18n-aria]').forEach((e) => e.setAttribute('aria-label', t(e.dataset.i18nAria)));
  root.querySelectorAll('[data-i18n-title]').forEach((e) => e.setAttribute('title', t(e.dataset.i18nTitle)));
}

if (typeof module !== 'undefined') module.exports = { LANGUAGES, LANGUAGE_NAMES, PLURAL_FORMS, lang, pickLanguage, fill, t, tn, languageName, systemLanguage };
