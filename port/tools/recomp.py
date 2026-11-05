"""PC build: static recompilation of MIPS functions to C (plan step 5), for the game functions
that have no C version yet. Each function's splat asm (asm/nonmatchings, generated from your own
ELF) becomes <OUT>/<name>.c (build_wasm.py: build/wasm/recomp, signatures from build/wasm/sigsrc):

- rc_<name>(void) runs the instructions on the emulated register file (port/runtime/recomp.h):
  integer, FPU, memory and control flow as C statements, VU0/MMI instructions through the asm
  runtime (ps2_exec; PS2X in the wasm build). Delay slots run before their branch, branch-likely slots only when taken.
- <name>(...) is a wrapper with the function's C signature (from the callers' declarations,
  read with clang), so native C calls it as usual: it puts the arguments in $a0-$t3 / $f12-$f19,
  runs rc_<name> and returns $v0 / $f0.
- Calls out of recompiled code go to rc_<callee> when the callee is recompiled too, otherwise to
  the native function, with its arguments taken from the registers by its C signature.
- %hi/%lo/%gp_rel relocations become host addresses of the symbols; an indirect jump (jr) is a
  switch over the function's labels, whose PS2 addresses the converted jump tables hold
  (port/tools/data.py).
This is a bridge to a running game, not readable code: a function's matching C replaces it.
usage: python port/tools/recomp.py NAME...   (normally run by port/tools/build_wasm.py)"""
import glob, hashlib, json, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
P = ''  # the module's C symbols carry no prefix (data.LABEL_PREFIX)
TARGET = 'wasm32-wasip1'  # the core's target (build_wasm.WASM_TARGET): the callers' declarations

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASM = os.path.join(ROOT, 'asm', 'nonmatchings', 'text')
SRC = os.path.join(ROOT, 'build', 'port', 'src')
OUT = os.path.join(ROOT, 'build', 'port', 'recomp')
# build_wasm.py sets this: the wasm output passes exactly the callee's declared arguments when its
# signature is not known from a declaration (the i686 output keeps its eight-register padding)
WASM_EMIT = False

GPR_NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6',
             't7', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp',
             'fp', 'ra']
GPR = dict({n: i for i, n in enumerate(GPR_NAMES)}, s8=30)
INT_ARGS, FLOAT_ARGS = list(range(4, 12)), list(range(12, 20))  # $a0-$t3, $f12-$f19

INSN = re.compile(r'^\s*/\* [0-9A-F]+ ([0-9A-F]{8}) ([0-9A-F]{8}) \*/\s+(\S+)\s*(.*?)\s*$')
# .Lxxx: or jlabel/ehlabel .Lxxx (a jump-table target, the start of an exception range)
LOCAL = re.compile(r'^\s*(?:\.L([0-9A-F]{8}):|[a-z]*label\s+\.L([0-9A-F]{8}))\s*$')
RELOC = re.compile(r'^%(hi|lo|gp_rel)\(\s*([A-Za-z_.$][\w.$]*)\s*(?:\+\s*(0x[0-9A-Fa-f]+|\d+))?\s*\)$')
MEM = re.compile(r'^(.*)\((\$\w+)\)$')
IMM_CHARS = re.compile(r'^[\s0-9a-fA-FxX()+\-*&|<>~]+$')

BRANCH = {'b': None, 'beq': '==', 'bne': '!=', 'beqz': '==0', 'bnez': '!=0', 'blez': '<=0', 'bgtz': '>0',
          'bltz': '<0', 'bgez': '>=0', 'bc1t': 'fcc', 'bc1f': '!fcc', 'bc0t': '1', 'bc0f': '0'}
LIKELY = {k + 'l': v for k, v in BRANCH.items() if k != 'b'}


class Fail(Exception):
    pass


# ---- parsing ----

MULDIV = ('mult', 'multu', 'madd', 'maddu', 'div', 'divu', 'mflo', 'mfhi', 'mtlo', 'mthi')


_entries = None


def entries():
    """Functions whose code starts past a few instructions splat gave them (padding after the
    previous function): ({name: offset of the real entry}, {name at the entry: function}).
    The asm refers to the entry as func_X + K, or by the name of an alabel just past the padding."""
    global _entries
    if _entries is None:
        offset, alias = {}, {}
        for p in glob.glob(os.path.join(ASM, '*.s')):
            t = open(p, encoding='utf-8', errors='replace').read()
            for f, k in re.findall(r'\b(func_[0-9A-F]{8}) \+ (0x[0-9A-F]+)', t):
                offset[f] = int(k, 16)
            name = os.path.basename(p)[:-2]
            m = re.search(INSN.pattern, t, re.M)
            for a in re.findall(r'^\s*alabel (func_([0-9A-F]{8}))\s*$', t, re.M):
                k = int(a[1], 16) - int(m.group(1), 16)
                if 0 < k <= 0x10:
                    alias[a[0]] = name
                    offset[name] = k
        _entries = (offset, alias)
    return _entries


_data_entries = None
UNCONDITIONAL = ('b', 'j', 'jr', 'jalr')


def data_entries():
    """{func_<entry>: (asm function, entry address)}: code addresses the data holds as plain
    numbers (vtables of the C++ runtime stubs, callbacks) that land inside a function splat made
    of several (it takes a function's tail and the alignment nop for the start of the next). An
    entry follows that nop, or a return or jump and opens a stack frame; other addresses inside
    functions (switch tables) aren't entries. Each one is recompiled as a function of its own."""
    global _data_entries
    if _data_entries is None:
        words = set()
        for p in glob.glob(os.path.join(ROOT, 'asm', 'data', '*.s')):
            words |= {int(w, 16) for w in re.findall(r'\.word (0x[0-9A-F]{8})\b', open(p, encoding='utf-8').read())}
        starts = {}
        for p in glob.glob(os.path.join(ASM, 'func_*.s')):
            starts[int(os.path.basename(p)[5:13], 16)] = p
        ordered = sorted(starts)
        _data_entries = {}
        for w in sorted(words):
            if w % 4 or w in starts or not ordered[0] < w < 0x263BC0:
                continue
            s = max(a for a in ordered if a < w)  # the asm function holding it (or none: decompiled)
            ins = {}
            for line in open(starts[s], encoding='utf-8', errors='replace'):
                m = INSN.match(line)
                if m:
                    ins[int(m.group(1), 16)] = (m.group(3), m.group(4))
            if w not in ins or w - 8 not in ins:
                continue
            before, prev, at = ins[w - 8], ins[w - 4], ins[w]
            padded = prev[0] == 'nop' and before[0] not in BRANCH and before[0] not in LIKELY \
                and before[0] not in ('j', 'jal', 'jr', 'jalr')
            frame = before[0] in UNCONDITIONAL and at[0] == 'addiu' and at[1].startswith('$sp, $sp, -')
            if padded or frame:
                _data_entries['func_%08X' % w] = (os.path.basename(starts[s])[:-2], w)
    return _data_entries


def parse(name):
    """[(addr, op, [operands])] and the set of label addresses of one function, from its real
    entry on (entries(), data_entries())."""
    insns, labels = [], set()
    offset, alias = entries()
    start = None
    whole, entry = data_entries().get(name, (name, None))
    for line in open(os.path.join(ASM, whole + '.s'), encoding='utf-8', errors='replace'):
        for f, k in offset.items():  # func_X + K, the entry, is func_X here
            line = line.replace('%s + 0x%X' % (f, k), f)
        for a, f in alias.items():
            line = re.sub(r'\b%s\b' % a, f, line)
        m = INSN.match(line)
        if m and start is None:
            start = int(m.group(1), 16)
        if m and int(m.group(1), 16) < (entry or start + offset.get(name, 0)):
            continue
        m = LOCAL.match(line)
        if m:
            labels.add(int(m.group(1) or m.group(2), 16))
            continue
        m = INSN.match(line)
        if not m:
            continue
        addr, word, op, rest = int(m.group(1), 16), m.group(2), m.group(3), m.group(4)
        rest = re.sub(r'/\*.*?\*/', '', rest)
        if op in ('.word', 'c1'):
            op, args = decode_cop1(int.from_bytes(bytes.fromhex(word), 'little'), name, addr)
        else:
            rest = rest.split('#')[0].strip()
            args = split_args(rest) if rest else []
        insns.append((addr, op, args))
    return insns, {a for a in labels if a >= insns[0][0]}


def decode_cop1(w, name, addr):
    """FPU words the disassembler left raw: EE sqrt.s (fd = sqrt(ft)) and cvt.w.s."""
    fmt, ft, fs, fd, funct = w >> 21 & 31, w >> 16 & 31, w >> 11 & 31, w >> 6 & 31, w & 63
    if w >> 26 == 0x11 and fmt == 0x10 and funct == 4:
        return 'sqrt.s', ['$f%d' % fd, '$f%d' % ft]
    if w >> 26 == 0x11 and fmt == 0x10 and funct == 0x24:
        return 'cvt.w.s', ['$f%d' % fd, '$f%d' % fs]
    raise Fail('%s %08X: raw word %08X' % (name, addr, w))


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        depth += ch == '('
        depth -= ch == ')'
        cur += ch
    return out + [cur.strip()]


# ---- operands ----

class Func:
    def __init__(self, name, recompiled, sigs):
        self.name, self.recompiled, self.sigs = name, recompiled, sigs
        self.symbols, self.callees, self.lines = set(), set(), []

    def gpr(self, s):
        s = s.lstrip('$')
        if s.isdigit():
            return int(s)
        if s not in GPR:
            raise Fail('%s: register %r' % (self.name, s))
        return GPR[s]

    @staticmethod
    def fpr(s):
        m = re.match(r'^\$f(\d+)$', s)
        if not m:
            raise Fail('fpr %r' % s)
        return int(m.group(1))

    def sym(self, name, addend):
        self.symbols.add(name)
        return '((uint32_t)(uintptr_t)A_%s + %d)' % (c_ident(name), addend) if addend else \
            '((uint32_t)(uintptr_t)A_%s)' % c_ident(name)

    def imm(self, s, kind='s16'):
        """C expression of an immediate; kind s16 (sign-extended), u16 (zero-extended), hi (lui)."""
        m = RELOC.match(s)
        if m:
            rel, name, add = m.group(1), m.group(2), int(m.group(3) or '0', 0)
            a = self.sym(name, add)
            if rel == 'hi' and kind == 'hi':
                return '((%s + 0x8000u) & 0xFFFF0000u)' % a
            if rel == 'lo' and kind == 's16':
                return '(int16_t)(%s & 0xFFFF)' % a
            raise Fail('%s: %s relocation in a %s operand' % (self.name, rel, kind))
        if not IMM_CHARS.match(s):
            raise Fail('%s: immediate %r' % (self.name, s))
        v = eval(s, {'__builtins__': {}})  # noqa: S307 (digits and operators only, checked above)
        if kind == 'hi':
            return '0x%Xu' % ((v & 0xFFFF) << 16)
        v &= 0xFFFF
        if kind == 's16' and v & 0x8000:
            v -= 0x10000
        return str(v)

    def addr(self, s):
        """C expression (uintptr_t) of a memory operand off(base)."""
        m = MEM.match(s)
        if not m:
            raise Fail('%s: memory operand %r' % (self.name, s))
        off, base = m.group(1).strip() or '0', self.gpr(m.group(2))
        r = RELOC.match(off)
        if r and r.group(1) == 'gp_rel':
            return '(uintptr_t)%s' % self.sym(r.group(2), int(r.group(3) or '0', 0))
        return 'ADDR(%d, %s)' % (base, self.imm(off))

    # ---- instructions without control flow ----

    def set32(self, rd, expr):
        if rd:
            self.lines.append('GPR(%d) = S32(%s);' % (rd, expr))

    def set64(self, rd, expr):
        if rd:
            self.lines.append('GPR(%d) = %s;' % (rd, expr))

    def cop2_move(self, op, a, text):
        """A COP2 register move (qmtc2, qmfc2, cfc2, ctc2) or a vcallms as C, or None to leave it
        on ps2_exec.

        The C does what ps2asm.c's run_ins does for the move, without ps2_exec's template lookup
        and program call (finding 090 SP11). The literal stays in the code through
        ps2_exec_kept(): its .rodata bytes keep their place, and ps2asm_aot.py still generates its
        program, so the module's data and function table do not move (SP1 found that dropping
        the templates changes the race trace by layout alone). Control registers with side effects
        or float views (R, I, Q, FBRST, VPU-STAT: 20, 21, 22, 28, 29), moves to $zero and any other
        spelling stay on ps2_exec."""
        if op == 'vcallms' and len(a) == 1 and re.fullmatch(r'0x[0-9A-Fa-f]+|\d+', a[0]):
            return 'ps2_exec_kept("%s"); ps2_vcallms("%s", %su);' % (text, text, a[0])
        move = op.split('.')[0]
        if move not in ('qmtc2', 'qmfc2', 'cfc2', 'ctc2') or op.count('.') > 1 or len(a) != 2:
            return None
        if op != move and op.split('.')[1] not in ('i', 'ni'):
            return None
        m = re.match(r'^\$v([fi])(\d+)$', a[1])
        rt = GPR.get(a[0][1:]) if a[0].startswith('$') else None
        if not m or rt is None or (m.group(1) == 'f') != (move in ('qmtc2', 'qmfc2')):
            return None
        n = int(m.group(2))
        if n > 31 or (rt == 0 and move in ('qmfc2', 'cfc2')):
            return None
        keep = 'ps2_exec_kept("%s"); ' % text
        if move == 'qmtc2':
            return keep + ('ps2.vf[%d] = ps2.gpr[%d];' % (n, rt) if n else '')
        if move == 'qmfc2':
            if n:
                return keep + 'ps2.gpr[%d] = ps2.vf[%d];' % (rt, n)
            return keep + ('ps2.gpr[%d].d[0] = 0; ps2.gpr[%d].d[1] = 0x3F80000000000000ull;' % (rt, rt))
        if n in (20, 21, 22, 28, 29):
            return None
        reg = 'ps2.vi[%d]' % n if n < 16 else 'ps2.vu0_ctrl[%d]' % (n - 16)
        if move == 'cfc2':
            return keep + 'ps2.gpr[%d].d[0] = %s;' % (rt, reg)
        if n == 0:
            return keep
        return keep + '%s = (%s)ps2.gpr[%d].d[0];' % (reg, 'uint16_t' if n < 16 else 'uint32_t', rt)

    def insn(self, op, a, text):
        g, f, L = self.gpr, self.fpr, self.lines
        # the wasm build's inline forms (recomp.h, finding 090 SP15); the same results
        STORE = 'rc' if WASM_EMIT else 'ps2'
        EE_ADD, EE_SUB, EE_MUL = ('rc_ee_add', 'rc_ee_sub', 'rc_float_mul') if WASM_EMIT else             ('ps2_ee_add', 'ps2_ee_sub', 'ps2_float_mul')
        EE_DIV, EE_SQRT, EE_CVT_W = ('ps2_ee_div', 'ps2_ee_sqrt', 'ps2_float_to_int') if WASM_EMIT else             ('ps2_fdiv', 'ps2_fsqrt', 'ps2_cvt_w_s')
        if op in ('nop', 'sync', 'sync.p', 'sync.l', 'break', 'ssnop'):
            return
        if op in ('addu', 'add'):
            return self.set32(g(a[0]), 'UW(%d) + UW(%d)' % (g(a[1]), g(a[2])))
        if op in ('subu', 'sub'):
            return self.set32(g(a[0]), 'UW(%d) - UW(%d)' % (g(a[1]), g(a[2])))
        r = RELOC.match(a[2]) if op in ('addiu', 'addi') and len(a) == 3 else None
        if r and r.group(1) == 'gp_rel':  # addiu rd, $gp, %gp_rel(sym): the symbol's address
            return self.set32(g(a[0]), self.sym(r.group(2), int(r.group(3) or '0', 0)))
        if op in ('addiu', 'addi'):
            return self.set32(g(a[0]), 'UW(%d) + (int32_t)%s' % (g(a[1]), self.imm(a[2])))
        if op == 'daddu':
            return self.set64(g(a[0]), 'GPR(%d) + GPR(%d)' % (g(a[1]), g(a[2])))
        if op == 'dsubu':
            return self.set64(g(a[0]), 'GPR(%d) - GPR(%d)' % (g(a[1]), g(a[2])))
        if op == 'daddiu':
            return self.set64(g(a[0]), 'GPR(%d) + (int64_t)%s' % (g(a[1]), self.imm(a[2])))
        if op == 'negu':
            return self.set32(g(a[0]), '0u - UW(%d)' % g(a[1]))
        if op in ('and', 'or', 'xor'):
            c = {'and': '&', 'or': '|', 'xor': '^'}[op]
            return self.set64(g(a[0]), 'GPR(%d) %s GPR(%d)' % (g(a[1]), c, g(a[2])))
        if op == 'nor':
            return self.set64(g(a[0]), '~(GPR(%d) | GPR(%d))' % (g(a[1]), g(a[2])))
        if op in ('andi', 'ori', 'xori'):
            c = {'andi': '&', 'ori': '|', 'xori': '^'}[op]
            return self.set64(g(a[0]), 'GPR(%d) %s (uint64_t)%s' % (g(a[1]), c, self.imm(a[2], 'u16')))
        if op == 'lui':
            return self.set32(g(a[0]), self.imm(a[1], 'hi'))
        if op in ('sll', 'srl', 'sra'):
            src = {'sll': 'UW(%d) << %s', 'srl': 'UW(%d) >> %s', 'sra': 'SW(%d) >> %s'}[op]
            return self.set32(g(a[0]), src % (g(a[1]), self.imm(a[2], 'u16')))
        if op in ('sllv', 'srlv', 'srav'):
            src = {'sllv': 'UW(%d) << (UW(%d) & 31)', 'srlv': 'UW(%d) >> (UW(%d) & 31)',
                   'srav': 'SW(%d) >> (UW(%d) & 31)'}[op]
            return self.set32(g(a[0]), src % (g(a[1]), g(a[2])))
        m = re.match(r'^d(sll|srl|sra)(32)?$', op)
        if m:
            sa = int(self.imm(a[2], 'u16')) + (32 if m.group(2) else 0)
            src = {'sll': 'GPR(%d) << %d', 'srl': 'GPR(%d) >> %d', 'sra': '(uint64_t)((int64_t)GPR(%d) >> %d)'}
            return self.set64(g(a[0]), src[m.group(1)] % (g(a[1]), sa))
        if op in ('dsllv', 'dsrlv', 'dsrav'):
            src = {'dsllv': 'GPR(%d) << (UW(%d) & 63)', 'dsrlv': 'GPR(%d) >> (UW(%d) & 63)',
                   'dsrav': '(uint64_t)((int64_t)GPR(%d) >> (UW(%d) & 63))'}[op]
            return self.set64(g(a[0]), src % (g(a[1]), g(a[2])))
        if op in ('slt', 'sltu'):
            t = 'int64_t' if op == 'slt' else 'uint64_t'
            return self.set64(g(a[0]), '(uint64_t)((%s)GPR(%d) < (%s)GPR(%d))' % (t, g(a[1]), t, g(a[2])))
        if op in ('slti', 'sltiu'):
            t = 'int64_t' if op == 'slti' else 'uint64_t'
            return self.set64(g(a[0]), '(uint64_t)((%s)GPR(%d) < (%s)(int64_t)%s)' % (t, g(a[1]), t, self.imm(a[2])))
        if op in ('movz', 'movn'):
            if g(a[0]):
                L.append('if (GPR(%d) %s 0) GPR(%d) = GPR(%d);' % (g(a[2]), '==' if op == 'movz' else '!=',
                                                                    g(a[0]), g(a[1])))
            return
        # the EE's second multiply/divide pipeline (mult1, div1, mflo1 ...) has its own LO1/HI1
        pipe = '1' if op.endswith('1') and op[:-1] in MULDIV else ''
        if pipe:
            op = op[:-1]
        lo, hi = 'ps2.lo' + pipe, 'ps2.hi' + pipe
        if op in ('mult', 'multu', 'madd', 'maddu'):
            rd, rs, rt = (0, g(a[0]), g(a[1])) if len(a) == 2 else (g(a[0]), g(a[1]), g(a[2]))
            t, w = ('int64_t', 'SW') if op in ('mult', 'madd') else ('uint64_t', 'UW')
            acc = '((uint64_t)(uint32_t)%s << 32 | (uint32_t)%s) + ' % (hi, lo) if op.startswith('madd') else ''
            L.append('{ uint64_t p = %s(uint64_t)((%s)%s(%d) * (%s)%s(%d)); %s = S32(p); %s = S32(p >> 32); }'
                     % (acc, t, w, rs, t, w, rt, lo, hi))
            return self.set64(rd, lo)
        if op in ('div', 'divu'):
            rs, rt = (g(a[1]), g(a[2])) if len(a) == 3 else (g(a[0]), g(a[1]))
            call = 'ps2_%s(%s(%d), %s(%d));' % (op, 'SW' if op == 'div' else 'UW', rs, 'SW' if op == 'div' else 'UW', rt)
            if pipe:  # ps2_div writes LO/HI: move its result over and keep pipeline 0's
                call = ('{ uint64_t l = ps2.lo, h = ps2.hi; %s ps2.lo1 = ps2.lo; ps2.hi1 = ps2.hi; '
                        'ps2.lo = l; ps2.hi = h; }' % call)
            L.append(call)
            return
        if op in ('mflo', 'mfhi'):
            return self.set64(g(a[0]), lo if op == 'mflo' else hi)
        if op in ('mtlo', 'mthi'):
            L.append('%s = GPR(%d);' % (lo if op == 'mtlo' else hi, g(a[0])))
            return
        loads = {'lb': ('M8', 32), 'lbu': ('MU8', 32), 'lh': ('M16', 32), 'lhu': ('MU16', 32), 'lw': ('M32', 32),
                 'lwu': ('MU32', 64), 'ld': ('M64', 64)}
        if op in loads:
            mac, bits = loads[op]
            ex = '%s(%s)' % (mac, self.addr(a[1]))
            return self.set32(g(a[0]), ex) if bits == 32 else self.set64(g(a[0]), '(uint64_t)' + ex)
        stores = {'sb': 'MU8', 'sh': 'MU16', 'sw': 'MU32', 'sd': 'M64'}
        if op in stores:
            if op in ('sw', 'sd'):
                L.append('%s_store%d(%s, GPR(%d));' % (STORE, 32 if op == 'sw' else 64, self.addr(a[1]), g(a[0])))
            else:
                L.append('%s(%s) = GPR(%d);' % (stores[op], self.addr(a[1]), g(a[0])))
            return
        if op == 'lq':
            if g(a[0]):
                L.append('ps2.gpr[%d] = M128(%s);' % (g(a[0]), self.addr(a[1])))
            return
        if op == 'sq':
            L.append('%s_store128((%s) & ~(uintptr_t)15, &ps2.gpr[%d]);' % (STORE, self.addr(a[1]), g(a[0])))
            return
        if op in ('ldl', 'ldr', 'lwl', 'lwr'):
            return self.set64(g(a[0]), 'ps2_%s(GPR(%d), %s)' % (op, g(a[0]), self.addr(a[1])))
        if op in ('sdl', 'sdr', 'swl', 'swr'):
            L.append('ps2_%s(GPR(%d), %s);' % (op, g(a[0]), self.addr(a[1])))
            return
        if op == 'cache':  # no caches on the host
            return
        # COP0: the kernel's interrupt switches (Status bit 16, EIE) and the cycle counter (Count)
        if op == 'mfc0':
            return self.set32(g(a[0]), 'ps2_mfc0(%d)' % int(a[1].lstrip('$')))
        if op == 'mtc0':
            L.append('ps2.cop0[%d] = UW(%d);' % (int(a[1].lstrip('$')), g(a[0])))
            return
        if op in ('ei', 'di'):
            L.append('ps2.cop0[12] %s;' % ('|= 0x10000' if op == 'ei' else '&= ~0x10000u'))
            return
        if op == 'por':
            for i in (0, 1):
                if g(a[0]):
                    L.append('ps2.gpr[%d].d[%d] = ps2.gpr[%d].d[%d] | ps2.gpr[%d].d[%d];'
                             % (g(a[0]), i, g(a[1]), i, g(a[2]), i))
            return
        # FPU
        if op == 'lwc1':
            L.append('FPRW(%d) = M32(%s);' % (f(a[0]), self.addr(a[1])))
            return
        if op == 'swc1':
            L.append('M32(%s) = FPRW(%d);' % (self.addr(a[1]), f(a[0])))
            return
        if op == 'mtc1':
            L.append(('FPRW(%d) = rc_opaque32((int32_t)UW(%d));' if WASM_EMIT else 'FPRW(%d) = (int32_t)UW(%d);')
                     % (f(a[1]), g(a[0])))
            return
        if op == 'mfc1':
            return self.set32(g(a[0]), 'FPRW(%d)' % f(a[1]))
        binf = {'add.s': EE_ADD + '(%s, %s)', 'sub.s': EE_SUB + '(%s, %s)',
                'mul.s': EE_MUL + '(%s, %s)', 'div.s': EE_DIV + '(%s, %s)',
                'max.s': 'ps2_float_minmax(%s, %s, 1)', 'min.s': 'ps2_float_minmax(%s, %s, 0)'}
        if op in binf:
            L.append('FPR(%d) = %s;' % (f(a[0]), binf[op] % ('FPR(%d)' % f(a[1]), 'FPR(%d)' % f(a[2]))))
            return
        unf = {'mov.s': '%s', 'neg.s': '-%s', 'abs.s': '__builtin_fabsf(%s)', 'sqrt.s': EE_SQRT + '(%s)'}
        if op in unf:
            L.append('FPR(%d) = %s;' % (f(a[0]), unf[op] % ('FPR(%d)' % f(a[1]))))
            return
        if op == 'rsqrt.s':  # unlike DIV and SQRT, rounds toward zero (ps2float.h)
            L.append('FPR(%d) = ps2_float_div(FPR(%d), ps2_float_sqrt(FPR(%d)));' % (f(a[0]), f(a[1]), f(a[2])))
            return
        if op == 'cvt.s.w':
            L.append('FPR(%d) = (float)FPRW(%d);' % (f(a[0]), f(a[1])))
            return
        if op == 'cvt.w.s':
            L.append('FPRW(%d) = %s(FPR(%d));' % (f(a[0]), EE_CVT_W, f(a[1])))
            return
        accf = {'mula.s': 'ps2.fpu_acc = ' + EE_MUL + '(%s, %s)',
                'madda.s': 'ps2.fpu_acc = ' + EE_ADD + '(ps2.fpu_acc, ' + EE_MUL + '(%s, %s))',
                'msuba.s': 'ps2.fpu_acc = ' + EE_SUB + '(ps2.fpu_acc, ' + EE_MUL + '(%s, %s))',
                'adda.s': 'ps2.fpu_acc = ' + EE_ADD + '(%s, %s)',
                'suba.s': 'ps2.fpu_acc = ' + EE_SUB + '(%s, %s)'}
        if op in accf:
            L.append(accf[op] % ('FPR(%d)' % f(a[0]), 'FPR(%d)' % f(a[1])) + ';')
            return
        if op in ('madd.s', 'msub.s'):
            L.append('FPR(%d) = %s(ps2.fpu_acc, %s(FPR(%d), FPR(%d)));' % (f(a[0]), EE_ADD if op == 'madd.s' else EE_SUB,
                                                                EE_MUL, f(a[1]), f(a[2])))
            return
        cmp = {'c.lt.s': '<', 'c.le.s': '<=', 'c.eq.s': '=='}
        if op in cmp:
            L.append('fcc = FPR(%d) %s FPR(%d);' % (f(a[0]), cmp[op], f(a[1])))
            return
        if op == 'c.f.s':
            L.append('fcc = 0;')
            return
        # COP2 loads/stores with relocations are done here; the rest goes to the asm runtime
        if op in ('lqc2', 'sqc2'):
            vf = int(re.match(r'^\$vf(\d+)$', a[0]).group(1))
            if op == 'sqc2':
                L.append('%s_store128((%s) & ~(uintptr_t)15, &ps2.vf[%d]);' % (STORE, self.addr(a[1]), vf))
            elif vf:
                L.append('ps2.vf[%d] = M128(%s);' % (vf, self.addr(a[1])))
            return
        inline = self.cop2_move(op, a, text)
        if inline is not None:
            L.append(inline)
            return
        if op.startswith('v') or op.split('.')[0] in ('qmtc2', 'qmfc2', 'cfc2', 'ctc2') or op.startswith('p'):
            if any('%' in x for x in a):
                raise Fail('%s: relocation in %s' % (self.name, text))
            if WASM_EMIT:  # straight to its generated code (ps2asm_aot.py), no run-time lookup
                L.append('PS2X(ps2x_%s, "%s");' % (hashlib.sha1(text.encode()).hexdigest()[:16], text))
                L.append(reload_named(a))
            else:
                L.append('ps2_exec("%s");' % text)
            return
        raise Fail('%s: no translation for %r' % (self.name, text))


# ---- guest registers in locals (the wasm build, finding 090 SP15) ----

RELOAD_ALL = 'RC_RELOAD_ALL;'
RELOAD_SOME = re.compile(r'^RC_RELOAD\(([\d,]*)\);$')


def reload_named(args):
    """The marker after an instruction run through its generated code (PS2X): the GPRs its
    operands name are read back, as the code works on ps2. An FPU operand (none of the VU0/MMI
    instructions has one) reads everything back."""
    regs = set()
    for x in args:
        for r in re.findall(r'\$(\w+)', x):
            if re.fullmatch(r'f\d+', r):
                return RELOAD_ALL
            if r.isdigit():
                regs.add(int(r))
            elif r in GPR:
                regs.add(GPR[r])
    return 'RC_RELOAD(%s);' % ','.join(str(r) for r in sorted(regs - {0}))


REG_USE = re.compile(r'\b(GPR|UW|SW|FPR|FPRW)\((\d+)\)|\bADDR\((\d+),')
REG_SET = re.compile(r'\b(GPR|FPR|FPRW)\((\d+)\)\s*=(?!=)')
# ps2.gpr[n] written in place (lq, por, qmfc2, cfc2, a quadword result), or passed by address to
# anything but a store
DIRECT_GPR = re.compile(r'\bps2\.gpr\[(\d+)\](?:\.d\[\d\])?\s*=(?!=)|(?<!_store128\()(?<!, )&ps2\.gpr\[(\d+)\]')
ACC_SET = re.compile(r'\bps2\.fpu_acc\s*=(?!=)')


def cache_registers(lines):
    """The body with the GPRs (their low 64 bits), FPRs and the FPU accumulator it uses in locals:
    (declarations, lines). A write sets the local and ps2 (store-through), a read takes the
    local; the locals are read back from ps2 after a call (RC_RELOAD_ALL), after an instruction
    run through its generated code (RC_RELOAD(n...)) and after a line that writes ps2.gpr[n]
    itself (lq, por, qmfc2, cfc2, a quadword result). ps2 therefore always holds
    the registers, as before: interrupts and fiber switches (an INTC_STAT read, an MMIO store)
    save and restore them around the game code they run, the state trace reads them there, and
    ps2_exec sees them. The upper 64 bits of the GPRs, LO/HI and the VU registers stay in ps2."""
    gprs, fprs, acc = set(), set(), False
    for l in lines:
        for m in REG_USE.finditer(l):
            if m.group(3) is not None:
                gprs.add(int(m.group(3)))
            elif m.group(1) in ('FPR', 'FPRW'):
                fprs.add(int(m.group(2)))
            else:
                gprs.add(int(m.group(2)))
        if 'ps2.fpu_acc' in l:
            acc = True
        if 'ps2.fpr[' in l:
            raise Fail('ps2.fpr named directly: %s' % l)
    gprs.discard(0)

    def reload(regs, floats=(), with_acc=False):
        out = ['rg%d = ps2.gpr[%d].d[0];' % (n, n) for n in sorted(regs & gprs)]
        out += ['rf%d.w = *(int32_t *)&ps2.fpr[%d];' % (n, n) for n in sorted(set(floats) & fprs)]
        if with_acc and acc:
            out.append('rfacc = ps2.fpu_acc;')
        return out

    def assign(m):
        kind, n = m.group(1), int(m.group(2))
        if kind == 'GPR':
            if n == 0:
                raise Fail('write to $zero: %s' % m.string)
            return 'ps2.gpr[%d].d[0] = rg%d =' % (n, n)
        if kind == 'FPR':
            return 'ps2.fpr[%d] = rf%d.f =' % (n, n)
        return '*(int32_t *)&ps2.fpr[%d] = rf%d.w =' % (n, n)

    def use(m):
        if m.group(3) is not None:
            n = int(m.group(3))
            return 'RADDR(%s,' % ('rg%d' % n if n else '0')
        kind, n = m.group(1), int(m.group(2))
        if kind == 'FPR':
            return 'rf%d.f' % n
        if kind == 'FPRW':
            return 'rf%d.w' % n
        if n == 0:
            return {'GPR': '((uint64_t)0)', 'UW': '0u', 'SW': '0'}[kind]
        return {'GPR': 'rg%d', 'UW': '((uint32_t)rg%d)', 'SW': '((int32_t)rg%d)'}[kind] % n

    out = []
    for l in lines:
        if l == RELOAD_ALL:
            out += reload(gprs, fprs, True)
            continue
        m = RELOAD_SOME.match(l)
        if m:
            out += reload({int(x) for x in m.group(1).split(',') if x})
            continue
        direct = {int(x or y) for x, y in DIRECT_GPR.findall(l)}
        l = ACC_SET.sub('\x01', l)
        l = REG_SET.sub(lambda m: assign(m).replace('ps2.', '\x02'), l)
        l = REG_USE.sub(use, l)
        l = l.replace('ps2.fpu_acc', 'rfacc').replace('\x01', 'ps2.fpu_acc = rfacc =').replace('\x02', 'ps2.')
        out.append(l)
        out += reload(direct)
    decls = ['uint64_t rg%d = ps2.gpr[%d].d[0];' % (n, n) for n in sorted(gprs)]
    decls += ['rc_fpr rf%d; rf%d.w = *(int32_t *)&ps2.fpr[%d];' % (n, n, n) for n in sorted(fprs)]
    if acc:
        decls.append('float rfacc = ps2.fpu_acc;')
    return decls, out


def c_ident(name):
    return re.sub(r'[^\w]', '_', name)


# ---- signatures ----

_alias_types = {}


def canonical_scalar(t, path, cmd):
    """Resolve scalar typedef widths, including GCC mode(DI) and mode(TI) aliases.

    FunctionDecl argument spellings retain typedef names. Pointer/aggregate
    handling remains the existing EE boundary convention.
    """
    original = t
    for _ in range(8):
        plain = re.sub(r'\b(const|volatile)\b', '', t).strip()
        if not re.fullmatch(r'[A-Za-z_]\w*', plain) or plain in (
                'void', 'float', 'double', 'char', 'short', 'int', 'long',
                'unsigned', 'signed', 'bool', '_Bool'):
            return t
        key = (path, plain)
        if key not in _alias_types:
            probe = list(cmd)
            probe[-2] = plain  # -ast-dump-filter's argument
            dump = subprocess.run(probe, capture_output=True, text=True,
                                  encoding='utf-8', errors='replace').stdout
            pattern = (r'(?:TypedefDecl|TypeAliasDecl) .*?\b' + re.escape(plain)
                       + r" '([^']*)'(?::'([^']*)')?")
            match = re.search(pattern, dump)
            _alias_types[key] = (match.group(2) or match.group(1)) if match else plain
        replacement = _alias_types[key]
        if replacement == plain:
            return t
        if re.search(r'\b(struct|union|enum)\b', replacement):
            return original  # retain the existing aggregate policy
        t = re.sub(r'\b' + re.escape(plain) + r'\b', lambda _: replacement, t)
    raise Fail('recursive scalar typedef %r' % t)


def ast_type(name, path):
    """clang's type string of name's declaration in path, and its decl kind."""
    cpp = not path.endswith('.c')
    cmd = ['clang', '--target=' + TARGET, '-fsyntax-only', '-w', '-x', 'c++' if cpp else 'c',
           '-std=gnu++98' if cpp else '-std=gnu89', '-I', os.path.join(ROOT, 'port', 'include'),
           '-I', os.path.join(ROOT, 'include'), '-Xclang', '-ast-dump', '-Xclang', '-ast-dump-filter',
           '-Xclang', name, path]
    out = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    for line in out.splitlines():
        m = re.search(r'(FunctionDecl|CXXMethodDecl|CXXConstructorDecl|CXXDestructorDecl) .*? (?:used |referenced )?'
                      + re.escape(name) + r" '([^']*)'(?::'([^']*)')?", line)
        if m and ' implicit ' not in line:
            spelling = m.group(3) or m.group(2)
            ret, args, var = split_type(spelling)
            ret = canonical_scalar(ret, path, cmd)
            args = [canonical_scalar(t, path, cmd) for t in args]
            if var: args.append('...')
            params = ', '.join(args) if args else ('void' if spelling.endswith('(void)') else '')
            return ret + ' (' + params + ')', m.group(1), ' static ' in line
    return None


def split_type(t):
    """('ret', ['arg', ...], variadic) of a function type string 'ret (args)'."""
    depth, i = 0, len(t) - 1
    while i >= 0:
        depth += t[i] == ')'
        depth -= t[i] == '('
        if depth == 0:
            break
        i -= 1
    ret, params = t[:i].strip(), t[i + 1:-1]
    args = [x for x in split_args(params) if x] if params.strip() not in ('', 'void') else []
    variadic = bool(args) and args[-1] == '...'
    return ret, [x for x in args if x != '...'], variadic


def abi_class(t):
    """How a C type travels: void, float, double, i128, i64, or i32 (pointers/enums/small ints)."""
    t = re.sub(r'\b(const|volatile|struct|union|enum|signed)\b', '', t).strip()
    if t == 'void':
        return 'void'
    if t == 'float':
        return 'float'
    if t in ('double', 'long double'):
        return 'double'
    if re.search(r'[*&\[]', t):
        return 'i32'
    if t in ('__int128', 'unsigned __int128'):
        return 'i128'
    if re.match(r'^(unsigned )?long long( int)?$', t):
        return 'i64'
    if re.match(r'^(unsigned )?(char|short|int|long|_Bool|bool)( int)?$', t) or t == 'unsigned' or ' ' not in t:
        return 'i32' if t not in ('char', 'short', 'unsigned char', 'unsigned short', 'bool', '_Bool') else t
    raise Fail('type %r' % t)


SIGNATURE_VERSION = '5'  # refresh all reviewed FP, DI and TI scalar contracts

# Animation table D_004D0278: the assembly handlers consume $a0/$a1/$f12
# and return $f0. The table initializer declares them void with two arguments;
# that loses both time and result at the native/recompiled boundary (finding 021).
ANIMATION_HANDLERS = (
    '002627A0', '002628A0', '00262B48', '00262C20', '00262D30', '00262FE0',
    '00263270', '00263320', '00263438', '00263578', '00263860', '002638D8', '002639D0',
)
ABI_SIGNATURES = {
    'func_' + address: {'ret': 'float', 'args': ['i32', 'i32', 'float'],
                       'variadic': False, 'from': 'EE animation callback ABI'}
    for address in ANIMATION_HANDLERS
}

# Canonical native contracts for assembly-only projection and software-double
# helpers. Apply even to an existing cache; integer placeholders lose the x87
# result and (for double) half the payload on i386.
ABI_SIGNATURES.update({
    'func_00260E08': {'ret': 'float', 'args': ['i32'] * 4, 'variadic': False, 'from': 'rider_geometry.h'},
    'func_001FD300': {'ret': 'double', 'args': ['float'], 'variadic': False, 'from': 'soft_double.h'},
    'func_001FC810': {'ret': 'double', 'args': ['double', 'double'], 'variadic': False, 'from': 'soft_double.h'},
    'func_001FCDA0': {'ret': 'i32', 'args': ['double', 'double'], 'variadic': False, 'from': 'soft_double.h'},
    'func_001FCDF0': {'ret': 'double', 'args': ['i32'], 'variadic': False, 'from': 'soft_double.h'},
    'func_001FD010': {'ret': 'float', 'args': ['double'], 'variadic': False, 'from': 'soft_double.h'},
    'func_00248A80': {'ret': 'double', 'args': ['double'], 'variadic': False, 'from': 'soft_double.h'},
})


# Checked against effective native definitions by abi_check.py. These contracts
# also override a current cache: declaration guesses must not truncate a scalar
# or assign an FPR input to the integer argument bank.
SCALAR_CLASS = {'int': 'i32', 'int64': 'i64', 'int128': 'i128',
                'float': 'float', 'double': 'double', 'void': 'void'}
NATIVE_SCALARS = json.load(open(os.path.join(ROOT, 'port', 'native_scalar_contracts.json')))
from scalar_abi import contracts as scalar_contracts
FORMATTED_VARARGS = {n:c['format_arg'] for n,c in scalar_contracts().items() if 'format_arg' in c}
for _name, _contract in scalar_contracts().items():
    if not _name.startswith('__builtin_'):
        ABI_SIGNATURES[_name] = dict(ret=SCALAR_CLASS[_contract['ret']],
            args=[SCALAR_CLASS[c] for c in _contract['args']], variadic=_contract.get('variadic', False),
            **{'from': 'reviewed EE scalar ABI'})
for _name, _contract in NATIVE_SCALARS.items():
    if _contract['ret'] is not None:
        ABI_SIGNATURES[_name] = dict(ret=_contract.get('native_return', SCALAR_CLASS[_contract['ret']]),
            args=[SCALAR_CLASS[c] for c in _contract['args']], variadic=_contract['variadic'],
            **{'from': _contract['from']})


def transparent_target(insns):
    """A compiler callback thunk forwards every argument and result unchanged.
    Only accept the complete save/call/restore pattern, including delay slots.
    """
    ops = [(op, args) for _, op, args in insns]
    while ops and ops[-1] == ('nop', []): ops.pop()
    if len(ops) != 7 or ops[2][0] != 'jal' or len(ops[2][1]) != 1: return None
    expected = [('addiu', ['$sp', '$sp', '-0x10']), ('sd', ['$ra', '0x0($sp)']),
                ops[2], ('nop', []), ('ld', ['$ra', '0x0($sp)']),
                ('jr', ['$ra']), ('addiu', ['$sp', '$sp', '0x10'])]
    return ops[2][1][0] if ops == expected else None


def forwarded_target(name):
    path = os.path.join(ASM, name + '.s')
    if not os.path.exists(path): return None
    insns = []
    for line in open(path, encoding='utf-8'):
        m = INSN.match(line)
        if not m: continue
        # Most functions are not thunks. Do not decode their entire bodies
        # merely to discover a seven-instruction forwarding wrapper.
        if len(insns) >= 7:
            if m[3] != 'nop': return None
            continue
        insns.append((int(m[1], 16), m[3], split_args(m[4]) if m[4] else []))
    return transparent_target(insns)


CALLED = re.compile(r'\b(\w+)\s*\(')  # what signatures() searched for name by name: r'\bNAME\s*\('


class SignatureMemo:
    """Each name's signature from the declarations (signatures()), remembered across builds and
    checkouts in DHD_DEVCACHES/dhd-modulecache/recomp-signatures.json (finding 151). The key is
    everything the answer is made from: the name, the text of every source file that names it,
    the headers those files can include (include/ and port/include: src/ has none, and no source
    includes another), clang's version, the target and this file. Without it every core build ran
    clang's AST dump on each name's files again: thousands of processes, 1 to 2 minutes."""
    base = None

    def __init__(self, files):
        devcaches = os.environ.get('DHD_DEVCACHES') or os.path.join(os.path.expanduser('~'), 'DevCaches')
        self.path = os.path.join(devcaches, 'dhd-modulecache', 'recomp-signatures.json')
        self.files = files
        self.sha = {}
        if not SignatureMemo.base:  # once a process: the headers do not change during a build
            h = hashlib.sha256(('%s\0%s\0' % (SIGNATURE_VERSION, TARGET)).encode())
            h.update(open(os.path.abspath(__file__), 'rb').read())
            h.update(subprocess.run(['clang', '--version'], capture_output=True).stdout)
            for d in (os.path.join(ROOT, 'include'), os.path.join(ROOT, 'port', 'include')):
                for p in sorted(glob.glob(os.path.join(d, '**', '*'), recursive=True)):
                    if os.path.isfile(p):
                        h.update(os.path.relpath(p, ROOT).replace(os.sep, '/').encode() + b'\0' + open(p, 'rb').read())
            SignatureMemo.base = h.digest()
        self.base = SignatureMemo.base
        try:
            self.known = json.load(open(self.path))
        except (OSError, ValueError):
            self.known = {}
        self.new = {}

    def key(self, name, paths):
        h = hashlib.sha256(self.base + name.encode())
        for p in sorted(paths):
            if p not in self.sha:
                self.sha[p] = hashlib.sha256(self.files[p].encode('utf-8', 'surrogateescape')).digest()
            h.update(os.path.relpath(p, SRC).replace(os.sep, '/').encode() + b'\0' + self.sha[p])
        return h.hexdigest()

    def get(self, name, paths):
        sig = self.known.get(self.key(name, paths))
        return dict(sig) if sig is not None else None

    def put(self, name, paths, sig):
        self.new[self.key(name, paths)] = dict(sig)

    def save(self):
        if not self.new:
            return
        try:
            try:
                known = json.load(open(self.path))
            except (OSError, ValueError):
                known = {}
            if len(known) > 200000:  # entries of sources long gone: start again
                known = {}
            known.update(self.new)
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            tmp = '%s.%d.tmp' % (self.path, os.getpid())
            with open(tmp, 'w') as f:
                json.dump(known, f)
            os.replace(tmp, self.path)
        except OSError:
            pass


def signatures(names):
    """{name: {'ret': class, 'args': [class...], 'variadic': bool}}; default 8 int args."""
    cache_path = os.path.join(OUT, 'signatures.json')
    version_path = os.path.join(OUT, 'signatures-version.txt')
    current = os.path.exists(version_path) and open(version_path).read().strip() == SIGNATURE_VERSION
    cache = json.load(open(cache_path)) if current and os.path.exists(cache_path) else {}
    # Apply on every invocation, including an existing declaration cache.
    cache.update(ABI_SIGNATURES)
    from float_abi import assembly_returns
    reviewed_returns = assembly_returns()
    names = list(names)
    from scalar_abi import verify_assembly_inputs
    verify_assembly_inputs(set(names)-set(NATIVE_SCALARS))
    forwarded = {}
    for n in names:
        if n in ABI_SIGNATURES: continue
        target = forwarded_target(n)
        if target and target != n:
            forwarded[n] = target
            if target not in names: names.append(target)
    todo = [n for n in names if n not in cache]
    if todo:
        files = {p: open(p, encoding='utf-8', errors='replace').read()
                 for p in glob.glob(os.path.join(SRC, '**', '*.c*'), recursive=True)}
        named = {}  # name -> the files that name it before a '(' (a declaration, a definition or a call)
        for p, t in files.items():
            for ident in set(CALLED.findall(t)):
                named.setdefault(ident, []).append(p)
        memo = SignatureMemo(files)
        for n in todo:
            known = memo.get(n, named.get(n, ()))
            if known is not None:
                cache[n] = known
                continue
            defin = re.compile(r'\b%s\s*\([^;{}]*\)\s*\{' % re.escape(n))
            # a variadic declaration first (sprintf declared with 4 int parameters somewhere still
            # takes more), then a definition
            varargs = re.compile(r'\b%s\s*\([^;{}()]*\.\.\.\s*\)' % re.escape(n))
            cands = sorted((0 if varargs.search(files[p]) else 1, 0 if defin.search(files[p]) else 1, p)
                           for p in named.get(n, ()))
            sig = unprototyped = None
            for _, _, p in cands[:6]:
                r = ast_type(n, p)
                if not r:
                    continue
                ret, args, var = split_type(r[0])
                if p.endswith('.c') and not args and r[0].endswith('()'):
                    unprototyped = unprototyped or abi_class(ret)  # f() in C: arguments unknown
                    continue
                this = ['i32'] if r[1] != 'FunctionDecl' and not r[2] else []
                found = {'ret': abi_class(ret), 'args': this + [abi_class(x) for x in args], 'variadic': var,
                         'from': os.path.relpath(p, SRC).replace(os.sep, '/')}
                if sig and sig['variadic'] != var:
                    break
                if not sig or sig['ret'] == 'void':  # a declaration that keeps the result, if any
                    sig = found
                if sig['ret'] != 'void':
                    break
            cache[n] = sig or {'ret': unprototyped or 'i32', 'args': ['i32'] * 8, 'variadic': False, 'from': None}
            memo.put(n, named.get(n, ()), cache[n])
        memo.save()
    for n in names:
        if n in NATIVE_SCALARS and NATIVE_SCALARS[n]['ret'] is None:
            cache[n]['args'] = [SCALAR_CLASS[c] for c in NATIVE_SCALARS[n]['args']]
        if n in reviewed_returns:
            cache[n]['ret'] = reviewed_returns[n]
    # A void(void) source declaration describes how the address is taken,
    # not what the callback consumes. Refresh stale cached thunk signatures.
    def inherit(n, seen):
        if n in seen: return None
        if n not in forwarded: return cache[n] if cache[n].get('from') else None
        sig = inherit(forwarded[n], seen | {n})
        if sig: cache[n] = dict(sig, **{'from': 'forwarding thunk -> ' + forwarded[n]})
        return sig
    for n in forwarded: inherit(n, set())
    os.makedirs(OUT, exist_ok=True)
    json.dump(cache, open(cache_path, 'w'), indent=1, sort_keys=True)
    with open(version_path, 'w') as f: f.write(SIGNATURE_VERSION + '\n')
    return cache


CTYPE = {'i32': 'uint32_t', 'i64': 'uint64_t', 'i128': 'ps2_u128', 'float': 'float', 'double': 'double', 'void': 'void',
         'char': 'int8_t', 'unsigned char': 'uint8_t', 'short': 'int16_t', 'unsigned short': 'uint16_t',
         'bool': 'uint8_t', '_Bool': 'uint8_t'}


def prototype(name, sig, params=False):
    args = sig['args']
    if not args:
        a = 'void'
    elif params:
        a = ', '.join('%s p%d' % (CTYPE[c], i) for i, c in enumerate(args))
    else:
        a = ', '.join(CTYPE[c] for c in args)
    if sig['variadic']:
        a += ', ...'
    return '%s %s(%s)' % (CTYPE[sig['ret']], name, a)


def arg_places(sig):
    """Where each argument of sig travels on the EE: ('gpr', n), ('fpr', n) or ('stack', offset).
    Integer arguments use a whole GPR each, including quadwords. Past $t3 they use 8-byte
    stack slots, or aligned 16-byte slots for quadwords, starting at 0($sp). The EE has no
    double FPU: a double is software float, passed like a 64-bit integer (ps2_d2u/ps2_u2d)."""
    ints, floats, slot, out = iter(INT_ARGS), iter(FLOAT_ARGS), 0, []
    for c in sig['args']:
        r = next(floats if c == 'float' else ints, None)
        if r is None:
            if c == 'i128': slot = (slot + 15) & ~15
            out.append(('stack', slot))
            slot += 16 if c == 'i128' else 8
        else:
            out.append(('fpr' if c == 'float' else 'gpr', r))
    return out


WIDE = {'i64': '%s', 'double': 'ps2_u2d(%s)'}  # 64-bit arguments: from the register bits
TO_BITS = {'i64': '%s', 'double': 'ps2_d2u(%s)'}


def marshal(sig):
    """Register sources of a native call's arguments, and the statement storing its result."""
    srcs = []
    for c, (where, n) in zip(sig['args'], arg_places(sig)):
        if c == 'i128':
            quad = 'ps2.gpr[%d]' % n if where == 'gpr' else 'M128(ADDR(29, %d))' % n
            srcs.append('ps2_quad_scalar(%s)' % quad)
        elif where == 'fpr':
            srcs.append('FPR(%d)' % n)
        elif where == 'stack' and c == 'float':
            srcs.append('MF32(ADDR(29, %d))' % n)
        elif where == 'stack':
            srcs.append(WIDE.get(c, '%s') % ('%s(ADDR(29, %d))' % ('M64' if c in WIDE else 'MU32', n)))
        else:
            srcs.append(WIDE[c] % ('GPR(%d)' % n) if c in WIDE else 'UW(%d)' % n)
    ret = {'void': '%s;', 'float': 'FPR(0) = %s;', 'i64': 'GPR(2) = %s;',
           'i128': 'ps2.gpr[2] = ps2_scalar_quad(%s);',
           'double': 'GPR(2) = ps2_d2u(%s);'}.get(sig['ret'], 'GPR(2) = S32(%s);')
    return srcs, ret


# ---- control flow ----

def translate(name, recompiled, sigs):
    insns, labels = parse(name)
    fn = Func(name, recompiled, sigs)
    L = fn.lines
    by_addr = {a: i for i, (a, _, _) in enumerate(insns)}
    start = insns[0][0]

    def target(s):
        m = re.match(r'^\.L([0-9A-F]{8})$', s)
        if m and int(m.group(1), 16) in by_addr:
            return 'L_' + m.group(1)
        if s == name:
            return 'L_%08X' % start
        raise Fail('%s: branch to %s' % (name, s))

    def text(op, a):
        return (op + ' ' + ', '.join(a)).strip()

    def call(callee):
        if callee in recompiled:
            fn.callees.add(callee)
            L.append('rc_%s();' % callee)
            if WASM_EMIT:
                L.append(RELOAD_ALL)
            return
        sig = sigs[callee]
        fn.callees.add(callee)
        srcs, ret = marshal(sig)
        if sig['variadic'] or (sig['from'] is None and not WASM_EMIT):
            srcs = srcs + ['UW(%d)' % r for r in INT_ARGS[len(srcs):]]  # pass every argument register
        L.append(ret % ('%s(%s)' % (callee, ', '.join(srcs))))
        if WASM_EMIT:
            L.append(RELOAD_ALL)

    labels.add(start)
    i = 0
    while i < len(insns):
        addr, op, a = insns[i]
        if addr in labels:
            L.append('L_%08X:;' % addr)
        # Fractional particle decay/update. Keep the original generated C at
        # 60; at high rates only, replace the integer exponent/integration
        # coefficients, not the surrounding type-specific VU operations.
        if name == 'func_0022B708':
            if addr == 0x0022B730:
                L.append('if (port_delta_clock()) FPR(20)=port_step();')
            if addr == 0x0022B834:
                L.append('if (port_delta_clock()) {')
                # The branch's delay slot stores the signed size slope. The
                # high-rate jump bypasses that slot, so materialize it first.
                L.append('MF32(ADDR(29,0x2C))=FPR(0);')
                for offset, function in [(0x14,'port_particle_sum'), (0x24,'port_particle_sum'),
                                         (0x10,'port_decay'), (0x20,'port_decay')]:
                    L.append('MF32(ADDR(29,0x%X))=%s(MF32(ADDR(29,0x%X)));' % (offset,function,offset))
                # Size/envelope slopes are signed additive rates, not decay
                # bases; fractional powers of a negative slope are invalid.
                for offset in (0x1c,0x2c):
                    L.append('MF32(ADDR(29,0x%X))*=port_step();' % offset)
                L.append('goto L_0022B898; }')
            if addr == 0x0022B938:
                L.append('if (port_delta_clock()) { GPR(2)=S32(UW(3)-port_whole_step()); goto L_fps_delay; }')
            if addr == 0x0022B93C:
                L.append('L_fps_delay:;')
        # This asm-only blend path shares the animation clock too.
        if name == 'func_00204F38' and addr == 0x00204F84:
            L.append('if (port_animation_scale()>1) FPR(20)=port_animation_fields(FPR(20));')
        if name == 'func_0023E288' and addr == 0x0023E3CC:
            L.append('if (port_delta_clock()) FPR(20)=port_step();')
        if name == 'func_0023E288' and addr == 0x0023E680:
            L.append('if (port_delta_clock()) FPR(0)=port_decay(FPR(0));')
        if name == 'func_0022B258' and addr == 0x0022B2B4:
            L.append('if (port_delta_clock()) { FPR(2)=port_step(); GPR(22)=1; }')
        if name == 'func_0022B258' and addr == 0x0022B29C:
            L.append('if (port_delta_clock()) GPR(3)=S32(UW(3)+1-port_whole_step());')
        if name == 'func_0022B258' and addr == 0x0022B350:
            L.append('if (port_delta_clock()) fcc=port_particle_dead(FPR(0),MF32(ADDR(9,12)));')
        if name == 'func_0022B258' and addr == 0x0022B3D4:
            # These particles store normalized lifetime in position.w.
            # Scaling gravity/phase alone leaves the full velocity (including
            # age) added once per frame, shortening the pickup by N/60.
            L.append('if (port_delta_clock()) { ps2_q128 saved=ps2.vf[15]; float h=port_step();')
            L.append('port_particle_rotation(&ps2.vf[16].f[0],&ps2.vf[17].f[0]);')
            L.append('for (int k=0;k<4;k++) ps2.vf[15].f[k]=ps2_float_mul(saved.f[k],h);')
            fn.insn(op,a,text(op,a))
            L.append('ps2.vf[15]=saved; } else {')
            fn.insn(op,a,text(op,a))
            L.append('}')
            i+=1
            continue
        # sceGsSyncV keeps only CSR.FIELD (bit 13) of this load, which the EE's CSR page always
        # holds exactly, even while a deferred vsync's CSR is provisional (port/hle/kernel.c,
        # finding 090 HO1). The page changes only on the EE, so a wait for the drawing cannot change
        # what it reads: read it without the graphics fence, which would wait for the whole field.
        if name == 'func_00243CE8' and addr == 0x00243D2C:
            if (op, a) != ('ld', ['$v0', '0x0($v1)']):
                raise Fail('%s %08X: expected the CSR load, found %s %s' % (name, addr, op, a))
            L.append('GPR(2) = *(volatile uint64_t *)(uintptr_t)UW(3);')
            i += 1
            continue
        control = op in BRANCH or op in LIKELY or op in ('j', 'jal', 'jr', 'jalr')
        if not control:
            fn.insn(op, a, text(op, a))
            i += 1
            continue
        slot = insns[i + 1] if i + 1 < len(insns) else (addr + 4, 'nop', [])
        slot_labelled = slot[0] in labels

        def delay():
            fn.insn(slot[1], slot[2], text(slot[1], slot[2]))
        if op in BRANCH or op in LIKELY:
            c = (BRANCH.get(op) if op in BRANCH else LIKELY[op])
            regs = [x for x in a[:-1]]
            if c is None:
                cond = '1'
            elif c in ('fcc', '!fcc', '1', '0'):
                cond = c
            elif c in ('==', '!='):
                cond = 'GPR(%d) %s GPR(%d)' % (fn.gpr(regs[0]), c, fn.gpr(regs[1]))
            else:
                cond = '(int64_t)GPR(%d) %s' % (fn.gpr(regs[0]), c.replace('0', ' 0'))
            dest = target(a[-1])
            if op in LIKELY:
                L.append('if (%s) {' % cond)
                delay()
                L.append('goto %s; }' % dest)
            else:
                L.append('{ int c = %s;' % cond)
                delay()
                L.append('if (c) goto %s; }' % dest)
        elif op == 'j':
            delay()
            if re.match(r'^\.L', a[0]) or a[0] == name:
                L.append('goto %s;' % target(a[0]))
            else:
                call(a[0])
                L.append('return;')
        elif op == 'jal':
            delay()
            call(a[0])
        elif op == 'jalr':
            delay()
            reg = fn.gpr(a[-1])
            L.append('if (!ps2_recomp_scalar_call(UW(%d)))' % reg)
            L.append('GPR(2) = S32(((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, '
                     'uint32_t, uint32_t))(uintptr_t)UW(%d))(UW(4), UW(5), UW(6), UW(7), UW(8), UW(9), UW(10), '
                     'UW(11)));' % reg)
            if WASM_EMIT:
                L.append(RELOAD_ALL)
        elif op == 'jr':
            reg = fn.gpr(a[0])
            if reg == 31:
                delay()
                L.append('return;')
            else:
                L.append('{ uint32_t t = UW(%d);' % reg)
                delay()
                L.append('switch (t) {')
                for la in sorted(labels):
                    L.append('case 0x%08Xu: goto L_%08X;' % (la, la))
                L.append('default: ps2_recomp_bad_jump("%s", t); } }' % name)
        if slot_labelled:  # something else jumps straight to the delay slot instruction
            L.append('goto L_after_%08X;' % slot[0])
            L.append('L_%08X:;' % slot[0])
            fn.insn(slot[1], slot[2], text(slot[1], slot[2]))
            L.append('L_after_%08X:;' % slot[0])
        i += 2
    return fn


def write(name, recompiled, sigs):
    fn = translate(name, recompiled, sigs)
    out = ['/* %s recompiled from its MIPS code by port/tools/recomp.py: generated, do not edit. */' % name,
           '#include <stdarg.h>', '#include "recomp.h"', '#include "quad_abi.h"', '#include "vararg_abi.h"']
    if any('port_' in line for line in fn.lines):  # the frame-rate hooks in translate()
        out.append('#include "fps.h"')
    out.append('')
    for s in sorted(fn.symbols):
        out.append('extern char A_%s[] __asm__("%s%s");' % (c_ident(s), P, s))
    for c in sorted(fn.callees):
        if c in recompiled:
            out.append('void rc_%s(void);' % c)
        else:
            out.append('extern %s;' % prototype(c, sigs[c]))
    sig = sigs[name]
    decls, body = cache_registers(fn.lines) if WASM_EMIT else ([], fn.lines)
    out += ['', 'void rc_%s(void)' % name, '{', '    int fcc = 0;', '    (void)fcc;']
    out += ['    ' + l for l in decls]
    out += ['    ' + l if not re.match(r'^L_\w+:;$', l) else l for l in body]
    out += ['    return;', '}', '']
    # the wrapper native callers see
    places, body = arg_places(sig), []
    stack_end = max([n + (16 if c == 'i128' else 8) for c, (w, n) in zip(sig['args'], places)
                     if w == 'stack'], default=0)
    stack = (stack_end + 15) & ~15
    if sig['variadic'] and sig['args']:
        stack = max(stack, 1024 if name in FORMATTED_VARARGS else 64)  # EE vararg slots
    if stack:
        body.append('    GPR(29) -= %d;' % stack)
    for k, (c, (where, n)) in enumerate(zip(sig['args'], places)):
        if c == 'i128':
            quad = 'ps2.gpr[%d]' % n if where == 'gpr' else 'M128(ADDR(29, %d))' % n
            body.append('    %s = ps2_scalar_quad(p%d);' % (quad, k))
        elif where == 'fpr':
            body.append('    FPR(%d) = p%d;' % (n, k))
        elif where == 'stack' and c == 'float':
            body.append('    MF32(ADDR(29, %d)) = p%d;' % (n,k))
        elif where == 'stack':
            body.append('    %s(ADDR(29, %d)) = %s;' % ('M64' if c in TO_BITS else 'MU32', n,
                                                      TO_BITS.get(c, '%s') % ('p%d' % k)))
        else:
            body.append('    GPR(%d) = %s;' % (n, TO_BITS.get(c, 'S32(%s)') % ('p%d' % k)))
    if sig['variadic'] and sig['args']:
        # the variable arguments: words from the host stack into the EE's next argument places
        # (integers and pointers; a double would need the format to tell)
        rest = [('gpr', r) for r in INT_ARGS if ('gpr', r) not in places]
        rest += [('stack', n) for n in range(stack_end, 64, 8)]
        body += ['    va_list ap;', '    va_start(ap, p%d);' % (len(sig['args']) - 1)]
        if name in FORMATTED_VARARGS:
            first=next((r for r in INT_ARGS if ('gpr',r) not in places),12)
            body.append('    ps2_format_varargs((const char *)(uintptr_t)p%d, &ap, %d, %d);' %
                        (FORMATTED_VARARGS[name],first,stack_end))
        else:
            for where, n in rest:
                body.append('    GPR(%d) = S32(va_arg(ap, uint32_t));' % n if where == 'gpr' else
                            '    MU32(ADDR(29, %d)) = va_arg(ap, uint32_t);' % n)
        body.append('    va_end(ap);')
    body.append('    rc_%s();' % name)
    if stack:
        body.append('    GPR(29) += %d;' % stack)
    ret = {'void': '', 'float': '    return FPR(0);', 'i64': '    return GPR(2);',
           'i128': '    return ps2_quad_scalar(ps2.gpr[2]);',
           'double': '    return ps2_u2d(GPR(2));'}.get(
        sig['ret'], '    return (%s)UW(2);' % CTYPE[sig['ret']])
    out += [prototype(name, dict(sig, variadic=sig['variadic'] and bool(sig['args'])), params=True), '{',
            '    ps2_recomp_enter_inline();'] + body + \
           ([ret] if ret else []) + ['}', '']
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + '.c')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    return path



def write_scalar_dispatch(sigs=None, path=None):
    """Known scalar callbacks must not pass through the generic eight-int cast.
    Use the same complete GPR/FPR/stack marshalling as a direct native call.
    Address aliases avoid conflicting C declarations or C++ name mangling.
    """
    sigs=ABI_SIGNATURES if sigs is None else sigs
    selected={n:s for n,s in sigs.items() if not s['variadic'] and
              ({s['ret']}|set(s['args'])) & {'float','double','i64','i128'}}
    lines=['/* Generated scalar callback dispatcher; do not edit. */',
           '#include "recomp.h"','#include "quad_abi.h"']
    for name in sorted(selected):
        lines.append('extern char scalar_%s[] __asm__("%s%s");' % (c_ident(name),P,name))
    lines += ['int ps2_recomp_scalar_call(uintptr_t target) {']
    for name,sig in sorted(selected.items()):
        args,store=marshal(sig)
        ctype='%s (*)(%s)' % (CTYPE[sig['ret']],', '.join(CTYPE[c] for c in sig['args']) or 'void')
        call='((%s)target)(%s)' % (ctype,', '.join(args))
        lines += ['    if (target==(uintptr_t)scalar_%s) {' % c_ident(name),
                  '        '+store % call,'        return 1;','    }']
    lines += ['    return 0;','}']
    path=path or os.path.join(OUT,'scalar_dispatch.c')
    with open(path,'w') as f:f.write('\n'.join(lines)+'\n')
    return path


def recompile(names):
    """Writes build/port/recomp/<name>.c for each name; returns (paths, failures)."""
    recompiled = set(names)
    callees, failures = set(), {}
    for n in names:
        try:
            insns = parse(n)[0]
        except Fail as e:
            failures[n] = str(e)
            continue
        for _, op, a in insns:
            if op in ('jal', 'j') and a and not a[0].startswith('.L') and a[0] not in recompiled:
                callees.add(a[0])
    sigs = signatures(sorted(callees | recompiled))
    for n in names:  # one that fails is called through its C name (written natively) instead
        if n not in failures:
            try:
                translate(n, recompiled, sigs)
            except Fail as e:
                failures[n] = str(e)
    recompiled -= set(failures)
    paths=[write(n, recompiled, sigs) for n in names if n in recompiled]
    if paths:paths.append(write_scalar_dispatch())
    return paths, failures


if __name__ == '__main__':
    ps, fails = recompile(sys.argv[1:])
    print('%d written, %d failed' % (len(ps), len(fails)))
    for n, e in fails.items():
        print('  %s: %s' % (n, e))
