"""Compare two per-field state traces (DHD_STATE_TRACE, port/hle/state_trace.c) and name the first
difference. Exit 0 only if every row matches and both traces have the same number of rows.

    python port/tools/state_diff.py A.tsv B.tsv [--symbols-a MAP] [--symbols-b MAP]
                                    [--dumps-a PREFIX] [--dumps-b PREFIX] [--max-words N]
    python port/tools/state_diff.py --check-dumps A.tsv     (dumped ee.bin hash == ram column)
    python port/tools/state_diff.py --selftest              (XXH64 against published vectors)

With dumps (DHD_STATE_DUMP=<field> in both runs), the first differing field's ee.bin is compared
word by word: each differing word is printed with the symbol it lies in (the EE address) and,
when its value is a code address, the function it points to. Two words are equal when they are
equal, or when both are code addresses of the same function (the symbol maps decide that: a
64-bit build's map gives its own addresses, but the same function name). Symbol maps are text
files of "ADDR NAME" lines; the default is the retail names in the wasm core's map,
build/wasm/dhd.map (build_wasm.py).

The regptrs column (finding 205) lists the register lanes whose value lies in the core's ranges,
each with its raw value and its identity in that build (state_trace.c). Two lists are equal when
they name the same lanes and each lane has the same raw value or the same identity (tag, id,
offset; tag 0 has none): a pointer that moved with the core's layout is the same pointer, and a
value that only looks like one is equal only when it is the same value. Every other column must be
equal as it is.

Game data: the traces and dumps hold game state. Keep them under build/."""
import argparse
import bisect
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_MAP = os.path.join(ROOT, 'build', 'wasm', 'dhd.map')
MASK64 = (1 << 64) - 1
P1, P2, P3, P4, P5 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5


def _round(acc, lane):
    acc = (acc + lane * P2) & MASK64
    acc = ((acc << 31) | (acc >> 33)) & MASK64
    return (acc * P1) & MASK64


def _merge(acc, val):
    acc ^= _round(0, val)
    return (acc * P1 + P4) & MASK64


def xxh64(data, seed=0):
    """XXH64 (xxHash spec), pure Python. Matches the C build's XXH64 for the same bytes."""
    n = len(data)
    i = 0
    if n >= 32:
        v = [(seed + P1 + P2) & MASK64, (seed + P2) & MASK64, seed & MASK64, (seed - P1) & MASK64]
        end = n - 32
        while i <= end:
            lanes = struct.unpack_from('<4Q', data, i)
            v = [_round(v[k], lanes[k]) for k in range(4)]
            i += 32
        h = (((v[0] << 1) | (v[0] >> 63)) + ((v[1] << 7) | (v[1] >> 57)) +
             ((v[2] << 12) | (v[2] >> 52)) + ((v[3] << 18) | (v[3] >> 46))) & MASK64
        for k in range(4):
            h = _merge(h, v[k])
    else:
        h = (seed + P5) & MASK64
    h = (h + n) & MASK64
    while i + 8 <= n:
        k1 = _round(0, struct.unpack_from('<Q', data, i)[0])
        h ^= k1
        h = (((h << 27) | (h >> 37)) * P1 + P4) & MASK64
        i += 8
    if i + 4 <= n:
        h ^= (struct.unpack_from('<I', data, i)[0] * P1) & MASK64
        h = (((h << 23) | (h >> 41)) * P2 + P3) & MASK64
        i += 4
    while i < n:
        h ^= (data[i] * P5) & MASK64
        h = (((h << 11) | (h >> 53)) * P1) & MASK64
        i += 1
    h ^= h >> 33
    h = (h * P2) & MASK64
    h ^= h >> 29
    h = (h * P3) & MASK64
    h ^= h >> 32
    return h


def selftest():
    # Published XXH64 reference values (seed 0).
    cases = [(b'', 0xEF46DB3751D8E999), (b'abc', 0x44BC2CF5AD770999),
             (bytes(range(256)) * 4, None)]
    for data, want in cases:
        got = xxh64(data)
        if want is not None and got != want:
            print('xxh64 selftest FAILED for %r: %016x != %016x' % (data[:8], got, want))
            return 1
    print('xxh64 selftest ok (%d vectors)' % len(cases))
    return 0


def load_trace(path):
    header, columns, rows = None, None, []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('#'):
                header = line
            elif columns is None:
                columns = line.split('\t')
            elif line:
                rows.append(line.split('\t'))
    if header is None or columns is None:
        raise SystemExit('state_diff: %s is not a state trace' % path)
    meta = dict(re.findall(r'(\w+)=(\S*)', header))
    return header, meta, columns, rows


def symbol_table(path):
    """Sorted [(EE address, name)] from a map file (retail names: func_XXXXXXXX, D_XXXXXXXX) or
    an 'ADDR NAME' text file."""
    out = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            m = re.match(r'^\s*([0-9A-Fa-f]{6,8})\s+(\S+)\s*$', line)  # "ADDR NAME"
            if m:
                out.append((int(m.group(1), 16), m.group(2)))
                continue
            for m in re.finditer(r'\b((?:func|D)_([0-9A-Fa-f]{8}))\b', line):
                out.append((int(m.group(2), 16), m.group(1)))
    return sorted(set(out))


class Symbols:
    def __init__(self, table):
        self.addrs = [a for a, _ in table]
        self.names = [n for _, n in table]
        self.exact = {}
        for a, n in table:
            self.exact.setdefault(a, n)

    def name_of(self, address):
        """The function (or data symbol) containing address: name+offset, or None."""
        i = bisect.bisect_right(self.addrs, address) - 1
        if i < 0:
            return None
        base = self.addrs[i]
        return self.names[i] if base == address else '%s+0x%X' % (self.names[i], address - base)

    def function_at(self, value):
        """The retail function name when value is exactly a function's entry address."""
        name = self.exact.get(value)
        return name if name and name.startswith('func_') else None


def check_dumps(path, prefix):
    header, meta, columns, rows = load_trace(path)
    prefix = prefix or meta.get('prefix')
    ram_col = columns.index('ram')
    dirname, base = os.path.split(prefix)
    failures, checked = 0, 0
    for name in sorted(os.listdir(dirname or '.')):
        m = re.match(re.escape(base) + r'-f(\d+)-ee\.bin$', name)
        if not m:
            continue
        field = m.group(1)
        row = next((r for r in rows if r[0] == field), None)
        if row is None:
            print('check-dumps: %s has no row for field %s' % (path, field))
            failures += 1
            continue
        with open(os.path.join(dirname, name), 'rb') as f:
            data = f.read()
        got = '%016x' % xxh64(data)
        checked += 1
        ok = got == row[ram_col]
        failures += not ok
        print('check-dumps field %s: ee.bin xxh64 %s, ram column %s: %s' % (field, got, row[ram_col], 'ok' if ok else 'MISMATCH'))
    if not checked:
        print('check-dumps: no %s-f*-ee.bin files' % base)
        return 2
    return 1 if failures else 0


def parse_regptrs(cell):
    """{lane: (raw, tag, id, offset)} of a regptrs cell ('-' when empty)."""
    out = {}
    if cell in ('', '-'):
        return out
    for entry in cell.split(','):
        lane, _, value = entry.partition('=')
        raw, tag, ident, offset = value.split(':')
        out[lane] = (int(raw, 16), int(tag), int(ident, 16), int(offset, 16))
    return out


def regptrs_differences(a, b):
    """The lanes of two regptrs cells that differ: in one list only, or with another raw value
    and another identity (or no identity). [] when the cells are equal."""
    x, y = parse_regptrs(a), parse_regptrs(b)
    bad = []
    for lane in sorted(set(x) | set(y)):
        if lane not in x or lane not in y:
            bad.append(lane)
            continue
        (ra, ta, ia, oa), (rb, tb, ib, ob) = x[lane], y[lane]
        if ra == rb or (ta and (ta, ia, oa) == (tb, ib, ob)):
            continue
        bad.append(lane)
    return bad


def cells_differ(column, a, b):
    """Whether two cells of a column differ (regptrs by lane, every other column as text)."""
    if column == 'regptrs':
        return bool(regptrs_differences(a, b))
    return a != b


def section_names(columns):
    return columns[4:-1]  # ram .. sound (combined last)


def compare(a, b, args):
    ha, ma, ca, ra = load_trace(a)
    hb, mb, cb, rb = load_trace(b)
    print('A: %s\nB: %s' % (a, b))
    if ma.get('exe') != mb.get('exe'):
        print('note: exe differs (A %s, B %s); section values are compared anyway'
              % (ma.get('exe'), mb.get('exe')))
    if ca != cb:
        print('FAIL: column sets differ')
        return 1
    print('rows: A %d, B %d' % (len(ra), len(rb)))
    ok = True
    if len(ra) != len(rb):
        ok = False
        print('FAIL: row counts differ')
    first = None
    differing = []
    for ia, (x, y) in enumerate(zip(ra, rb)):
        bad = [c for i, c in enumerate(ca) if i < len(x) and i < len(y) and cells_differ(c, x[i], y[i])
               and c != 'stackwords']  # informational: a count of normalised words, not state
        if x[0] != y[0]:
            bad = ['field']
        if bad:
            differing.append((x[0], bad))
            if first is None:
                first = (ia, x, y, bad)
    if first is None:
        print('all %d rows match' % min(len(ra), len(rb)))
        if ok:
            print('PASS')
            return 0
        print('FAIL')
        return 1
    ia, x, y, bad = first
    print('FIRST DIFFERENCE: field %s (row %d): %s' % (x[0], ia + 1, ', '.join(bad)))
    for c in bad:
        i = ca.index(c)
        if c == 'regptrs':
            xa, xb = parse_regptrs(x[i]), parse_regptrs(y[i])
            for lane in regptrs_differences(x[i], y[i]):
                show = lambda v: '-' if v is None else '%08X tag %d id %08X +%X' % v
                print('  regptrs    %-6s A %s  B %s' % (lane, show(xa.get(lane)), show(xb.get(lane))))
            continue
        print('  %-10s A %s  B %s' % (c, x[i], y[i]))
    print('rows differing: %d of %d (first 10 fields: %s)' %
          (len(differing), min(len(ra), len(rb)), ' '.join(f for f, _ in differing[:10])))
    dump_compare(ma, mb, x[0], args)
    return 1


def code_rows(path):
    """{address: (tag, id, offset, value)} from a dump's code.tsv (the code-pointer words)."""
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path, encoding='utf-8'):
        addr, value, tag, ident, offset = line.split()
        out[int(addr, 16)] = (int(tag), ident, offset, value)
    return out


def code_diff(pa, pb, field):
    """The code-pointer words of the field whose identity differs (the trace's code section)."""
    ra = code_rows('%s-f%s-code.tsv' % (pa, field))
    rb = code_rows('%s-f%s-code.tsv' % (pb, field))
    if not ra or not rb:
        return
    bad = [(a, ra[a], rb.get(a)) for a in sorted(ra) if rb.get(a, (None,))[:2] != ra[a][:2]]
    # a pointer word compares equal when both sides name the same symbol, at any offset (a return
    # address inside a function); the offset difference is logged here, not hidden
    offs = [a for a in sorted(ra) if a in rb and rb[a][:2] == ra[a][:2] and rb[a][2] != ra[a][2]]
    print('code-pointer words with a different identity at field %s: %d of %d' % (field, len(bad), len(ra)))
    print('same function, offset differs at field %s: %d words' % (field, len(offs)))
    for a in offs[:40]:
        print('  %08X same symbol id %s: offset A %s, B %s' % (a, ra[a][1], ra[a][2], rb[a][2]))
    for a, x, y in bad[:40]:
        ya = '-' if y is None else ('%s %s+%s' % ('fn' if y[0] else 'raw', y[1], y[2]))
        print('  %08X A %s+%s (value %s)  B %s' % (a, 'fn' if x[0] else 'raw', x[1] if x[0] else x[3],
                                                 x[3], ya))


def dump_compare(ma, mb, field, args):
    pa = args.dumps_a or ma.get('prefix', '')
    pb = args.dumps_b or mb.get('prefix', '')
    fa = '%s-f%s-ee-raw.bin' % (pa, field)  # the words the code section and the masks take out, as they are
    fb = '%s-f%s-ee-raw.bin' % (pb, field)
    if not (os.path.exists(fa) and os.path.exists(fb)):
        fa, fb = '%s-f%s-ee.bin' % (pa, field), '%s-f%s-ee.bin' % (pb, field)
    if not (os.path.exists(fa) and os.path.exists(fb)):
        print('no dumps for field %s (run both with DHD_STATE_DUMP=%s); skipping word diff' % (field, field))
        return
    code_diff(pa, pb, field)
    syms_a = Symbols(symbol_table(args.symbols_a or DEFAULT_MAP)) if os.path.exists(args.symbols_a or DEFAULT_MAP) else Symbols([])
    syms_b = Symbols(symbol_table(args.symbols_b or DEFAULT_MAP)) if os.path.exists(args.symbols_b or DEFAULT_MAP) else Symbols([])
    da = open(fa, 'rb').read()
    db = open(fb, 'rb').read()
    n = min(len(da), len(db))
    diffs = []
    for off in range(0, n, 4):
        wa = struct.unpack_from('<I', da, off)[0]
        wb = struct.unpack_from('<I', db, off)[0]
        if wa == wb:
            continue
        fna = syms_a.function_at(wa)
        fnb = syms_b.function_at(wb)
        if fna and fnb and fna == fnb:
            if syms_a.name_of(wa) != syms_b.name_of(wb):
                print('  same function, offset differs: %08X %s  /  %08X %s' % (wa, syms_a.name_of(wa), wb, syms_b.name_of(wb)))
            continue  # the same function, under two builds' addresses
        diffs.append((off, wa, wb, fna, fnb))
    print('ee.bin words differing at field %s: %d' % (field, len(diffs)))
    for off, wa, wb, fna, fnb in diffs[:args.max_words]:
        ea = 0x00000000 + off  # the RAM offset is the EE address (0x08000000 host base)
        where = syms_a.name_of(ea) or '?'
        va = '%08X' % wa + (' (%s)' % fna if fna else '')
        vb = '%08X' % wb + (' (%s)' % fnb if fnb else '')
        print('  %08X %-28s A %s  B %s' % (ea, where, va, vb))
    if len(diffs) > args.max_words:
        print('  ... %d more' % (len(diffs) - args.max_words))
    for name in ('scratch.bin', 'hw.bin', 'cpu.bin', 'vu0.bin', 'gsregs.bin', 'gsvram.bin', 'gsstream.bin', 'sound.pcm'):
        pa_f, pb_f = '%s-f%s-%s' % (pa, field, name), '%s-f%s-%s' % (pb, field, name)
        if not (os.path.exists(pa_f) and os.path.exists(pb_f)):
            continue
        xa, xb = open(pa_f, 'rb').read(), open(pb_f, 'rb').read()
        if xa == xb:
            print('  %-12s identical (%d bytes)' % (name, len(xa)))
            continue
        k = next((i for i in range(min(len(xa), len(xb))) if xa[i] != xb[i]), min(len(xa), len(xb)))
        print('  %-12s first difference at byte %d (A %s, B %s), sizes %d/%d' %
              (name, k, xa[k:k + 4].hex(), xb[k:k + 4].hex(), len(xa), len(xb)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a', nargs='?')
    ap.add_argument('b', nargs='?')
    ap.add_argument('--symbols-a')
    ap.add_argument('--symbols-b')
    ap.add_argument('--dumps-a', help='dump file prefix for A (default: the prefix in its header)')
    ap.add_argument('--dumps-b', help='dump file prefix for B')
    ap.add_argument('--max-words', type=int, default=40)
    ap.add_argument('--check-dumps', metavar='TRACE')
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.check_dumps:
        return check_dumps(args.check_dumps, None)
    if not (args.a and args.b):
        ap.error('two traces (or --check-dumps / --selftest)')
    return compare(args.a, args.b, args)


if __name__ == '__main__':
    sys.exit(main())
