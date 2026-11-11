"""Compare two state traces column by column (finding 081): the first differing field of each
section, and PASS when every section outside --ignore matches in every row.

    python port/tools/trace_columns.py A.tsv B.tsv [--ignore ram,cpu]

The regptrs column compares lane by lane, by raw value or identity (state_diff.py, finding 205);
every other column as it is."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from state_diff import cells_differ  # noqa: E402  (regptrs compares by lane, finding 205)

SKIP = {'combined', 'stackwords', 'field'}


def load(path):
    header, rows = None, []
    for line in open(path, encoding='ascii'):
        cells = line.rstrip('\n').split('\t')
        if line.startswith('field'):
            header = cells
        elif line[:1].isdigit():
            rows.append(dict(zip(header, cells)))
    return header, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--ignore', default='', help='comma-separated sections allowed to differ')
    args = ap.parse_args()
    ignore = {c for c in args.ignore.split(',') if c}
    ha, ra = load(args.a)
    hb, rb = load(args.b)
    columns = [c for c in ha if c in hb and c not in SKIP]
    first = {}
    for x, y in zip(ra, rb):
        for c in columns:
            if c not in first and cells_differ(c, x[c], y[c]):
                first[c] = int(x['field'])
    print(f'rows: A {len(ra)}, B {len(rb)}')
    for c in columns:
        state = f'differs from field {first[c]}' if c in first else 'identical'
        print(f'  {c:10} {state}{" (ignored)" if c in ignore and c in first else ""}')
    failed = len(ra) != len(rb) or any(c not in ignore for c in first)
    print('FAIL' if failed else 'PASS')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
