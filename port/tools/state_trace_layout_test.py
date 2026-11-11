"""The state trace's own test, in game runs (finding 205): it must report a real change of game state,
and it must not report a change of the core's layout alone.

    python port/tools/state_trace_layout_test.py flips [--exe EXE] [--scenario race] [--fields 4100]
    python port/tools/state_trace_layout_test.py build-pad [--pad 4096] [-j 6]
    python port/tools/state_trace_layout_test.py layout [--exe EXE] [--pad-exe EXE] [--recipes boot,race]

flips: one run of the recipe as it is, then three with DHD_STATE_FLIP (state_trace.c), each
changing one thing just before a field is hashed: the player's rider's position (x of its body
node, a float, 50 fields before the end; found through the pointers replay.c follows, read from
the base run's dump of that field), a RAM word (50 fields before the end) and an EE register (the
last field). state_diff.py must report each at its field, in the column it belongs to (ram,
cpu); the rider flip must also change what the game does next (another column differs later).

build-pad: the same checkout built again with DHD_CORE_LAYOUT_PAD=<bytes> (build_wasm.py): the core
gets read-only data, data, bss and function-table entries in front of everything else, so every
address of the core's own moves and the game does not change. The game goes to
build/host64-layout-pad/dhd64.exe; build/wasm is then rebuilt without the pad (the core cache).

layout: the recipes on the normal build and on the padded one; every trace must be equal, and
the traces' headers must show that the core's layouts differ (core=: the shadow stack's top, the
heap's base, the function table's rows).

Runs are headless, deterministic and take the game lock (state_trace_run.py). Exit 0 when every
check holds."""
import argparse
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'port' / 'tools'
TRACES = ROOT / 'build' / 'state-trace'
EXE = ROOT / 'build' / 'host64-exe' / 'dhd64.exe'
PAD_DIR = ROOT / 'build' / 'host64-layout-pad'
PY = sys.executable

# The player's rider (replay.c): a pointer at 0x002AF2C0 + 0x50130; its body node at +0x792C; the
# node's position at +0x10.
PLAYER_RIDER = 0x002AF2C0 + 0x50130
RAM_WORD = 0x00500000            # a word of the game's heap
REGISTER = 16                    # s0


def trace_run(scenario, name, exe, fields=None, env=(), dump=None):
    cmd = [PY, str(TOOLS / 'state_trace_run.py'), scenario, '--name', name, '--exe', str(exe)]
    if fields:
        cmd += ['--fields', str(fields)]
    if dump:
        cmd += ['--dump', str(dump)]
    for item in env:
        cmd += ['--env', item]
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print(p.stdout.strip())
    if p.returncode:
        sys.exit('%s: run failed (exit %d)\n%s' % (name, p.returncode, p.stderr[-2000:]))
    return TRACES / (name + '.tsv')


def diff(a, b):
    p = subprocess.run([PY, str(TOOLS / 'state_diff.py'), str(a), str(b)], cwd=ROOT, capture_output=True, text=True)
    return p.returncode, p.stdout


def player_position(base, field):
    """The EE address of the player's rider's body position (x) at field, from the base run's
    ee-raw.bin dump of that field (the run is deterministic: the flip run has the same)."""
    ram = open('%s-f%d-ee-raw.bin' % (str(base)[:-4], field), 'rb').read()
    word = lambda address: struct.unpack_from('<I', ram, address & 0x01FFFFFC)[0]
    rider = word(PLAYER_RIDER)
    node = word(rider + 0x792C)
    if not rider or not node:
        sys.exit('flips: no player rider at field %d (rider %08X, node %08X)' % (field, rider, node))
    return (node + 0x10) & 0x01FFFFFF


def columns_changed(a, b):
    """The columns trace_columns.py finds different."""
    p = subprocess.run([PY, str(TOOLS / 'trace_columns.py'), str(a), str(b)], cwd=ROOT, capture_output=True, text=True)
    return [line.split()[0] for line in p.stdout.splitlines() if 'differs from field' in line]


def flips(args):
    fields = args.fields
    early = fields - 50
    base = trace_run(args.scenario, 'flip-base', args.exe, fields, dump=early)
    position = player_position(base, early)
    print('the player rider\'s position: EE 0x%08X' % position)
    cases = [('rider', '%d:ram:0x%X:0x00400000' % (early, position), early, 'ram'),
             ('ram', '%d:ram:0x%X:0x1' % (early, RAM_WORD), early, 'ram'),
             ('register', '%d:reg:%d:0x1' % (fields, REGISTER), fields, 'cpu')]
    failed = 0
    for label, flip, field, column in cases:
        flipped = trace_run(args.scenario, 'flip-' + label, args.exe, fields, ['DHD_STATE_FLIP=' + flip])
        code, out = diff(base, flipped)
        first = re.search(r'FIRST DIFFERENCE: field (\d+) \(row \d+\): (.*)', out)
        ok = bool(code == 1 and first and int(first.group(1)) == field and column in first.group(2).split(', '))
        changed = columns_changed(base, flipped)
        if label == 'rider':  # the game rode on from the changed position
            ok = ok and any(c not in ('ram', 'combined') for c in changed)
        failed += not ok
        print('%s flip (%s): %s; columns that differ: %s' % (label, flip, 'reported' if ok else 'NOT AS EXPECTED',
                                                              ', '.join(changed)))
        print('  ' + '\n  '.join(line for line in out.splitlines() if line.startswith(('FIRST', 'rows differing', '  '))))
    print('flips: %s' % ('PASS' if not failed else 'FAIL'))
    return 1 if failed else 0


def build_pad(args):
    env = dict(os.environ, DHD_CORE_LAYOUT_PAD=str(args.pad))
    jobs = ['-j', str(args.j)]
    steps = [([PY, str(TOOLS / 'build_wasm.py')] + jobs, env),
             ([PY, str(TOOLS / 'build_host64.py'), str(ROOT / 'build' / 'wasm' / 'dhd_core.wasm'), '--out',
               str(PAD_DIR), '--jobs', str(args.j)], env),
             ([PY, str(TOOLS / 'build_wasm.py')] + jobs, {k: v for k, v in os.environ.items()
                                                          if k != 'DHD_CORE_LAYOUT_PAD'})]
    for cmd, step_env in steps:
        print('==', ' '.join(cmd[1:]), flush=True)
        if subprocess.run(cmd, cwd=ROOT, env=step_env).returncode:
            sys.exit('build-pad: step failed')
    print('padded game: %s' % (PAD_DIR / 'dhd64.exe'))
    return 0


def core_layout(path):
    with open(path, encoding='ascii', errors='replace') as f:
        m = re.search(r' core=(\S+)', f.readline())
    return m.group(1) if m else None


def layout(args):
    failed = 0
    for recipe in args.recipes.split(','):
        a = trace_run(recipe, 'layout-%s-normal' % recipe, args.exe)
        b = trace_run(recipe, 'layout-%s-pad' % recipe, args.pad_exe)
        code, out = diff(a, b)
        la, lb = core_layout(a), core_layout(b)
        moved = la and lb and la != lb
        ok = code == 0 and moved
        failed += not ok
        print('%s: core layouts %s / %s (%s); diff %s' % (recipe, la, lb, 'moved' if moved else 'NOT MOVED',
                                                         'PASS' if code == 0 else 'FAIL'))
        if code:
            print('  ' + '\n  '.join(out.splitlines()[:12]))
    print('layout: %s' % ('PASS' if not failed else 'FAIL'))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    f = sub.add_parser('flips')
    f.add_argument('--exe', default=str(EXE))
    f.add_argument('--scenario', default='race')
    f.add_argument('--fields', type=int, default=4100, help='run length; the race is under way from about 3650')
    b = sub.add_parser('build-pad')
    b.add_argument('--pad', type=int, default=4096, help='bytes of each padding kind (a multiple of 4)')
    b.add_argument('-j', type=int, default=6)
    lay = sub.add_parser('layout')
    lay.add_argument('--exe', default=str(EXE))
    lay.add_argument('--pad-exe', default=str(PAD_DIR / 'dhd64.exe'))
    lay.add_argument('--recipes', default='boot,race')
    args = ap.parse_args()
    return {'flips': flips, 'build-pad': build_pad, 'layout': layout}[args.command](args)


if __name__ == '__main__':
    sys.exit(main())
