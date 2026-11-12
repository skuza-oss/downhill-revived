#!/usr/bin/env python3
"""The gate of port/tools/stack_escapes.py --check against the two false passes of finding 219.

Needs the matching toolchain (the same as the check). Two parts:

1. A unit that does not compile with -gstabs used to be a WARN that never failed the check. Here one
   made-up unit with a syntax error joins the analysis: `--check` must fail on it, pass when the
   unit is listed with a reason in the compile-failure list, and fail on a listed unit that
   compiles. The old behaviour (warn, exit 0) is shown for the report-only run.
2. A reviewed 'fixed' line used to pass whenever its unit had any port patch. Here each fix is
   removed in turn, from a temporary copy of the patch table (the repository's files are never
   touched), and the check must fail for that fix's lines: the patched source is analysed, so the
   overrun comes back. The old predicate is run on the same table to show it still passes.

    python port/tools/test_stack_escapes_gate.py [--only FUNCTION ...]
"""
import argparse, contextlib, io, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stack_escapes as se  # noqa: E402

sys.path.insert(0, os.path.join(se.ROOT, 'port'))
import patches  # noqa: E402
import patches_stack  # noqa: E402

FAILS = []


def expect(name, got, want):
    if got != want:
        FAILS.append(name)
        print('FAIL %s: %r, expected %r' % (name, got, want))
    else:
        print('ok   %s' % name)


def captured(fn, *args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        r = fn(*args)
    return r, out.getvalue()


# ---- 1. compile failures ----------------------------------------------------------------------

def test_compile_failures(jobs):
    test_dir = os.path.join(se.ROOT, 'build', 'stack_escapes_test')
    os.makedirs(test_dir, exist_ok=True)
    broken = os.path.join(test_dir, 'broken.c')
    with open(broken, 'w', newline='\n') as f:
        f.write('int f( {\n')
    real_units, real_list = se.units, se.COMPILE_FAILURES
    listing = os.path.join(test_dir, 'compile_failures.txt')

    def with_broken():
        return real_units() + [('made-up/broken.c', broken, [])]

    se.units = with_broken
    se.COMPILE_FAILURES = listing
    try:
        for text in ('', 'made-up/broken.c a syntax error, on purpose\n', 'src/gone.c it does not exist\n'):
            with open(listing, 'w', newline='\n') as f:
                f.write(text)
            funcs, summ, rows, failed, rounds, fitting = se.analyse_all(jobs)
            expected = se.load_compile_failures()
            names = [r for r, e in failed]
            if not text:
                expect('the made-up unit fails to compile', names, ['made-up/broken.c'])
                bad, out = captured(se.check_compile_failures, failed, expected, True)
                expect('--check: an unlisted failing unit is an error (it was a WARN with exit 0)', bad, 1)
                expect('  and says which unit', 'ERROR src/made-up/broken.c' in out, True)
                bad, out = captured(se.check_compile_failures, failed, expected, False)
                expect('report-only: still just a warning', (bad, out.startswith('WARN src/made-up/broken.c')), (0, True))
            elif text.startswith('made-up'):
                bad, out = captured(se.check_compile_failures, failed, expected, True)
                expect('--check: a listed failing unit passes', (bad, out), (0, ''))
            else:
                bad, out = captured(se.check_compile_failures, failed, expected, True)
                expect('--check: a listed unit that compiles (or is gone) fails', bad, 2)  # broken + stale line
                expect('  and the list names it', 'src/gone.c is listed as not compiling' in out, True)
    finally:
        se.units, se.COMPILE_FAILURES = real_units, real_list
        for p in (broken, listing):
            if os.path.exists(p):
                os.remove(p)


# ---- 2. each fix removed in turn --------------------------------------------------------------

def stack_edits(unit):
    d = {}
    patches_stack.add(d)
    return d.get(unit, [])


def stack_fix(unit):
    mine = stack_edits(unit)
    return lambda edit: edit in mine


# function, unit, which of the unit's edits are the fix
FIXES = [
    ('func_0019E550', 'unsorted/0019E550.c', lambda e: 'frame' in e[2].lower()),            # finding 207
    ('func_0017A678', 'unsorted/0017A678.cpp', lambda e: e[2] == '    char buf[0x40];'),  # finding 094
    ('func_00242998', 'unsorted/00242998.cpp', lambda e: 'distance[4]' in e[2]),            # older, patches.py
    ('func_00155BA8', 'unsorted/00155BA8.c', stack_fix('unsorted/00155BA8.c')),
    ('func_0017E440', 'unsorted/0017E440.c', stack_fix('unsorted/0017E440.c')),
    ('func_001F8CA8', 'unsorted/001F8CA8.c', stack_fix('unsorted/001F8CA8.c')),
    ('func_0013C6A8', 'unsorted/0013C6A8.cpp', stack_fix('unsorted/0013C6A8.cpp')),
    ('func_00150C58', 'unsorted/001508D8.cpp', stack_fix('unsorted/001508D8.cpp')),
    ('func_001AE358', 'unsorted/001AE358.cpp', stack_fix('unsorted/001AE358.cpp')),
    ('func_00124B40', 'unsorted/00124B40.cpp', stack_fix('unsorted/00124B40.cpp')),
]


def old_check_fixed(reviewed):
    """The check before finding 219: a fixed line passed when its unit was a key of PATCHES."""
    import re
    bad = 0
    for key, (verdict, reason, n) in reviewed.items():
        if verdict == 'fixed':
            m = re.match(r'(\S+?):', reason)
            if not m or m.group(1) not in patches.PATCHES:
                bad += 1
    return bad


def fixed_lines(reviewed, function):
    return [k for k, v in reviewed.items() if v[0] == 'fixed' and k[0] == function]


def test_fixes(jobs, only):
    reviewed = se.load_reviewed()
    # compiled once: the units a run does not change keep their assembly, only the unit whose fix is
    # removed is compiled again (its patched text differs); the analysis itself runs in full each time
    real_compile, memo, mutated = se.compile_port_unit, {}, set()

    def compile_once(job):
        if job[0] in mutated:
            return real_compile(job)
        if job[0] not in memo:
            memo[job[0]] = real_compile(job)
        return memo[job[0]]

    se.compile_port_unit = compile_once
    try:
        test_each_fix(jobs, only, reviewed, mutated)
    finally:
        se.compile_port_unit = real_compile


def test_each_fix(jobs, only, reviewed, mutated):
    funcs, summ, rows, failed, rounds, fitting = se.analyse_all(jobs)
    bad, out = captured(se.check_fixed, reviewed, rows, funcs, fitting)
    expect('the fixes as they are: every fixed line proven', (bad, out), (0, ''))
    original = {u: list(p) for u, p in patches.PATCHES.items()}
    try:
        for function, unit, is_fix in FIXES:
            if only and function not in only:
                continue
            started = time.time()
            lines = fixed_lines(reviewed, function)
            expect('%s has reviewed fixed line(s)' % function, bool(lines), True)
            dropped = [e for e in original[unit] if is_fix(e)]
            expect('%s: the fix is %d edit(s) of %s' % (function, len(dropped), unit), 0 < len(dropped) <= 4, True)
            # the unit keeps an unrelated edit (the reviewer's case: widescreen edits on the same unit),
            # here its own first line with a trailing blank
            with open(os.path.join(se.ROOT, 'src', unit), encoding='utf-8', errors='replace') as f:
                first = f.readline().rstrip('\r\n')
            patches.PATCHES[unit] = [e for e in original[unit] if not is_fix(e)] + [('text', first, first + ' ')]
            mutated.clear()
            mutated.add(unit)
            try:
                expect('%s: the old check still passes with the fix gone (the false pass)'
                       % function, old_check_fixed(reviewed), 0)
                funcs, summ, rows, failed, rounds, fitting = se.analyse_all(jobs)
                bad, out = captured(se.check_fixed, reviewed, rows, funcs, fitting)
                mine = [l for l in out.splitlines() if l.startswith('ERROR') and ': %s ' % function in l]
                expect('%s: the check fails (%d line(s) of it)' % (function, len(mine)), bad >= 1 and len(mine) >= 1, True)
                expect('%s: because the overrun is back in the patched source' % function,
                       any('still overruns' in l for l in mine), True)
                for l in mine[:1]:
                    print('       ' + l)
            finally:
                patches.PATCHES.clear()
                patches.PATCHES.update({u: list(p) for u, p in original.items()})
            print('       (%.0f s)' % (time.time() - started))
    finally:
        patches.PATCHES.clear()
        patches.PATCHES.update(original)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--jobs', '-j', type=int, default=6)
    ap.add_argument('--only', action='append', default=[], help='only this function\'s fix (repeatable)')
    ap.add_argument('--skip-compile', action='store_true')
    a = ap.parse_args()
    if not a.skip_compile and not a.only:
        test_compile_failures(a.jobs)
    test_fixes(a.jobs, a.only)
    print('test_stack_escapes_gate: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'OK'))
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
