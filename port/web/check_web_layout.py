#!/usr/bin/env python3
"""Layout check for the browser build's wasm modules: the game's, which share one linear memory, and
the settings and disc modules, which must keep their own.

  dhd_core.wasm                 the game reactor; defines the memory (exported as "memory")
  graphics.wasm                 GS/VIF/VU1 module; imports env.memory
  renderer/dhd_render_bg.wasm   the Rust renderer; imports only from wbg, including its memory
                                (wbg.memory)
  vu.wasm                       a threaded build's VIF1/VU1 module (its VU worker, finding 090
                                SP4); imports env.memory; only in a threaded build, and there
                                required
  dhd_settings.wasm             dhd.ini's crate for the page and the game worker (port/web/www/
                                settings.js, finding 102): its own memory, apart from the game's
  dhd_disc.wasm                 the online disc check for the game worker (port/web/www/disc.js,
                                finding 137): its own memory; it imports only the worker's read

All of them must be there (the JS renderer and its renderer-less builds are gone: cleanup CL5,
finding 088; --require-renderer is accepted for old callers).

Usage:
  python port/web/check_web_layout.py BUILD_WEB_DIR
  python port/web/check_web_layout.py --selftest
  python port/web/check_web_layout.py --write-fixtures DIR

Exit codes: 0 layout holds, 1 a rule is violated (each violation prints its rule id),
2 a module is missing, unreadable or malformed. Fixtures are synthetic; no game data is read.

Rules:
  M1  memory ownership: core defines and exports one memory; graphics imports env.memory; renderer imports wbg.memory
  M2  memory flags: no memory64 bit; no shared (threads) bit, except in a threaded build (gqueue.wasm
      present: build.py --threads, finding 087 BS2), where every memory must carry it
  M3  import limits: import min <= core's initial pages; an import max must admit core's max
  G1  mutable globals take constant initialisers
  D1  data: active segments on memory 0 with constant offsets, inside core's initial memory
      (the stack pointer too); no passive segments
  S   the stack pointer is an exported mutable i32 global
  R1  renderer imports come from module wbg only (env is refused, memory included)
  R2  renderer data, bss window and stack lie in RUST_WINDOW and not in LOW_RESERVED
  R3  renderer is not stack-first: at least RUST_MIN_STACK_BYTES stay free above __data_end
  R4  renderer exports __data_end and __heap_base; data ends <= __data_end <= __heap_base <= window top
  R5  renderer has no start function (its init is explicit, from the worker)
  D2  reserved ranges of different modules never overlap (data, stacks, renderer bss window)
  P1  the settings module imports nothing, defines one memory, exported as "memory", neither shared
      nor memory64, and has no start function: it never touches the game's memory
  P2  the disc module likewise, except that it imports one function, env.dhd_disc_read (the
      worker's read of the disc image), and nothing else

RESERVATION TABLE (the rules above read these values)
"""
import argparse
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIB = 1 << 20
PAGE = 1 << 16

LOW_RESERVED = (0, MIB)                 # no renderer data or stack here (graphics' stack lives here today)
# Renderer data, stack, bss gap and heap all lie in this window. The bottom is the
# --initial-memory=0x50000000 cap from plan 3.3; the top sits below the core's static data,
# which starts at 0x70100000 in the current build.
RUST_WINDOW = (0x50000000, 0x70000000)
CORE_STACK_BYTES = MIB                  # link flag -z stack-size=1048576 (build_graphics.py)
GRAPHICS_STACK_BYTES = MIB              # build_graphics.py -Wl,-z,stack-size=1048576
VU_STACK_BYTES = MIB                    # build_graphics.py build_vu: -Wl,-z,stack-size=1048576
RUST_MIN_STACK_BYTES = MIB              # the Rust stack size is not readable from the module: at least this much must stay free above __data_end

# role, file under the build dir, module that must carry the shared memory import (None = defines it), stack bytes
ROLES = (
    ('core', 'dhd_core.wasm', None, CORE_STACK_BYTES),
    ('graphics', 'graphics.wasm', 'env', GRAPHICS_STACK_BYTES),
    ('renderer', 'renderer/dhd_render_bg.wasm', 'wbg', RUST_MIN_STACK_BYTES),
    ('vu', 'vu.wasm', 'env', VU_STACK_BYTES),
)
THREADED_ONLY = {'vu'}   # roles a threaded build (gqueue.wasm present) has, and only it
STACK_BYTES = {role: size for role, _, _, size in ROLES}
MEMORY_MODULE = {role: mod for role, _, mod, _ in ROLES}
FILES = {role: name for role, name, _, _ in ROLES}
SETTINGS_FILE = 'dhd_settings.wasm'   # outside ROLES: not on the shared memory (rule P1)
DISC_FILE = 'dhd_disc.wasm'           # outside ROLES too (rule P2)
DISC_IMPORTS = [('env', 'dhd_disc_read', 'func')]


class WasmError(Exception):
    pass


# ---- minimal reader for the sections the checks need ----------------------------------------

class Reader:
    def __init__(self, data, pos, end):
        self.b, self.i, self.end = data, pos, end

    def byte(self):
        if self.i >= self.end:
            raise WasmError('unexpected end of section')
        v = self.b[self.i]
        self.i += 1
        return v

    def uleb(self):
        n = shift = 0
        while True:
            x = self.byte()
            n |= (x & 0x7F) << shift
            shift += 7
            if not x & 0x80:
                return n
            if shift > 63:
                raise WasmError('LEB128 too long')

    def sleb(self):
        n = shift = 0
        while True:
            x = self.byte()
            n |= (x & 0x7F) << shift
            shift += 7
            if not x & 0x80:
                if x & 0x40:
                    n -= 1 << shift
                return n
            if shift > 63:
                raise WasmError('LEB128 too long')

    def take(self, n):
        if self.i + n > self.end:
            raise WasmError('section overruns its size')
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def name(self):
        return self.take(self.uleb()).decode('utf-8')

    def limits(self):
        flags = self.byte()
        mn = self.uleb()
        mx = self.uleb() if flags & 1 else None
        return flags, mn, mx

    def const_expr(self):
        op = self.byte()
        if op == 0x41:
            v = ('i32', self.sleb())
        elif op == 0x42:
            v = ('i64', self.sleb())
        elif op == 0x43:
            self.take(4)
            v = ('f32', None)
        elif op == 0x44:
            self.take(8)
            v = ('f64', None)
        elif op == 0x23:
            v = ('global', self.uleb())
        elif op == 0xD0:
            self.byte()
            v = ('ref', None)
        elif op == 0xD2:
            self.uleb()
            v = ('ref', None)
        else:
            raise WasmError('unsupported constant expression opcode 0x%02x' % op)
        if self.byte() != 0x0B:
            raise WasmError('constant expression not terminated')
        return v


class Module:
    def __init__(self, role, path):
        self.role, self.path = role, path
        self.imports = []        # (module, field, kind)
        self.mem_imports = []    # (module, field, (flags, min, max))
        self.memories = []       # defined: (flags, min, max)
        self.global_imports = 0
        self.globals = []        # defined: (valtype, mut, init)
        self.exports = {}        # name -> (kind, index)
        self.data = []           # active: (offset_expr, length, memory index)
        self.passive = 0
        self.start = None


def parse(role, path):
    data = Path(path).read_bytes()
    if data[:8] != b'\x00asm\x01\x00\x00\x00':
        raise WasmError('%s: not a wasm 1.0 module' % path)
    m = Module(role, path)
    pos = 8
    while pos < len(data):
        head = Reader(data, pos, len(data))
        sid = head.byte()
        size = head.uleb()
        start = head.i
        end = start + size
        if end > len(data):
            raise WasmError('%s: section %d overruns the file' % (path, sid))
        s = Reader(data, start, end)
        pos = end
        if sid == 0:
            continue
        if sid == 2:
            for _ in range(s.uleb()):
                mod, field, kind = s.name(), s.name(), s.byte()
                if kind == 0:
                    s.uleb()
                    m.imports.append((mod, field, 'func'))
                elif kind == 1:
                    s.byte()
                    s.limits()
                    m.imports.append((mod, field, 'table'))
                elif kind == 2:
                    lim = s.limits()
                    m.imports.append((mod, field, 'memory'))
                    m.mem_imports.append((mod, field, lim))
                elif kind == 3:
                    s.byte()
                    s.byte()
                    m.imports.append((mod, field, 'global'))
                    m.global_imports += 1
                elif kind == 4:
                    s.byte()
                    s.uleb()
                    m.imports.append((mod, field, 'tag'))
                else:
                    raise WasmError('%s: import kind %d' % (path, kind))
        elif sid == 5:
            m.memories = [s.limits() for _ in range(s.uleb())]
        elif sid == 6:
            for _ in range(s.uleb()):
                vt, mut = s.byte(), s.byte()
                m.globals.append((vt, mut, s.const_expr()))
        elif sid == 7:
            for _ in range(s.uleb()):
                name = s.name()
                kind = s.byte()
                m.exports[name] = (kind, s.uleb())
        elif sid == 8:
            m.start = s.uleb()
        elif sid == 11:
            for _ in range(s.uleb()):
                flags = s.uleb()
                if flags == 1:
                    m.passive += 1
                    s.take(s.uleb())
                    continue
                if flags == 0:
                    mem = 0
                elif flags == 2:
                    mem = s.uleb()
                else:
                    raise WasmError('%s: data flags %d' % (path, flags))
                off = s.const_expr()
                n = s.uleb()
                s.take(n)
                m.data.append((off, n, mem))
    return m


# ---- checks ---------------------------------------------------------------------------------

def _export_const(m, name, rule, fail):
    """Value of an exported immutable/mutable i32 global with a constant initialiser, else None."""
    if name not in m.exports or m.exports[name][0] != 3:
        fail(rule, m.role, 'missing exported global %s' % name)
        return None
    k = m.exports[name][1] - m.global_imports
    if k < 0:
        fail(rule, m.role, '%s is an imported global; it needs a constant initialiser' % name)
        return None
    vt, mut, init = m.globals[k]
    if vt != 0x7F or init[0] != 'i32':
        fail(rule, m.role, '%s is not an i32 constant' % name)
        return None
    return init[1] & 0xFFFFFFFF


def _segment_range(off, n):
    if off[0] != 'i32':
        return None
    lo = off[1] & 0xFFFFFFFF
    return lo, lo + n


def check(mods, threads=False):
    """mods: role -> Module; threads: a threaded build (every memory shared). Returns (violations,
    report lines); violations are (rule, role, message)."""
    v = []
    lines = []

    def fail(rule, role, msg):
        v.append((rule, role, msg))

    core = mods['core']
    if len(core.memories) != 1 or core.mem_imports:
        fail('M1', 'core', 'core must define exactly one memory and import none')
        return v, lines
    if core.exports.get('memory', (None,))[0] != 2:
        fail('M1', 'core', 'core must export its memory (the worker passes it to the other modules)')
    _, owner_min, owner_max = core.memories[0]
    owner_bytes = owner_min * PAGE

    for role, m in mods.items():
        # M1 / M2 / M3: memory ownership, flags and limits
        want_flags = 2 if threads else 0   # the shared bit: on every memory of a threaded build, else none
        for flags, _, _ in m.memories:
            if flags & ~3 or flags & 2 != want_flags:
                fail('M2', role, 'defined memory flags 0x%x: %s' % (flags, 'memory64, or not shared in a threaded build' if threads else 'shared or memory64'))
        for mod, field, (flags, mn, mx) in m.mem_imports:
            if flags & ~3 or flags & 2 != want_flags:
                fail('M2', role, 'imported memory %s.%s flags 0x%x: %s' % (mod, field, flags, 'memory64, or not shared in a threaded build' if threads else 'shared or memory64'))
        if role == 'core':
            continue
        want = MEMORY_MODULE[role]
        if m.memories or len(m.mem_imports) != 1 or m.mem_imports[0][:2] != (want, 'memory'):
            fail('M1', role, 'must import exactly one memory, %s.memory, and define none' % want)
            continue
        _, mn, mx = m.mem_imports[0][2]
        if mn > owner_min:
            fail('M3', role, 'imports min %d pages, above core initial %d pages' % (mn, owner_min))
        if mx is not None and (owner_max is None or owner_max > mx):
            fail('M3', role, 'imports max %d pages; core max %s does not fit' % (mx, owner_max))

    for role, m in mods.items():
        # G1: mutable globals have constant initialisers
        for k, (vt, mut, init) in enumerate(m.globals):
            if mut and init[0] not in ('i32', 'i64', 'f32', 'f64'):
                fail('G1', role, 'mutable global %d initialised from %s' % (k + m.global_imports, init[0]))
        # D1: data placement and passive segments
        if m.passive:
            fail('D1', role, '%d passive data segment(s); only active segments are allowed' % m.passive)
        for off, n, mem in m.data:
            if mem != 0:
                fail('D1', role, 'data segment targets memory %d' % mem)
                continue
            rng = _segment_range(off, n)
            if rng is None:
                fail('D1', role, 'data offset is not an i32 constant (%s)' % off[0])
            elif rng[1] > owner_bytes:
                fail('D1', role, 'data [0x%x,0x%x) runs past core initial memory 0x%x' % (rng[0], rng[1], owner_bytes))

    # Reserved ranges per module: label, lo, hi
    reserved = {}
    stack_pointer = {}
    for role, m in mods.items():
        ivs = []
        for off, n, mem in m.data:
            rng = _segment_range(off, n)
            if rng and rng[1] > rng[0] and mem == 0:
                ivs.append((rng[0], rng[1], 'data'))
        sp = _export_const(m, '__stack_pointer', 'S', fail)
        if sp is not None and m.exports['__stack_pointer'][0] == 3 and \
                m.globals[m.exports['__stack_pointer'][1] - m.global_imports][1] == 0:
            fail('S', role, '__stack_pointer is not mutable')
        stack_pointer[role] = sp
        if sp is not None:
            if sp > owner_bytes:
                fail('D1', role, '__stack_pointer 0x%x above core initial memory 0x%x' % (sp, owner_bytes))
            ivs.append((max(0, sp - STACK_BYTES[role]), sp, 'stack'))
        reserved[role] = ivs

    if 'renderer' in mods:
        r = mods['renderer']
        sp = stack_pointer['renderer']
        for mod, field, kind in r.imports:
            if mod != 'wbg':
                fail('R1', 'renderer', 'imports %s.%s (%s); the renderer may import only from wbg' % (mod, field, kind))
        if r.start is not None:
            fail('R5', 'renderer', 'declares a start function (index %d)' % r.start)
        data_end = _export_const(r, '__data_end', 'R4', fail)
        heap_base = _export_const(r, '__heap_base', 'R4', fail)
        max_end = max([_segment_range(o, n)[1] for o, n, mem in r.data
                       if mem == 0 and _segment_range(o, n)] or [0])
        if data_end is not None:
            if max_end > data_end:
                fail('R4', 'renderer', 'data ends at 0x%x, past __data_end 0x%x' % (max_end, data_end))
            if heap_base is not None:
                if heap_base < data_end:
                    fail('R4', 'renderer', '__heap_base 0x%x below __data_end 0x%x' % (heap_base, data_end))
                if heap_base > RUST_WINDOW[1]:
                    fail('R4', 'renderer', '__heap_base 0x%x above the Rust window top 0x%x' % (heap_base, RUST_WINDOW[1]))
                if heap_base > data_end:
                    reserved['renderer'].append((data_end, heap_base, 'stack and bss gap'))
        if sp is not None and data_end is not None and sp - RUST_MIN_STACK_BYTES < data_end:
            fail('R3', 'renderer', 'stack-first or too small: __stack_pointer 0x%x leaves less than %d bytes above __data_end 0x%x'
                 % (sp, RUST_MIN_STACK_BYTES, data_end))
        for lo, hi, label in reserved['renderer']:
            if hi <= lo:
                continue
            if lo < LOW_RESERVED[1] and hi > LOW_RESERVED[0]:
                fail('R2', 'renderer', '%s [0x%x,0x%x) overlaps the low reserved range 0x%x-0x%x'
                     % (label, lo, hi, LOW_RESERVED[0], LOW_RESERVED[1]))
            elif lo < RUST_WINDOW[0] or hi > RUST_WINDOW[1]:
                fail('R2', 'renderer', '%s [0x%x,0x%x) lies outside the Rust window 0x%x-0x%x'
                     % (label, lo, hi, RUST_WINDOW[0], RUST_WINDOW[1]))

    # D2: no overlap between the reserved ranges of different modules
    ivs = sorted((lo, hi, role, label) for role in mods for lo, hi, label in reserved[role] if hi > lo)
    found = 0
    for i, (lo, hi, ra, la) in enumerate(ivs):
        for lo2, hi2, rb, lb in ivs[i + 1:]:
            if lo2 >= hi:
                break
            if rb != ra and max(lo, lo2) < min(hi, hi2):
                found += 1
                if found <= 5:
                    fail('D2', ra, '%s [0x%x,0x%x) overlaps %s %s [0x%x,0x%x) at 0x%x'
                         % (la, lo, hi, rb, lb, lo2, hi2, max(lo, lo2)))
    if found > 5:
        fail('D2', 'all', '%d overlapping ranges in total' % found)

    for role, m in mods.items():
        spans = [_segment_range(o, n) for o, n, mem in m.data if mem == 0]
        spans = [x for x in spans if x]
        span = ('data %d segment(s) in [0x%x,0x%x)' % (len(spans), min(x[0] for x in spans), max(x[1] for x in spans))
                if spans else 'no data')
        sp = stack_pointer.get(role)
        mem = ('defines' if m.memories else 'imports %s.memory' % MEMORY_MODULE[role]) if role != 'core' else 'defines'
        pages = owner_min if role == 'core' else mods['core'].memories[0][1]
        lines.append('%-9s %s: memory %s, core initial %d pages (0x%x bytes), %s, sp %s'
                     % (role, FILES[role], mem, pages, pages * PAGE, span,
                        ('0x%x' % sp) if sp is not None else 'n/a'))
    return v, lines


def check_own_memory(m, rule, role, imports, reader):
    """P1 / P2: a module with its own memory, importing exactly `imports`; violations as check's."""
    v = []
    if m.imports != imports:
        v.append((rule, role, 'imports %s; it may import %s' % (
            ', '.join('%s.%s' % (a, b) for a, b, _ in m.imports) or 'nothing',
            ', '.join('%s.%s (%s)' % i for i in imports) or 'nothing')))
    if len(m.memories) != 1:
        v.append((rule, role, 'defines %d memories; it must define exactly one, its own' % len(m.memories)))
    for flags, _, _ in m.memories:
        if flags & ~1:
            v.append((rule, role, 'memory flags 0x%x: shared or memory64' % flags))
    if m.exports.get('memory', (None,))[0] != 2:
        v.append((rule, role, 'does not export its memory as "memory" (%s reads its results there)' % reader))
    if m.start is not None:
        v.append((rule, role, 'declares a start function (index %d)' % m.start))
    return v


def check_settings(m):
    """P1 for the settings module; violations as check's."""
    return check_own_memory(m, 'P1', 'settings', [], 'settings.js')


def check_disc(m):
    """P2 for the disc module; violations as check's."""
    return check_own_memory(m, 'P2', 'disc', DISC_IMPORTS, 'disc.js')


# ---- loading and CLI ------------------------------------------------------------------------

def load(build_dir):
    mods = {}
    threads = (Path(build_dir) / 'gqueue.wasm').exists()
    for role, name, _, _ in ROLES:
        path = Path(build_dir) / name
        if role in THREADED_ONLY and not threads:
            if path.exists():
                raise WasmError('%s in a build without gqueue.wasm (a stale threaded module?)' % path)
            continue
        if not path.exists():
            raise WasmError('missing %s' % path)
        mods[role] = parse(role, path)
    path = Path(build_dir) / SETTINGS_FILE
    if not path.exists():
        raise WasmError('missing %s' % path)
    mods['settings'] = parse('settings', path)
    path = Path(build_dir) / DISC_FILE
    if not path.exists():
        raise WasmError('missing %s' % path)
    mods['disc'] = parse('disc', path)
    return mods


def run(build_dir):
    try:
        mods = load(build_dir)
    except (OSError, WasmError, UnicodeDecodeError) as e:
        print('ERROR %s' % e)
        return 2
    threads = (Path(build_dir) / 'gqueue.wasm').exists()
    settings, disc = mods.pop('settings'), mods.pop('disc')
    violations, lines = check(mods, threads)
    violations += check_settings(settings) + check_disc(disc)
    for role, name, m in (('settings', SETTINGS_FILE, settings), ('disc', DISC_FILE, disc)):
        lines.append('%-9s %s: its own memory (%s), %d import(s)' % (
            role, name, ', '.join('%d pages' % mn for _, mn, _ in m.memories) or 'none', len(m.imports)))
    if threads:
        lines.append('threaded build (gqueue.wasm): every memory must be shared')
    for line in lines:
        print(line)
    for rule, role, msg in violations:
        print('FAIL %s [%s] %s' % (rule, role, msg))
    if violations:
        print('web layout: FAIL (%d violation(s))' % len(violations))
        return 1
    print('web layout: OK (%s)' % ', '.join(sorted([*mods, 'settings', 'disc'])))
    return 0


# ---- synthetic fixtures (no game data): used by --selftest and by hand --------------------

def _leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def _sleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if (n == 0 and not b & 0x40) or (n == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def _sec(sid, payload):
    return bytes([sid]) + _leb(len(payload)) + payload


def _vec(items):
    return _leb(len(items)) + b''.join(items)


def _name(s):
    b = s.encode()
    return _leb(len(b)) + b


def _lim(mn, mx=None):
    return (b'\x01' + _leb(mn) + _leb(mx)) if mx is not None else (b'\x00' + _leb(mn))


def _ei32(v):
    return b'\x41' + _sleb(v) + b'\x0b'


def _glob(mut, v):
    return b'\x7f' + bytes([mut]) + _ei32(v)


def _data(off, n):
    return b'\x00' + _ei32(off) + _leb(n) + b'\x01' * n


def _module(*secs):
    return b'\x00asm\x01\x00\x00\x00' + b''.join(secs)


def _settings(import_memory=False):
    if import_memory:   # a settings module on the game's memory
        return _module(_sec(2, _vec([_name('env') + _name('memory') + b'' + _lim(28864, 32768)])))
    return _module(_sec(5, _vec([_lim(17)])), _sec(7, _vec([_name('memory') + b'' + _leb(0)])))


def _disc(import_memory=False):
    read = _name('env') + _name('dhd_disc_read') + b'\x00' + _leb(0)
    types = _sec(1, _vec([b'\x60' + _vec([b'\x7f'] * 4) + _vec([b'\x7f'])]))
    if import_memory:   # a disc module on the game's memory
        return _module(types, _sec(2, _vec([read, _name('env') + _name('memory') + b'\x02' + _lim(28864, 32768)])))
    return _module(types, _sec(2, _vec([read])), _sec(5, _vec([_lim(17)])),
                   _sec(7, _vec([_name('memory') + b'\x02' + _leb(0)])))


def _core(sp=0x70DF8890):
    return _module(
        _sec(5, _vec([_lim(28896, 32768)])),
        _sec(6, _vec([_glob(1, sp)])),
        _sec(7, _vec([_name('__stack_pointer') + b'\x03' + _leb(0), _name('memory') + b'\x02' + _leb(0)])),
        _sec(11, _vec([_data(0x70100000, 16)])))


def _graphics(sp=MIB, data=(0x40000000, 16)):
    return _module(
        _sec(2, _vec([_name('env') + _name('memory') + b'\x02' + _lim(28864, 32768)])),
        _sec(6, _vec([_glob(1, sp)])),
        _sec(7, _vec([_name('__stack_pointer') + b'\x03' + _leb(0)])),
        _sec(11, _vec([_data(data[0], data[1])])))


def _renderer(sp=0x60000000, data=((0x50001000, 16),), data_end=0x50001010, heap_base=0x60000000,
              mem_min=28864, env_import=False):
    imports = [_name('wbg') + _name('memory') + b'\x02' + _lim(mem_min, 32768),
               _name('wbg') + _name('log') + b'\x00' + _leb(0)]
    if env_import:
        imports.append(_name('env') + _name('host_fn') + b'\x00' + _leb(0))
    return _module(
        _sec(1, _vec([b'\x60' + _vec([b'\x7f']) + _vec([])])),
        _sec(2, _vec(imports)),
        _sec(6, _vec([_glob(1, sp), _glob(0, data_end), _glob(0, heap_base)])),
        _sec(7, _vec([_name('__stack_pointer') + b'\x03' + _leb(0),
                      _name('__data_end') + b'\x03' + _leb(1),
                      _name('__heap_base') + b'\x03' + _leb(2)])),
        _sec(11, _vec([_data(o, n) for o, n in data])))


FIXTURES = {
    # name: (renderer kwargs, graphics kwargs, expected rule ids; empty = must pass[, settings kwargs[,
    # disc kwargs]])
    'good': (dict(), dict(), ()),
    'settings_shared': (dict(), dict(), ('P1',), dict(import_memory=True)),
    'disc_shared': (dict(), dict(), ('P2',), dict(), dict(import_memory=True)),
    # wasm-ld default: stack at the bottom of the window, data above it (stack-first)
    'stack_first': (dict(sp=0x50100000, data=((0x50100000, 16),), data_end=0x50100010), dict(), ('R3',)),
    # stack and data in the low reserved range [0,1M)
    'data_low': (dict(sp=MIB, data=((0x400, 16),), data_end=0x410), dict(), ('R2', 'R3')),
    'env_import': (dict(env_import=True), dict(), ('R1',)),
    'memory_min_too_big': (dict(mem_min=28897), dict(), ('M3',)),
    # graphics data [0x50000800,0x50001800) overlaps the renderer's data at 0x50001000
    'data_overlap': (dict(), dict(data=(0x50000800, 0x1000)), ('D2',)),
}


def write_fixtures(out):
    out = Path(out)
    for name, (ren, gfx, _, *own) in FIXTURES.items():
        d = out / name
        (d / 'renderer').mkdir(parents=True, exist_ok=True)
        (d / 'dhd_core.wasm').write_bytes(_core())
        (d / 'graphics.wasm').write_bytes(_graphics(**gfx))
        (d / 'renderer' / 'dhd_render_bg.wasm').write_bytes(_renderer(**ren))
        (d / SETTINGS_FILE).write_bytes(_settings(**(own[0] if own else {})))
        (d / DISC_FILE).write_bytes(_disc(**(own[1] if len(own) > 1 else {})))
    return sorted(FIXTURES)


def selftest(out):
    import contextlib
    import io
    write_fixtures(out)
    failures = []
    for name, (_, _, expect, *_) in FIXTURES.items():
        mods = load(Path(out) / name)
        settings, disc = mods.pop('settings'), mods.pop('disc')
        violations, _ = check(mods)
        violations += check_settings(settings) + check_disc(disc)
        rules = {rule for rule, _, _ in violations}
        with contextlib.redirect_stdout(io.StringIO()):
            code = run(Path(out) / name)
        want_code = 1 if expect else 0
        ok = code == want_code and set(expect) <= rules and (expect or not violations)
        if not ok:
            failures.append('%s: exit %d (want %d), rules %s (want %s)' % (name, code, want_code, sorted(rules), expect))
    if failures:
        print('selftest: FAIL')
        for f in failures:
            print('  ' + f)
        return 1
    print('selftest: OK (%d fixtures: good passes, each bad fixture exits 1 with its rule)' % len(FIXTURES))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description='Check the layout of the web build modules sharing one memory.')
    ap.add_argument('build_dir', nargs='?', type=Path, help='directory holding dhd_core.wasm, graphics.wasm, renderer/, dhd_settings.wasm, dhd_disc.wasm')
    ap.add_argument('--require-renderer', action='store_true', help='accepted for old callers: the Rust renderer is always required')
    ap.add_argument('--selftest', action='store_true', help='build the synthetic fixtures and check their verdicts')
    ap.add_argument('--write-fixtures', type=Path, metavar='DIR', help='write the synthetic fixtures under DIR')
    a = ap.parse_args(argv)
    try:
        if a.selftest:
            return selftest(ROOT / 'build' / 'web-layout-fixtures')
        if a.write_fixtures:
            names = write_fixtures(a.write_fixtures)
            print('wrote %d fixtures under %s: %s' % (len(names), a.write_fixtures, ', '.join(names)))
            return 0
        if a.build_dir is None:
            ap.error('give BUILD_WEB_DIR, --selftest or --write-fixtures DIR')
        return run(a.build_dir)
    except Exception:
        traceback.print_exc()
        return 2


if __name__ == '__main__':
    sys.exit(main())
