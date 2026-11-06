"""PC build, wasm32 stage: the portable game core as one wasm module (finding 081, phase 3).

Writes build/wasm/dhd_core.wasm from
  - every game unit (src/**), transformed (port/tools/transform.py, port/patches.py) with an empty
    label prefix (wasm C symbols carry none, so the asm labels must not either);
  - the recompiled asm-only functions (port/tools/recomp.py), found by a fixed point over the
    objects' undefined names;
  - the module-side port files (finding 081, phase 2): runtime and hle, compiled from a mirror of
    port/ in which the data declarations are numbers (below) and RETAIL labels have no prefix;
  - the game data's fixups (data.py), as a table of {PS2 address, value} pairs; code addresses
    are table indices, data addresses numbers.

Data symbols are numbers, not linker symbols: wasm-ld has no --defsym, the wasm assembler rejects
absolute symbols, and an alias needs an anchor as big as the guest space. So each C declaration of
a data name (D_xxxxxxxx, _gp) becomes a macro for its PS2 address (data.absolute). Design and the
checks behind it: build/audit/w5-wasm-module.md.

The module's imports are the host_api.h functions only (--allow-undefined-file). Anything else that
doesn't resolve fails the strict link; a diagnostic link with --allow-undefined then writes the
module anyway, and every unresolved symbol is grouped by owner (census.json, link.txt).

The build stands on its own: everything it needs is generated here from the checkout, splat's asm/
and assets/vutext.bin (DHD_VUTEXT), including the VU1 AOT include (port/tools/vu1_aot.py) that
the native VU0 routines are derived from.

usage: python port/tools/build_wasm.py [-j 8]
Needs LLVM's clang, wasm-ld, llvm-nm, llvm-objcopy, llvm-cxxfilt on PATH (clang also assembles the
data image as a COFF object, for its checks only), WABT's wasm-objdump, the wasi-sdk sysroot
(WASI_SYSROOT) and SIMDe (SIMDE_DIR). The native hosts run wasm2c themselves (build_host64.py)."""
import argparse, collections, glob, hashlib, json, os, re, shutil, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PORT = os.path.join(ROOT, 'port')
sys.path.insert(0, HERE)
sys.path.insert(0, PORT)  # patches.py

WASM_TARGET = 'wasm32-wasip1'  # the WASI headers need __wasi__ (wasi/version.h); no WASI import is used by the module
import data, eefloat, objcache, port_source, recomp, transform  # noqa: E402
from port_source import apply_patches, align_aggregate_allocas  # noqa: E402
import vu1_aot  # noqa: E402
import wasm_retail_names  # noqa: E402
from codeids import write_table, wasm_code_rows, wasm_data_rows, wasm_fixup_source  # noqa: E402
from pathlib import Path
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches
IMAGE = data.ImageNames()

OUT = os.path.join(ROOT, 'build', 'wasm')
SRC_ROOT = os.path.join(ROOT, 'src')
SRC_OUT = os.path.join(OUT, 'src')
INC_OUT = os.path.join(OUT, 'include')          # transformed game headers (transform.port_headers)
PORT_OUT = os.path.join(OUT, 'port')            # mirror of port/ (rewritten copies)
OBJ = os.path.join(OUT, 'obj')
RECOMP_OUT = os.path.join(OUT, 'recomp')
DATA_OUT = os.path.join(OUT, 'data')
SHIM = os.path.join(OUT, 'shim')                # SSE/intrinsic headers over SIMDe, the wasm ee_native_float.h
WASI = os.environ.get('WASI_SYSROOT', str(DEVCACHES / 'wasi-sdk/wasi-sdk-34.0-x86_64-windows/share/wasi-sysroot'))
SIMDE = os.environ.get('SIMDE_DIR', str(DEVCACHES / 'simde-src'))
WABT = os.environ.get('WABT_DIR', str(DEVCACHES / 'wabt/wabt-1.0.42'))
WASM = os.path.join(OUT, 'dhd_core.wasm')

# The module's own memory: above the scratchpad (0x70000000), see build/audit/w5-wasm-module.md 3.
STACK_BASE = 0x70100000
STACK_SIZE = 1 << 20
DATA_BASE = STACK_BASE + STACK_SIZE
INITIAL_MEMORY = 0x70E00000   # covers data+bss (0x705B1220 in run 24) and the scratchpad below it
MAX_MEMORY = 0x80000000

# finding 081, phase 2: the module side (port/runtime, port/hle)
MODULE_UNITS = [
    'runtime/ps2asm.c', 'runtime/vu0_micro.c', 'runtime/vu0_interp.c', 'runtime/recomp.c',
    'runtime/libc_shims.c', 'runtime/memory.c',
    'runtime/sha256.c', 'hle/kernel.c', 'hle/timer.c', 'hle/boot.c', 'hle/sif.c', 'hle/cdvd.c', 'hle/game_data.c',
    'hle/iop.c', 'hle/mc.c', 'hle/dma.c', 'hle/graphics.c', 'hle/pad.c', 'hle/menu_page.c',
    'hle/pc_settings.c', 'hle/widescreen.c', 'hle/exit_menu.c', 'hle/puppet.c', 'hle/online.c',
    'hle/online_menu.c', 'hle/autopad.c', 'hle/state_trace.c', 'hle/rate_audit.c', 'hle/sound.c', 'hle/game_options.c',
    'hle/fps.c', 'hle/replay.c', 'hle/keyboard.c', 'hle/vif0.c', 'hle/presence.c', 'hle/touch_field.c',
    'hle/touch_settings.c', 'hle/controls_settings.c', 'runtime/wasi_adapter.c',
]
MIRROR_DIRS = ['runtime', 'hle', 'include', 'host', 'gs', 'net']

# The game units' flags (those of the retired 32-bit build, minus its x86-only -msse2
# -mfpmath=sse -malign-double), for wasm32; -w is kept (warnings stay off). -O2 is the brief's
# level for the backend; the front end's -disable-llvm-passes makes its -O irrelevant.
BASE = ['--target=' + WASM_TARGET, '--sysroot=' + WASI, '-isystem', WASI + '/include/wasm32-wasip1', '-c', '-O2', '-msimd128',
        '-ftrivial-auto-var-init=zero', '-ffp-contract=off', '-fwrapv', '-fno-strict-aliasing', '-fno-builtin',
        '-fno-delete-null-pointer-checks', '-ferror-limit=0', '-w',
        '-Wno-error=incompatible-pointer-types', '-Wno-error=int-conversion',
        '-Wno-error=incompatible-function-pointer-types', '-Wno-error=implicit-function-declaration',
        '-Wno-error=pointer-arith', '-D_WASI_EMULATED_PROCESS_CLOCKS']
OPT_LANG_C = ['-x', 'c', '-std=gnu89', '-fcommon', '-Wno-return-mismatch']
OPT_LANG_CPP = ['-x', 'c++', '-std=gnu++98', '-fno-exceptions', '-fno-rtti', '-fno-strict-return']
ERROR = re.compile(r'^[^\n]*?:(\d+):\d+: (?:fatal )?error: (.*)$', re.M)

# A declaration statement, and one declarator of it: [pre] [*] NAME [dims] [asm label]
EXTERN = re.compile(r'\bextern\s+([^;{}]+?)\s*;')
DECLARATOR = re.compile(r'^(?P<pre>.*?)(?P<ptr>(?:\*\s*)*)(?P<name>\w+)\s*(?P<dims>(?:\[[^\]]*\]\s*)*)'
                        r'(?:\s*(?:__asm__|__asm|asm)\s*\(\s*"(?P<label>\w+)"\s*\)|\s*__attribute__\s*\(\(.*\)\))*\s*$', re.S)
# a data variable that holds a function pointer: void (*NAME)(args); its pointer type is `(**)(args)`
FPTR = re.compile(r'^(?P<pre>[^()]*?)\(\s*\*\s*(?P<name>\w+)\s*(?P<dims>(?:\[[^\]]*\]\s*)*)\)\s*'
                  r'(?P<post>\(.*?\))\s*(?:(?:__asm__|__asm|asm)\s*\(\s*"(?P<label>\w+)"\s*\))?\s*$', re.S)
DATA_NAME = re.compile(r'^(?:D_[0-9A-F]{8}|_gp)$')
TOKEN = re.compile(r'0x[0-9A-Fa-f]+|\d+|[A-Za-z_.$][\w.$]*|[+\-]')


def run(cmd, **kw):
    cmd = [shutil.which(cmd[0]) or cmd[0]] + cmd[1:]
    return subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', **kw)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


# ---- the data names as numbers ---------------------------------------------------------------

DATA_ANY = re.compile(r'^(?:D_[0-9A-F]+|_gp)$')


TABLE_BASE = 0x12100  # wasm function-table base (W7i: 73984)
# variadic definitions: their wasm type is (fixed..., va buffer). Each non-variadic declaration of one
# becomes variadic after the same fixed parameters, so clang builds the buffer from the actual arguments
VARIADIC = {'func_00254D08': 1, 'func_00254D68': 2, 'func_00259120': 2, 'func_0020D028': 1,
            'func_002189F8': 1, 'func_00234FB0': 2, 'func_002350E8': 2}
VARIADIC_DECL = re.compile(r'(?m)^([ \t]*(?:extern\s+(?:"C"\s+)?)?(?:static\s+)?(?!(?:return|else|case|goto|do)\b)'
                           r'[A-Za-z_][\w\s\*]*?\b(%s)(?:__c\w+)?\s*)\(([^()]*)\)(\s*(?:(?:__asm__|asm)\s*\([^)]*\)\s*)?[;,])'
                           % '|'.join(VARIADIC))
# per unit (by source path under src/): C++ definitions that other units call by their C name
C_DEFINITIONS = {'unsorted/001B9BE0.cpp': ('func_001B9BE0',)}
# per unit: the va_list typedef is the compiler's builtin on wasm (the varargs units of 00234FB0 and 002350E8)
WASM_VA_LIST = {'unsorted/00234FB0.c', 'unsorted/002350E8.c'}


def is_local_label(name):
    """A numeric local code label (.L<hex>, the recompiler's A__L...): its PS2 address is its number."""
    return bool(re.fullmatch(r'\.L[0-9A-Fa-f]{8}', name))


def is_numeric_data(name):
    """A data name the wasm build writes as its PS2 address: the image's labels, _gp, and every
    D_<hex> name a declaration (or recompiled code) names: in RAM, VU memory, the scratchpad, the
    registers, and below the image too (D_1010 = 0x1010, a VU address; D_001FD730, an address in
    the code), as data.py gives every such name no object defines. The fixups keep a D_ name that
    an object defines as that object's symbol (fixup_expression)."""
    if is_local_label(name) or name == '_gp' or name in IMAGE:
        return True
    return bool(re.fullmatch(r'D_[0-9A-F]+', name))


def data_address(name):
    if is_local_label(name):
        return int(name[2:], 16)
    if name == '_gp':
        return data.GP
    if name in IMAGE.labels:
        return IMAGE.labels[name]
    return int(name[2:], 16)


def guest_data_address(name):
    """The PS2 address a data name stands for, as C text."""
    return data.absolute(data_address(name))


def is_data_name(name):
    """Any name the image or the RAM map can give an address: an image label, _gp, or D_<hex>."""
    return is_local_label(name) or name in IMAGE.labels or bool(DATA_ANY.match(name))


def split_declarators(body):
    """The declarators of one declaration, split at the commas outside brackets and braces."""
    parts, depth, cur = [], 0, ''
    for ch in body:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    parts.append(cur)
    return [x.strip() for x in parts]


def extern_statements(text):
    """(start, end, body) of each code extern declaration in text: the text from after `extern` to
    the `;` that ends it at brace depth 0 (comments and strings skipped). `extern "C"` blocks are
    not declarations and are left out."""
    mask = transform.code_mask(text)
    out = []
    for m in re.finditer(r'\bextern\b', mask):
        if re.match(r'\s*"[^"\n]*"\s*\{', text[m.end():]):
            continue  # extern "C" { ... }: its items are the declarations inside
        lk = re.match(r'\s*"[^"\n]*"', text[m.end():])
        depth, i = 0, m.end() + (lk.end() if lk else 0)
        nxt = re.search(r'[;{]', mask[i:]) if lk else None
        if nxt and mask[i + nxt.start()] == '{':
            continue  # a function definition: not a declaration of data
        while i < len(mask):
            c = mask[i]
            if c in '([{':
                depth += 1
            elif c in ')]}':
                depth -= 1
            elif c == ';' and depth == 0:
                break
            i += 1
        if i < len(mask):
            out.append((m.start(), i + 1, text[m.end() + (lk.end() if lk else 0):i]))
    return out


def data_declarations(text):
    """Each extern statement that declares a data name the rewrite looks at (a numeric one, or an
    alias): [(span, [(declarator, base type, pointer stars, name, dims, target)])]. A declarator that
    doesn't parse drops the statement."""
    out = []
    for a, b, body in extern_statements(text):
        items, base = [], None
        for i, d in enumerate(split_declarators(body)):
            mm = DECLARATOR.match(d)
            fp = None if mm else FPTR.match(d)
            if not mm and not fp:
                items = None
                break
            if fp:
                if i == 0:
                    base = fp.group('pre').strip()
                items.append((d, base, '(**)' + fp.group('post'), fp.group('name'), fp.group('dims').strip(),
                              fp.group('label') or fp.group('name')))
                continue
            if i == 0:
                base = mm.group('pre').strip()
            target = mm.group('label') or mm.group('name')
            items.append((d, base, mm.group('ptr').replace(' ', ''), mm.group('name'),
                          mm.group('dims').strip(), target))
        if items and any(is_data_name(t) for *_, t in items):
            out.append(((a, b), items))
    return out


FUNC_ARRAY = re.compile(r'^(func_[0-9A-F]{8})\s*\[\s*\]$')


def fix_function_arrays(text):
    """A code name declared as a char array (extern char func_X[];) is taken as data by the object
    file; a function's address is its table index. Such declarators become prototypes of the
    function (the other declarators of the statement stay)."""
    out, last = [], 0
    for a, b, body in extern_statements(text):
        parts = split_declarators(body)
        funcs = [FUNC_ARRAY.match(p) for p in parts]
        if not any(funcs):
            continue
        base = DECLARATOR.match(parts[0]).group('pre').strip()
        lines = []
        for p, f in zip(parts, funcs):
            if not f:
                m = DECLARATOR.match(p)
                lines.append('extern %s %s%s%s;' % (base, m.group('ptr'), m.group('name'), m.group('dims').strip()))
        lines += ['void %s(void);' % f.group(1) for f in funcs if f]
        repl = '\n'.join(lines)
        out += [text[last:a], repl + '\n' * max(1, text[a:b].count('\n') - repl.count('\n'))]
        last = b
    out.append(text[last:])
    return ''.join(out)


CPP_DECL = re.compile(r'(?m)(^[ \t]*|[{;][ \t]*)((?:(?!return\b|else\b|case\b|default\b)[\w:<>\*&]+[ \t\*&]+)+?)'
                      r'(func_[0-9A-F]{8})\s*\(((?:[^();{}]|\([^()]*\))*)\)\s*;')


def cpp_linkage(text):
    """A C++ unit that declares a C function (extern or a struct member) mangles the reference, so
    it stays undefined: the definition is the C symbol. Each prototype of a code name gets an asm
    label with its plain name (a member prototype's label names the member's symbol, the same
    address). Definitions and calls are untouched; transform.hoist_asm_labels labels no call."""
    return CPP_DECL.sub(lambda m: '%s%s%s(%s) __asm__("%s");' % (m.group(1), m.group(2), m.group(3),
                                                                 m.group(4), m.group(3)), text)


def rewrite(text, aliases):
    """Each data declaration becomes a macro for its PS2 address; the lines keep their count. A data
    name that is an alias of a code symbol (data.forwarding_aliases) becomes that symbol, declared
    here (no prototype) when the unit doesn't name it. A C variable (not in the image) keeps its
    declaration. An inline struct type becomes a typedef first, since a cast can't define one."""
    mask = transform.code_mask(text)
    out, last = [], 0
    made = set()  # the names this unit turned into address macros

    def keep_macros(seg):
        """A source #undef of such a name (a merged unit's, a no-op in i686 where it is a variable) would
        remove the macro: its line becomes a comment, the line count stays."""
        seg = re.sub(r'^#undef (\w+)[ \t]*$', lambda m: ('/* %s: the address macro stays */' % m.group(0))
                     if m.group(1) in made else m.group(0), seg, flags=re.M)

        def self_alias(m):  # #define NAME (*(T *)&NAME): the same object, at its PS2 address
            if m.group(1) not in made:
                return m.group(0)
            return '#define %s (*(%s *)(unsigned)%s)' % (m.group(1), m.group(2),
                                                        guest_data_address(m.group(1)))
        return re.sub(r'^#define (\w+) \(\*\((\w[\w ]*?) \*\)&\1\)[ \t]*$', self_alias, seg, flags=re.M)
    for (a, b), items in data_declarations(text):
        lines, typedefs, kept = [], {}, 0
        for k, (d, base, ptr, name, dims, target) in enumerate(items):
            if '{' in base and base not in typedefs:
                typedefs[base] = 'T_%s' % name
                lines.append('typedef %s %s;' % (base, typedefs[base]))
            base_name = typedefs.get(base, base)
            label_addr = name.startswith('A_D_') and re.fullmatch(r'D_[0-9A-Fa-f]+', target)
            if target not in aliases and not is_numeric_data(target) and not label_addr:
                kept += 1
                lines.append('extern %s;' % (d if k == 0 else base + ' ' + d))
                continue
            if target in aliases:
                fn = aliases[target]
                if not re.search(r'\b' + re.escape(fn) + r'\b', mask):
                    lines.append('void %s();' % fn)
                lines.append('#undef %s' % name)
                lines.append('#define %s %s' % (name, fn))
                made.add(name)
                continue
            t = base_name + (' ' + ptr if ptr else '')
            value = guest_data_address(target)
            if ptr.startswith('(**)'):
                cast = '%s %s' % (base_name, ptr)  # a type name: one pair of parentheses around it, no more
                body = ('((%s)(unsigned)%s)' if dims else '(*(%s)(unsigned)%s)') % (cast, value)
            elif dims:  # the outer bound is unknown (any size); inner bounds keep the element type
                inner = ''.join(re.findall(r'\[[^\]]*\]', dims)[1:])
                body = '(*(%s (*)[0x7fffffffu / sizeof(%s%s)]%s)(unsigned)%s)' % (t, t, inner, inner, value)
            else:
                body = '(*(%s *)(unsigned)%s)' % (t, value)
            lines.append('#undef %s' % name)
            lines.append('#define %s %s' % (name, body))
            made.add(name)
        if kept == len(items):
            continue  # nothing to rewrite in this statement
        repl = '\n'.join(lines)
        if a > 0 and text[a - 1] != '\n':  # a directive starts its own line (extern "C" { ... } on one line)
            repl = '\n' + repl
        out += [keep_macros(text[last:a]), repl + '\n' * max(1, text[a:b].count('\n') - repl.count('\n'))]
        last = b
    out.append(keep_macros(text[last:]))
    text = ''.join(out)
    text = re.sub(r'([^\n])(#(?:undef|define)\b)', r'\1\n\2', text)  # a directive starts its own line
    return scalar_abi(fall_off_returns(variadic_declarations(text)))


# Non-void functions that fall off their end after a call (-Wreturn-type over all game units: only
# these two). MIPS returns the callee's v0, i686 its eax; wasm returned an unrelated local (race
# field 1452: an object respawned on track node 0, not 3). The callee really returns void *.
FALL_OFF = (('extern void func_001177F8(void *, short);', 'extern void *func_001177F8(void *, short);'),
            ('extern void func_001177F8(void *, int);', 'extern void *func_001177F8(void *, int);'),
            ('(index = first, index >= 0)) func_001177F8(obj, index);',
             '(index = first, index >= 0)) return (int)func_001177F8(obj, index);'),
            ('(index = indices[1]) >= 0) func_001177F8(obj, index);',
             '(index = indices[1]) >= 0) return func_001177F8(obj, index);'))

# Boundaries the wasm types can't show. A 4-byte Color by value: MIPS and i686 pass and return it like
# an int, as every caller declares; wasm32 passes it by pointer (byval) and returns it by hidden
# pointer (sret). Race field 2953: func_00161868 read the colour value as an address. And a same-unit
# alias with another return type (int vs unsigned char, i32 vs i8 in IR): wasm-ld calls a trapping
# .Lfunc_0022C350_bitcast_invalid (race field 3249); the caller keeps the low byte, as i686 with al.
SCALAR_ABI = (('unsigned int func_00161868(Color color, float *light)\n{\n',
               'extern "C" unsigned int func_00161868(unsigned int color_bits, float *light)\n{\n'
               '    Color color;\n    color.packed = color_bits;\n'),
              ('Color func_0023E188(int index, Color *table, float base, float scale)\n{\n',
               'static Color func_0023E188_body(int index, Color *table, float base, float scale);\n'
               'unsigned int func_0023E188(int index, Color *table, float base, float scale)\n{\n'
               '    Color c = func_0023E188_body(index, table, base, scale);\n    unsigned int bits;\n'
               '    __builtin_memcpy(&bits, &c, 4);\n    return bits;\n}\n'
               'static Color func_0023E188_body(int index, Color *table, float base, float scale)\n{\n'),
              ('extern int func_0022C350__c11(u128__c11 *,float) asm("func_0022C350");',
               'extern unsigned char func_0022C350__c11(u128__c11 *,float) asm("func_0022C350");'))


def fall_off_returns(text):
    for old, new in FALL_OFF:
        text = text.replace(old, new)
    return text


def scalar_abi(text):
    for old, new in SCALAR_ABI:
        text = text.replace(old, new)
    return text


def variadic_declarations(text):
    """Also merged-unit aliases (NAME__cN ... asm("NAME")), the first declarator of a list, and a
    variadic declaration with more fixed parameters than the definition (they would be wasm params).
    A declaration with fewer fixed parameters than the definition gets the pointer types of i686's
    cdecl wrapper (void *, const void *)."""
    def fix(m):
        n = VARIADIC[m[2]]
        params = [p.strip() for p in m[3].split(',')] if m[3].strip() not in ('', 'void') else []
        fixed = [p for p in params if p != '...']
        if '...' in params and len(fixed) == n:
            return m[0]
        if len(fixed) < n:
            fixed = ['void *', 'const void *'][:n]
        return '%s(%s, ...)%s' % (m[1], ', '.join(fixed[:n]), m[4])
    return VARIADIC_DECL.sub(fix, text)


def declared_data_names(paths):
    names = set()
    for p in paths:
        for _, items in data_declarations(open(p, encoding='utf-8', errors='replace').read()):
            names |= {t for *_, t in items if is_data_name(t)}
    return names


def scan_sources():
    """Every file whose declarations the wasm build rewrites: src/, port/ (the mirrored dirs), include/."""
    out = glob.glob(os.path.join(SRC_ROOT, '**', '*.c*'), recursive=True)
    for d in MIRROR_DIRS:
        out += [p for p in glob.glob(os.path.join(PORT, d, '**', '*'), recursive=True)
                if os.path.splitext(p)[1] in ('.c', '.h', '.inc', '.cpp')]
    out += [p for p in glob.glob(os.path.join(ROOT, 'include', '**', '*.h'), recursive=True)]
    return out


# ---- the image: aliases, fixups, the checks of data.py ----------------------------------------

def image_stage(wanted):
    """data.py's image: its labels as numbers, the aliases, the fixups (checked against the retail
    ELF). Returns (aliases {data name: code name}, fixups [(label, operands)], offsets {label: PS2 address})."""
    data.OUT = DATA_OUT
    os.makedirs(DATA_OUT, exist_ok=True)
    paths, syms, fixups = data.convert_all(wanted)
    symbols_s = open(paths[1], encoding='utf-8').read()
    aliases = {}
    for m in re.finditer(r'/alternatename:(\w+)=(\w+)', symbols_s):
        aliases[m.group(1)] = m.group(2)
    p = run(['clang', '--target=i686-pc-windows-itanium', '-c', paths[0], '-o', paths[0] + '.o'])
    if p.returncode:
        sys.exit(p.stderr)
    image_obj = paths[0] + '.o'
    image_nm = run(['llvm-nm', image_obj]).stdout.splitlines()
    bad = data.check_image(image_obj, syms, image_nm)
    if bad:
        sys.exit('PS2 data image misplaced: %s' % '; '.join(bad[:10]))
    data.write_fixups(image_obj, fixups, image_nm)  # checks that the image is the retail ELF outside the fixups
    at = {}
    for line in image_nm:
        parts = line.split()
        if len(parts) == 3 and parts[2].startswith('fix.'):
            at[parts[2]] = int(parts[0], 16)
    offsets = {label: data.VUTEXT[0] + at[label] for label, _ in fixups}
    return aliases, fixups, offsets


def symbol_kinds(objs):
    """{name: 'func' | 'data'} of the objects' defined symbols (llvm-nm types: text or data)."""
    kinds = {}
    for i in range(0, len(objs), 200):
        for line in run(['llvm-nm', '--defined-only'] + objs[i:i + 200]).stdout.splitlines():
            m = re.match(r'^[0-9A-Fa-f]+ ([A-Za-z]) (\S+)$', line)
            if m:
                kinds[m.group(2)] = 'func' if m.group(1) in 'TtWw' else 'data'
    return kinds


def fixup_expression(expr, aliases, kinds, funcs, datas):
    """C for a fixup operand. A data name the module defines is its address (a symbol); a PS2
    address is a number; a code symbol is its table index (a function's address)."""
    out = []
    for tok in TOKEN.findall(expr):
        if tok in '+-':
            out.append(tok)
        elif tok[0].isdigit():
            out.append('0x%X' % int(tok, 0))
        elif tok in aliases:
            funcs.add(aliases[tok])
            out.append('(unsigned)(uintptr_t)&%s' % aliases[tok])
        elif kinds.get(tok) == 'data':
            datas.add(tok)
            out.append('(unsigned)(uintptr_t)&%s' % tok)
        elif tok in IMAGE.labels or (is_numeric_data(tok) and not (tok in kinds and data_address(tok) < data.VUTEXT[0])):
            # below the image, a name an object defines (a function) is that object's symbol
            out.append('0x%X' % int(guest_data_address(tok), 16))
        else:
            funcs.add(tok)
            out.append('(unsigned)(uintptr_t)&%s' % tok)
    return ' '.join(out)


def fixup_source(fixups, offsets, aliases, kinds):
    funcs, datas, rows = set(), set(), []
    for label, operands in fixups:
        for k, expr in enumerate(operands):
            rows.append('  {0x%XU, %s},' % (offsets[label] + 4 * k, fixup_expression(expr, aliases, kinds, funcs, datas)))
    lines = ['/* Generated by port/tools/build_wasm.py from the retail image (data.py fixups): one {PS2 address,',
             '   value} pair per word that is not the retail one. Code addresses are wasm table indices. */',
             '#include <stdint.h>']
    lines += ['extern void %s(void);' % f for f in sorted(funcs)]
    lines += ['extern char %s[];' % d for d in sorted(datas)]
    lines += ['__attribute__((section(".rodata.ps2fixups")))',
              'const uint32_t ps2_fixups[][2] __asm__("_ps2_fixups") = {']
    lines += rows or ['  {0, 0},']
    lines += ['};',
              '__attribute__((section(".rodata.ps2fixups")))',
              'const uint32_t ps2_fixups_end[][2] __asm__("_ps2_fixups_end") = {{0, 0}};', '']
    return '\n'.join(lines), len(rows)


# ---- headers ----------------------------------------------------------------------------------

def shim_ee_float():
    """ee_native_float.h with its SSE2 double helpers (inline asm, MXCSR) as plain C: the wasm module
    has no MXCSR and rounds to nearest, which is what these helpers force."""
    text = open(eefloat.HEADER, encoding='utf-8').read()
    start = text.index('/* One SSE2 double')
    end = text.index('#undef PS2_NATIVE_DOUBLE') + len('#undef PS2_NATIVE_DOUBLE')
    block = ('#define PS2_NATIVE_DOUBLE(name, op) \\\n'
             '    PS2_NATIVE double name(double a, double b) { return a op b; }\n'
             'PS2_NATIVE_DOUBLE(ps2_native_dadd, +)\n'
             'PS2_NATIVE_DOUBLE(ps2_native_dsub, -)\n'
             'PS2_NATIVE_DOUBLE(ps2_native_dmul, *)\n'
             'PS2_NATIVE_DOUBLE(ps2_native_ddiv, /)\n'
             '#undef PS2_NATIVE_DOUBLE')
    text = text[:start] + block + text[end:]
    # float add/sub/div: WASM_EE_FLOAT's versions replace the native helpers
    for name in ('ps2_native_fadd', 'ps2_native_fsub'):
        text = re.sub(r'PS2_NATIVE float %s\(float a, float b\) \{[^\n]*\}\n' % name, '', text, count=1)
    text = re.sub(r'PS2_NATIVE float ps2_native_fdiv\(float a, float b\)\n\{.*?\n\}\n', '', text, count=1, flags=re.S)
    if 'ps2_native_fadd' in text or 'ps2_native_fdiv' in text:
        raise ValueError('ee_native_float.h: unexpected float helper templates')
    # before the header's x86-only block (when it has one): wasm32 skips that branch
    k = text.find('#if defined(__i386__)')
    k = k if k >= 0 else text.index('#define PS2_NATIVE_DOUBLE')
    path = os.path.join(SHIM, 'ee_native_float_shim.h')
    write(path, text[:k] + WASM_EE_FLOAT + '\n' + text[k:])
    return path


# EE float rounding independent of the host's MXCSR and of compile-time folding. The native helpers and
# float multiplies round in the run-time mode (toward zero on the EE), but wasm clang folds constant
# operands to nearest (race field 1443: 0.48888889f*60.0f folded to 29.333334, i686 29.333332). So the
# result is exact in double and rounded by bits. Checked natively: bit-identical to SSE at MXCSR 0xFFC0
# over 2 x 20M random pairs, with the host in either mode (finding 082).
WASM_EE_FLOAT = r"""
/* wasm32/host64: exact double result, then rounded by bits: no host mode, no RN folding. */
PS2_NATIVE float ps2_native_daz(float a)
{
    __UINT32_TYPE__ x;
    __builtin_memcpy(&x, &a, 4);
    if (!(x & 0x7f800000u)) x &= 0x80000000u;  /* DAZ: denormal operand reads as signed zero */
    __builtin_memcpy(&a, &x, 4);
    return a;
}
/* Toward zero to float, denormal results flushed (FTZ), as SSE at MXCSR 0xFFC0. */
PS2_NATIVE float ps2_native_rz(double v)
{
    __UINT64_TYPE__ bits;
    unsigned e;
    __builtin_memcpy(&bits, &v, 8);
    e = (unsigned)(bits >> 52) & 0x7ff;
    if (e == 0x7ff)
        return (float)v;
    if (e < 1023 - 126)
        bits &= (__UINT64_TYPE__)1 << 63;
    else if (e > 1023 + 127)
        bits = (bits & (__UINT64_TYPE__)1 << 63) | 0x47EFFFFFE0000000ull;  /* FLT_MAX */
    else
        bits &= ~(__UINT64_TYPE__)0x1fffffff;
    __builtin_memcpy(&v, &bits, 8);
    return (float)v;
}
PS2_NATIVE float ps2_native_fadd(float a, float b)
{ ps2_native_align(&a, &b); return ps2_native_rz((double)ps2_native_daz(a) + (double)ps2_native_daz(b)); }
PS2_NATIVE float ps2_native_fsub(float a, float b)
{ ps2_native_align(&a, &b); return ps2_native_rz((double)ps2_native_daz(a) - (double)ps2_native_daz(b)); }
PS2_NATIVE float ps2_native_fmul(float a, float b)
{ return ps2_native_rz((double)ps2_native_daz(a) * (double)ps2_native_daz(b)); }
/* As the native helper: the double quotient rounds in the EE mode at run time (toward zero, DAZ, on
   i686 and in host64's EE fibers alike). The barrier only keeps clang from folding it to nearest. */
PS2_NATIVE float ps2_native_fdiv(float a, float b)
{
    double q;
    if (b == 0)
        return a / b;
    q = (double)a;
    __asm__("" : "+r"(q));
    return ps2_native_dtof(q / (double)b);
}
"""
FMUL = re.compile(r'^(\s*%[-\w.$]+ = )fmul float ([^,\s]+), ([^,\s]+)$')


def fmul_rewrite(ir):
    """`fmul float` -> ps2_native_fmul (eefloat leaves float multiplies native: the run-time mode)."""
    out, skip = [], False
    for line in ir.split('\n'):
        d = re.match(r'^define [^@]*@"?(\w+)', line)
        if d:
            skip = d.group(1).startswith('ps2_native_')
        elif line == '}':
            skip = False
        m = None if skip else FMUL.match(line)
        out.append('%scall float @ps2_native_fmul(float %s, float %s)' % (m[1], m[2], m[3]) if m else line)
    return '\n'.join(out)


SIMD_HEADERS = {  # x86 intrinsic headers -> SIMDe (native aliases on)
    'xmmintrin.h': 'simde/x86/sse.h', 'emmintrin.h': 'simde/x86/sse2.h', 'pmmintrin.h': 'simde/x86/sse3.h',
    'tmmintrin.h': 'simde/x86/ssse3.h', 'smmintrin.h': 'simde/x86/sse4.1.h', 'nmmintrin.h': 'simde/x86/sse4.2.h',
    'immintrin.h': 'simde/x86/avx2.h', 'wmmintrin.h': 'simde/x86/aes.h',
    'arm_neon.h': 'simde/arm/neon.h',  # gs/xxhash.h includes it on wasm SIMD (the scalar path is XXH_VECTOR=0)
}


def shim_headers():
    for name, target in SIMD_HEADERS.items():
        write(os.path.join(SHIM, name), '#define SIMDE_ENABLE_NATIVE_ALIASES\n#include <%s>\n' % target)
    # intrin.h: __rdtsc for the DHD_VU0_STATS timing only (a stub: the diagnostic reads 0)
    write(os.path.join(SHIM, 'intrin.h'), '#define SIMDE_ENABLE_NATIVE_ALIASES\n#include <simde/x86/sse2.h>\n'
          'static inline unsigned long long __rdtsc(void) { return 0; }\n')


def host_api_names():
    """The functions port/host/host_api.h declares: the only imports the module may have."""
    text = open(os.path.join(PORT, 'host', 'host_api.h'), encoding='utf-8').read()
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    names = {m.group(1) for m in re.finditer(r'\b(\w+)\s*\([^()]*\)\s*;', text)}
    return sorted(n for n in names if n.startswith(('host_', 'snd_host_')))


# ---- compile ----------------------------------------------------------------------------------

def compile_src(job):
    """Game unit: front end to IR, eefloat rewrite, IR back end (two steps)."""
    rel, src, obj, cpp = job[:4]
    extra = job[4] if len(job) > 4 else []
    if src is None:
        return {'unit': 'src/' + rel, 'status': 'failed', 'stage': 'transform', 'errors': []}
    lang = OPT_LANG_CPP if cpp else OPT_LANG_C
    inc = ['-I', PORT_OUT + '/include', '-I', INC_OUT, '-I', os.path.join(ROOT, 'include'), '-I', SHIM]
    inc += ['-DPS2_ASM_DIRECT']  # port.h PS2A: inline asm calls its generated function (ps2asm_aot.py)
    front = ['clang'] + [f for f in BASE if f != '-c'] + lang + inc + ['-S', '-emit-llvm', '-Xclang', '-disable-llvm-passes',
                                                                     '-include', HEADER_SHIM[0]] + extra
    rec = {'unit': 'src/' + rel, 'status': 'ok', 'stage': '', 'errors': []}
    # the shared object cache: both steps and the IR rewrites between them (this file, eefloat.py)
    cache = objcache.source_key(front + [src, '-o', '-'], '-', [' '.join(BASE + extra)] + REWRITERS)
    if cache and objcache.fetch(cache, obj):
        if cpp:
            wasm_retail_names.rename(obj, thunks=INTRA_THUNKS)
        return rec
    p = run(front + [src, '-o', '-'])
    errors = [(int(l), m) for l, m in ERROR.findall(p.stderr)]
    if p.returncode or errors:
        rec.update(status='failed', stage='front', errors=errors or [(0, p.stderr.strip()[-300:])])
        return rec
    try:
        ir = fmul_rewrite(eefloat.rewrite(align_aggregate_allocas(p.stdout))[0])
    except ValueError as e:
        rec.update(status='failed', stage='eefloat', errors=[(0, str(e))])
        return rec
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    p = run(['clang'] + BASE + extra + ['-x', 'ir', '-', '-o', obj], input=ir)
    if p.returncode:
        rec.update(status='failed', stage='backend', errors=[(0, p.stderr.strip()[-300:])])
        return rec
    if cache:
        objcache.store(cache, obj)  # before the renames below, which edit the object in place
    if cpp:
        # the C++ symbol policy (wasm_retail_names.py): definitions and references get their retail names
        wasm_retail_names.rename(obj, thunks=INTRA_THUNKS)
    return rec


HEADER_SHIM = [None]
REWRITERS = objcache.file_hashes(os.path.abspath(__file__), eefloat.__file__, port_source.__file__)
INTRA_THUNKS = {}  # (name, C signature) -> D signature: C++ adapters the intra-object ABI stage needs


def compile_c(job):
    """Port files and recompiled functions: one step, no eefloat rewrite."""
    name, src, obj, extra = job
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    p = objcache.compile(['clang'] + BASE + extra + [src, '-o', obj], obj, run)
    rec = {'unit': name, 'status': 'ok' if not p.returncode else 'failed', 'stage': '' if not p.returncode else 'direct',
           'errors': []}
    if p.returncode:
        errors = [(int(l), m) for l, m in ERROR.findall(p.stderr)]
        rec['errors'] = errors or [(0, p.stderr.strip()[-300:])]
    return rec


def port_include(src_dir_hint):
    return ['-fdeclspec', '-DXXH_VECTOR=0', '-I', PORT_OUT + '/runtime', '-I', PORT_OUT + '/include', '-I', INC_OUT,
            '-I', os.path.join(ROOT, 'include'), '-I', SHIM, '-I', SIMDE]


# ---- the recompiled functions (a fixed point over the objects) --------------------------------

ASM_ADDR = re.compile(r'/\* [0-9A-F]+ ([0-9A-F]{8}) ')
REGIONS = [  # (start, end, name), as in tools/queue.py
    (0x10A000, 0x10A208, 'crt0'), (0x10A208, 0x1FC000, 'game'), (0x1FC000, 0x201000, 'libgcc'),
    (0x201000, 0x243000, 'engine'), (0x243000, 0x263BC0, 'sdk')]
# C library functions the module's libc provides: never recompiled
LIBC = {'abs', 'memcpy', 'memmove', 'memset', 'memcmp', 'strcpy', 'strncpy', 'strcat', 'strncat',
        'strcmp', 'strncmp', 'strlen', 'memchr', 'strchr', 'strrchr', 'strstr', 'sprintf', 'vsprintf', 'printf',
        'fabsf', 'floorf', 'ceilf', 'sqrtf', 'sinf', 'cosf', 'atan2f', 'powf', 'malloc', 'free', 'rand',
        'srand', 'atoi', 'toupper', 'tolower'}


def region(addr):
    return next((n for a, b, n in REGIONS if a <= addr < b), None)


def asm_functions():
    """{name: vram} of every function splat wrote a .s for (decompiled or not)."""
    out = {}
    for p in glob.glob(os.path.join(ROOT, 'asm', 'nonmatchings', '**', '*.s'), recursive=True):
        m = ASM_ADDR.search(open(p, encoding='utf-8', errors='replace').read(4096))
        if m:
            out[os.path.splitext(os.path.basename(p))[0]] = int(m.group(1), 16)
    return out


def nm(paths, *flags):
    """{path: [symbol, ...]} via llvm-nm, in batches."""
    res = collections.defaultdict(list)
    for i in range(0, len(paths), 200):
        batch = paths[i:i + 200]
        cur = batch[0] if len(batch) == 1 else None
        for line in run(['llvm-nm', '-f', 'just-symbols', *flags] + batch).stdout.splitlines():
            if line.endswith(':') and line[:-1] in batch:
                cur = line[:-1]
            elif line and cur:
                res[cur].append(line)
    return res


FUNC_ADDR = re.compile(r'extern char (\w+)\[\] __asm__\("(\w+)"\);')
LOCAL_LABEL = re.compile(r'extern char (\w+)\[\] __asm__\("\.L([0-9A-Fa-f]{8})"\);')


def function_addresses(text, funcs):
    """The recompiler declares an address materialised from a code label as an array (a data
    reference). Where the label is a function, it is declared as one: its address is then the
    table index, as in the fixups, and the object's reference has the function's symbol type."""
    text = FUNC_ADDR.sub(lambda m: ('extern void %s(void) __asm__("%s");' % (m.group(1), m.group(2)))
                         if m.group(2) in funcs else m.group(0), text)
    # a local code label (.L<hex>) has no symbol in the module: its address is the PS2 address
    return LOCAL_LABEL.sub(lambda m: '#define %s ((char *)(unsigned)0x%s)' % (m.group(1), m.group(2)), text)


def recompile_objects(objs, rewrite_aliases, max_workers=8):
    """{name: asm} of the asm-only functions the objects need, recompiled to C and compiled."""
    recomp.OUT = RECOMP_OUT
    recomp.WASM_EMIT = True
    # the callee signatures come from the declarations in this checkout's sources (recomp.SRC): the raw
    # src/ tree, the declarations as the sources write them. Without them every callee defaults to
    # eight integer arguments. A cache from another source set is stale and is removed.
    # src/ with the port patches applied (apply_patches) and none of the wasm rewrites: the variadic
    # declarations of the wasm copies are not the functions' own signatures
    recomp.SRC = os.path.join(OUT, 'sigsrc')
    for path in glob.glob(os.path.join(SRC_ROOT, '**', '*.c*'), recursive=True):
        rel = os.path.relpath(path, SRC_ROOT).replace(os.sep, '/')
        write(os.path.join(recomp.SRC, rel), apply_patches(rel, open(path, encoding='utf-8', errors='replace').read()))
    for stale in ('signatures.json', 'signatures-version.txt'):
        if os.path.exists(os.path.join(RECOMP_OUT, stale)):
            os.remove(os.path.join(RECOMP_OUT, stale))
    kinds0 = symbol_kinds(objs)
    asm = asm_functions()
    in_data = set()
    for p in glob.glob(os.path.join(ROOT, 'asm', 'data', '*.s')):
        in_data |= set(re.findall(r'\bfunc_[0-9A-F]{8}\b', open(p, encoding='utf-8').read()))
    in_data |= set(recomp.data_entries())
    asm.update({n: e for n, (_, e) in recomp.data_entries().items()})
    names, out, failed, refused = set(), [], {}, {}
    with ThreadPoolExecutor(max_workers) as ex:
        while True:
            defined = {s for v in nm(objs + out, '--defined-only', '-g').values() for s in v}
            # port code may call a recompiled function at its register entry, rc_<name>
            wanted = in_data | {s.removeprefix('rc_') for v in nm(objs + out, '--undefined-only').values() for s in v}
            new = sorted(n for n in wanted - defined - names - LIBC - set(refused)
                         if n in asm and region(asm[n]) in ('game', 'engine', 'crt0', 'libgcc', 'sdk'))
            if not new:
                failed.update(refused)
                REFUSED.update(refused)
                return out, names, failed
            names |= set(new)
            while True:  # the scalar guard (scalar_abi.py) can refuse names: they stay undefined, the rest go on
                try:
                    paths, failures = recomp.recompile(sorted(names))
                    break
                except SystemExit as e:
                    bad = set(re.findall(r'(func_[0-9A-F]{8}): unreviewed', str(e)))
                    if not bad:
                        raise
                    for n in bad:
                        refused[n] = 'scalar input guard: its unit does not compile for wasm'
                    if not bad & names:  # a refused callee: the guard checks the callees too, so refuse its callers
                        bad = {n for n in names if any(op in ('jal', 'j') and a and a[0] in bad
                                                       for _, op, a in recomp.parse(n)[0])}
                        if not bad:
                            raise
                        for n in bad:
                            refused[n] = 'scalar input guard: calls a refused function'
                    names -= bad
                    print('scalar guard refused %d: %s' % (len(bad), ', '.join(sorted(bad))), flush=True)
            failed.update(failures)
            print('recompile step: %d names, %d files, %d failures' % (len(new), len(paths), len(failures)), flush=True)
            funcs = set(names) | {n for n, k in kinds0.items() if k == 'func'}
            for p in paths:  # the generated code declares data as numbers too
                write(p, rewrite(function_addresses(open(p, encoding='utf-8').read(), funcs), rewrite_aliases))
            jobs = [(os.path.basename(p), p, os.path.join(OBJ, 'recomp', os.path.basename(p) + '.o'),
                     ['-I', PORT_OUT + '/runtime', '-I', PORT_OUT + '/include', '-I', INC_OUT, '-I', SHIM])
                    for p in paths]
            for j in jobs:
                JOBS[j[2]] = (compile_c, j)
            results = list(ex.map(compile_c, jobs))
            bad = [r for r in results if r['status'] != 'ok']
            if bad:
                print('recompiled functions that failed to compile for wasm: %d' % len(bad))
            out = [os.path.join(OBJ, 'recomp', os.path.basename(p) + '.o') for p in paths
                   if os.path.exists(os.path.join(OBJ, 'recomp', os.path.basename(p) + '.o'))]


# ---- the link ---------------------------------------------------------------------------------

def builtins_lib():
    """compiler-rt's builtins from the wasi-sdk (the same code for every wasm32 OS)."""
    return os.path.normpath(os.path.join(WASI, '..', '..', 'lib', 'clang', '23', 'lib', 'wasm32-unknown-wasi',
                                         'libclang_rt.builtins.a'))


def wasm_link(objs, out, allow_all, host_syms):
    """The link with the code and data tables (state_trace.c's identities) of this module: each pass
    derives the rows from the module it linked, until the generated table is unchanged."""
    objs = [o for o in objs if not str(o).replace('\\', '/').endswith('/obj/data/codeids.c.o')]
    c = os.path.join(OUT, 'codeids.c')
    obj = c + '.o'

    def table(rows):
        map_path = os.path.join(OUT, 'dhd.map')
        write_table(rows, wasm_data_rows(map_path) if os.path.exists(map_path) else [], c,
                    wasm_fixup_source(os.path.join(DATA_OUT, 'fixups.c')))
    table([])
    for _ in range(3):
        r = run(['clang'] + BASE + [c, '-o', obj])
        if r.returncode:
            sys.exit(r.stderr)
        result = wasm_link_once(objs + [obj], out, allow_all, host_syms)
        if result.returncode:
            return result
        before = open(c, encoding='utf-8').read()
        table(wasm_code_rows(out))
        if before == open(c, encoding='utf-8').read():
            return result
    sys.exit('identity table did not settle')


def wasm_link_once(objs, out, allow_all, host_syms):
    rsp = os.path.join(OUT, 'objs.rsp')
    write(rsp, ''.join(o.replace(os.sep, '/') + '\n' for o in objs))
    cmd = ['wasm-ld', '--no-entry', '--no-stack-first', '--global-base=%d' % STACK_BASE,
           '-z', 'stack-size=%d' % STACK_SIZE, '--initial-memory=%d' % INITIAL_MEMORY,
           '--max-memory=%d' % MAX_MEMORY, '--export=__main_argc_argv', '--export=__stack_pointer',
           '--export-memory', '--export-table', '--gc-sections', '--allow-multiple-definition',
           '--allow-undefined-file=' + host_syms, '-o', out, '@' + rsp.replace(os.sep, '/'),
           # the state trace classifies a value as a code identity: a table index that no small integer equals
           '--table-base=%d' % TABLE_BASE, '--Map=' + os.path.join(OUT, 'dhd.map'),
           os.path.join(WASI, 'lib', 'wasm32-wasip1', 'libc.a'),
           os.path.join(WASI, 'lib', 'wasm32-wasip1', 'libwasi-emulated-process-clocks.a'), builtins_lib()]
    if allow_all:
        cmd.insert(1, '--allow-undefined')
    return run(cmd)


def wasm_objdump(path, *flags):
    """wasm-objdump's output. A run that hasn't ended after OBJDUMP_TIMEOUT seconds is killed and
    tried again: on Windows it can hang at start with no CPU used (a file scanner holding the new
    object), which stalled whole builds for most of an hour."""
    cmd = [os.path.join(WABT, 'bin', 'wasm-objdump')] + list(flags) + [path]
    for attempt in range(OBJDUMP_TRIES):
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             encoding='utf-8', errors='replace')
        try:
            out, err = p.communicate(timeout=OBJDUMP_TIMEOUT)
        except subprocess.TimeoutExpired:
            p.kill()
            try:
                p.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                pass  # a process that can't even be killed is left behind; the retry doesn't wait for it
            print('wasm-objdump: no answer in %d s on %s, try %d of %d'
                  % (OBJDUMP_TIMEOUT, os.path.basename(path), attempt + 1, OBJDUMP_TRIES), flush=True)
            continue
        if p.returncode == 0:
            return out
        print('wasm-objdump: exit %d on %s, try %d of %d: %s'
              % (p.returncode, os.path.basename(path), attempt + 1, OBJDUMP_TRIES, err.strip()[:200]), flush=True)
    sys.exit('wasm-objdump failed %d times on %s' % (OBJDUMP_TRIES, path))


OBJDUMP_TIMEOUT, OBJDUMP_TRIES = 120, 3


def show_failures(records, what, names=None):
    """Prints the failed units grouped by their first error, with the first unit of each group."""
    groups = collections.defaultdict(list)
    for i, r in enumerate(records):
        err = r['errors'][0][1] if r['errors'] else '?'
        kind = re.sub(r"'[^']*'", "'X'", re.sub(r'\d+', 'N', err))[:150]
        groups[kind].append((r['unit'], r['errors'][0][0] if r['errors'] else 0, err[:200]))
    print('%d %s failed:' % (len(records), what))
    for kind, items in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:25]:
        print('  %4d  %s   e.g. %s:%d %s' % (len(items), kind[:90], items[0][0], items[0][1], items[0][2][:110]))


# ---- signatures across objects, and ABI thunks ------------------------------------------------
#
# wasm-ld rejects a function that two objects define or declare with different signatures; i686
# does not care. On the retail MIPS game and on i686, a call passes its arguments in registers or
# on the stack, and the callee reads as many as it uses: extra arguments are never read, missing
# ones are whatever the register held, and a value the callee doesn't return is whatever the
# register held. So the wasm build reproduces the call through a generated thunk:
# func_X__as_<params>_<ret> takes the caller's signature, drops extra arguments, passes 0 for
# missing ones, returns 0 when the caller wants a value and the definition is void, and discards
# the value when the caller is void. The caller objects that disagree are rebuilt with
# -Dfunc_X=func_X__as_<tag>, so their calls go through the thunk; the definition is unchanged.

WASM_TYPE = {'i32': 'i', 'i64': 'l', 'f32': 'f', 'f64': 'd'}
C_TYPE = {'i32': 'int', 'i64': 'long long', 'f32': 'float', 'f64': 'double'}
SECTION = re.compile(r'^(\w+)\[\d+\]:\s*$')
TYPE_LINE = re.compile(r'^\s*- type\[(\d+)\] \(([^)]*)\) -> (\S+)\s*$')
FUNC_LINE = re.compile(r'^\s*- func\[\d+\] sig=(\d+) <([^>]+)>(?: <- (\S+))?\s*$')
FUNC_DEF = re.compile(r'^[A-Za-z_][^\n;{}()]*?\b(func_[0-9A-F]{8})\s*\([^;]*\)\s*\{', re.M)
JOBS = {}      # object path -> (compile function, its base job): how to rebuild it with extra flags
REFUSED = {}   # recompiled names the scalar guard refused (recompile_objects fills it)


class ObjectSignatures:
    """object_signatures' answers by the object's SHA-256, kept across builds and checkouts in
    CORE_CACHE/object-signatures.json (finding 151): an object of the same bytes has the same
    signatures, and a build reads those of about 4,500 objects (one wasm-objdump each: 5,000
    processes, a third of a rebuild's time when every object came from the object cache)."""
    def __init__(self):
        self.path = CORE_CACHE / 'object-signatures.json'
        self.lock = threading.Lock()
        try:
            self.known = json.loads(self.path.read_text())
        except (OSError, ValueError):
            self.known = {}
        self.new = {}
        # the answers also depend on the parser and on wasm-objdump: both are in every key, so a
        # branch that changes either one gets its own entries
        import inspect
        tool = hashlib.sha256(inspect.getsource(object_signatures_uncached).encode() +
                              repr([(r.pattern, r.flags) for r in (SECTION, TYPE_LINE, FUNC_LINE)]).encode())
        tool.update(subprocess.run([os.path.join(WABT, 'bin', 'wasm-objdump'), '--version'],
                                   capture_output=True).stdout)
        self.tool = tool.digest()

    def get(self, path):
        key = hashlib.sha256(self.tool + open(path, 'rb').read()).hexdigest()
        found = self.known.get(key) or self.new.get(key)
        if found is None:
            defs, imps = object_signatures_uncached(path)
            with self.lock:
                self.new[key] = [{n: [list(p), r] for n, (p, r) in d.items()} for d in (defs, imps)]
            return defs, imps
        return tuple({n: (tuple(p), r) for n, (p, r) in d.items()} for d in found)

    def save(self):
        if not self.new:
            return
        try:
            try:
                known = json.loads(self.path.read_text())
            except (OSError, ValueError):
                known = {}
            if len(known) > 100000:  # objects of sources long gone: start again
                known = {}
            known.update(self.new)
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_name('object-signatures.json.%d.tmp' % os.getpid())
            tmp.write_text(json.dumps(known))
            os.replace(tmp, self.path)
        except OSError:
            pass


SIGNATURES = []  # the build's ObjectSignatures, made on first use
SIGNATURES_LOCK = threading.Lock()


def object_signatures(path):
    """({defined name: sig}, {imported name: sig}) of a wasm object; sig = (param types, result or
    None). Remembered by the object's bytes (ObjectSignatures)."""
    with SIGNATURES_LOCK:
        if not SIGNATURES:
            SIGNATURES.append(ObjectSignatures())
    return SIGNATURES[0].get(path)


def object_signatures_uncached(path):
    types, defs, imps, section = {}, {}, {}, None
    for line in wasm_objdump(path, '-x').splitlines():
        m = SECTION.match(line)
        if m:
            section = m.group(1)
            continue
        if section == 'Type':
            t = TYPE_LINE.match(line)
            if t:
                params = tuple(p.strip() for p in t.group(2).split(',') if p.strip())
                types[int(t.group(1))] = (params, None if t.group(3) == 'nil' else t.group(3))
        elif section in ('Function', 'Import'):
            f = FUNC_LINE.match(line)
            if not f:
                continue
            sig = types[int(f.group(1))]
            if section == 'Import':
                imps[f.group(2).split('.', 1)[1]] = sig  # env.NAME
            else:
                defs[f.group(2)] = sig
    return defs, imps


def sig_tag(sig):
    params, ret = sig
    return ''.join(WASM_TYPE[p] for p in params) + '_' + ('v' if ret is None else WASM_TYPE[ret])


def thunk_source(name, csig, dsig):
    """C for func_X__as_<csig>: the caller's signature, forwarding to the definition's. A void
    definition returns 0 to a caller that reads a value."""
    cp, cr = csig
    dp, dr = dsig
    params = ', '.join('%s a%d' % (C_TYPE[t], k) for k, t in enumerate(cp)) or 'void'
    ret = 'void' if cr is None else C_TYPE[cr]
    dparams = ', '.join(C_TYPE[t] for t in dp) or 'void'
    dret = 'void' if dr is None else C_TYPE[dr]
    args = ['a%d' % k if k < len(cp) else '0' for k in range(len(dp))]
    call = '%s(%s)' % (name, ', '.join(args))
    if cr is None:
        body = ['    %s;' % call]
    elif dr is None:
        body = ['    %s;' % call, '    return 0;']
    else:
        body = ['    return (%s)%s;' % (ret, call)]
    return '\n'.join(['extern %s %s(%s);' % (dret, name, dparams),
                      '%s %s__as_%s(%s) {' % (ret, name, sig_tag(csig), params)] + body + ['}', ''])


def handling(csig, dsig):
    cp, cr = csig
    dp, dr = dsig
    parts = []
    if len(cp) > len(dp):
        parts.append('drop %d extra argument(s)' % (len(cp) - len(dp)))
    if len(dp) > len(cp):
        parts.append('pass 0 for %d missing argument(s)' % (len(dp) - len(cp)))
    if cr is not None and dr is None:
        parts.append('return 0: caller reads a value the definition does not return')
    if cr is None and dr is not None:
        parts.append('discard the definition result')
    return '; '.join(parts) or 'same arity and result'


def call_uses(src_path, name):
    """(statement calls, line numbers of other uses) of name in a unit's C source. A call used as a
    statement discards its result. Declarations and comments are not calls."""
    pat = re.compile(r'\b%s\s*\(' % re.escape(name))
    stmt, uses = 0, []
    for n, line in enumerate(open(src_path, encoding='utf-8', errors='replace').read().splitlines(), 1):
        s = line.strip()
        if not pat.search(s) or s.startswith(('extern', 'static', '//', '*')):
            continue
        if re.match(r'%s\s*\(' % re.escape(name), s) and s.endswith(';'):
            stmt += 1
        else:
            uses.append(n)
    return stmt, uses


def rebuild_with(obj, renames):
    """The object again from a copy of its source in which each identifier in renames (a dict: name
    -> new name) is replaced as a whole word. A word match also covers asm("name") labels, which a
    -D macro can't reach."""
    fn, job = JOBS[obj]
    src = job[1]
    text = open(src, encoding='utf-8', errors='replace').read()
    for name, new in renames.items():
        text = re.sub(r'\b%s\b' % re.escape(name), new, text)
    base, ext = os.path.splitext(src)
    copy = base + '.abi' + ext  # same directory: the quoted includes still resolve
    write(copy, text)
    # the -D macros reach the names declared in headers the unit includes (the source rename can't)
    extra = ['-D%s=%s' % (name, new) for name, new in renames.items()]
    if fn is compile_src:
        return compile_src((job[0], copy, job[2], job[3], extra))
    return compile_c((job[0], copy, job[2], job[3] + extra))


# The functions no unit defines and the recompiler can't give: SDK functions it can't translate
# (syscalls, COP0) and labels inside SDK code that the data and the recompiled code name. The game
# never runs them: each is a trap stub (the retired 32-bit link stubbed the same names). Any other
# name nothing defines stays fatal (the strict link).
MISSING = {'_ri0_000', '_ri0_001', '_ri0_010', '_ri0_011', '_ri0_100', '_ri0_101', '_ri0_110', '_ri0_111',
           '_rix_000', '_rix_001', '_rix_010', '_rix_011', '_rix_100', '_rix_101', '_rix_110', '_rix_111',
           'func_0024FFD8', 'func_0025A650', 'func_0025AA80', 'func_0025AAA0', 'func_0025F940',
           'kCopy', 'kFindAddress'}


def stub_source(names):
    out = ['/* Functions of units that do not compile for wasm (their bodies are not in the module).',
           '   A call traps (unreachable) in the function named after the stub. */']
    for n in names:
        out.append('__attribute__((noinline)) void ps2_trap_%s(void) { __builtin_trap(); }' % n)
        out.append('int %s(void) { ps2_trap_%s(); return 0; }' % (n, n))
    return '\n'.join(out) + '\n'


LIBC_A = os.path.join(WASI, 'lib', 'wasm32-wasip1', 'libc.a')


def libc_signatures(names):
    """{name: sig} for the names libc.a defines: the prototype of the library's own member object."""
    members, out = {}, {}
    for line in run(['llvm-nm', '-A', '--defined-only', LIBC_A]).stdout.splitlines():
        m = re.match(r'^.*:([^:]+): [0-9a-fA-F]+ [TtWw] (\S+)$', line)
        if m and m.group(2) in names:
            members.setdefault(m.group(2), m.group(1))
    tmp = os.path.join(OUT, 'abi', 'libc')
    os.makedirs(tmp, exist_ok=True)
    for member in sorted(set(members.values())):
        run(['llvm-ar', 'x', LIBC_A, member], cwd=tmp)
    for name, member in members.items():
        out[name] = object_signatures(os.path.join(tmp, member))[0].get(name)
    return {n: s for n, s in out.items() if s is not None}


def abi_stage(objs, rc_objs, failed_units, max_workers=8):
    """abi_stage_core, with the adapters that the C++ renames asked for (INTRA_THUNKS) compiled into
    the objects, until the set of adapters is stable."""
    c = os.path.join(OUT, 'abi', 'intra.c')
    obj = os.path.join(OBJ, 'abi', 'intra.c.o')
    for _ in range(4):
        keys = set(INTRA_THUNKS)
        if keys:
            write(c, '\n'.join(thunk_source(name, csig, dsig) for (name, csig), dsig in sorted(INTRA_THUNKS.items())))
            job = ('intra-object C++ adapters', c, obj, [])
            r = compile_c(job)
            assert r['status'] == 'ok', r
            JOBS[obj] = (compile_c, job)
            if obj not in objs:
                objs.append(obj)
        result = abi_stage_core(objs, rc_objs, failed_units, max_workers)
        if keys == set(INTRA_THUNKS):
            return result
    sys.exit('intra-object adapters did not settle')


def abi_stage_core(objs, rc_objs, failed_units, max_workers=8):
    """Stubs for the uncompiled functions, then the signature table, the rebuilt callers and the
    thunks. Returns the extra objects to link, and the table."""
    all_objs = objs + rc_objs
    with ThreadPoolExecutor(max_workers) as ex:
        sigs = dict(zip(all_objs, ex.map(object_signatures, all_objs)))
    defined = {n for d, _ in sigs.values() for n in d}
    stubs = set(REFUSED) - defined
    stubs |= MISSING - defined
    for unit in failed_units:
        if unit.startswith('src/'):
            text = open(os.path.join(ROOT, unit), encoding='utf-8', errors='replace').read()
            stubs |= set(FUNC_DEF.findall(text)) - defined
    stub_c, stub_o = os.path.join(OUT, 'abi', 'stubs.c'), os.path.join(OBJ, 'abi', 'stubs.c.o')
    write(stub_c, stub_source(sorted(stubs)))
    r = compile_c(('abi stubs', stub_c, stub_o, []))
    if r['status'] != 'ok':
        sys.exit('stub object: %s' % r['errors'])
    sigs[stub_o] = object_signatures(stub_o)
    libc_sigs = libc_signatures({n for o in sigs for n in sigs[o][1] if n not in defined})

    def conflicts():
        defs = {}
        for o in sigs:
            for n, s in sigs[o][0].items():
                defs.setdefault(n, (s, o))
        # a name no object defines (an import: host or uncompiled SDK code) keeps the signature most
        # of its callers use; the callers that differ go through a thunk to it
        refs = collections.defaultdict(collections.Counter)
        for o in sigs:
            for n, s in sigs[o][1].items():
                if n not in defs:
                    refs[n][s] += 1
        for n, counts in refs.items():
            defs[n] = (libc_sigs[n] if n in libc_sigs else counts.most_common(1)[0][0], None)
        rows = []
        for o in sigs:
            for n, s in sigs[o][1].items():
                if n in defs and n not in sigs[o][0] and s != defs[n][0]:
                    rows.append((n, o, s, defs[n][0], defs[n][1]))
        return rows

    rows = conflicts()
    print('signature conflicts: %d caller(s) on %d function(s); %d stub(s)' %
          (len(rows), len({r[0] for r in rows}), len(stubs)), flush=True)
    renames = collections.defaultdict(dict)
    thunks = {}
    for name, caller, csig, dsig, dobj in rows:
        renames[caller][name] = '%s__as_%s' % (name, sig_tag(csig))
        thunks[(name, csig)] = dsig
    for caller, names in renames.items():
        if caller not in JOBS:
            sys.exit('cannot rebuild %s: no recorded job' % caller)
        r = rebuild_with(caller, names)
        if r['status'] != 'ok':
            print('rebuild failed: %s: %s' % (caller, r['errors'][:1]), flush=True)
        sigs[caller] = object_signatures(caller)
    thunk_c, thunk_o = os.path.join(OUT, 'abi', 'thunks.c'), os.path.join(OBJ, 'abi', 'thunks.c.o')
    write(thunk_c, '\n'.join(thunk_source(n, c, d) for (n, c), d in sorted(thunks.items(), key=lambda kv: (kv[0][0], sig_tag(kv[0][1])))))
    r = compile_c(('abi thunks', thunk_c, thunk_o, []))
    if r['status'] != 'ok':
        sys.exit('thunk object: %s' % r['errors'])
    sigs[thunk_o] = object_signatures(thunk_o)
    left = conflicts()
    print('signature conflicts after the thunks: %d' % len(left), flush=True)

    # the table: one row per caller that differs, with what the thunk does and whether the value is used
    table = ['function\tcaller\tcaller_sig\tdefinition_sig\tdefinition_object\tthunk\thandling\tcaller_reads_value']
    for name, caller, csig, dsig, dobj in sorted(rows):
        reads = 'n/a'
        src_dir = os.path.join(OBJ, 'src') + os.sep
        if csig[1] is not None and dsig[1] is None:
            if caller.startswith(src_dir):
                rel = os.path.relpath(caller, os.path.join(OBJ, 'src'))[:-2]
                stmt, uses = call_uses(os.path.join(SRC_ROOT, rel), name)
                reads = ('discarded (%d statement call(s))' % stmt) if not uses else \
                    'CHECK: other use at line(s) %s' % ','.join(map(str, uses[:12]))
            else:
                reads = 'CHECK: not a src unit'
        sig_text = lambda s: '(%s)->%s' % (','.join(s[0]), s[1] or 'void')
        where = os.path.relpath(dobj, OBJ) if dobj else (
            'libc.a member' if name in libc_sigs else 'no definition (import; canonical caller signature)')
        table.append('\t'.join([name, os.path.relpath(caller, OBJ), sig_text(csig), sig_text(dsig),
                                where, '%s__as_%s' % (name, sig_tag(csig)),
                                handling(csig, dsig), reads]))
    write(os.path.join(OUT, 'abi_thunks.tsv'), '\n'.join(table) + '\n')
    if SIGNATURES:
        SIGNATURES[0].save()
    return {'objs': [stub_o, thunk_o], 'stubs': sorted(stubs), 'thunks': len(thunks), 'rows': len(rows),
            'left': len(left), 'table': table}


# ---- the core cache: a finished module reused when none of its inputs changed -----------------
# Every checkout (worktrees too) builds the same module from the same inputs again and again (a
# reference build, a change to docs or to the Rust renderer); the object cache saves the compiles
# but not the rest (about 10 minutes). DHD_MODULE_CACHE=0 turns it off.
CORE_CACHE = DEVCACHES / 'dhd-modulecache'
CORE_CACHE_KEEP = 12
# what a hit puts back in build/wasm: the module and the files later steps and tools read
CORE_CACHE_FILES = ['dhd_core.wasm', 'dhd.map', 'census.json', 'link.txt', 'postlink.json', 'offset_scan.json',
                    'host_api.syms', 'abi_thunks.tsv', 'recompiled.txt', 'recompiled-with-c.txt',
                    'ps2asm_aot.inc', 'vu0_aot.inc', 'vu0_native_words.inc']
CORE_CACHE_DIRS = ['vu1aot']
# tracked files that can't change the module (everything else in the checkout counts)
NOT_CORE = ('docs/', 'online/', '.github/', 'port/launcher/', 'port/render/', 'port/web/', 'port/android/',
            'port/release/')
# where a new, not yet added file can change the module (the sources, the mirrored port folders, the tools)
CORE_NEW_FILES = ('src/', 'include/', 'port/runtime/', 'port/hle/', 'port/include/', 'port/host/', 'port/gs/',
                  'port/net/', 'port/tools/', 'port/patches')
# the environment the build reads (the tools' folders are in the key by their resolved paths)
CORE_ENV = ('DHD_VUTEXT', 'WASI_SYSROOT', 'SIMDE_DIR', 'WABT_DIR')


def core_inputs_key():
    """A hash of everything the module is built from: the checkout's files (tracked, and new ones
    git doesn't ignore yet, with their working-tree content) except NOT_CORE and Markdown, the
    generated asm/ and assets/, the tools' versions and the environment that points at inputs. New
    files count only in the folders the module is built from (CORE_NEW_FILES), so a checkout's own
    scratch files don't change the key; nor do the run-time DHD_ variables (only CORE_ENV)."""
    h = hashlib.sha256(b'dhd-module-cache 3\n')
    def git_files(*args):
        out = subprocess.run(['git', 'ls-files', '-z'] + list(args), cwd=ROOT, capture_output=True).stdout
        return [n for n in out.decode('utf-8', 'replace').split('\0') if n]
    names = {n for n in git_files('--cached') if not n.startswith(NOT_CORE) and not n.endswith('.md')}
    names |= {n for n in git_files('--others', '--exclude-standard') if n.startswith(CORE_NEW_FILES)}
    for d in ('asm', 'assets'):
        for p in glob.glob(os.path.join(ROOT, d, '**', '*'), recursive=True):
            if os.path.isfile(p):
                names.add(os.path.relpath(p, ROOT).replace(os.sep, '/'))
    memo = FileHashes()
    for n in sorted(names):
        p = os.path.join(ROOT, n)
        if os.path.isfile(p):
            # line endings as git stores them: checkouts differ (core.autocrlf), the module doesn't
            h.update(n.encode() + b'\0' + memo.sha256(p, lf=True))
    for k in CORE_ENV:
        h.update(('%s=%s\n' % (k, os.environ.get(k, ''))).encode())
    # the inputs outside the checkout by their content, not only their place: an external
    # DHD_VUTEXT (vu1_aot.py reads it), SIMDe's headers, the WASI sysroot's headers and libraries
    vutext = os.environ.get('DHD_VUTEXT')
    if vutext and os.path.isfile(vutext):
        h.update(b'vutext\0' + memo.sha256(vutext))
    for base, patterns in ((SIMDE, ('**/*.h',)), (WASI, ('include/**/*.h', 'lib/**/*.a'))):
        for pattern in patterns:
            for p in sorted(glob.glob(os.path.join(base, pattern), recursive=True)):
                h.update(os.path.relpath(p, base).encode() + b'\0' + memo.sha256(p))
    h.update(('%s|%s|%s\n' % (WASI, SIMDE, WABT)).encode())
    h.update(run(['clang', '--version']).stdout.encode())
    memo.save()
    return h.hexdigest()


class FileHashes:
    """Files' SHA-256 remembered across builds by path, size, modification time and file id
    (CORE_CACHE/file-hashes.json): the key re-reads only files changed since (finding 151: the
    WASI sysroot and SIMDe alone are 272 MB, 5 s of every build, hits included). lf: the hash of
    the text with CRLF as LF."""
    def __init__(self):
        self.path = CORE_CACHE / 'file-hashes.json'
        try:
            self.known = json.loads(self.path.read_text())
        except (OSError, ValueError):
            self.known = {}
        self.changed = False

    def sha256(self, path, lf=False):
        if lf:  # the checkout's own files are always read: an edit can keep size and mtime (and on
            # Windows st_ctime is the creation time), and they are a small part of the bytes
            return hashlib.sha256(open(path, 'rb').read().replace(b'\r\n', b'\n')).digest()
        st = os.stat(path)
        name = os.path.normcase(os.path.abspath(path))
        # ctime too (POSIX: the change time): a copy that puts the old mtime back still changes it.
        # These are the toolchain trees (WASI sysroot, SIMDe), replaced whole by an update.
        stamp = [st.st_size, st.st_mtime_ns, st.st_ino, st.st_ctime_ns]
        entry = self.known.get(name)
        if entry and entry[:4] == stamp:
            return bytes.fromhex(entry[4])
        data = open(path, 'rb').read()
        digest = hashlib.sha256(data.replace(b'\r\n', b'\n') if lf else data).digest()
        self.known[name] = stamp + [digest.hex()]
        self.changed = True
        return digest

    def save(self):
        if not self.changed:
            return
        try:  # merged with what other checkouts stored meanwhile; a lost race only costs a re-read
            try:
                known = json.loads(self.path.read_text())
            except (OSError, ValueError):
                known = {}
            known.update(self.known)
            if len(known) > 100000:  # files of removed checkouts
                known = {k: v for k, v in known.items() if os.path.exists(k.removeprefix('lf:'))}
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_name('file-hashes.json.%d.tmp' % os.getpid())
            tmp.write_text(json.dumps(known))
            os.replace(tmp, self.path)
        except OSError:
            pass


def core_cache_get(key):
    entry = CORE_CACHE / key
    if os.environ.get('DHD_MODULE_CACHE') == '0' or not (entry / 'dhd_core.wasm').is_file():
        return False
    os.makedirs(OUT, exist_ok=True)
    for f in CORE_CACHE_FILES:
        if (entry / f).is_file():
            shutil.copy2(entry / f, os.path.join(OUT, f))
    for d in CORE_CACHE_DIRS:
        if (entry / d).is_dir():
            shutil.rmtree(os.path.join(OUT, d), ignore_errors=True)
            shutil.copytree(entry / d, os.path.join(OUT, d))
    os.utime(entry)
    image = open(WASM, 'rb').read()
    print('core cache: hit %s, %s (%d bytes, sha256 %s); the intermediate files in build/wasm are not '
          'refreshed (DHD_MODULE_CACHE=0 for a full build)' % (key[:16], WASM, len(image), hashlib.sha256(image).hexdigest()),
          flush=True)
    return True


def core_cache_put(key):
    if os.environ.get('DHD_MODULE_CACHE') == '0':
        return
    try:
        tmp = CORE_CACHE / (key + '.tmp%d' % os.getpid())
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        for f in CORE_CACHE_FILES:
            if os.path.isfile(os.path.join(OUT, f)):
                shutil.copy2(os.path.join(OUT, f), tmp / f)
        for d in CORE_CACHE_DIRS:
            if os.path.isdir(os.path.join(OUT, d)):
                shutil.copytree(os.path.join(OUT, d), tmp / d)
        if (CORE_CACHE / key).exists():
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            os.replace(tmp, CORE_CACHE / key)
        entries = sorted((p for p in CORE_CACHE.iterdir() if p.is_dir() and '.tmp' not in p.name),
                         key=lambda p: p.stat().st_mtime, reverse=True)
        for old in entries[CORE_CACHE_KEEP:]:
            shutil.rmtree(old, ignore_errors=True)
        print('core cache: stored %s' % key[:16], flush=True)
    except OSError as e:
        print('core cache: not stored (%s)' % e, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=8)
    a = ap.parse_args()
    t0 = time.time()
    key = core_inputs_key()
    if core_cache_get(key):
        print('done in %.0f s' % (time.time() - t0))
        return
    import build_slots
    with build_slots.slot('build_wasm'):
        build(a, t0, key)


def build(a, t0, key):
    for d in (OUT, SHIM, OBJ):
        os.makedirs(d, exist_ok=True)
    shim_headers()
    HEADER_SHIM[0] = shim_ee_float()
    host_syms = os.path.join(OUT, 'host_api.syms')
    write(host_syms, '\n'.join(host_api_names()) + '\n')

    # 1. the data image and the aliases (the declarations' names come from the sources first)
    scan = scan_sources()
    aliases, fixups, offsets = image_stage(declared_data_names(scan))
    print('image: %d fixups; %d aliases of code symbols' % (sum(len(o) for _, o in fixups), len(aliases)), flush=True)

    # 2. mirror of port/ (rewritten), transformed game headers
    transform.port_headers(os.path.join(ROOT, 'include'), INC_OUT)
    for p in glob.glob(os.path.join(INC_OUT, '**', '*.h'), recursive=True):
        write(p, rewrite(open(p, encoding='utf-8', errors='replace').read(), aliases))
    mirror = []
    for d in MIRROR_DIRS:
        for p in glob.glob(os.path.join(PORT, d, '**', '*'), recursive=True):
            if not os.path.isfile(p):
                continue
            rel = os.path.relpath(p, PORT)
            dst = os.path.join(PORT_OUT, rel)
            if os.path.splitext(p)[1] in ('.c', '.h', '.inc', '.cpp'):
                write(dst, rewrite(open(p, encoding='utf-8', errors='replace').read(), aliases))
            else:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(p, dst)
            mirror.append(dst)

    # 3. game units (transform with an empty prefix)
    def transform_unit(path):
        rel = os.path.relpath(path, SRC_ROOT).replace(os.sep, '/')
        cpp = not path.endswith('.c')
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
            text = transform.fix_abi_calls(text, rel, None)
            text = apply_patches(rel, text)
            if rel in WASM_VA_LIST:  # wasm32's __builtin_va_list is void *, i686's is char *: the builtin's own type
                text = text.replace('typedef char *va_list;', 'typedef __builtin_va_list va_list;')
            for name in C_DEFINITIONS.get(rel, ()):  # per unit: a C++ definition other units call by its C name
                text = re.sub(r'(?m)^(\w[\w\s\*]*?\s+)(%s)\s*\(' % name, r'extern "C" \1\2(', text)
            text = transform.transform(cpp_linkage(text) if cpp else text, cpp, '', IMAGE)
        except Exception as e:  # noqa: BLE001 (reported, the build goes on)
            return rel, None, cpp, str(e)
        dst = os.path.join(SRC_OUT, rel)
        write(dst, '#include "port.h"\n#line 1 "src/%s"\n%s' % (rel, rewrite(fix_function_arrays(text), aliases)))
        return rel, dst, cpp, None
    srcs = sorted(glob.glob(os.path.join(SRC_ROOT, '**', '*.c*'), recursive=True))
    with ThreadPoolExecutor(a.j) as ex:
        transformed = list(ex.map(transform_unit, srcs))
    records = [{'unit': 'src/' + rel, 'status': 'failed', 'stage': 'transform', 'errors': [(0, err)]}
               for rel, dst, cpp, err in transformed if dst is None]
    jobs = [(rel, dst, os.path.join(OBJ, 'src', rel + '.o'), cpp) for rel, dst, cpp, err in transformed if dst]
    print('transform: %d game units, %d failed' % (len(transformed), len(records)), flush=True)
    with ThreadPoolExecutor(a.j) as ex:
        for j in jobs:
            JOBS[j[2]] = (compile_src, j)
        records += list(ex.map(compile_src, jobs))
    src_objs = [j[2] for j, r in zip(jobs, records[len(records) - len(jobs):]) if r['status'] == 'ok']
    print('game units compiled: %d of %d' % (len(src_objs), len(srcs)), flush=True)
    show_failures([r for r in records if r['status'] != 'ok'], 'game units')

    # 4. module-side port units (from the mirror)
    port_jobs, skipped = [], []
    # The native VU0 routines (vu0_native.h, finding 066): the module runs VU0, so it takes their word
    # arrays from the VU1 AOT include (port/tools/vu1_aot.py, made here from assets/vutext.bin), not
    # the VU1 blocks, which run on the host.
    vu1_aot_inc = os.path.join(OUT, 'vu1aot', 'vu1_aot.inc')
    print('VU1 AOT: %d straight-line blocks' % vu1_aot.generate(vu1_aot_inc), flush=True)
    __import__('vu0_native_words').derive(vu1_aot_inc, os.path.join(OUT, 'vu0_native_words.inc'))
    # The VU0 microprograms compiled to C (port/tools/vu0_aot.py, finding 090 SP1), for the entries
    # without a native routine: the module runs every vcallms.
    print('VU0 compiled: %d entries' % len(__import__('vu0_aot').generate(os.path.join(OUT, 'vu0_aot.inc'))), flush=True)
    vu0_native = ['-DPS2_VU0_NATIVE', '-DPS2_VU0_COMPILED', '-I', OUT]
    for rel in MODULE_UNITS:
        src = os.path.join(PORT_OUT, rel)
        extra = vu0_native if rel == 'runtime/vu0_interp.c' else []
        # The port units take the compiler's builtins (only the game units are built with
        # -fno-builtin). Without them every memcpy(&q, p, 16), fabsf and sqrtf of the VU and EE
        # interpreters is a call into wasi-libc's byte/word loop. The two units that define libc
        # functions themselves keep -fno-builtin.
        if rel not in ('runtime/libc_shims.c', 'runtime/wasi_adapter.c'):
            extra = extra + ['-fbuiltin']
        port_jobs.append((rel, src, os.path.join(OBJ, 'port', rel + '.o'), port_include(None) + extra))
    with ThreadPoolExecutor(a.j) as ex:
        for j in port_jobs:
            JOBS[j[2]] = (compile_c, j)
        port_res = list(ex.map(lambda j: compile_c((j[0], j[1], j[2], j[3])), port_jobs))
    port_objs = [j[2] for j, r in zip(port_jobs, port_res) if r['status'] == 'ok']
    skipped = [(j[0], r) for j, r in zip(port_jobs, port_res) if r['status'] != 'ok']
    records += [{'unit': 'port/' + j[0], 'status': r['status'], 'errors': r['errors'], 'stage': r['stage']}
                for j, r in zip(port_jobs, port_res)]
    print('module port units: %d compiled, %d skipped' % (len(port_objs), len(skipped)), flush=True)
    show_failures([r for r in port_res if r['status'] != 'ok'], 'module port units', names=[j[0] for j in port_jobs])

    # 5. recompiled asm-only functions, to a fixed point over the objects
    objs = src_objs + port_objs
    rc_objs, rc_names, rc_failed = recompile_objects(objs, aliases, a.j)
    print('recompiled functions: %d (%d failed in the recompiler)' % (len(rc_names), len(rc_failed)), flush=True)

    # 5a. ps2asm.c again, with generated code for every inline asm template and recompiled COP2
    # instruction now in the sources (ps2asm_aot.py): the interpreter's own per-instruction code
    # with the decoding folded away, used only for a byte-identical parse. Its symbols do not
    # change, so the recompiler's fixed point above stays valid.
    found, sequences = __import__('ps2asm_aot').generate(os.path.join(OUT, 'ps2asm_aot.inc'),
                                                         [SRC_OUT, INC_OUT, RECOMP_OUT])
    asm_job = next(j for j in port_jobs if j[0] == 'runtime/ps2asm.c')
    asm_job = (asm_job[0], asm_job[1], asm_job[2], asm_job[3] + ['-DPS2_ASM_AOT', '-I', OUT])
    JOBS[asm_job[2]] = (compile_c, asm_job)
    asm_res = compile_c(asm_job)
    if asm_res['status'] != 'ok':
        sys.exit('runtime/ps2asm.c with ps2asm_aot.inc: %s' % asm_res['errors'])
    print('inline asm: %d templates, %d generated instruction sequences' % (found, sequences), flush=True)
    write(os.path.join(OUT, 'recompiled.txt'), '\n'.join(sorted(rc_names)) + '\n')
    # A game function a unit defines in C runs that C: none may be recompiled from its MIPS because
    # its unit failed to compile for wasm, or because its C++ definition kept a mangled name (one
    # wasm_retail_names.py did not give its retail name).
    with_c = set()
    for r in records:
        if r['status'] != 'ok' and r['unit'].startswith('src/'):
            with_c |= set(FUNC_DEF.findall(open(os.path.join(ROOT, r['unit']), encoding='utf-8', errors='replace').read()))
    mangled = sorted({s for v in nm(src_objs, '--defined-only').values() for s in v if s.startswith('_Z')})
    if mangled:
        demangled = run(['llvm-cxxfilt'], input=''.join(s + '\n' for s in mangled)).stdout.splitlines()
        with_c |= {n for n in map(wasm_retail_names.retail_name, demangled) if n}
    ran_mips = sorted(set(rc_names) & with_c)
    write(os.path.join(OUT, 'recompiled-with-c.txt'), '\n'.join(ran_mips) + '\n')
    if ran_mips:
        sys.exit('recompiled although a game unit defines them in C (%d): %s' % (len(ran_mips), ' '.join(ran_mips)))

    # 5b. signatures across objects: stubs, then thunks for the callers that disagree
    abi = abi_stage(objs, rc_objs, [r['unit'] for r in records if r['status'] != 'ok'], a.j)
    abi_objs = abi['objs']

    # 6. the fixups, once the functions and the data names of every object are known
    kinds = symbol_kinds(objs + rc_objs)
    fx_src, nfix = fixup_source(fixups, offsets, aliases, kinds)
    fx_path = os.path.join(DATA_OUT, 'fixups.c')
    write(fx_path, fx_src)
    fx_obj = os.path.join(OBJ, 'data', 'fixups.c.o')
    os.makedirs(os.path.dirname(fx_obj), exist_ok=True)
    p = run(['clang'] + BASE + [fx_path, '-o', fx_obj])
    if p.returncode:
        sys.exit(p.stderr)

    # 7. the link: strict first (imports: host_api.h only), then a diagnostic link that allows the rest
    all_objs = objs + rc_objs + abi_objs + [fx_obj]  # wasm_link adds the code and data tables (codeids.c)
    strict = wasm_link(all_objs, WASM, False, host_syms)
    print('strict link: %s' % ('ok' if strict.returncode == 0 else 'FAILED'), flush=True)
    link_txt = strict.stdout + strict.stderr
    unresolved = collections.defaultdict(set)
    for name, refs in re.findall(r'undefined symbol: (\S+)\n((?:>>> .*\n?)*)', strict.stderr):
        unresolved[name] |= set(re.findall(r'>>> referenced by .*?\(([^)]+)\)', refs) or ['?'])
    if strict.returncode:
        diag = wasm_link(all_objs, WASM, True, host_syms)
        link_txt += '\n--- diagnostic link (--allow-undefined) ---\n' + diag.stdout + diag.stderr
        if diag.returncode:
            sys.exit(diag.stderr)
    write(os.path.join(OUT, 'link.txt'), link_txt)
    # the indirect-call fix (wasm_postlink.py): one shadow table per call type, cdecl-slot adapters; before wasm2c
    __import__('wasm_postlink').postlink(WASM, WASM, report=os.path.join(OUT, 'postlink.json'))
    # every data name must resolve to its PS2 address: an undefined one links to 0 (the diagnostic link
    # allows it as an import, the strict link reports it), so either one fails the build
    unresolved = sorted(set(re.findall(r'undefined symbol: ((?:A_)?D_\w+|_gp)\b', link_txt)))
    unresolved += sorted({m.group(1) for line in wasm_objdump(WASM, '-x').splitlines()
                          for m in [re.search(r'<-\s*\S+\.((?:A_)?D_[0-9A-Fa-f]+|_gp)\s*$', line)] if m})
    if unresolved:
        sys.exit('data names not resolved to their PS2 addresses (%d): %s' % (len(unresolved), ' '.join(unresolved[:20])))
    # a load/store offset that wraps in 32 bits on i686 but not in wasm (p[0x3fffffff] is p - 4 there,
    # 4 GiB past p here: commit edf2dc201), or that points at no guest memory, fails the build
    if __import__('wasm_offset_scan').run(WASM, report=os.path.join(OUT, 'offset_scan.json')):
        sys.exit('wasm offset scan: fix the source (port/patches.py) or allow the hit in wasm_offset_scan.ALLOW')
    # the module's identity for online rooms (online.c's hle_core_id): its hash written into it last,
    # so a build of this module on any platform carries the same one
    image = open(WASM, 'rb').read()
    mark = b'@@DHD_CORE_ID@@@'
    if image.count(mark) != 1:
        sys.exit('the module holds %d copies of online.c\'s core id mark (one expected)' % image.count(mark))
    core_id = hashlib.sha256(image).hexdigest()[:16].encode()
    open(WASM, 'wb').write(image.replace(mark, core_id))
    print('core id: %s' % core_id.decode(), flush=True)

    # the module's imports, grouped by owner
    imports = []
    for line in wasm_objdump(WASM, '-x').splitlines():
        m = re.match(r'\s*- func\[\d+\] sig=\d+ <(\S+)> <- (\S+)\.(\S+)', line)
        if m:
            imports.append((m.group(2), m.group(3)))  # (module, name)
    host = set(host_api_names())
    skipped_defs = collections.defaultdict(set)
    for rel, r in skipped:
        text = open(os.path.join(PORT_OUT, rel), encoding='utf-8', errors='replace').read()
        for fn in re.findall(r'^[\w\s\*]+?[\s\*](\w+)\s*\([^;{]*\)\s*\{', text, re.M):
            skipped_defs[fn].add(rel)
    failed_defs = {}
    for r in records:
        if r['status'] != 'ok' and r['unit'].startswith('src/'):
            text = open(os.path.join(ROOT, r['unit']), encoding='utf-8', errors='replace').read()
            for fn in FUNC_DEF.findall(text):
                failed_defs[fn] = r['unit']
    port_defs = collections.defaultdict(set)
    module_rel = set(MODULE_UNITS)
    for p in glob.glob(os.path.join(PORT_OUT, '**', '*.c'), recursive=True):
        rel = os.path.relpath(p, PORT_OUT).replace(os.sep, '/')
        if rel in module_rel:
            continue
        text = open(p, encoding='utf-8', errors='replace').read()
        for fn in re.findall(r'^[\w\s\*]+?[\s\*](\w+)\s*\([^;{]*\)\s*\{', text, re.M):
            port_defs[fn].add(rel)
    owners = collections.defaultdict(list)
    for module, name in imports:
        if name in host:
            owner = 'host_api'
        elif module == 'wasi_snapshot_preview1' or name.startswith(('__wasi', 'wasi_')):
            owner = 'WASI libc (the module must not import WASI)'
        elif name in skipped_defs:
            owner = 'missing module file: ' + ', '.join(sorted(skipped_defs[name]))
        elif name in failed_defs:
            owner = 'game unit that does not compile for wasm: ' + failed_defs[name]
        elif name in port_defs:
            owner = 'port file outside the module list: ' + ', '.join(sorted(port_defs[name]))
        elif name.startswith('_Z'):
            owner = 'C++ mangled name: a C++ unit defines it under its C++ name'
        elif name.startswith('rc_'):
            owner = 'rc_ reference: a recompiled function named through the rc_ prefix'
        elif name in rc_failed:
            owner = 'SDK function the recompiler can\'t translate (port/hle has to provide it)'
        else:
            owner = 'other'
        owners[owner].append(name)

    units = [r for r in records if r['status'] != 'ok']
    summary = {
        'game_units': len(srcs), 'game_units_compiled': len(src_objs),
        'module_port_units': len(MODULE_UNITS), 'module_port_compiled': len(port_objs),
        'recompiled': len(rc_names), 'recompiled_failed_in_recompiler': sorted(rc_failed),
        'fixup_words': nfix, 'imports': len(imports),
        'strict_link_ok': strict.returncode == 0,
        'imports_by_owner': {k: sorted(v) for k, v in owners.items()},
        'failed_units': units, 'stubbed_functions': abi['stubs'], 'thunks': abi['thunks'],
        'signature_conflicts': abi['rows'], 'conflicts_after_thunks': abi['left'],
    }
    json.dump(summary, open(os.path.join(OUT, 'census.json'), 'w'), indent=1)
    print('module: %s, %d imports; census in %s' % (WASM, len(imports), os.path.join(OUT, 'census.json')))
    for k, v in sorted(owners.items(), key=lambda kv: -len(kv[1])):
        print('%6d  %s   e.g. %s' % (len(v), k, ', '.join(v[:5])))
    core_cache_put(key)
    print('done in %.0f s' % (time.time() - t0))


if __name__ == '__main__':
    main()
