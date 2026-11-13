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

// The languages shipped: their codes (the file names) in the Language row's order. A translator
// adds the code here with the file.
const LANGUAGES = ['en', 'de'];
const PLURAL_FORMS = ['zero', 'one', 'two', 'few', 'many', 'other'];

const lang = { code: 'en', words: {}, english: {} };

// The language to use: saved (dhd.ini's language, '' = the system's), else the first of wanted
// (navigator.languages) with a file: an exact code (pt-BR), then the longest file code it starts
// with (zh-Hans for zh-Hans-CN), then its base (pt), then another file of its language (pt-BR for
// pt-PT; never across Chinese scripts: zh-TW doesn't take zh-Hans), else English.
function pickLanguage(saved, wanted, available = LANGUAGES) {
  const low = (c) => String(c).toLowerCase();
  const has = (c) => available.find((a) => low(a) === low(c));
  if (saved && has(saved)) return has(saved);
  for (const w of wanted || []) {
    if (has(w)) return has(w);
    const prefix = available.filter((a) => low(w).startsWith(`${low(a)}-`)).sort((x, y) => y.length - x.length)[0];
    if (prefix) return prefix;
    const base = low(w).split('-')[0];
    if (has(base)) return has(base);
    const sibling = base !== 'zh' && available.find((a) => low(a).split('-')[0] === base);
    if (sibling) return sibling;
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

// A language's own name for the Language row (its language.name key).
function languageName(code, table) {
  return (table && table['language.name']) || code;
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

if (typeof module !== 'undefined') module.exports = { LANGUAGES, PLURAL_FORMS, lang, pickLanguage, fill, t, tn, languageName };
