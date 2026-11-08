"""Guard for the state trace's learned pointer words (finding 081, phase 2 / W1).

A learned word is a host code or data address, hashed by its symbol. A word that is not a pointer
can still look like one: a float of game state that differs between builds (a sound stream
position, a timer) and that happens to end in 0x00 in both builds. The guard flags a word whose two
values both end in 0x00 and are both finite floats in [1e-3, 1e6]. A flagged word is not learned;
it goes to review.

    python port/tools/codeptrs_guard.py --list port/tools/state_trace_codeptrs.txt A.bin B.bin

checks the learned list against a pair of EE RAM dumps (the raw ee dumps of the same field in two
builds) and prints the suspects. Exit 1 when there is any suspect.
"""
import argparse
import struct
import sys


def is_float_like(v):
    """A 32-bit value that is a finite float in [1e-3, 1e6] and ends in 0x00."""
    if v & 0xFF:
        return False
    f = struct.unpack('<f', struct.pack('<I', v))[0]
    return f == f and 1e-3 <= abs(f) <= 1e6


def is_suspect(a, b):
    """True when a learned word's two values (build A, build B) look like floats, not pointers."""
    return is_float_like(a) and is_float_like(b)


def learned_addresses(path):
    out = []
    for line in open(path, encoding='utf-8'):
        if line.startswith('0x'):
            out.append(int(line.split()[0], 16))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--list', required=True, help='the learned list (state_trace_codeptrs.txt)')
    ap.add_argument('a', help='EE RAM dump, build A (raw ee dump of one field)')
    ap.add_argument('b', help='EE RAM dump, build B (the same field)')
    args = ap.parse_args()
    A = open(args.a, 'rb').read()
    B = open(args.b, 'rb').read()
    suspects = []
    for addr in learned_addresses(args.list):
        x = struct.unpack_from('<I', A, addr)[0]
        y = struct.unpack_from('<I', B, addr)[0]
        if x != y and is_suspect(x, y):
            suspects.append((addr, x, y))
    for addr, x, y in suspects:
        fx = struct.unpack('<f', struct.pack('<I', x))[0]
        fy = struct.unpack('<f', struct.pack('<I', y))[0]
        print('suspect 0x%08X: %08X (%g) vs %08X (%g)' % (addr, x, fx, y, fy))
    print('%d suspects' % len(suspects))
    return 1 if suspects else 0


if __name__ == '__main__':
    sys.exit(main())
