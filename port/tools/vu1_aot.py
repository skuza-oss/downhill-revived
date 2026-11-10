"""Translate straight-line VU1 blocks from the user's local microcode into C.

The generated include is game-derived data and belongs only under build/.
This first tier removes instruction decoding; pipeline state remains in the
reference runtime. Unknown or modified blocks use the interpreter unchanged.
No upstream implementation is copied.

build_wasm.py (for the native VU0 routines' words), build_host64.py and port/web/build_graphics.py
generate it into their own build folders.

A block runs only where micro memory holds the words it was translated from. The executable
keeps no copy of them: the include lists where each block's words are in .vutext, and
ps2_vu_aot_load (port/runtime/vu0_interp.c) copies them from the data image read from the
player's disc. The test and bench programs, which have no disc image, define
PS2_VU_AOT_EMBED and get the words as constants.
"""
from pathlib import Path
import os
import struct

from vudis import BASE, walk

ROOT = Path(__file__).resolve().parents[2]


def ends_block(lo, hi):
    if hi & (1 << 30):
        return True
    if hi >> 31:
        return False
    # XGKICK is a boundary: its callback may inspect/change micro memory.
    # Simple branch/E delay slots can be fused by blocks().
    return lo >> 25 in (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f) or (
        lo >> 25 == 0x40 and (lo & 63) >= 60 and ((lo >> 6 & 31) * 4 + (lo & 3)) == 0x6c)


def blocks(pairs):
    starts = set()
    for pc, (lo, hi) in pairs.items():
        if hi & (1 << 30):
            starts.add(pc + 2)  # next program after the E delay slot
        if not hi >> 31 and lo >> 25 in (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
            starts.add(pc + 2)  # conditional fallthrough / return address
            if lo >> 25 not in (0x24, 0x25):
                immediate = lo & 0x7ff
                if immediate & 0x400:
                    immediate -= 0x800
                starts.add((pc + 1 + immediate) & 0x7ff)
    result = {}
    at = min(pairs)
    while at <= max(pairs):
        if at not in pairs:
            at += 1
            continue
        start, block = at, []
        while at in pairs and len(block) < 32:
            pair = pairs[at]
            block.append(pair)
            at += 1
            if ends_block(*pair) or at in starts:
                break
        # Fuse a control transfer with its simple delay slot. The run loop
        # completes that transition immediately after this block returns.
        if ends_block(*block[-1]) and at in pairs:
            lo, hi = block[-1]
            branch = not hi >> 31 and lo >> 25 in (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f)
            if (branch or hi & (1 << 30)) and not ends_block(*pairs[at]):
                block.append(pairs[at])
                # Still create an independent block for the slot: an entry or
                # another branch may target it without the preceding branch.
        result[start] = tuple(block)
    return result


def delay_slots(pairs):
    """Single pairs for the slots after a branch or an E bit, where the interpreter does not
    enter straight-line blocks: the run loop keeps the branch and end transitions."""
    result = {}
    for pc, (lo, hi) in pairs.items():
        branch = not hi >> 31 and lo >> 25 in (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f)
        if (branch or hi & (1 << 30)) and pc + 1 in pairs:
            result[pc + 1] = (pairs[pc + 1],)
    return result


BRANCH_OPS = (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f)


def mac_writer(hi):
    """'mac' if the upper instruction is an FMAC that updates the MAC flags (and the status bits
    they imply), 'op' for OPMULA/OPMSUB (also MAC writers, whose stored result is not the value
    their flags come from: never lazy), None otherwise. Mirrors upper() in vu0_interp.c."""
    op = hi & 0x3F
    if op >= 0x3C:
        sub = (hi >> 6 & 31) << 2 | (op & 3)
        if sub == 0x2E:
            return 'op'
        if sub < 0x10 or 0x18 <= sub < 0x1C or sub in (0x1C, 0x20, 0x21, 0x24, 0x25, 0x1E, 0x22, 0x23, 0x26, 0x27,
                                                       0x28, 0x29, 0x2A, 0x2C, 0x2D):
            return 'mac'
        return None  # NOP, ITOF, FTOI, ABS, CLIP (clip flags)
    if op == 0x2E:
        return 'op'
    if op < 0x1C:
        return None if 0x10 <= op < 0x18 else 'mac'  # MAX, MINI set no flags
    if op in (0x1D, 0x1F, 0x2B, 0x2F):  # MAXi, MINIi, MAX, MINI
        return None
    return 'mac'


def flag_reads(lo, hi):
    """('mac', 'sticky') of the lower instruction: whether it reads the MAC flags or the status
    bits they set (Z S U O), and whether it reads or replaces the sticky bits."""
    if hi >> 31:
        return False, False  # the lower word is I's value
    op = lo >> 25
    if op in (0x18, 0x1A, 0x1B):  # FMEQ, FMAND, FMOR
        return True, False
    if op in (0x14, 0x17):  # FSEQ, FSOR: all twelve status bits
        return True, True
    if op == 0x15:  # FSSET replaces the sticky bits
        return False, True
    if op == 0x16:  # FSAND imm12
        imm = (lo >> 10 & 0x800) | (lo & 0x7FF)
        return bool(imm & 0xF), bool(imm & 0xFC0)
    return False, False


def lazy_flags(pairs):
    """The pcs of this microcode package whose FMAC need not queue its MAC/status update (finding
    090 VU1R): no flag read can observe it. The run then only accumulates its sticky bits and
    keeps the result as the latest MAC, built at the end of the run (drain_flags) or at a native
    kernel's entry, so the end state stays exact.

    A read at pair R sees the latest MAC update issued at least 4 cycles before it. Each pair
    takes at least one cycle, so an update at R-1..R-3 may or may not be seen (stalls) and one
    at R-4 or earlier is landed: an update A is unobservable if on every path from A each read
    has a MAC writer at least 4 pairs before it and after A. Backward dataflow over the pairs,
    IN(p) = the distances (0..3, 4 meaning 4 or more) from p to reads reachable without such a
    writer. The control flow is over-approximated: every pair falls through to the next (an E
    delay slot or a branch slot may be entered directly), a branch slot also goes to its target,
    an indirect jump's slot to every pair, and code outside the package is unknown (any read).
    The end of the run is not a read. A package with any read of the sticky bits (or FSSET) gets
    no lazy updates; neither overlay has one."""
    readers, sticky = set(), False
    for pc, (lo, hi) in pairs.items():
        mac, s = flag_reads(lo, hi)
        if mac:
            readers.add(pc)
        sticky |= s
    if sticky:
        return set()
    FULL = 0b11111

    def shift(mask):
        out = 0
        for d in range(5):
            if mask >> d & 1:
                out |= 1 << min(d + 1, 4)
        return out

    succ = {}
    for pc in pairs:
        s = [pc + 1]
        if pc - 1 in pairs:
            lo, hi = pairs[pc - 1]
            if not hi >> 31 and lo >> 25 in BRANCH_OPS:
                if lo >> 25 in (0x24, 0x25):
                    s += ['any']
                else:
                    imm = lo & 0x7FF
                    s.append((pc + (imm - 0x800 if imm & 0x400 else imm)) & 0x7FF)
        succ[pc] = s
    IN = {pc: 0 for pc in pairs}
    changed = True
    while changed:
        changed = False
        anywhere = FULL  # indirect jumps can reach a read at any distance
        for pc in sorted(pairs, reverse=True):
            out = 0
            for s in succ[pc]:
                out |= anywhere if s == 'any' else IN.get(s, FULL)
            new = shift(out)
            if mac_writer(pairs[pc][1]):
                new &= 0b01111
            if pc in readers:
                new |= 1
            if new != IN[pc]:
                IN[pc], changed = new, True
    lazy = set()
    for pc, (lo, hi) in pairs.items():
        if mac_writer(hi) != 'mac':
            continue
        out = 0
        for s in succ[pc]:
            out |= FULL if s == 'any' else IN.get(s, FULL)
        if not out:
            lazy.add(pc)
    return lazy


def program(data, package):
    """{pc: (lo, hi)} of one microcode package, and {pc: PS2 address of that pair in .vutext}."""
    pairs, where = {}, {}
    for address, chunk, source in walk(data, package, sources=True)[1]:
        for offset, pair in enumerate(struct.iter_unpack('<II', chunk)):
            pairs[address // 8 + offset] = pair
            where[address // 8 + offset] = source + 8 * offset
    return pairs, where


def fills(name, data, sources, words):
    """Copy entries (name, word offset, PS2 address, word count) for an array of the given
    words, whose pairs are at sources in .vutext; checked against data."""
    out = []
    for k, source in enumerate(sources):
        if out and out[-1][2] + 4 * out[-1][3] == source:
            out[-1] = out[-1][:3] + (out[-1][3] + 2,)
        else:
            out.append((name, 2 * k, source, 2))
    for _, at, source, count in out:
        got = struct.unpack_from('<%dI' % count, data, source - BASE)
        if list(got) != list(words[at:at + count]):
            raise ValueError('%s: the words at 0x%08X are not the translated ones' % (name, source))
    return out


WORDS_MACRO = ['#ifndef AOT_WORDS',
               '#ifdef PS2_VU_AOT_EMBED',
               '#define AOT_WORDS(name, count, ...) static const uint32_t name[count] = {__VA_ARGS__};',
               "#else  /* filled by ps2_vu_aot_load from the game's .vutext */",
               '#define AOT_WORDS(name, count, ...) static uint32_t name[count];',
               '#endif',
               '#endif']


def fill_function(name, entries):
    """C of the function that copies the words in (entries of fills), without PS2_VU_AOT_EMBED."""
    lines = ['#ifndef PS2_VU_AOT_EMBED', 'static const struct { uint32_t *words; unsigned at, source, count; } '
             '%s_fill[] = {' % name]
    lines += ['    {%s, %d, 0x%08X, %d},' % e for e in entries]
    lines += ['};', 'static void %s_load(const uint8_t *vutext) {' % name,
              '    for (unsigned i = 0; i < sizeof %s_fill / sizeof *%s_fill; i++)' % (name, name),
              '        memcpy(%s_fill[i].words + %s_fill[i].at, vutext + (%s_fill[i].source - 0x%X), %s_fill[i].count * 4);'
              % (name, name, name, BASE, name), '}', '#endif']
    return lines


class ReadyOperands:
    """Conservative component readiness within one straight-line block.

    None means an unknown entry timestamp. A write is ready in four cycles;
    every pair consumes at least one cycle, and a checked read is ready now.
    Stalls can only expire more timestamps, so ignoring their extra cycles is
    conservative. Runtime ready arrays remain intact for all other blocks.
    """
    def __init__(self):
        self.remaining = [[None] * 4 for _ in range(32)]

    def step(self, lo, hi):
        lines = []

        def read(reg, fields):
            if not reg: return
            need = 0
            for c in range(4):
                if fields & (8 >> c):
                    if self.remaining[reg][c] != 0: need |= 8 >> c
                    self.remaining[reg][c] = 0
            if need: lines.append(f'read_fields(u, {reg}, {need});')

        def write(reg, fields):
            if reg:
                for c in range(4):
                    if fields & (8 >> c): self.remaining[reg][c] = 4

        op, fs, ft, fd, d = hi & 63, hi >> 11 & 31, hi >> 16 & 31, hi >> 6 & 31, hi >> 21 & 15
        sub = fd * 4 + (op & 3)
        # Mirror pair_stalls(), including its ordering before upper/lower.
        if not (op >= 60 and sub == 0x2f):
            if op >= 60 and sub == 0x1f:
                read(fs, 14); read(ft, 1)
            else:
                read(fs, d)
                if op < 28: read(ft, 8 >> (op & 3))
                elif 40 <= op < 48: read(ft, d)
                elif op >= 60 and (sub < 16 or 24 <= sub < 28): read(ft, 8 >> (op & 3))
                elif op >= 60 and 40 <= sub <= 46: read(ft, d)
        lop, ls, lt, ld = lo >> 25, lo >> 11 & 31, lo >> 16 & 31, lo >> 21 & 15
        lsub = (lo >> 6 & 31) * 4 + (lo & 3)
        if not hi >> 31:
            if lop == 1: read(ls, ld)
            if lop == 0x40 and lo & 63 >= 60:
                if lsub in (0x30, 0x35, 0x37): read(ls, ld)
                elif lsub == 0x31: read(ls, (ld >> 1) | ((ld & 1) << 3))
                elif lsub in (0x38, 0x3a):
                    read(ls, 8 >> (lo >> 21 & 3)); read(lt, 8 >> (lo >> 23 & 3))
                elif lsub == 0x39: read(lt, 8 >> (lo >> 23 & 3))
                elif lsub in (0x3c, 0x42, 0x43): read(ls, 8 >> (lo >> 21 & 3))
                elif lsub in (0x70, 0x71, 0x72, 0x73, 0x76): read(ls, 15 if lsub == 0x76 else 14)
                elif lsub in (0x78, 0x79, 0x7a): read(ls, 8 >> (lo >> 21 & 3))
                if lsub in (0x38, 0x39, 0x3a, 0x3b):
                    lines.append('if (u->q_ready > u->cycle) u->cycle = u->q_ready;')
                elif lsub in (0x70, 0x71, 0x72, 0x73, 0x76, 0x78, 0x79, 0x7a, 0x7b):
                    lines.append('if (u->p_ready) { if (u->p_ready - 1 > u->cycle) u->cycle = u->p_ready - 1; u->c->p = u->p_next; u->p_ready = 0; }')
            if lop == 0 or (lop == 0x40 and lo & 63 >= 60 and lsub in (0x30, 0x31, 0x34, 0x36, 0x3d, 0x40, 0x41, 0x64)):
                write(lt, ld)
        # Lower writes first; the upper destination wins a same-pair conflict.
        if op >= 60:
            if sub in (0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1d): write(ft, d)
        elif mac_writer(hi) or op in (0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1d, 0x1f, 0x2b, 0x2f):
            write(fd, d & 14 if op == 0x2e else d)
        self.remaining = [[None if n is None else max(0, n - 1) for n in r] for r in self.remaining]
        return ' '.join(lines)


class FiniteOperands:
    """Local, per-lane exponent-255 exclusion. Entry registers are unknown.

    FMAC/ITOF results are finite under the VU rounding mode. Raw loads, FTOI,
    MFIR and P remain unknown. Moves and MIN/MAX preserve only proven lanes.
    Upper reads precede lower writes, and the upper destination wins a conflict.
    Proofs never cross a block boundary or an XGKICK callback.
    """
    def __init__(self):
        self.vf = [15] + [0] * 31
        self.acc = 0
        self.i = False

    def step(self, lo, hi):
        fs, ft, fd, d = hi >> 11 & 31, hi >> 16 & 31, hi >> 6 & 31, hi >> 21 & 15
        op = hi & 63
        sub = fd * 4 + (op & 3) if op >= 60 else None
        s, t = self.vf[fs], self.vf[ft]
        # Match upper()'s vector/broadcast/Q/I source selection. Do not infer Q.
        if (op < 0x1c) or (sub is not None and (sub < 0x10 or 0x18 <= sub < 0x1c)):
            t = 15 if t & (8 >> (op & 3)) else 0
        elif op in (0x1c, 0x20, 0x21, 0x24, 0x25) or sub in (0x1c, 0x20, 0x21, 0x24, 0x25):
            t = 0
        elif op in (0x1d, 0x1e, 0x1f, 0x22, 0x23, 0x26, 0x27) or sub in (0x1e, 0x22, 0x23, 0x26, 0x27):
            t = 15 if self.i else 0
        if op == 0x2e or sub == 0x2e: d &= 14  # OPMULA/OPMSUB preserve W
        # Only written lanes contribute a result or flags. Arithmetic in other
        # lanes is discarded; it cannot make a raw W lane invalidate an XYZ
        # proof (host VU exceptions are masked).
        proof = (1 if s & d == d else 0) | (2 if t & d == d else 0) | (4 if self.acc & d == d else 0)
        dst, finite, acc = None, 0, False
        if op >= 60:
            if sub in (0x10, 0x11, 0x12, 0x13): dst, finite = ft, 15
            elif sub in (0x14, 0x15, 0x16, 0x17): dst = ft
            elif sub == 0x1d: dst, finite = ft, s
            elif mac_writer(hi): dst, finite, acc = 0, 15, True
        elif mac_writer(hi): dst, finite = fd, 15
        elif op in (0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1d, 0x1f, 0x2b, 0x2f):
            dst, finite = fd, s & t
        # Lower reads the old file, before the upper result is committed.
        if hi >> 31:
            self.i = (lo & 0x7f800000) != 0x7f800000
        else:
            lop, lt, ls, ld = lo >> 25, lo >> 16 & 31, lo >> 11 & 31, lo >> 21 & 15
            lsub = (lo >> 6 & 31) * 4 + (lo & 3)
            lower_finite, writes = 0, lop == 0
            if lop == 0x40:
                writes = lsub in (0x30, 0x31, 0x34, 0x36, 0x3d, 0x40, 0x41, 0x64)
                if lsub == 0x30: lower_finite = self.vf[ls]
                elif lsub == 0x31:
                    m = self.vf[ls]; lower_finite = ((m << 1) | (m >> 3)) & 15
                if lsub == 0x6c:
                    self.vf = [15] + [0] * 31; self.acc = 0; self.i = False
            if writes and lt:
                self.vf[lt] = (self.vf[lt] & ~ld) | (lower_finite & ld)
        if dst is not None:
            if acc: self.acc = (self.acc & ~d) | (finite & d)
            elif dst: self.vf[dst] = (self.vf[dst] & ~d) | (finite & d)
        return proof


def emit_block(prefix, i, block, unit, lazy=(), words=None):
    """C for one straight-line block: the interpreter's steps for each pair with the words as
    constants (pair_stalls only on VU1, as in the interpreter). Pairs whose offset is in lazy
    commit their MAC update lazily (commit_upper_lazy); words names the block's word array when
    another variant of the same block already has one.

    A LOI immediate (the lower word of a pair whose upper word has the I bit) is read from that
    array, not written in as a constant: the array holds what micro memory holds whenever the
    block runs (aot_find compares them), and on a 64-bit host it is filled from guest RAM at the
    first VU1 program, after widescreen has changed the sprite widths there (finding 192)."""
    hint = 'VU1(u); ' if unit else 'VU0(u); '
    fused = len(block) > 1 and ends_block(*block[-2])
    fused_end = fused and bool(block[-2][1] & (1 << 30))
    name = f'{prefix}_block_{i}'
    own_words = words is None
    if own_words:
        words = f'{prefix}_words_{i}'
    signature = (f'static inline __attribute__((always_inline)) int {name}_impl(Vu *u, unsigned pc, int *incoming_bad, int sticky_complete) {{'
                 if lazy else f'static int {name}(Vu *u, unsigned pc, int *incoming_bad) {{')
    lines = []
    if own_words:  # before the function: its LOI immediates are read from it
        lines += [f'AOT_WORDS({words}, {2 * len(block)}, ' + ','.join(f'0x{w:08X}u' for pair in block for w in pair) + ')']
    lines += [signature,
             '    int failure = *incoming_bad; int *bad = &failure;',
             '    Upper up; int next = -1, transfer = -1;', f'    u->unit = {unit};']
    finite, ready = FiniteOperands(), ReadyOperands()
    for offset, (lo, hi) in enumerate(block):
        proof = finite.step(lo, hi) if unit else 0
        lines += ['    {']
        if unit:
            lines += [f'    VU1(u); {ready.step(lo, hi)}']
        lines += [f'    {hint}apply_flags(u); update_q(u);',
                  f'    {hint}if (upper_proven(u, 0x{hi:08X}u, &up, {proof}) < 0) {{ *incoming_bad = 1; return -1; }}']
        if hi >> 31:
            lines += [f'    memcpy(&u->c->i, &{words}[{2 * offset}], 4);  /* LOI 0x{lo:08X} in the retail code */']
        else:
            lines += [f'    if (!(up.flags == 2 && {lo >> 25} == 0x11)) {{',
                      f'        {hint}next = lower(u, 0x{lo:08X}u, pc + {offset}, bad);', '    }']
        commit = (f'commit_upper_lazy_proven(u, &up, {lo >> 16 & 31}, sticky_complete)' if offset in lazy
                  else f'commit_upper(u, &up, {lo >> 16 & 31})')
        lines += ['    if (*bad) { *incoming_bad = *bad; return -1; }', f'    {hint}{commit};']
        if fused and offset == len(block) - 2:
            lines += ['    transfer = next;']
        if offset != len(block) - 1:
            lines += [f'    trace_pair(u, pc + {offset});', '    u->cycle++;']
        lines += ['    }']
    lines += [f'    return {"transfer" if fused else "next"};', '}']
    if lazy:
        lines += [f'static int {name}(Vu *u, unsigned pc, int *bad) {{',
                  f'    if ((u->status & 0xc0) == 0xc0) return {name}_impl(u, pc, bad, 1);',
                  f'    return {name}_impl(u, pc, bad, 0);', '}']
    lines += [f'static const AotBlock {prefix}_info_{i} = {{{len(block)}, {2 if fused_end else 1 if fused else 0}, '
              f'{words}, {prefix}_block_{i}, {1 if lazy else 0}}};']
    return lines


def generate(destination):
    destination = Path(destination).resolve()
    if (ROOT / 'build').resolve() not in destination.parents:  # build/ may be a junction off C:
        raise ValueError('game-derived AOT output must stay under build/')
    data = Path(os.environ.get('DHD_VUTEXT', ROOT / 'assets/vutext.bin')).read_bytes()
    # Each block is keyed by its pairs and the offsets of those that commit their MAC update
    # lazily (lazy_flags): the same pairs can be lazy in one package and not in the other.
    overlays, found, packages = [], {}, []
    for package in (0x265C00, 0x269C50):
        pairs, where = program(data, package)
        lazy = lazy_flags(pairs)
        packages.append((pairs, where))
        tables = []
        for table in (blocks(pairs), delay_slots(pairs)):
            tables.append({pc: (block, tuple(k for k in range(len(block)) if pc + k in lazy))
                           for pc, block in table.items()})
            for pc, block in table.items():
                found.setdefault(block, [where[pc + k] for k in range(len(block))])
        overlays.append(tables)
    unique = sorted(set(key for overlay in overlays for table in overlay for key in table.values()))
    ids = {key: i for i, key in enumerate(unique)}
    lines = ['/* Generated from local microcode. Do not commit. */',
             '/* fused: 0 = ordinary block, 1 = branch + slot, 2 = E + slot. lazy: a pair commits its MAC update',
             '   lazily, which is valid only while micro memory holds the whole package it was analysed in. */',
             'typedef struct { unsigned count, fused; const uint32_t *words; int (*run)(Vu *, unsigned, int *); unsigned lazy; } AotBlock;']
    lines += WORDS_MACRO
    entries, words_of = [], {}
    for i, (block, lazy) in enumerate(unique):
        lines += emit_block('aot', i, block, 1, lazy, words_of.get(block))
        if block not in words_of:
            words_of[block] = 'aot_words_%d' % i
            entries += fills('aot_words_%d' % i, data, found[block], [w for pair in block for w in pair])
    lines += ['static const AotBlock *const aot_blocks[2][2][2048] = {  /* [overlay][delay slot][pc] */']
    for overlay in overlays:
        lines.append(' {')
        for table in overlay:
            lines.append('  {')
            lines += [f'   &aot_info_{ids[table[pc]]},' if pc in table else '   NULL,' for pc in range(2048)]
            lines.append('  },')
        lines.append(' },')
    lines.append('};')
    # Each package's own words, by runs of consecutive pairs: the lazy blocks run only while micro
    # memory holds one of them in full (checked once per ps2_vu1_code_version).
    for k, (pairs, where) in enumerate(packages):
        runs = []
        for pc in sorted(pairs):
            if runs and runs[-1][0] + runs[-1][1] == pc:
                runs[-1][1] += 1
            else:
                runs.append([pc, 1])
        words = [w for pc in sorted(pairs) for w in pairs[pc]]
        lines += [f'AOT_WORDS(aot_package{k}_words, {len(words)}, ' + ','.join(f'0x{w:08X}u' for w in words) + ')']
        entries += fills(f'aot_package{k}_words', data, [where[pc] for pc in sorted(pairs)], words)
        spans, offset = [], 0
        for pc, count in runs:
            spans.append(f'{{{pc}, {count}, {offset}}}')
            offset += 2 * count
        lines += [f'static const unsigned aot_package{k}_runs[{len(runs)}][3] = {{' + ', '.join(spans) + '};  /* pc, pairs, word */']
    lines += ['static int aot_package_match(const uint32_t *code, const unsigned (*runs)[3], unsigned n, const uint32_t *words) {',
              '    for (unsigned i = 0; i < n; i++)',
              '        if (memcmp(code + 2 * runs[i][0], words + runs[i][2], runs[i][1] * 8)) return 0;',
              '    return 1;', '}',
              '/* The package micro memory holds in full (0 or 1), or -1. */',
              'static int aot_package(const uint32_t *code) {',
              '    if (aot_package_match(code, aot_package0_runs, sizeof aot_package0_runs / sizeof *aot_package0_runs, aot_package0_words)) return 0;',
              '    if (aot_package_match(code, aot_package1_runs, sizeof aot_package1_runs / sizeof *aot_package1_runs, aot_package1_words)) return 1;',
              '    return -1;', '}']
    # A block runs only where micro memory holds its exact words. In the game's VU1 micro memory
    # (0x11008000, changed only by VIF1 MPG, which bumps ps2_vu1_code_version) the block found
    # (or none) is remembered until the next change; any other code buffer (the oracle tests)
    # compares each time. The micro memory is a guest address: GADDR (identity on i686) also
    # recognises it on a 64-bit host, where it sits at guest_base + 0x11008000.
    # A block with lazy pairs runs only where micro memory holds the whole package whose control
    # flow lazy_flags analysed; the others wherever their own words match. The package held is
    # tried first. Other code buffers (the oracle tests) look the package up every time.
    lines += ['static struct { uint64_t version; const AotBlock *block; } aot_resolved[2][2048];  /* version + 1 */',
              'static uint64_t aot_package_version;  /* version + 1 */',
              'static int aot_package_held;',
              'static const AotBlock *aot_find(unsigned pc, const uint32_t *words, unsigned remaining, int slot) {',
              '    const int game = GADDR(words - pc * 2) == 0x11008000;',
              '    const AotBlock *found = NULL;',
              '    if (game && aot_resolved[slot][pc].version == ps2_vu1_code_version + 1)',
              '        found = aot_resolved[slot][pc].block;',
              '    else {',
              '        int held;',
              '        if (!game) held = aot_package(words - pc * 2);',
              '        else {',
              '            if (aot_package_version != ps2_vu1_code_version + 1)',
              '                aot_package_held = aot_package(words - pc * 2), aot_package_version = ps2_vu1_code_version + 1;',
              '            held = aot_package_held;',
              '        }',
              '        for (unsigned k = 0; k < 2 && !found; k++) {',
              '            const unsigned overlay = held == 1 ? 1 - k : k;',
              '            const AotBlock *block = aot_blocks[overlay][slot][pc];',
              '            if (block && (!block->lazy || (int)overlay == held) && !memcmp(words, block->words, block->count * 8))',
              '                found = block;',
              '        }',
              '        if (game) aot_resolved[slot][pc].version = ps2_vu1_code_version + 1, aot_resolved[slot][pc].block = found;',
              '    }',
              '    return found && found->count <= remaining ? found : NULL;', '}']
    # VU0 micro mode: the game's one VU0 package (finding 019: 426 instructions at 0x26CF80), run by
    # vcallms from the EE. The interpreter has no stall model for VU0; the blocks skip it too.
    # The blocks compare their words on every dispatch (blocks are short) instead of being
    # remembered. The native VU0 routines' signatures below are compared once per change of VU0
    # micro memory (ps2_vu0_code_version, bumped by VIF0 MPG; vu0_interp.c vu0_native_code): the
    # game writes VU0 code only through VIF0 MPG (finding 090 OP2; DHD_VU0_CODE_CHECK=1 checks it).
    pairs0, where0 = program(data, 0x26CF80)
    # The complete native VU0 culling entry, including its E-bit delay slot.
    cull = [pairs0[pc] for pc in range(0xc0 // 8)]
    lines += ['AOT_WORDS(vu0_cull_words, 48, ' + ','.join(f'0x{w:08X}u' for pair in cull for w in pair) + ')']
    entries += fills('vu0_cull_words', data, [where0[pc] for pc in range(0xc0 // 8)],
                     [w for pair in cull for w in pair])
    basis = [pairs0[pc] for pc in range(0x5e8 // 8, 0x760 // 8)]
    lines += ['AOT_WORDS(vu0_basis_words, 94, ' + ','.join(f'0x{w:08X}u' for pair in basis for w in pair) + ')']
    entries += fills('vu0_basis_words', data, [where0[pc] for pc in range(0x5e8 // 8, 0x760 // 8)],
                     [w for pair in basis for w in pair])
    bounds = [pairs0[pc] for pc in range(0x2e0 // 8, 0x4f0 // 8)]
    lines += ['AOT_WORDS(vu0_bounds_words, 132, ' + ','.join(f'0x{w:08X}u' for pair in bounds for w in pair) + ')']
    entries += fills('vu0_bounds_words', data, [where0[pc] for pc in range(0x2e0 // 8, 0x4f0 // 8)],
                     [w for pair in bounds for w in pair])
    for name, start, end in (('length', 0x7b8, 0x808), ('direction', 0x808, 0x830)):
        native_pairs = [pairs0[pc] for pc in range(start // 8, end // 8)]
        words = [w for pair in native_pairs for w in pair]
        lines += [f'AOT_WORDS(vu0_{name}_words, {len(words)}, ' + ','.join(f'0x{w:08X}u' for w in words) + ')']
        entries += fills(f'vu0_{name}_words', data, [where0[pc] for pc in range(start // 8, end // 8)], words)
    tables0 = (blocks(pairs0), delay_slots(pairs0))
    found0 = {}
    for table in tables0:
        for pc, block in table.items():
            found0.setdefault(block, [where0[pc + k] for k in range(len(block))])
    unique0 = sorted(set(block for table in tables0 for block in table.values()))
    ids0 = {block: i for i, block in enumerate(unique0)}
    for i, block in enumerate(unique0):
        lines += emit_block('aot0', i, block, 0)
        entries += fills('aot0_words_%d' % i, data, found0[block], [w for pair in block for w in pair])
    lines += ['static const AotBlock *const aot0_blocks[2][512] = {  /* [delay slot][pc] */']
    for table in tables0:
        lines.append(' {')
        lines += [f'  &aot0_info_{ids0[table[pc]]},' if pc in table else '  NULL,' for pc in range(512)]
        lines.append(' },')
    lines += ['};',
              'static const AotBlock *aot0_find(unsigned pc, const uint32_t *words, unsigned remaining, int slot) {',
              '    const AotBlock *block = aot0_blocks[slot][pc & 511];',
              '    return block && block->count <= remaining && pc + block->count <= 512 &&',
              '           !memcmp(words, block->words, block->count * 8) ? block : NULL;', '}']
    lines += fill_function('aot_words', entries)
    text = '\n'.join(lines) + '\n'
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or destination.read_bytes() != text.encode():
        destination.write_bytes(text.encode())
    from vu1_native_clip import generate as generate_clip
    generate_clip(destination.parent / "vu1_native_clip.inc")
    return len(unique)


if __name__ == '__main__':
    print('VU1 AOT:', generate(ROOT / 'build/vu1aot/vu1_aot.inc'), 'straight-line blocks')
