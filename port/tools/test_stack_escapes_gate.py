#!/usr/bin/env python3
"""The gate of port/tools/stack_escapes.py --check against the two false passes of finding 219.

Needs the matching toolchain (the same as the check). Part one:

1. A unit that does not compile with -gstabs used to be a WARN that never failed the check. Here one
   made-up unit with a syntax error joins the analysis: `--check` must fail on it, pass when the
   unit is listed with a reason in the compile-failure list, and fail on a listed unit that
   compiles. The old behaviour (warn, exit 0) is shown for the report-only run.

    python port/tools/test_stack_escapes_gate.py
"""
import argparse, contextlib, io, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stack_escapes as se  # noqa: E402


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
            funcs, summ, raw, failed, rounds = se.run(jobs)
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--jobs', '-j', type=int, default=6)
    ap.add_argument('--only', action='append', default=[], help='only this function\'s fix (repeatable)')
    a = ap.parse_args()
    test_compile_failures(a.jobs)
    print('test_stack_escapes_gate: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'OK'))
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
