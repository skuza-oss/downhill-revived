"""The States a record from another game may name (port/hle/puppet_record.h PUPPET_STATES, finding
220) against the game's own source: the rider's constructor (func_0012B620) lists its State objects
in its table at +0x6394..+0x63D8, the race's setup (func_0012BF88) walks those 18 and starts each
rider in one, and every switch (func_0012FE38's callers in src/) names one. The list must be that
table exactly, and every State offset any switch or the setup names must be on it; else a real
rider's State would be refused and its puppet frozen on every other game.

    python port/tools/test_rider_states.py     (also run by test_runtime.py)"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'src' / 'unsorted'


def listed():
    text = (ROOT / 'port/hle/puppet_record.h').read_text(encoding='utf-8')
    body = re.search(r'PUPPET_STATES\[\]\s*=\s*\{(.*?)\};', text, re.S).group(1)
    return [int(x, 16) for x in re.findall(r'\b0x([0-9A-Fa-f]+)\b', re.sub(r'/\*.*?\*/', '', body, flags=re.S))]


def constructor_table():
    """func_0012B620's stores into +0x6394..+0x63D8: each a pointer to obj + offset."""
    text = (SRC / '0012B620.c').read_text(encoding='utf-8')
    names = {}
    for name, off in re.findall(r'(\w+)\s*=\s*(?:\(\w+\s*\*\))?\(?obj\s*\+\s*0x([0-9A-Fa-f]+)\)?;', text):
        names[name] = int(off, 16)
    table = {}
    for slot, name in re.findall(r'\*\(int \*\)\(obj \+ 0x(63[9A-D][0-9A-Fa-f])\) = \(int\)(\w+);', text):
        table[int(slot, 16)] = names[name]
    assert sorted(table) == list(range(0x6394, 0x63DC, 4)), sorted(map(hex, table))
    return [table[k] for k in sorted(table)]


WINDOW = 10  # lines before a switch where a caller picks its candidates (mode = &rider->x62E0; ...)
# Rider offsets near a switch that are not States (looked at by hand):
NOT_STATES = {
    0x4EF0: "the thrown rider's body (func_001A4F70); func_0012E940 pushes it just before its switch",
}


def named_by_switches():
    """Every State offset a switch (a func_0012FE38 call) or the setup names: any rider offset
    (0xNNNN or a member xNNNN) on the call's line or in the WINDOW lines before it, where the
    dynamic callers pick their candidates (&obj->x5E40, ITEM(0x5E70), base + 0x5E90, ...). This
    takes in some offsets that are not States; any of those in the States' span fails the test
    and is looked at by hand."""
    found = {}
    for f in SRC.iterdir():
        text = f.read_text(encoding='utf-8', errors='replace')
        if 'func_0012FE38' not in text or f.name.startswith('0012FE38'):
            continue
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if not re.search(r'func_0012FE38\s*\(', line) or re.match(r'\s*(extern|void|class|\w+\s+func_0012FE38\()', line):
                continue
            for near in lines[max(0, i - WINDOW):i + 1]:
                for off in re.findall(r'(?:0x|\bx)([4-7][0-9A-Fa-f]{3})\b', near):
                    n = int(off, 16)
                    if 0x4ED0 <= n <= 0x6330:
                        found.setdefault(n, f.name)
    setup = (SRC / '0012BF88.c').read_text(encoding='utf-8')
    start = re.search(r'x4ECC\s*=\s*(\w+);', setup).group(1)
    found.setdefault(int(re.search(start + r'\s*=\s*&obj->x([0-9A-Fa-f]+)', setup).group(1), 16), '0012BF88.c')
    return found


def main():
    states = listed()
    table = constructor_table()
    assert states == table, 'PUPPET_STATES is not the constructor\'s table: %s vs %s' % (
        [hex(x) for x in states], [hex(x) for x in table])
    named = named_by_switches()
    # every offset a switch names must be on the list, but for the known non-States near a switch
    missing = {hex(k): v for k, v in named.items() if k not in states and k not in NOT_STATES and 0x4ED0 <= k <= 0x6330}
    assert not missing, 'a switch names a State not on PUPPET_STATES: %s' % missing
    assert 0x5FE0 in states and 0x4ED0 in states and 0x4EE0 in states
    print('rider states: %d on the list = the constructor\'s table; %d offsets named by switches, all on it'
          % (len(states), len([k for k in named if k in states])))


if __name__ == '__main__':
    main()
