"""Load/store offsets that would wrap on i686 but not in wasm32 (dhd_core.wasm).

The EE and i686 add an address and a constant in 32 bits, so p[0x3fffffff] (float) is p - 4.
clang for wasm32 folds such a constant into the load/store's memarg offset, and wasm adds the
offset WITHOUT wrapping (wasm2c: (u64)base + offset): the access lands 4 GiB past p. The browser
traps (out of bounds), the 64-bit host faults on uncommitted guest memory (commit edf2dc201,
func_00224CC8's weather packet builder).

The module's memory is at most MAX_MEMORY (2 GiB, build_wasm.py), so every valid base is below
2 GiB, and base + offset can reach 2^32 (where i686 and wasm differ) only when the offset is at
least WRAP_OFFSET = 2^32 - MAX_MEMORY. Every such offset is a hit; with a constant base (an
absolute address) it is out of bounds anyway. So "no hits" is a complete check for this class
in the module, and wasm2c translates the module one to one (no wasm-opt between them).

The second list is informational: offsets outside every guest region (port/runtime/memory.c,
port/host64/guest_memory.c and memory_layout.h): an offset there is a constant address or a
folded displacement that points at no mapped memory. Such offsets also fail unless allowed.

A hit that is known to be harmless goes in ALLOW below, as (function name, offset) with a comment
saying why. The build (build_wasm.py) fails on any other hit.

usage: python port/tools/wasm_offset_scan.py [build/wasm/dhd_core.wasm] [--all] [--json FILE]
"""
import argparse
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wasm_postlink as w  # noqa: E402 (the module parser and instruction walker)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAX_MEMORY = 0x80000000                 # build_wasm.py --max-memory
WRAP_OFFSET = (1 << 32) - MAX_MEMORY    # an offset this big can carry base + offset past 2^32


def module_base():
    """HOST64_MODULE_BASE: the module's own memory (stack/data/bss/heap) starts there."""
    text = open(os.path.join(ROOT, 'port', 'host64', 'memory_layout.h'), encoding='utf-8').read()
    return int(re.search(r'#define\s+HOST64_MODULE_BASE\s+(0x[0-9A-Fa-f]+)', text).group(1), 16)


# Guest regions an offset may point into: (start, end, name). An offset inside one is a constant
# address there, or a displacement off a base whose region the folded constant names.
REGIONS = [
    (0x00000000, 0x02000000, 'RAM (PS2 addresses, struct displacements)'),
    (0x08000000, 0x0A000000, 'RAM view (PS2_PTR, PS2_RAM_BASE)'),
    (0x10000000, 0x10010000, 'EE hardware registers'),
    (0x11000000, 0x11010000, 'VU memory'),
    (0x12000000, 0x12002000, 'GS privileged registers'),
    (0x13000000, 0x14000000, 'host GS block'),
    (0x20000000, 0x22000000, 'RAM uncached view'),
    (0x28000000, 0x2A000000, 'RAM view (uncached | PS2_RAM_BASE)'),
    (0x30000000, 0x32000000, 'RAM uncached-accelerated view'),
    (0x38000000, 0x3A000000, 'RAM view (uncached-accelerated | PS2_RAM_BASE)'),
    (0x70000000, 0x70004000, 'scratchpad'),
    (0x70004000, 0x70008000, 'HLE window (memory.h PS2_HLE_WINDOW: the SIF RPC packets)'),
]

# (function, offset): hits reviewed and known harmless. Each entry needs a comment.
ALLOW = {
}

OPNAMES = {0x28: 'i32.load', 0x29: 'i64.load', 0x2A: 'f32.load', 0x2B: 'f64.load',
           0x2C: 'i32.load8_s', 0x2D: 'i32.load8_u', 0x2E: 'i32.load16_s', 0x2F: 'i32.load16_u',
           0x30: 'i64.load8_s', 0x31: 'i64.load8_u', 0x32: 'i64.load16_s', 0x33: 'i64.load16_u',
           0x34: 'i64.load32_s', 0x35: 'i64.load32_u', 0x36: 'i32.store', 0x37: 'i64.store',
           0x38: 'f32.store', 0x39: 'f64.store', 0x3A: 'i32.store8', 0x3B: 'i32.store16',
           0x3C: 'i64.store8', 0x3D: 'i64.store16', 0x3E: 'i64.store32'}


def memarg_offset(b, i):
    align, i = w.uleb(b, i)
    if align & 0x40:  # multi-memory: a memory index follows
        _, i = w.uleb(b, i)
    return w.uleb(b, i)[0]


def has_memarg(op, sub):
    if op in w.MEMARG_OPS:
        return True
    if op == 0xFD:  # SIMD loads/stores, lane loads/stores
        return sub <= 11 or sub in (92, 93) or 84 <= sub <= 91
    return op == 0xFE and sub != 3  # atomics (3 is atomic.fence)


def opname(op, sub):
    if op in OPNAMES:
        return OPNAMES[op]
    return '%s.%d' % ({0xFD: 'simd', 0xFE: 'atomic'}[op], sub)


def scan(path):
    """[(function, code offset, op, memarg offset)] for every load/store with an offset."""
    m = w.Module(open(path, 'rb').read())
    out = []
    for k in range(len(m.bodies)):
        b, start = m.body_code(k)
        name = m.name(k + m.imported_funcs)
        prev = None
        for at, op, sub, imm, nxt in w.instructions(b, start, len(b)):
            if has_memarg(op, sub):
                off = memarg_offset(b, imm)
                if off:
                    # a load right after i32.const reads a constant address (stores: unknown here)
                    const = prev is not None and prev[0] == 0x41 and op < 0x36
                    out.append((name, at, opname(op, sub), off, const))
            prev = (op, sub)
    return m, out


def region(off, top):
    for lo, hi, what in REGIONS:
        if lo <= off < hi:
            return what
    if module_base() <= off < top:
        return 'module memory (stack, data, bss, heap)'
    return None


def memory_limits(m):
    """(initial, maximum) bytes of the module's memory (its own, not imported); max None if open."""
    p = m.section(5)
    if not p:
        return 0, None
    p = p[1]
    _, i = w.uleb(p, 0)
    flags = p[i]
    lo, i = w.uleb(p, i + 1)
    hi = w.uleb(p, i)[0] if flags & 1 else None
    return lo * 65536, None if hi is None else hi * 65536


def check(path):
    """(wrap hits, unmapped hits, census): the hits leave out ALLOW."""
    m, rows = scan(path)
    initial, maximum = memory_limits(m)
    if maximum is None or maximum > MAX_MEMORY:
        # a bigger memory allows bigger bases, so smaller offsets could wrap: the bound is wrong
        raise SystemExit('wasm offset scan: module memory max %s > %#x; update MAX_MEMORY here'
                         % (maximum if maximum is None else hex(maximum), MAX_MEMORY))
    top = max(initial, module_base())
    wrap, unmapped = [], []
    census = collections.Counter()
    for name, at, op, off, const in rows:
        where = region(off, top)
        census[where or 'NO REGION'] += 1
        row = {'function': name, 'code_offset': at, 'op': op, 'offset': off, 'constant_base': const,
               'region': where, 'allowed': (name, off) in ALLOW}
        if off >= WRAP_OFFSET:
            wrap.append(row)
        elif where is None:
            unmapped.append(row)
    return wrap, unmapped, census


def show(rows, title):
    print('%s: %d' % (title, len(rows)))
    for r in rows:
        print('  %-40s +%#08x  %-16s offset %#010x%s%s' % (
            r['function'], r['code_offset'], r['op'], r['offset'],
            '  (constant base)' if r['constant_base'] else '',
            '  ALLOWED: ' + ALLOW[(r['function'], r['offset'])] if r['allowed'] else ''))


def run(path, report=None, verbose=False):
    """Prints the scan; returns the number of hits that are not allowed (build_wasm.py's check)."""
    wrap, unmapped, census = check(path)
    show(wrap, 'offsets >= %#x (base + offset can pass 2^32: wraps on i686, not in wasm)' % WRAP_OFFSET)
    show(unmapped, 'offsets in no guest region')
    if verbose:
        for k, v in sorted(census.items(), key=lambda kv: -kv[1]):
            print('%8d  %s' % (v, k))
    if report:
        with open(report, 'w') as f:
            json.dump({'wrap_offset': WRAP_OFFSET, 'wrap': wrap, 'unmapped': unmapped,
                       'census': dict(census)}, f, indent=1)
    bad = [r for r in wrap + unmapped if not r['allowed']]
    print('offset scan: %s' % ('clean' if not bad else '%d hit(s) not in ALLOW (port/tools/wasm_offset_scan.py)' % len(bad)))
    return len(bad)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('wasm', nargs='?', default=os.path.join(ROOT, 'build', 'wasm', 'dhd_core.wasm'))
    ap.add_argument('--all', action='store_true', help='also print the census by region')
    ap.add_argument('--json', help='write the hits and the census here')
    a = ap.parse_args()
    sys.exit(1 if run(a.wasm, a.json, a.all) else 0)


if __name__ == '__main__':
    main()
