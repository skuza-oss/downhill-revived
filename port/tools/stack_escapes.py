#!/usr/bin/env python3
"""Stack-local escapes against the PS2 frame (finding 213; the class of findings 094 and 207).

The matching source reproduces the retail compiler's stack frames, and the retail code sometimes
treats two neighbouring locals as one object: a constructor is given the address of a small local
and writes on into the local the EE frame puts right after it. In the port clang lays the locals
out as it likes, so the same write lands on something else (or off the fiber's stack slot).

This tool finds those places statically:

1. every unit of src/ is compiled once more with the matching compiler (SN cc1, the unit's own
   flags) plus -gstabs: the stabs give each local's EE frame offset and size, `.frame` the frame
   size (cached under build/stack_escapes, keyed by the unit's and its headers' contents);
2. a MIPS dataflow over every function (the compiler's output for C functions, the splat asm for
   the rest) tracks registers and stack slots that hold "$sp + N" or "argument k + c" (and small
   constants), and summarises for each function and argument the byte range it writes (and reads)
   through that argument, following calls with the callee's summary (to a fixed point);
3. every place a C function passes the address of a local (or of an element of one) to a callee
   is an *escape*; it is a *candidate* when the callee's write range runs outside the local's
   declared size, and *unbounded* when the callee writes through the pointer in a loop, with a
   variable index, by a copy routine with a non-constant length, as a string, or passes it on to
   an indirect call (what the static tool can't size: the run-time red zones, finding 213, see
   those).

Only C functions matter (the recompiled asm functions keep their EE frame on the emulated stack).

usage: python port/tools/stack_escapes.py [--jobs N] [--report FILE] [--check] [--json FILE]
       [--function NAME ...]
--check fails (exit 1) on a candidate that port/tools/stack_escapes_reviewed.txt doesn't list, and on a
unit the matching compiler cannot build with -gstabs (a unit that is not compiled is not analysed) unless
port/tools/stack_escapes_compile_failures.txt lists it with the reason; a listed unit that compiles fails
too. Report-only runs warn about both.
"""
import argparse, collections, glob, hashlib, json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
CACHE = os.path.join(ROOT, 'build', 'stack_escapes')
REVIEWED = os.path.join(HERE, 'stack_escapes_reviewed.txt')
COMPILE_FAILURES = os.path.join(HERE, 'stack_escapes_compile_failures.txt')
TOOL_VERSION = b'stack-escapes 1\n'

GPR_NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6',
             't7', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp',
             'fp', 'ra']
GPR = dict({'$' + n: i for i, n in enumerate(GPR_NAMES)}, **{'$%d' % i: i for i in range(32)})
GPR['$s8'] = 30
ARGS = list(range(4, 12))          # the EE passes eight integer arguments in $a0-$t3
CALLEE_SAVED = set(range(16, 24)) | {28, 29, 30}
SP = 29

STORES = {'sb': 1, 'sh': 2, 'sw': 4, 'sd': 8, 'sq': 16, 'swl': 4, 'swr': 4, 'sdl': 8, 'sdr': 8,
          'swc1': 4, 'sdc1': 8, 'sqc2': 16, 'usw': 4, 'ush': 2, 'usd': 8}
LOADS = {'lb': 1, 'lbu': 1, 'lh': 2, 'lhu': 2, 'lw': 4, 'lwu': 4, 'ld': 8, 'lq': 16, 'lwl': 4, 'lwr': 4,
         'ldl': 8, 'ldr': 8, 'lwc1': 4, 'ldc1': 8, 'lqc2': 16, 'ulw': 4, 'ulh': 2, 'ulhu': 2, 'uld': 8}
THREE_OP_MULT = {'mult', 'multu', 'mult1', 'multu1', 'madd', 'maddu', 'madd1', 'maddu1'}
LEFT = {'swl', 'sdl', 'lwl', 'ldl'}
FPR_FIRST = {'swc1', 'sdc1', 'sqc2', 'lwc1', 'ldc1', 'lqc2'}  # the first operand is not a GPR
# instructions whose first operand is read, not written
SOURCE_FIRST = {'beq', 'bne', 'beql', 'bnel', 'bgez', 'bgtz', 'blez', 'bltz', 'bgezl', 'bgtzl', 'blezl',
                'bltzl', 'bgezal', 'bltzal', 'beqz', 'bnez', 'beqzl', 'bnezl', 'b', 'bal', 'j', 'jr', 'jal',
                'jalr', 'mtc1', 'dmtc1', 'ctc1', 'mtc0', 'qmtc2', 'ctc2', 'mthi', 'mtlo', 'mthi1', 'mtlo1',
                'teq', 'tne', 'tge', 'tlt', 'tgeu', 'tltu', 'mult', 'multu', 'div', 'divu', 'mult1',
                'multu1', 'div1', 'divu1', 'madd', 'maddu', 'madd1', 'maddu1', 'mtsa', 'mtsab', 'mtsah',
                'cache', 'pref', 'sync', 'sync.l', 'sync.p', 'syscall', 'break', 'nop', 'ssnop', 'eret',
                'ei', 'di', 'bc1t', 'bc1f', 'bc1tl', 'bc1fl', 'bc0t', 'bc0f', 'bc2t', 'bc2f', 'mtpc', 'mtps',
                'pmthi', 'pmtlo', 'pmthl.lw'} | set(STORES)
# the copy routines: (destination argument, length argument); the length is the write's size
COPY = {'func_00254204': (0, 2), 'memcpy': (0, 2), 'memmove': (0, 2), 'memset': (0, 2), 'strncpy': (0, 2), 'bzero': (0, 1),
        'bcopy': (1, 2)}
# string writers: the write's size is the text's (unbounded here; the fortify pass sizes literals)
STRING = {'strcpy': 0, 'strcat': 0, 'strncat': 0, 'sprintf': 0, 'vsprintf': 0, 'func_00254D68': 0, 'func_00254DE8': 0,
          'func_002542C4': 0, 'func_00254544': 0}
UNBOUNDED = 1 << 30
RETURNS = {}  # function -> (argument, offset, walks): it returns that argument + offset


# ---- compiling the units with stabs -----------------------------------------------------------

UNIT_MARK = re.compile(r'^/\*\s*UNIT\s+(.*?)\s*\*/')


def units():
    out = []
    for p in sorted(glob.glob(os.path.join(ROOT, 'src', '**', '*.c*'), recursive=True)):
        rel = os.path.relpath(p, os.path.join(ROOT, 'src')).replace(os.sep, '/')
        with open(p, encoding='utf-8', errors='replace') as f:
            m = UNIT_MARK.match(f.readline())
        flags = [x for x in (m.group(1).split() if m else []) if x.startswith('-')]
        out.append((rel, p, flags))
    return out


def file_hash(p):
    with open(p, 'rb') as f:
        return hashlib.sha256(f.read().replace(b'\r\n', b'\n')).hexdigest()


def compile_unit(job):
    """The unit's cc1 assembly with stabs, from the cache when the unit and its headers are unchanged."""
    import toolchain
    rel, path, flags = job
    s = os.path.join(CACHE, 's', rel + '.s')
    keyf = s + '.key'
    key = hashlib.sha256(TOOL_VERSION + ' '.join(flags).encode() + file_hash(path).encode()).hexdigest()
    if os.path.exists(s) and os.path.exists(keyf):
        old = json.load(open(keyf))
        if old.get('key') == key and all(os.path.exists(os.path.join(ROOT, d)) and
                                         file_hash(os.path.join(ROOT, d)) == h for d, h in old['deps'].items()):
            return rel, s, None
    os.makedirs(os.path.dirname(s), exist_ok=True)
    dep = s + '.d'
    lang = 'c' if path.endswith('.c') else 'c++'
    base = toolchain.CFLAGS if lang == 'c' else toolchain.CXXFLAGS
    extra = [x for x in flags if x != '-sce29']  # SCE-2.9 units: SN's layout stands in (noted per unit)
    srcrel = os.path.relpath(path, ROOT)
    cmd = [toolchain.CC, '-x', lang, '-S', srcrel, '-o', s, '-Wp,-MD,' + dep, '-I', 'include', '-I', 'src',
           '-gstabs'] + base + extra
    p = subprocess.run(toolchain.exe(cmd[0]) + cmd[1:], cwd=ROOT, capture_output=True, text=True,
                       stdin=subprocess.DEVNULL)
    if p.returncode:
        return rel, None, (p.stdout + p.stderr).strip()[-400:]
    deps = {}
    if os.path.exists(dep):
        for d in open(dep).read().replace('\\\n', ' ').split(':', 1)[1].split():
            d = os.path.relpath(os.path.join(ROOT, d), ROOT).replace(os.sep, '/')
            if os.path.isfile(os.path.join(ROOT, d)) and not d.startswith('..'):
                deps[d] = file_hash(os.path.join(ROOT, d))
    json.dump({'key': key, 'deps': deps}, open(keyf, 'w'))
    return rel, s, None


# ---- stabs types ------------------------------------------------------------------------------

BUILTIN = {'int': 4, 'char': 1, 'long int': 4, 'unsigned int': 4, 'long unsigned int': 4,
           'long long int': 8, 'long long unsigned int': 8, 'short int': 2, 'short unsigned int': 2,
           'signed char': 1, 'unsigned char': 1, 'float': 4, 'double': 8, 'long double': 8, 'void': 0,
           'bool': 1, 'wchar_t': 4, '__int128': 16, 'unsigned __int128': 16,
           'complex int': 8, 'complex float': 8, 'complex double': 16, 'complex long double': 16}
TYPEDEF_RE = re.compile(r'^([^:]*):[tT](\d+)=')
DEF_RE = re.compile(r'(\d+)=')


class Types:
    """Type sizes of one unit's stabs (type numbers are per unit)."""

    def __init__(self, wide=()):
        self.wide = set(wide)  # typedef names declared mode(TI): stabs give them a 64-bit base type
        self.defs = {}      # number -> descriptor text following '='
        self.names = {}     # number -> builtin name
        self.tags = {}      # struct tag -> number

    def add(self, stab):
        m = TYPEDEF_RE.match(stab)
        if m:
            self.names.setdefault(int(m.group(2)), '__int128' if m.group(1) in self.wide else m.group(1))
            if stab[m.end(1) + 1] == 'T':
                self.tags[m.group(1)] = int(m.group(2))
        for d in DEF_RE.finditer(stab):
            a = d.start()
            if a and (stab[a - 1].isdigit() or stab[a - 1] == '-'):
                continue
            n = int(d.group(1))
            if n not in self.defs:
                self.defs[n] = stab[d.end():]

    def size(self, desc, depth=0):
        """Size in bytes of a type descriptor ('12', '12=ar1;0;3;2', ...); None if unknown."""
        if depth > 40:
            return None
        m = re.match(r'(\d+)', desc)
        if m and not desc[m.end():].startswith('='):
            return self.size_of(int(m.group(1)), depth + 1)
        if m:
            desc = desc[m.end() + 1:]
        return self.size_desc(desc, m and int(m.group(1)), depth)

    def size_of(self, n, depth=0):
        if n in self.names and self.names[n] in BUILTIN:
            return BUILTIN[self.names[n]]
        if n not in self.defs:
            return None
        return self.size_desc(self.defs[n], n, depth + 1)

    def size_desc(self, d, n, depth):
        if depth > 40 or not d:
            return None
        c = d[0]
        if c in 'su':
            m = re.match(r'[su](\d+)', d)
            return int(m.group(1)) if m else None
        if c in '*&':
            return 4
        if c == 'e':
            return 4
        if c == 'a':  # ar<index>;lo;hi;<elem>
            m = re.match(r'ar(?:\d+(?:=[^;]*)?);(-?\d+);(-?\d+);(.*)', d)
            if not m:
                return None
            lo, hi = int(m.group(1)), int(m.group(2))
            e = self.size(m.group(3), depth + 1)
            if e is None:
                return None
            return max(0, hi - lo + 1) * e
        if c == 'r':
            m = re.match(r'r(\d+);(-?\d+);(-?\d+);', d)
            if not m:
                return None
            if n is not None and n in self.names and self.names[n] in BUILTIN:
                return BUILTIN[self.names[n]]
            lo, hi = int(m.group(2)), int(m.group(3))
            if hi == 0 and lo > 0:  # a float: r<int>;<bytes>;0;
                return lo
            for size in (1, 2, 4, 8):
                if -(1 << (8 * size - 1)) <= lo and hi < (1 << (8 * size)):
                    return size
            return 16
        if c in 'kB':
            return self.size(d[1:], depth + 1)
        if c == 'x':  # a cross reference to a tag defined (maybe) elsewhere in the unit
            m = re.match(r'x[sue]([^:]+):', d)
            if m and m.group(1) in self.tags and self.tags[m.group(1)] != n:
                return self.size_of(self.tags[m.group(1)], depth + 1)
            return None
        if c == 'f' or c == '#':
            return None
        if c.isdigit():
            return self.size(d, depth + 1)
        return None


# ---- reading the assembly ---------------------------------------------------------------------

STAB = re.compile(r'^\s*\.stabs\s+"((?:[^"\\]|\\.)*)",\s*(\d+),\s*\d+,\s*\d+,\s*(.*)$')
STABN = re.compile(r'^\s*\.stabn\s+(\d+),')
SPLAT_INSN = re.compile(r'^\s*/\* [0-9A-F]+ ([0-9A-F]{8}) [0-9A-F]{8} \*/\s+(\S+)\s*(.*?)\s*$')
INCLUDE = re.compile(r'^\s*\.include\s+"([^"]+)"')
SPLAT_LABEL = re.compile(r'^\s*(?:\.L([0-9A-F]{8}):|[a-z]*label\s+\.L([0-9A-F]{8}))\s*$')


class Function:
    def __init__(self, name, kind, unit):
        self.name, self.kind, self.unit = name, kind, unit   # kind: 'c' or 'asm'
        self.insns = []        # (mnemonic, [operands], noreorder)
        self.frame = 0
        self.locals = []       # (name, offset from $sp, size or None, scope depth)


MANGLED = re.compile(r'^(func_[0-9A-F]{8})__\w+$')


def base_name(name):
    """func_XXXXXXXX for a C++ symbol of it (func_00203590__FP6Handleiii): one function, one name."""
    m = MANGLED.match(name)
    return m.group(1) if m else name


def split_ops(s):
    s = s.split('#')[0].strip()
    if not s:
        return []
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    out.append(cur.strip())
    return out


TI_TYPEDEF = re.compile(r'typedef\s+[\w\s]+?\s(\w+)\s*__attribute__\s*\(\(\s*(?:__)?mode(?:__)?\s*\(\s*(?:__)?TI(?:__)?\s*\)')


def wide_typedefs(path):
    """mode(TI) typedef names in a cc1 output's source and headers (its .key lists them)."""
    names = set()
    try:
        deps = json.load(open(path + '.key'))['deps']
    except (OSError, ValueError, KeyError):
        return names
    for d in deps:
        try:
            names |= set(TI_TYPEDEF.findall(open(os.path.join(ROOT, d), encoding='utf-8', errors='replace').read()))
        except OSError:
            pass
    return names


def read_cc1(path, unit):
    """The C functions of one cc1 output, with their locals; the asm files it includes."""
    funcs, includes, types = [], [], Types(wide_typedefs(path))
    cur, noreorder, last = None, False, None
    scope = 0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = STAB.match(line)
        if m:
            text, kind, value = m.group(1), int(m.group(2)), m.group(3).strip()
            types.add(text)
            if kind == 36:          # N_FUN: the function's stabs follow
                name = base_name(text.split(':')[0])
                last = next((f for f in funcs if f.name == name), None)
                scope = 0
            elif kind == 128 and last is not None and not re.match(r'^[^:]*:[tT]', text):
                name, _, typ = text.partition(':')
                if typ[:1].isalpha() and typ[0] not in 'k':
                    continue
                try:
                    off = int(value, 0)
                except ValueError:
                    continue
                last.locals.append((name, last.frame + off, types.size(typ), scope))
            continue
        m = STABN.match(line)
        if m:
            if m.group(1) == '192':
                scope += 1
            elif m.group(1) == '224':
                scope -= 1
            continue
        s = line.strip()
        m = INCLUDE.match(s)
        if m:
            if 'nonmatchings' in m.group(1):
                includes.append(m.group(1))
            continue
        if s.startswith('.ent'):
            cur = Function(base_name(s.split()[1]), 'c', unit)
            funcs.append(cur)
            noreorder = False
            continue
        if s.startswith('.end') and cur is not None and not s.startswith('.endr'):
            cur = None
            continue
        if cur is None:
            continue
        if s.startswith('.frame'):
            cur.frame = int(split_ops(s[6:])[1], 0)
        elif s.split() == ['.set', 'noreorder']:
            noreorder = True
        elif s.split() == ['.set', 'reorder']:
            noreorder = False
        elif re.fullmatch(r'\$L\d+:', s):
            cur.insns.append(('label', [s[:-1]], noreorder))
        elif s and not s.startswith(('.', '$', '#')) and not s.endswith(':'):
            mn, _, ops = s.partition(' ')
            if '\t' in mn:
                mn, _, rest = mn.partition('\t')
                ops = rest + ' ' + ops
            cur.insns.append((mn, split_ops(ops), noreorder))
    return funcs, includes


def read_splat(path):
    name = os.path.basename(path)[:-2]
    f = Function(name, 'asm', None)
    for line in open(path, encoding='utf-8', errors='replace'):
        m = SPLAT_INSN.match(line)
        if m:
            f.insns.append((m.group(2), split_ops(m.group(3)), True))
            continue
        m = SPLAT_LABEL.match(line)
        if m:
            f.insns.append(('label', ['.L' + (m.group(1) or m.group(2))], True))
        elif line.startswith('endlabel'):
            break
    return f


# ---- the dataflow -----------------------------------------------------------------------------

def imm(s):
    s = s.strip()
    m = re.fullmatch(r'\(?(-?(?:0x[0-9A-Fa-f]+|\d+))\)?', s)
    return int(m.group(1), 0) if m else None


def mem(op):
    """'off(base)' -> (offset or None, register) ."""
    m = re.fullmatch(r'(.*)\((\$\w+)\)', op.strip())
    if not m:
        return None, None
    off = imm(m.group(1)) if m.group(1).strip() else 0
    return off, GPR.get(m.group(2))


class Summary:
    """What a function does through one pointer argument: byte ranges written and read, and why
    the range may be larger than that (flags)."""
    __slots__ = ('w', 'r', 'flags', 'via')

    def __init__(self):
        self.w, self.r, self.flags, self.via = None, None, set(), {}

    def key(self):
        return (self.w, self.r, tuple(sorted(self.flags)))


def widen(rng, lo, hi):
    if rng is None:
        return (lo, hi)
    return (min(rng[0], lo), max(rng[1], hi))


BRANCH_LIKELY = {'beql', 'bnel', 'bgezl', 'bgtzl', 'blezl', 'bltzl', 'beqzl', 'bnezl', 'bc1tl', 'bc1fl'}


def is_label(op):
    return op.startswith(('$L', '.L'))


def is_control(mn, ops):
    return (mn.startswith('b') and mn != 'break' and bool(ops) and is_label(ops[-1])) or mn in ('j', 'jr')


def blocks_of(insns):
    """Basic blocks: [(start, end, successors)]; a block ends after its branch or jump (and the
    delay slot when the code is noreorder). Calls (jal, jalr) don't end a block."""
    labels = {ops[0]: i for i, (mn, ops, _) in enumerate(insns) if mn == 'label'}
    leaders = {0} | set(labels.values())
    for i, (mn, ops, nr) in enumerate(insns):
        if is_control(mn, ops):
            leaders.add(i + 2 if nr and i + 1 < len(insns) and insns[i + 1][0] != 'label' else i + 1)
    starts = sorted(x for x in leaders if x < len(insns))
    index = {st: n for n, st in enumerate(starts)}
    out = []
    for n, st in enumerate(starts):
        en = starts[n + 1] if n + 1 < len(starts) else len(insns)
        last = None
        for k in range(st, en):
            if is_control(*insns[k][:2]):
                last = k
        succ = []
        if last is None:
            if en < len(insns):
                succ.append(index[en])
        else:
            mn, ops, nr = insns[last]
            if mn in ('j', 'b') and ops and is_label(ops[-1]):
                if ops[-1] in labels:
                    succ.append(index[labels[ops[-1]]])
            elif mn == 'jr' and ops and ops[0] not in ('$31', '$ra'):
                succ.extend(index[x] for x in labels.values())  # a jump table: every label
            elif mn in ('jr', 'j'):
                pass  # a return or a tail call
            else:
                if ops[-1] in labels:
                    succ.append(index[labels[ops[-1]]])
                if en < len(insns):
                    succ.append(index[en])
        out.append((st, en, succ))
    return out


def merge_val(old, new):
    if old == new:
        return old
    if old is None or new is None:
        return None
    if old[0] == new[0] == 'sp' or (old[0] == new[0] == 'arg' and old[1] == new[1]):
        return old[:-1] + (True,)  # the same object at another offset: a walk over it
    return None


def merge_state(old, new):
    """old's entries that new agrees with (or the same object at another offset, as a walk)."""
    regs = {}
    for r in old[0]:
        if r in new[0]:
            v = merge_val(old[0][r], new[0][r])
            if v is not None:
                regs[r] = v
    slots = {}
    for k in old[1]:
        if k in new[1]:
            v = merge_val(old[1][k], new[1][k])
            if v is not None:
                slots[k] = v
    return regs, slots


def analyse(f, summaries, record):
    """A forward dataflow over f's blocks. Values: ('sp', N, var), ('arg', k, c, var), ('const', v,
    var); var: the pointer walks (a loop's step) or is indexed. The escapes and the summaries'
    accesses are keyed by instruction, so a later pass over a block replaces an earlier one's."""
    insns = f.insns
    if not insns:
        return
    blocks = blocks_of(insns)
    entry = {SP: ('sp', 0, False), 0: ('const', 0, False)}
    for i, a in enumerate(ARGS):
        entry[a] = ('arg', i, 0, False)
    ins = {0: (entry, {})}
    work = [0]
    visits = collections.Counter()
    records = {}
    while work:
        b = work.pop(0)
        visits[b] += 1
        if visits[b] > 40:
            continue
        regs, slots = dict(ins[b][0]), dict(ins[b][1])
        st, en, succ = blocks[b]
        run_block(insns, st, en, regs, slots, records)
        for s in succ:
            if s not in ins:
                ins[s] = (dict(regs), dict(slots))
                work.append(s)
            else:
                m = merge_state(ins[s], (regs, slots))
                if m != ins[s]:
                    ins[s] = m
                    if s not in work:
                        work.append(s)
    for rec in records.values():
        record(*rec)


def run_block(insns, st, en, regs, slots, records):
    def setr(r, v):
        n = GPR.get(r) if isinstance(r, str) else r
        if n is None or n == 0 or n == SP:  # $sp: offsets are from the body's $sp
            return
        if v is None:
            regs.pop(n, None)
        else:
            regs[n] = v

    def val(r):
        return regs.get(GPR.get(r)) if isinstance(r, str) else regs.get(r)

    def add(v, c, var=False):
        if v is None:
            return None
        if v[0] == 'const':
            return ('const', v[1] + c, v[2]) if not var else None
        if v[0] == 'sp':
            return ('sp', v[1] + c, v[2] or var)
        return ('arg', v[1], v[2] + c, v[3] or var)

    cur = [0]

    def record(kind, *a):
        if kind == 'call':
            key = (cur[0], kind, a[1])
        elif kind == 'access':
            key = (cur[0], kind, a[0], a[3])
        elif kind in ('ret', 'derived'):
            key = (cur[0], kind)
        else:
            key = (cur[0], kind)
        records[key] = (kind,) + a

    def access(base, off, size, write):
        if base is None or off is None:
            return
        if base[0] == 'arg':
            record('access', base[1], base[2] + off, size, write, base[3])

    def call(target, indirect=False):
        args = [regs.get(a) for a in ARGS]
        consts = [v[1] if v and v[0] == 'const' else None for v in args]
        for k, v in enumerate(args):
            if v is None or v[0] == 'const':
                continue
            record('call', target, k, v, consts, indirect)
        for r in list(regs):
            if r not in CALLEE_SAVED and r not in (0, 'prologue'):
                del regs[r]
        ret = RETURNS.get(target)  # a callee that returns a pointer into an argument (p + 0x1C)
        if ret and args[ret[0]] is not None and args[ret[0]][0] in ('sp', 'arg'):
            regs[2] = add(args[ret[0]], ret[1], ret[2])
            if args[ret[0]][0] == 'sp':
                record('derived', target, args[ret[0]][1], ret[1])

    i = st
    while i < en:
        mn, ops, noreorder = insns[i]
        cur[0] = i
        i += 1
        if mn == 'label':
            continue
        if mn in ('jal', 'j') and ops and not ops[0].startswith('$') and not is_label(ops[0]):
            if noreorder and i < en:  # the delay slot first
                step(insns[i], regs, slots, val, setr, add, access, record)
                i += 1
            call(base_name(ops[0]))
            continue
        if mn in ('jalr', 'jr'):
            if noreorder and i < en:
                step(insns[i], regs, slots, val, setr, add, access, record)
                i += 1
            if mn == 'jr' and ops and ops[0] in ('$31', '$ra'):
                record('ret', regs.get(2))
            if mn == 'jalr':
                call('<indirect>', True)
            continue
        if mn in BRANCH_LIKELY:
            i += 1 if noreorder and i < en else 0  # its slot runs only when taken: left out
            continue
        step(insns[i - 1], regs, slots, val, setr, add, access, record)


def step(insn, regs, slots, val, setr, add, access, record):
    mn, ops, _ = insn
    if not ops:
        return
    if mn in STORES:
        off, base = mem(ops[1]) if len(ops) > 1 else (None, None)
        if base is None:
            return
        bv = regs.get(base)
        src = None if mn in FPR_FIRST else regs.get(GPR.get(ops[0]))
        if base == SP and SP not in regs:
            return
        if base == SP and off is not None:
            off += regs[SP][1]
            for k in [k for k in slots if k < off + STORES[mn] and off < k + 8]:
                del slots[k]
            if src is not None and STORES[mn] >= 4:
                slots[off] = src
            return
        if mn in LEFT and off is not None:  # swl/sdl at X: the bytes up to X (the pair's other half from X-size+1)
            off -= STORES[mn] - 1
        access(bv, off, STORES[mn], True)
        if src is not None and src[0] in ('sp', 'arg') and STORES[mn] >= 4:
            record('stored', src)
        return
    if mn in LOADS:
        off, base = mem(ops[1]) if len(ops) > 1 else (None, None)
        if base is None:  # a load from a symbol (lw $6,D_0029D99C): the register holds data now
            if mn not in FPR_FIRST:
                setr(ops[0], None)
            return
        if mn in LEFT and off is not None:
            off -= LOADS[mn] - 1
        if mn in FPR_FIRST:
            access(regs.get(base), off, LOADS[mn], False)
            return
        if base == SP and SP not in regs:
            setr(ops[0], None)
            return
        if base == SP and off is not None and LOADS[mn] >= 4:
            setr(ops[0], slots.get(off + regs[SP][1]))
            return
        access(regs.get(base), off, LOADS[mn], False)
        setr(ops[0], None)
        return
    if mn in THREE_OP_MULT and len(ops) == 3:  # the EE's three-operand forms write rd too
        setr(ops[0], None)
        return
    if mn in SOURCE_FIRST or mn.startswith(('b', 'c.', 'sq', 'vsq')):
        return
    rd = GPR.get(ops[0])
    if rd == SP:
        # the prologue's adjust (the first) makes the body's $sp, offset 0; a later constant adjust
        # (a VLA of constant size) moves it by that much; anything else (alloca, a VLA) to an
        # unknown place: $sp-relative addresses are then not the frame's
        if mn in ('addiu', 'daddiu', 'addu', 'daddu', 'subu', 'dsubu') and len(ops) == 3 and \
                ops[1] in ('$sp', '$29') and imm(ops[2]) is not None:
            if 'prologue' not in regs:
                regs['prologue'] = True
            elif SP in regs:
                c = imm(ops[2])
                regs[SP] = ('sp', regs[SP][1] + (-c if mn.endswith('subu') else c), False)
            return
        v = val(ops[1]) if mn in ('move', 'dmove') or (len(ops) == 3 and GPR.get(ops[2]) == 0) else None
        if v is not None and v[0] == 'sp':
            regs[SP] = v  # back to a known place ($sp = $fp after a VLA's scope)
        else:
            regs.pop(SP, None)
        return
    if rd is None:
        return
    a = val(ops[1]) if len(ops) > 1 else None
    if mn in ('move', 'dmove'):
        setr(rd, a)
    elif mn in ('addiu', 'daddiu', 'addi', 'daddi', 'addu', 'daddu', 'add', 'dadd', 'subu', 'dsubu',
                'sub', 'dsub') and len(ops) >= 2:
        if len(ops) == 2:
            ops = [ops[0], ops[0], ops[1]]
            a = val(ops[1])
        c = imm(ops[2])
        neg = mn.startswith(('sub', 'dsub'))
        if c is not None:
            v = add(a, -c if neg else c)
            if v is not None and ops[1] == ops[0] and GPR.get(ops[1]) == rd and v[0] in ('sp', 'arg') \
                    and rd != SP:
                v = v[:-1] + (True,)  # p += c on itself: a walk (a loop's step), open-ended
            setr(rd, v)
        elif ops[2].startswith('$'):
            b = val(ops[2])
            if GPR.get(ops[2]) == 0:
                setr(rd, a)
            elif GPR.get(ops[1]) == 0 and not neg:
                setr(rd, b)
            elif a is not None and a[0] in ('sp', 'arg'):
                setr(rd, add(a, b[1]) if b and b[0] == 'const' and not neg else add(a, 0, True))
            elif b is not None and b[0] in ('sp', 'arg') and not neg:
                setr(rd, add(b, a[1]) if a and a[0] == 'const' else add(b, 0, True))
            elif a and b and a[0] == b[0] == 'const':
                setr(rd, ('const', a[1] - b[1] if neg else a[1] + b[1], False))
            else:
                setr(rd, None)
        else:
            setr(rd, None)  # %lo(sym): an address of data
    elif mn in ('or', 'ori') and len(ops) == 3:
        if mn == 'or' and GPR.get(ops[2]) == 0:
            setr(rd, a)
        elif mn == 'or' and GPR.get(ops[1]) == 0:
            setr(rd, val(ops[2]))
        elif mn == 'ori' and a and a[0] == 'const' and imm(ops[2]) is not None:
            setr(rd, ('const', a[1] | imm(ops[2]), False))
        else:
            setr(rd, None)
    elif mn == 'li' and len(ops) == 2 and imm(ops[1]) is not None:
        setr(rd, ('const', imm(ops[1]), False))
    elif mn == 'lui' and len(ops) == 2 and imm(ops[1]) is not None:
        setr(rd, ('const', imm(ops[1]) << 16, False))
    else:
        setr(rd, None)


def run(jobs=6, only=None):
    """The analysis: (functions, escapes)."""
    us = units()
    with ThreadPoolExecutor(jobs) as ex:
        compiled = list(ex.map(compile_unit, us))
    failed = [(r, e) for r, s, e in compiled if s is None]
    funcs = {}
    for rel, s, err in compiled:
        if s is None:
            continue
        cf, _ = read_cc1(s, rel)
        for f in cf:
            funcs[f.name] = f
    for p in glob.glob(os.path.join(ROOT, 'asm', 'nonmatchings', '**', '*.s'), recursive=True):
        name = os.path.basename(p)[:-2]
        if name not in funcs:
            funcs[name] = read_splat(p)

    # summaries to a fixed point
    summ = collections.defaultdict(lambda: collections.defaultdict(Summary))
    edges = collections.defaultdict(list)  # (callee, j) -> [(caller, k, c, var)]
    raw_escapes = []
    derived = []  # (function, $sp offset, callee, c): the callee returned that local's address + c

    # what each function returns, when it is always argument k + c (first pass; one level of such
    # callees is followed by the second)
    RETURNS.clear()
    for f in funcs.values():
        rets = []
        analyse(f, None, lambda kind, *a: rets.append(a[0]) if kind == 'ret' else None)
        if rets and all(r is not None and r[0] == 'arg' and r[1:] == rets[0][1:] for r in rets):
            RETURNS[f.name] = (rets[0][1], rets[0][2], rets[0][3])
    for f in funcs.values():
        def record(kind, *a, f=f):
            if kind == 'access':
                k, lo, size, write, var = a
                s = summ[f.name][k]
                if write:
                    s.w = widen(s.w, lo, lo + size)
                else:
                    s.r = widen(s.r, lo, lo + size)
                if var:
                    s.flags.add('walk-w' if write else 'walk-r')
            elif kind == 'derived' and f.kind == 'c':
                target, off, c = a
                derived.append((f.name, off, target, c))
            elif kind == 'stored':
                v = a[0]
                if v[0] == 'arg':
                    summ[f.name][v[1]].flags.add('stored')
                elif f.kind == 'c':
                    raw_escapes.append((f.name, v[1], '<stored>', None, v[2], None, False))
            elif kind == 'call':
                target, j, v, consts, indirect = a
                if v[0] == 'arg':
                    s = summ[f.name][v[1]]
                    if indirect:
                        s.flags.add('indirect')
                    elif target in COPY or target in STRING:
                        lib_access(s, target, j, v[2], consts, v[3])
                    else:
                        edges[(target, j)].append((f.name, v[1], v[2], v[3]))
                elif v[0] == 'sp' and f.kind == 'c':
                    raw_escapes.append((f.name, v[1], target, j, v[2], consts, indirect))
        analyse(f, summ, record)

    changed, rounds = True, 0
    grew = collections.Counter()
    while changed and rounds < 60:
        changed, rounds = False, rounds + 1
        for (callee, j), callers in edges.items():
            if callee not in funcs:
                for caller, k, c, var in callers:
                    s = summ[caller][k]
                    if 'unknown-callee' not in s.flags:
                        s.flags.add('unknown-callee')
                        changed = True
                continue
            src = summ[callee].get(j)
            if src is None:
                continue
            for caller, k, c, var in callers:
                s = summ[caller][k]
                if 'recursive' in s.flags:
                    continue
                before = s.key()
                if src.w:
                    s.w = widen(s.w, max(-UNBOUNDED, src.w[0] + c), min(UNBOUNDED, src.w[1] + c))
                    s.via[callee] = (j, c, widen(s.via.get(callee, (0, 0, None))[2], src.w[0] + c, src.w[1] + c))
                if src.r:
                    s.r = widen(s.r, src.r[0] + c, src.r[1] + c)
                s.flags |= src.flags - {'stored'} | ({'stored'} if 'stored' in src.flags else set())
                if var and (src.w or src.r):
                    s.flags.add('walk-w' if src.w else 'walk-r')
                if s.key() != before:
                    changed = True
                    grew[(caller, k)] += 1
                    if grew[(caller, k)] > 12:  # a recursion that moves the pointer each call: open-ended
                        s.flags.add('recursive')
    for name, off, target, c in derived:
        # the returned pointer as an escape of its own: a local whose address + c leaves it
        raw_escapes.append((name, off, target, 0, False, ('derived', c), False))
    return funcs, summ, raw_escapes, failed, rounds


def lib_access(s, target, j, c, consts, var):
    if target in COPY:
        dst, ln = COPY[target]
        n = consts[ln] if ln < len(consts) else None
        if j == dst:
            if n is not None and not var:
                s.w = widen(s.w, c, c + n)
            else:
                s.flags.add('copy-var')
        elif target != 'memset' and j == (1 if dst == 0 else 0):
            if n is not None:
                s.r = widen(s.r, c, c + n)
    elif j == STRING[target]:
        s.flags.add('string')


def explain(summ, name, k, indent, depth=0, seen=None):
    """Which callees give arg k of name its write range (the widest first)."""
    seen = seen or set()
    if depth > 8 or (name, k) in seen:
        return
    seen.add((name, k))
    s = summ[name].get(k)
    if not s:
        return
    for callee, (j, c, rng) in sorted(s.via.items(), key=lambda kv: -kv[1][2][1])[:3]:
        print('%s%s(arg%d at +%d) writes %s' % (indent, callee, j, c, rng))
        explain(summ, callee, j, indent + '  ', depth + 1, seen)


def local_at(f, off):
    """The innermost declared local whose bytes hold frame offset off."""
    best = None
    for name, o, size, scope in f.locals:
        if size is not None and o <= off < o + max(size, 1):
            if best is None or scope > best[3] or (scope == best[3] and size < best[2]):
                best = (name, o, size, scope)
    return best


def after(f, end, name):
    """What the EE frame keeps from byte end on: the locals there."""
    out = [(o, n, s) for n, o, s, sc in f.locals if s and o >= end and n != name]
    out.sort()
    return ['%s@0x%X(%d)' % (n, o, s) for o, n, s in out[:2]]


def classify(funcs, summ, raw_escapes):
    rows = []
    for fname, off, target, j, var, consts, indirect in raw_escapes:
        f = funcs[fname]
        loc = local_at(f, off)
        row = {'function': fname, 'unit': f.unit, 'offset': off, 'callee': target, 'arg': j,
               'local': loc[0] if loc else None, 'local_offset': loc[1] if loc else None,
               'local_size': loc[2] if loc else None}
        if loc is None:
            row['verdict'] = 'unnamed'   # a compiler temporary (a struct return, an argument copy)
            rows.append(row)
            continue
        inner = off - loc[1]
        flags, w, r = set(), None, None
        if isinstance(consts, tuple) and consts[0] == 'derived':
            c = consts[1]
            row['write'] = row['read'] = None
            row['flags'] = ['derived']
            if not 0 <= inner + c < loc[2]:
                row['verdict'] = 'overrun-write'
                row['write'] = (inner + c, inner + c + 4)
                row['after'] = after(f, loc[1] + loc[2], loc[0])
            else:
                row['verdict'] = 'fits'
            rows.append(row)
            continue
        if target == '<stored>':
            flags.add('stored')
        elif indirect:
            flags.add('indirect')
        elif target in COPY or target in STRING:
            s = Summary()
            lib_access(s, target, j, 0, consts, var)
            flags, w, r = s.flags, s.w, s.r
        elif target not in funcs:
            flags.add('unknown-callee')
        else:
            s = summ[target].get(j)
            if s is not None:
                flags, w, r = set(s.flags), s.w, s.r
                row['via'] = {k: v for k, v in list(s.via.items())[:4]}
        if var:
            flags.add('indexed')
        row['write'] = w and (w[0] + inner, w[1] + inner)
        row['read'] = r and (r[0] + inner, r[1] + inner)
        row['flags'] = sorted(flags)
        size = loc[2]
        over_w = w and (w[1] + inner > size or w[0] + inner < 0)
        over_r = r and (r[1] + inner > size or r[0] + inner < 0)
        if over_w:
            row['verdict'] = 'overrun-write'
            row['after'] = after(f, loc[1] + size, loc[0])
        elif over_r:
            row['verdict'] = 'overrun-read'
            row['after'] = after(f, loc[1] + size, loc[0])
        elif flags & {'walk-w', 'copy-var', 'string', 'indirect', 'unknown-callee', 'stored', 'indexed',
                      'recursive'}:
            row['verdict'] = 'unbounded'
        else:
            row['verdict'] = 'fits'
        rows.append(row)
    return rows


# ---- sprintf into a local: the format's shortest and longest text (finding 094's shape) -------

SPRINTF = 'func_00254D68'
CONV = re.compile(r'%([-+ #0]*)(\d*|\*)(?:\.(\d*|\*))?(hh|h|ll|l|L|q|j|z|t)?([diouxXeEfgGcspn%])')
ASCIZ = re.compile(r'dlabel (D_[0-9A-F]{8})\s*\n\s*/\* [0-9A-F]+ [0-9A-F]{8} \*/ \.asciz "((?:[^"\\]|\\.)*)"')
_STRINGS = {}


def data_strings():
    """D_XXXXXXXX -> its text, for the data labels that are one .asciz (the extracted data asm)."""
    if not _STRINGS:
        for p in glob.glob(os.path.join(ROOT, 'asm', 'data', '*.s')):
            for m in ASCIZ.finditer(open(p, encoding='utf-8', errors='replace').read()):
                _STRINGS[m.group(1)] = m.group(2).encode().decode('unicode_escape')
    return _STRINGS


def format_lengths(fmt):
    """(shortest, longest) text a printf format can make (longest None: %s without a precision)."""
    lo = hi = 0
    pos = 0
    for m in CONV.finditer(fmt):
        lit = len(fmt[pos:m.start()])
        lo, hi = lo + lit, (hi + lit if hi is not None else None)
        pos = m.end()
        flags, width, prec, size, conv = m.groups()
        w = int(width) if width and width != '*' else 0
        if conv == '%':
            a, b = 1, 1
        elif conv in 'di':
            a, b = 1, 20 if size in ('ll', 'q', 'j') else 11
        elif conv in 'ouxX':
            a, b = 1, {'o': 11, 'u': 10, 'x': 8, 'X': 8}[conv] * (2 if size in ('ll', 'q') else 1)
        elif conv == 'c':
            a, b = 1, 1
        elif conv == 's':
            a, b = 0, int(prec) if prec and prec != '*' else None
        elif conv in 'fF':
            p = int(prec) if prec and prec != '*' else 6
            a, b = 1 + (p + 1 if p else 0), 48 + p
        elif conv in 'eEgG':
            a, b = 1, 24
        else:  # %p %n
            a, b = 0 if conv == 'n' else 1, 0 if conv == 'n' else 10
        if '*' in (width or ''):
            b = None
        lo += max(a, w)
        hi = None if (hi is None or b is None) else hi + max(b, w)
    lit = len(fmt[pos:])
    return lo + lit, (hi + lit if hi is not None else None)


def sprintf_calls(text, name):
    """(start, args) of each call of a sprintf alias in text."""
    import transform
    mask = transform.code_mask(text)
    aliases = {SPRINTF} | set(re.findall(r'\b(\w+)\s*\([^;]*?\)\s*(?:__asm__|asm)\s*\(\s*"%s"\s*\)' % SPRINTF, text))
    for m in re.finditer(r'\b(%s)\s*\(' % '|'.join(map(re.escape, sorted(aliases))), mask):
        end = transform.close_paren(mask, m.end() - 1)
        if end is None:
            continue
        args = [text[a:b].strip() for a, b in transform.split_top(text, mask, m.end(), end, ',')]
        if args and re.match(r'(?:const\s+)?(?:unsigned\s+)?(?:char|void)\b', args[0]):
            continue  # a declaration
        yield m.start(), args


def function_body(text, name):
    import transform
    mask = transform.code_mask(text)
    for m in re.finditer(r'\b%s\s*\([^;{}]*\)\s*\{' % re.escape(name), mask):
        end = transform.close_paren(mask, m.end() - 1)
        if end:
            return m.end() - 1, end
    return None


def format_rows(funcs):
    """sprintf into a C function's own local: the format's text against the local's size."""
    sys.path.insert(0, HERE)
    strings = data_strings()
    rows = []
    by_unit = collections.defaultdict(list)
    for f in funcs.values():
        if f.kind == 'c' and f.locals:
            by_unit[f.unit].append(f)
    for unit, fs in by_unit.items():
        text = open(os.path.join(ROOT, 'src', unit), encoding='utf-8', errors='replace').read()
        if SPRINTF not in text:
            continue
        calls = list(sprintf_calls(text, SPRINTF))
        for f in fs:
            rng = function_body(text, f.name)
            if not rng:
                continue
            sizes = {n: s for n, o, s, sc in f.locals if s}
            for at, args in calls:
                if not (rng[0] <= at < rng[1]) or len(args) < 2:
                    continue
                dst = re.sub(r'^\(\s*(?:const\s+)?(?:unsigned\s+)?char\s*\*\s*\)\s*', '', args[0])
                m = re.fullmatch(r'\(?\s*&?\s*\(?(\w+)\)?\s*(?:\[\s*0\s*\])?\s*\)?', dst)
                if not m or m.group(1) not in sizes:
                    continue
                fmt_arg = re.sub(r'^\(\s*(?:const\s+)?char\s*\*\s*\)\s*', '', args[1]).strip('&() ')
                if fmt_arg.startswith('"'):
                    fmt = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', fmt_arg)).encode().decode('unicode_escape')
                elif fmt_arg in strings:
                    fmt = strings[fmt_arg]
                else:
                    continue
                lo, hi = format_lengths(fmt)
                size = sizes[m.group(1)]
                verdict = 'format-overrun' if lo + 1 > size else \
                    'format-maybe' if hi is not None and hi + 1 > size else None
                if verdict:
                    rows.append({'function': f.name, 'unit': unit, 'local': m.group(1), 'local_size': size,
                                 'callee': SPRINTF, 'arg': 0, 'verdict': verdict, 'format': fmt,
                                 'text': [lo + 1, hi + 1 if hi is not None else None], 'flags': ['string'],
                                 'local_offset': next(o for n, o, s, sc in f.locals if n == m.group(1)),
                                 'write': [0, lo + 1], 'read': None})
    return rows


# what the report lists, and what fails --check until reviewed
FLAGGED = ('overrun-write', 'overrun-read', 'format-overrun', 'format-maybe')
FAILING = ('overrun-write', 'format-overrun', 'format-maybe')


def check_fixed(reviewed):
    """A 'fixed' line names the unit whose port patch fixes it: the unit must still have patches."""
    sys.path.insert(0, os.path.dirname(HERE))
    import patches
    bad = 0
    for key, (verdict, reason, n) in sorted(reviewed.items()):
        if verdict not in ('fixed', 'safe'):
            print('ERROR port/tools/stack_escapes_reviewed.txt:%d: verdict %r (fixed or safe)' % (n, verdict))
            bad += 1
        elif verdict == 'fixed':
            m = re.match(r'(\S+?):', reason)
            if not m or m.group(1) not in patches.PATCHES:
                print('ERROR port/tools/stack_escapes_reviewed.txt:%d: fixed, but %s has no port patch'
                      % (n, m.group(1) if m else '(no unit named)'))
                bad += 1
    return bad


def load_compile_failures():
    """{unit: (reason, line)}: the units whose retail text the matching compiler cannot build with -gstabs."""
    out = {}
    if not os.path.exists(COMPILE_FAILURES):
        return out
    for n, line in enumerate(open(COMPILE_FAILURES, encoding='utf-8'), 1):
        line = line.split('#', 1)[0].strip()
        if line:
            unit, _, reason = line.partition(' ')
            if not reason.strip():
                sys.exit('%s:%d: UNIT REASON' % (COMPILE_FAILURES, n))
            out[unit] = (reason.strip(), n)
    return out


def check_compile_failures(failed, expected, check):
    """A unit that does not compile is not analysed. In --check that fails unless the unit is listed
    with its reason, and a listed unit that compiles now fails too (the list cannot rot). Returns the
    number of errors."""
    bad = 0
    where = 'port/tools/stack_escapes_compile_failures.txt'
    broken = {r for r, e in failed}
    for r, e in failed:
        if r in expected:
            continue
        if check:
            print('ERROR src/%s: the matching compiler failed with -gstabs, so the unit is not analysed '
                  '(fix the unit, or list it with the reason in %s): %s'
                  % (r, where, e.splitlines()[-1] if e else ''))
            bad += 1
        else:
            print('WARN src/%s: the matching compiler failed with -gstabs: %s' % (r, e.splitlines()[-1] if e else ''))
    if check:
        for unit, (reason, n) in sorted(expected.items()):
            if unit not in broken:
                print('ERROR %s:%d: %s is listed as not compiling, but it compiles (or is gone): remove the line'
                      % (where, n, unit))
                bad += 1
    return bad


def load_reviewed():
    out = {}
    if not os.path.exists(REVIEWED):
        return out
    for n, line in enumerate(open(REVIEWED, encoding='utf-8'), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 4)
        if len(parts) < 4:
            sys.exit('%s:%d: FUNCTION LOCAL CALLEE VERDICT [REASON]' % (REVIEWED, n))
        out[(parts[0], parts[1], parts[2])] = (parts[3], parts[4] if len(parts) > 4 else '', n)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--jobs', '-j', type=int, default=6)
    ap.add_argument('--json')
    ap.add_argument('--report')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--function', action='append', default=[])
    a = ap.parse_args()
    funcs, summ, raw, failed, rounds = run(a.jobs)
    rows = classify(funcs, summ, raw) + format_rows(funcs)
    bad = check_compile_failures(failed, load_compile_failures(), a.check)
    cfuncs = [f for f in funcs.values() if f.kind == 'c']
    counts = collections.Counter(r['verdict'] for r in rows)
    with_locals = {r['function'] for r in rows}
    print('functions: %d C (%d with an escaped local), %d asm; summaries in %d rounds'
          % (len(cfuncs), len(with_locals), len(funcs) - len(cfuncs), rounds))
    print('escapes: %d (%s)' % (len(rows), ', '.join('%s %d' % kv for kv in sorted(counts.items()))))
    if a.function:
        for r in rows:
            if r['function'] in a.function:
                print(json.dumps(r))
        for name in a.function:
            f = funcs.get(name)
            if f:
                print(name, 'frame', f.frame, 'locals', f.locals)
                for k, s in sorted(summ[name].items()):
                    print('  arg%d w=%s r=%s flags=%s' % (k, s.w, s.r, sorted(s.flags)))
                    explain(summ, name, k, '    ')
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=1)
    reviewed = load_reviewed()
    if a.report or a.check:
        lines = []
        for r in rows:
            if r['verdict'] not in FLAGGED:
                continue
            key = (r['function'], r['local'], r['callee'])
            rv = reviewed.get(key)
            line = ('%s %s %s: %s (%s %d bytes at sp+0x%X) callee %s arg%d writes %s reads %s flags %s; after: %s'
                    % (r['verdict'], r['function'], r['local'], r['unit'], r['local'], r['local_size'],
                       r['local_offset'], r['callee'], r['arg'], r['write'], r['read'], ','.join(r['flags']),
                       ' '.join(r.get('after', []))))
            if 'format' in r:
                line += ' format %r makes %s bytes' % (r['format'], r['text'])
            lines.append((rv, line))
            if rv is None and r['verdict'] in FAILING:
                bad += 1
                if a.check:
                    print('ERROR src/%s: %s not reviewed (port/tools/stack_escapes_reviewed.txt)' % (r['unit'], line))
        if a.report:
            with open(a.report, 'w') as fh:
                for rv, line in lines:
                    fh.write(('[%s] ' % rv[0] if rv else '[new] ') + line + '\n')
    if a.check:
        bad += check_fixed(reviewed)
        print('stack_escapes: %d problem(s): unreviewed overrun candidates, units that do not compile' % bad)
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
