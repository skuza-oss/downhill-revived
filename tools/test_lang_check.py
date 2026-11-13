"""Tests of tools/lang_check.py (the launcher's string table, finding 225) on made-up folders.

    python tools/test_lang_check.py
"""
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lang_check  # noqa: E402

SOURCE = {'a.title': 'Hello {name}', 'a.bold': 'New in **{version}**', 'a.count.one': '{count} race', 'a.count.other': '{count} races'}


def folder(languages, extra=None, listed=None, code="t('a.title')"):
    d = Path(tempfile.mkdtemp())
    (d / 'lang').mkdir()
    (d / 'lang' / 'en.json').write_text(json.dumps(SOURCE), encoding='utf-8')
    for code_name, table in languages.items():
        (d / 'lang' / (code_name + '.json')).write_text(table if isinstance(table, str) else json.dumps(table), encoding='utf-8')
    names = listed if listed is not None else ['en'] + list(languages)
    (d / 'i18n.js').write_text('const LANGUAGES = [%s];\n' % ', '.join("'%s'" % n for n in names), encoding='utf-8')
    (d / 'app.js').write_text(code + '\n' + (extra or ''), encoding='utf-8')
    return d


def result(d, listing=False):
    lines = []
    code = lang_check.run(d, listing, lines.append)
    return code, lines


def main():
    # a good partial translation (a count's own forms cover its key): missing keys are listed only
    code, lines = result(folder({'de': {'a.title': 'Hallo {name}', 'a.count.one': '{count} Rennen', 'a.count.few': '{count} x'}}), True)
    assert code == 0, lines
    assert any('de.json, 3 of 4 translated, 0 stale' in l for l in lines), lines
    assert any(l.strip() == 'missing a.bold' for l in lines), lines
    # a stale key fails
    code, lines = result(folder({'de': {'a.gone': 'weg'}}))
    assert code == 1 and any('a.gone is not in the source' in l for l in lines), lines
    # a placeholder renamed fails; a bold span dropped fails
    code, lines = result(folder({'de': {'a.title': 'Hallo {nom}'}}))
    assert code == 1 and any('placeholders' in l for l in lines), lines
    code, lines = result(folder({'de': {'a.bold': 'Neu in {version}'}}))
    assert code == 1 and any('bold' in l for l in lines), lines
    # not JSON, or not strings
    code, lines = result(folder({'de': '{"a.title": 1}'}))
    assert code == 1, lines
    # LANGUAGES must match the files
    code, lines = result(folder({'de': {}}, listed=['en']))
    assert code == 1 and any('LANGUAGES' in l for l in lines), lines
    # the code names a key the source lacks
    code, lines = result(folder({}, code="t('a.nothere')"))
    assert code == 1 and any('a.nothere' in l for l in lines), lines
    # a count named by its base key is the source's
    code, lines = result(folder({}, code="tn('a.count', 2)"))
    assert code == 0, lines
    # the real table
    assert lang_check.run(lang_check.UI, False, lambda s: None) == 0
    print('test_lang_check: ok')


if __name__ == '__main__':
    main()
