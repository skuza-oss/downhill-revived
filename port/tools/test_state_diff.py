"""state_diff.py's comparison of two state traces (finding 205), on small made-up traces: a pointer
that moved with the core's layout is equal, a value that only looks like a core address is equal
only when it is the same value, and a real change in any column is reported.

    python port/tools/test_state_diff.py     (also run by test_runtime.py)"""
import argparse
import contextlib
import io
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import state_diff  # noqa: E402
import trace_columns  # noqa: E402

COLUMNS = ('field\tmenu\tphase\tclock\tram\tscratch\thw\tcpu\tvu0\tgs_stream\tgs_regs\tgs_vram\tsound\tcode\t'
           'regptrs\tcombined\tstackwords')
HASH = '0123456789abcdef'


def row(field, regptrs='-', ram=HASH, cpu=HASH):
    return '\t'.join([str(field), '3', '1', '0', ram, HASH, HASH, cpu, HASH, HASH, HASH, HASH, HASH, HASH,
                      regptrs, HASH, '0'])


def trace(rows, core):
    return '# state-trace 2 exe=x core=%s prefix=p env=\n%s\n%s\n' % (core, COLUMNS, '\n'.join(rows))


def run_tools(a_rows, b_rows):
    """(state_diff exit, its output, trace_columns exit) for two traces of these rows."""
    with tempfile.TemporaryDirectory() as d:
        pa, pb = os.path.join(d, 'a.tsv'), os.path.join(d, 'b.tsv')
        open(pa, 'w', encoding='ascii').write(trace(a_rows, '70C00000:70D00010:7045'))
        open(pb, 'w', encoding='ascii').write(trace(b_rows, '70C00010:70D00020:7046'))
        args = argparse.Namespace(dumps_a=None, dumps_b=None, symbols_a=None, symbols_b=None, max_words=40)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = state_diff.compare(pa, pb, args)
        argv = sys.argv
        sys.argv = ['trace_columns.py', pa, pb]
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                columns_code = trace_columns.main()
        finally:
            sys.argv = argv
        return code, out.getvalue(), columns_code


def expect(name, a_rows, b_rows, equal, first=None):
    code, out, columns_code = run_tools(a_rows, b_rows)
    assert (code == 0) == equal, '%s: state_diff exit %d\n%s' % (name, code, out)
    assert (columns_code == 0) == equal, '%s: trace_columns exit %d' % (name, columns_code)
    if first:
        assert 'FIRST DIFFERENCE: field %s' % first[0] in out and first[1] in out, '%s:\n%s' % (name, out)


def main():
    # the lanes of the lane-203 case: r8 holds text (0x7049544E) in both builds; it lies in a bss
    # object whose base moved 16 bytes, so the identity's offset differs; the raw value is the same
    text_a = 'c8.0=7049544E:2:A30D15E8:19A9B6'
    text_b = 'c8.0=7049544E:2:A30D15E8:19A9A6'
    expect('text that looks like a core address', [row(1, text_a)], [row(1, text_b)], True)
    # a real pointer into a moved object: raw values differ by the move, the identity is the same
    expect('a pointer that moved with the data', [row(1, 'c4.0=7029F2B4:2:11111111:4')],
           [row(1, 'c4.0=7029F2C4:2:11111111:4')], True)
    expect('a function pointer whose table index moved', [row(1, 'c31.0=00012345:1:22222222:0')],
           [row(1, 'c31.0=00012385:1:22222222:0')], True)
    expect('a shadow stack address under a moved stack', [row(1, 'v29.0=70C0FF00:3:00000000:100')],
           [row(1, 'v29.0=70C0FF10:3:00000000:100')], True)
    expect('several lanes, none different', [row(1, text_a + ',c4.1=70123456:0:00000000:0')],
           [row(1, text_b + ',c4.1=70123456:0:00000000:0')], True)
    # real differences: another value with another identity, a value with no identity, a lane in
    # one list only, a pointer to another offset of the same object
    expect('text changed', [row(1), row(2, text_a)], [row(1), row(2, 'c8.0=7049544F:2:A30D15E8:19A9A7')],
           False, ('2', 'c8.0'))
    expect('no identity, other value', [row(1, 'c4.1=70123456:0:00000000:0')],
           [row(1, 'c4.1=70123457:0:00000000:0')], False, ('1', 'regptrs'))
    expect('a lane only in one build', [row(1, '-')], [row(1, text_b)], False, ('1', 'regptrs'))
    expect('the same object, another offset', [row(1, 'c4.0=7029F2B4:2:11111111:4')],
           [row(1, 'c4.0=7029F2C8:2:11111111:8')], False, ('1', 'regptrs'))
    expect('the same raw tag-0 value twice', [row(1, 'c4.1=70123456:0:00000000:0')],
           [row(1, 'c4.1=70123456:0:00000000:0')], True)
    # every other column is compared as it is
    expect('a RAM word', [row(1), row(2)], [row(1), row(2, ram='fedcba9876543210')], False, ('2', 'ram'))
    expect('a register outside the core ranges', [row(1)], [row(1, cpu='fedcba9876543210')], False, ('1', 'cpu'))
    expect('a missing row', [row(1), row(2)], [row(1)], False)
    print('test_state_diff: ok (13 cases)')


if __name__ == '__main__':
    main()
