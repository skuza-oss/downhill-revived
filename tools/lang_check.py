#!/usr/bin/env python3
"""The launcher's string table (finding 225; port/launcher/ui/lang/README.md): checks the language
files against the English source and the page's code against the source.

    python tools/lang_check.py            # errors fail (exit 1); a summary line per language
    python tools/lang_check.py --list     # also every missing and stale key per language

The source is port/launcher/ui/lang/en.json; every other lang/<code>.json is a language. Errors:
- a file that isn't one JSON object of strings;
- a language key the source lacks (a stale key; a count's form such as key.few counts when the
  source has key.other), or a string whose {placeholders} or **bold** spans differ from the source's;
- the languages in ui/i18n.js's LANGUAGES not being exactly the files there;
- a key the page's code names (t('...'), tn, tNodes, data-i18n...) or Android's native side words an
  error with (DiscImport.coded("...")) that the source lacks;
- a language in LANGUAGES without its own name in i18n.js's LANGUAGE_NAMES (the Language row's text).
A key the source has and a language lacks is only listed: the page shows English for it. A count has
the forms its language uses (Japanese and Chinese one, key.other; English, German and most others
key.one and key.other; Russian, Polish and Arabic more): a form a language does not use is not
missing, a form it uses and lacks is.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / 'port' / 'launcher' / 'ui'
PLURAL_FORMS = ('zero', 'one', 'two', 'few', 'many', 'other')
# The count forms a language writes out (Unicode CLDR's plural rules, by its base code). Any other
# language has one and other. CLDR also gives pt, es, fr and it a "many" for millions ("1 million of
# races"): no count here gets that big, and the page falls back to other, so it is not asked for.
ONLY_OTHER = ('ja', 'zh', 'ko', 'vi', 'th', 'id', 'ms', 'my', 'lo', 'km')
PLURAL_FORMS_OF = {
    'ru': ('one', 'few', 'many', 'other'), 'uk': ('one', 'few', 'many', 'other'), 'pl': ('one', 'few', 'many', 'other'),
    'cs': ('one', 'few', 'many', 'other'), 'sk': ('one', 'few', 'many', 'other'), 'hr': ('one', 'few', 'other'),
    'sr': ('one', 'few', 'other'), 'ar': ('zero', 'one', 'two', 'few', 'many', 'other'), 'sl': ('one', 'two', 'few', 'other'),
    'lt': ('one', 'few', 'many', 'other'), 'lv': ('zero', 'one', 'other'), 'ro': ('one', 'few', 'other'),
}
# t('key'), tn('key', ...), tNodes('key'), and keys handed to helpers as 'row.x' / 'bind.x' strings
CODE_KEY = re.compile(r"""\b(?:t|tn|tNodes)\(\s*'([A-Za-z0-9_.]+)'""")
LITERAL_KEY = re.compile(r"""'((?:row|bind|tab|home|footer|disc|help|crash|settings|server|online|notes|stick|rider|nav|window|language)\.[A-Za-z0-9_.]+)'""")
HTML_KEY = re.compile(r'data-i18n(?:-aria|-title)?="([A-Za-z0-9_.]+)"')
# a phone's native side words an error by key (DiscImport.coded("disc.problem.noRoom", ...))
NATIVE = ROOT / 'port' / 'android' / 'app' / 'src' / 'main' / 'java'
NATIVE_KEY = re.compile(r'\bcoded\(\s*"([A-Za-z0-9_.]+)"')


def load(path):
    """The file's table, or an error string."""
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        return '%s: not JSON (%s)' % (path.name, e)
    if not isinstance(data, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in data.items()):
        return '%s: must be one object of "key": "string" lines' % path.name
    return data


def shape(text):
    """What a translation must keep: the placeholders' names and the number of bold spans."""
    return sorted(set(re.findall(r'\{(\w+)\}', text))), text.count('**')


def source_key(key, source):
    """The source key a language key stands for (a count's form: key.other's), or None."""
    if key in source:
        return key
    base, _, form = key.rpartition('.')
    if form in PLURAL_FORMS and base + '.other' in source:
        return base + '.other'
    return None


def forms_of(code):
    """The count forms a language writes out (by its code's base: zh-Hans -> zh)."""
    base = code.split('-')[0].lower()
    if base in ONLY_OTHER:
        return ('other',)
    return PLURAL_FORMS_OF.get(base, ('one', 'other'))


def plural_base(key):
    """A count's base key (server.races for server.races.one), or None for a key that is no form."""
    base, _, form = key.rpartition('.')
    return base if form in PLURAL_FORMS else None


def wanted(code, source):
    """Every key a language is asked for: the source's plain keys, and each count in the forms the
    language uses (the source's own forms don't matter: Japanese has no .one, Russian needs .few)."""
    keys = [k for k in source if plural_base(k) is None]
    counts = sorted({plural_base(k) for k in source if plural_base(k) is not None})
    return keys + ['%s.%s' % (b, f) for b in counts for f in forms_of(code)]


def check_language(code, table, source):
    """(errors, missing, stale) for one language."""
    errors, stale = [], []
    for key, text in table.items():
        k = source_key(key, source)
        if k is None:
            stale.append(key)
            continue
        if shape(text)[0] != shape(source[k])[0]:
            errors.append('%s.json: %s has placeholders %s, the source %s' % (code, key, shape(text)[0], shape(source[k])[0]))
        if text.count('**') % 2 or (shape(source[k])[1] and not text.count('**')):
            errors.append('%s.json: %s must keep the **bold** span of the source' % (code, key))
    for key in stale:
        errors.append('%s.json: %s is not in the source (en.json)' % (code, key))
    missing = [k for k in wanted(code, source) if k not in table]
    return errors, missing, stale


def languages_listed(text):
    m = re.search(r"const LANGUAGES = \[([^\]]*)\]", text)
    return re.findall(r"'([^']+)'", m.group(1)) if m else None


def language_names(text):
    """The codes in i18n.js's LANGUAGE_NAMES table, or None."""
    m = re.search(r"const LANGUAGE_NAMES = \{(.*?)\};", text, re.S)
    return re.findall(r"""^\s*'([^']+)'\s*:\s*'[^']+'""", m.group(1), re.M) if m else None


def code_keys(ui, native=None):
    """Every key the page's code (and a phone's native side, native) names literally, with its file."""
    found = {}
    for f in sorted(ui.glob('*.js')) + sorted(ui.glob('*.html')):
        text = f.read_text(encoding='utf-8')
        for pattern in (CODE_KEY, LITERAL_KEY) if f.suffix == '.js' else (HTML_KEY,):
            for key in pattern.findall(text):
                found.setdefault(key, f.name)
    for f in sorted(native.rglob('*.java')) if native and native.is_dir() else []:
        for key in NATIVE_KEY.findall(f.read_text(encoding='utf-8')):
            found.setdefault(key, f.name)
    return found


def run(ui, listing=False, out=print):
    lang = ui / 'lang'
    errors = []
    source = load(lang / 'en.json')
    if isinstance(source, str):
        out('lang_check: ' + source)
        return 1
    codes = sorted(p.stem for p in lang.glob('*.json'))
    listed = languages_listed((ui / 'i18n.js').read_text(encoding='utf-8'))
    if listed is None:
        errors.append('i18n.js: no LANGUAGES list')
    elif sorted(listed) != codes:
        errors.append('i18n.js: LANGUAGES %s, the lang folder has %s' % (listed, codes))
    named = language_names((ui / 'i18n.js').read_text(encoding='utf-8'))
    if named is None:
        errors.append('i18n.js: no LANGUAGE_NAMES table')
    elif listed is not None and sorted(named) != sorted(listed):
        errors.append('i18n.js: LANGUAGE_NAMES has %s, LANGUAGES %s' % (named, listed))
    for key, where in code_keys(ui, NATIVE if ui == UI else None).items():
        if source_key(key, source) is None and not any(k.startswith(key + '.') for k in source):
            errors.append('%s names %s, which en.json lacks' % (where, key))
    out('lang_check: en.json, %d strings (the source)' % len(source))
    for code in codes:
        if code == 'en':
            continue
        table = load(lang / (code + '.json'))
        if isinstance(table, str):
            errors.append(table)
            continue
        e, missing, stale = check_language(code, table, source)
        errors += e
        total = len(wanted(code, source))
        out('lang_check: %s.json, %d of %d translated, %d stale' % (code, total - len(missing), total, len(stale)))
        if listing:
            for k in missing:
                out('  missing %s' % k)
            for k in stale:
                out('  stale   %s' % k)
    for e in errors:
        out('ERROR ' + e)
    out('lang_check: %d error(s)' % len(errors))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(run(UI, '--list' in sys.argv[1:]))
