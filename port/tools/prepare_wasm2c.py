"""Adapt WABT 1.0.42 host addresses for guest RAM mirrors.

Run on every generated C translation unit, before compiling at any optimization
level. Refuse unknown templates rather than silently lose mirror correctness.
"""
from pathlib import Path
import argparse
import re
import shutil
import struct

# An f32 constant that WABT writes as a decimal double literal (no f suffix): `var_f2 = 6.10351562e-05;`.
F32_DOUBLE_LITERAL = re.compile(r'(\bvar_f\d+ = )(-?)(\d+\.\d*(?:e[-+]?\d+)?|\d+e[-+]?\d+)(?=;)')


def exact_f32_literals(text):
    """Write f32 constants as exact hex float literals.

    WABT 1.0.42 prints some f32 constants as 9-digit decimal literals without an f
    suffix: double constants whose conversion to float happens at run time under
    strict FP (the host build's -ffp-model=strict honours the dynamic rounding mode).
    In the EE's round-toward-zero mode, 6.10351562e-05 (meant: 2^-14) then becomes the
    float below 2^-14 (W7i race field 1360: func_00222B68). The 9 digits round-trip
    to the intended f32 in round-to-nearest, so that value is written exactly."""
    def exact(m):
        value = struct.unpack('<f', struct.pack('<f', float(m[3])))[0]
        return '%s%s%sf' % (m[1], m[2], value.hex())
    return F32_DOUBLE_LITERAL.sub(exact, text)


def specialize_dispatch(text, hot_functions=()):
    """Guarded direct calls for small signature groups; keep checked fallback.

    Native entry equality is tested AFTER WABT's bounds/type check. A changed
    table entry or a cross-instance funcref still receives its actual context.
    Large groups stay indirect to bound code growth; this is no closed-table
    assumption and does not remove a wasm trap.
    """
    groups = {}
    for ft, fn in re.findall(r'\{RefFunc, (\w+), \(wasm_rt_function_ptr_t\)(\w+),', text):
        groups.setdefault(ft, set()).add(fn)

    requested = set(hot_functions)
    known = set().union(*groups.values()) if groups else set()
    if requested - known:
        raise ValueError(f'unknown wasm2c dispatch targets: {sorted(requested - known)}')

    def rewrite(match):
        # wasm2c emits one call expression per line. Split only top-level commas
        # (the second field is a function-pointer type containing more commas).
        args, depth, start = [], 0, 0
        expr = match[1]
        for i, char in enumerate(expr):
            if char == '(':
                depth += 1
            elif char == ')':
                depth -= 1
            elif char == ',' and depth == 0:
                args.append(expr[start:i].strip()); start = i + 1
        args.append(expr[start:].strip())
        if len(args) < 5:
            raise ValueError('unsupported wasm2c indirect call')
        table, cast, ft, index, *actual = args
        candidates = sorted(groups.get(ft, ()))
        if requested:
            candidates = [fn for fn in candidates if fn in requested]
        if not 1 <= len(candidates) <= 4:
            return match[0]
        # Generated operands must be side-effect-free locals/field accesses.
        if not all(re.fullmatch(r'[\w.>\[\]-]+', x) for x in [table, ft, index, *actual]):
            raise ValueError('unsupported wasm2c indirect-call operand')
        call = 'DO_CALL_INDIRECT(' + ', '.join([table, cast, index, *actual]) + ')'
        for fn in reversed(candidates):
            call = (f'({table}.data[{index}].func == (wasm_rt_function_ptr_t){fn} ? '
                    f'{fn}({", ".join(actual)}) : {call})')
        return f'(CHECK_CALL_INDIRECT({table}, {ft}, {index}), {call})'

    return re.sub(r'(?<![\w])CALL_INDIRECT\(([^\n]+)\)(?=;)', rewrite, text)


# One scalar/SIMD access (size 1..16): memory_access.h's cheaper inline test, same result.
MEMOP_DEFINE = '#define MEM_ADDR_MEMOP(mem, addr, n) host64_access_address(wasm_rt_local_memory_base, (addr), (n))'

# WABT's scalar and SIMD access templates (DEFINE_LOAD(i32_load, u32, u32, u32, FORCE_READ_INT) ...).
ACCESS_TEMPLATE = re.compile(r'^(DEFINE_(?:LOAD|STORE|SIMD_LOAD_FUNC|SIMD_LOAD_LANE|SIMD_STORE|SIMD_STORE_LANE))'
                             r'\((\w+),([^\n]*)\)$', re.M)
# One access of the default memory, as wasm2c writes every one: (u64)(base) [+ offset], then the
# stored value or lane operands, if any.
ACCESS_SITE = re.compile(r'\b(\w+)_default32\(wasm_rt_local_memory_base, wasm_rt_local_memory_size, '
                         r'&instance->w2c_memory, \(u64\)\((\w+)\)(?: \+ (\d+)u)?(?=[,)])')


def access_families(text):
    """Beside each access template, its _hot and _cold copies (memory_access.h, "Accesses split
    at the call site"): hot addresses base + address, cold calls the adapter out of line. The
    copies sit in the template's own preprocessor branch."""
    def add(m):
        kind, name, rest = m[1], m[2], m[3]
        if name.endswith(('_hot', '_cold')) or f'{kind}({name}_hot,' in text:
            return m[0]
        return '\n'.join([
            m[0], '#pragma push_macro("MEM_ADDR_MEMOP")', '#undef MEM_ADDR_MEMOP',
            '#define MEM_ADDR_MEMOP(mem, addr, n) (wasm_rt_local_memory_base + (addr))',
            f'{kind}({name}_hot,{rest})', '#undef MEM_ADDR_MEMOP',
            '#define MEM_ADDR_MEMOP(mem, addr, n) host64_cold_address(wasm_rt_local_memory_base, (addr), (n))',
            f'{kind}({name}_cold,{rest})', '#pragma pop_macro("MEM_ADDR_MEMOP")'])
    return ACCESS_TEMPLATE.sub(add, text)


def split_accesses(text, path='unit'):
    """Every access site NAME_default32(..., (u64)(base) + offset, ...) becomes
    (host64_base_ok(base, offset) ? NAME_hot_default32(...) : NAME_cold_default32(...)), the
    arguments copied verbatim (one of the two arms runs, so each is evaluated once). Refuses an
    access of the default memory in any other shape. Idempotent."""
    out, last = [], 0
    for m in ACCESS_SITE.finditer(text):
        if m[1].endswith(('_hot', '_cold')):
            continue
        depth, i = 0, m.start() + len(m[1]) + len('_default32')
        while True:  # the call's closing parenthesis
            c = text[i]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    break
            elif c == '\n':
                raise ValueError(f'{path}: access call across lines: {m[0]}')
            i += 1
        args = text[m.start() + len(m[1]) + len('_default32'):i + 1]
        out += [text[last:m.start()], f'(host64_base_ok({m[2]}, {m[3] or 0}u) ? {m[1]}_hot_default32{args} : '
                f'{m[1]}_cold_default32{args})']
        last = i + 1
    text = ''.join(out) + text[last:]
    rest = [s for s in re.findall(r'\b(\w+)_default32\(wasm_rt_local_memory_base\b', text)
            if not s.endswith(('_hot', '_cold'))]
    if rest:
        raise ValueError(f'{path}: unsupported wasm2c access shape ({len(rest)}, e.g. {rest[0]})')
    return text


def prepare(path, *, minimum_size=True, dispatch=True, hot_functions=()):
    text = path.read_text()
    if dispatch:
        text = specialize_dispatch(text, hot_functions)
    if minimum_size:
        text = re.sub(r'(\bwasm_rt_local_memory_size = )(?!host64_memory_size\()([^;\n]+);',
                      r'\1host64_memory_size(\2);', text)
    text = split_accesses(text, path)
    marker = '#include "memory_access.h"'
    copy_old = 'memmove(MEM_ADDR(dest, dest_addr, n), MEM_ADDR(src, src_addr, n), n);'
    copy_new = 'host64_memory_copy(dest->data, dest_addr, src->data, src_addr, n);'
    if copy_old in text:  # every memory.copy site in the unit uses this template
        text = text.replace(copy_old, copy_new)
    elif copy_new not in text and re.search(r'\bmemmove\(', text):
        # a memmove call that is not the memory.copy template: refuse rather than guess
        raise ValueError(f'{path}: unsupported wasm2c memory.copy template')
    # no memory.copy in this unit (clang lowers memmove to a module function, as the wasm32 build does)
    elif copy_new not in text:
        pass
    if marker in text:
        # Upgrade the previous barrier adapter instead of silently retaining it.
        legacy = {
            'host64_memory_address(&((mem)->data[addr]))':
            'host64_memory_address((mem)->data, (addr), (n))',
            'host64_memory_address(&wasm_rt_local_memory_base[(addr)])':
            'host64_memory_address(wasm_rt_local_memory_base, (addr), (n))',
            '#define MEM_ADDR_MEMOP(mem, addr, n) host64_memory_address(wasm_rt_local_memory_base, (addr), (n))':
            MEMOP_DEFINE,
        }
        for old, new in legacy.items():
            text = text.replace(old, new)
        if ('#define MEM_ADDR(mem, addr, n) host64_memory_address((mem)->data, (addr), (n))' not in text or
            MEMOP_DEFINE not in text):
            raise ValueError(f'{path}: unsupported prepared wasm2c memory template')
        path.write_text(access_families(text))
        return
    replacements = {
        '#define MEM_ADDR(mem, addr, n) &((mem)->data[addr])':
        '#define MEM_ADDR(mem, addr, n) host64_memory_address((mem)->data, (addr), (n))',
        '#define MEM_ADDR_MEMOP(mem, addr, n) (&wasm_rt_local_memory_base[(addr)])':
        MEMOP_DEFINE,
    }
    for old, new in replacements.items():
        if text.count(old) == 1:
            text = text.replace(old, new)
        elif 'MEM_ADDR' not in text:
            pass  # a split part: its MEM_ADDR defines come from the prepared impl header
        else:
            raise ValueError(f'{path}: unsupported wasm2c memory template: {old}')
    text = marker + '\n' + access_families(text)
    path.write_text(text)



# ---- the module's C for the native hosts (build_host64.py, every target) ----

def module_units(out, run, wasm2c, wasm=None, sources=(), num_outputs=1):
    """The module's C units in out: wasm2c's output for a .wasm (module name core), else copies
    of WABT .c units and the headers beside them. run(cmd, timeout=...) runs a command."""
    if wasm:
        if num_outputs < 1:
            raise ValueError('--num-outputs must be positive')
        run([wasm2c, wasm, '-n', 'core', '--num-outputs=' + str(num_outputs), '-o', out / 'core.c'],
            timeout=900)
        return [out / 'core.c'] if num_outputs == 1 else [out / f'core_{i}.c' for i in range(num_outputs)]
    units = []
    for source in sources:
        source = source.resolve()
        if source.suffix != '.c':
            raise ValueError('module units must be .c files')
        if re.fullmatch(r'core_\d+\.c', source.name) and not (source.parent / 'core-impl.h').exists():
            # split parts declare each other's functions in core-impl.h; one left in out by an
            # older build would compile against the wrong module
            raise ValueError('%s: no core-impl.h beside it (split units need their own)' % source)
        for header in source.parent.glob('*.h'):
            if header.resolve() != (out / header.name).resolve():
                shutil.copyfile(header, out / header.name)
        target = out / source.name
        if source != target:
            shutil.copyfile(source, target)
        units.append(target)
    return units


def check_launcher_exports(exports):
    """The exports launcher.c needs (exports: gen_host64_imports.generate's)."""
    required = {'memory', '__stack_pointer'}
    if not required <= exports.keys():
        raise ValueError(f'missing launcher exports: {required - exports.keys()}')
    if not {'main', '__main_argc_argv'} & exports.keys():
        raise ValueError('missing launcher entry export: main or __main_argc_argv')


def prepare_units(units, out, exact_f32=False):
    """Every unit prepared before compilation. WABT's split helper templates live in the shared
    implementation header: prepare that once, and specialize the individual units only where
    complete templates/metadata are present. exact_f32: f32 constants exact, independent of the
    EE rounding mode at run time (exact_f32_literals)."""
    for source in units:
        if exact_f32:
            source.write_text(exact_f32_literals(source.read_text()))
        if '#define MEM_ADDR(mem' in source.read_text():
            prepare(source)
        else:
            source.write_text(split_accesses(specialize_dispatch(source.read_text()), source))
    for header in out.glob('*-impl.h'):
        if '#define MEM_ADDR(mem' in header.read_text():
            prepare(header)
        header.write_text(builtin_float_tests(header.read_text()))


def builtin_float_tests(text):
    """wasm2c's float helpers (wasm_quietf on every f32.promote, wasm_quiet on every f64.demote,
    wasm_fabs, floor/ceil/trunc/nearest/sqrt) test for NaN with isnan(), and FMIN/FMAX use
    signbit(). Under the UCRT headers isnan(x) is fpclassify(x) == FP_NAN: a compare with zero, a
    NaN compare, an infinity compare and a normal/subnormal compare. With -ffp-model=strict clang
    keeps every one of those compares, even the unused ones, because a compare may raise a flag.
    In the race that was about 12% of the EE thread's samples (finding 090 SP11). The compiler's
    __builtin_isnan / __builtin_signbit give the same answer for every input in one test. The
    values computed do not change, so neither does the game."""
    for name in ('isnan', 'signbit'):
        text = re.sub(r'(?<![\w.])%s\(' % name, '__builtin_%s(' % name, text)
    return text

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', type=Path, nargs='+')
    parser.add_argument('--no-dispatch', action='store_true', help='disable guarded direct-call promotion for A/B measurement')
    parser.add_argument('--dispatch-target', action='append', default=[],
                        help='profile-selected wrapper symbol; at most four per signature promoted')
    args = parser.parse_args()
    for path in args.files:
        prepare(path, dispatch=not args.no_dispatch, hot_functions=args.dispatch_target)
