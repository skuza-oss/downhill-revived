"""The source and IR steps every game-unit build shares (build_wasm.py, testprog.game_unit, abi_check.py): the port patches
(port/patches.py) and the 16-byte alignment of aggregate locals."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import patches  # noqa: E402  (port/patches.py)
import transform  # noqa: E402


def keep_lines(old, new):
    """new padded with newlines to old's line count, so later lines keep their numbers."""
    return new + '\n' * max(0, old.count('\n') - new.count('\n'))


# Macros the port's build never defines: NONMATCHING (the C of a function that does not match yet:
# the core runs that function's recompiled asm instead, build_wasm.py recompile_objects) and
# __mips__ (the core is wasm32). Source under them is never compiled, so a patch there never runs
# (finding 165).
UNDEFINED = ('NONMATCHING', '__mips__')
DIRECTIVE = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b([^\n]*)', re.M)
HEAD = re.compile(r'\b(\w+)\s*\([^;{)]*\)\s*(?:const\s*)?\{')
NOT_FUNCTIONS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'do'}
ASM_NAME = re.compile(r'\bINCLUDE_ASM\s*\([^,)]*,\s*(\w+)\s*\)')


class DeadPatch(ValueError):
    pass


def condition(kind, expr):
    """True or False when the port's build decides a directive's condition, None when it may be
    either (the guard then counts the branch as compiled)."""
    expr = expr.strip()
    if kind in ('ifdef', 'ifndef'):
        return (kind == 'ifndef') if expr in UNDEFINED else None
    m = re.fullmatch(r'(!?)\s*defined\s*(?:\(\s*(\w+)\s*\)|(\w+))', expr)
    if m and (m[2] or m[3]) in UNDEFINED:
        return bool(m[1])
    return False if expr == '0' else None


def dead_ranges(text):
    """[(start, end, directive, recompiled)]: the spans of text the port's preprocessor drops.
    directive names the branch; recompiled is the function the conditional's other branch hands to
    the recompiler (its INCLUDE_ASM), or None."""
    mask = transform.code_mask(text)
    frames, out = [], []
    for m in DIRECTIVE.finditer(mask):
        kind, expr = m[1], ' '.join(m[2].split())
        nl = mask.find('\n', m.end())
        body = len(mask) if nl < 0 else nl + 1
        if kind in ('if', 'ifdef', 'ifndef'):
            outer = bool(frames) and frames[-1]['dead']
            c = condition(kind, expr)
            frames.append({'outer': outer, 'taken': c is True, 'dead': outer or c is False, 'start': body,
                           'chain': body, 'why': '#%s %s' % (kind, expr), 'spans': []})
            continue
        if not frames:
            continue
        f = frames[-1]
        if f['dead'] and not f['outer']:
            f['spans'].append((f['start'], m.start(), f['why']))
        if kind == 'endif':
            frames.pop()
            names = ASM_NAME.findall(mask, f['chain'], m.start())
            out += [(a, b, why, names[0] if names else None) for a, b, why in f['spans']]
            continue
        c = None if kind == 'else' else condition('if', expr)
        f['dead'] = f['outer'] or f['taken'] or c is False
        f['taken'] = f['taken'] or c is True
        f['start'] = body
        f['why'] = '#elif %s' % expr if kind == 'elif' else '#else (after %s)' % f['why']
    return sorted(out)


def check_live(rel, index, patch, text, spans, found=None):
    """Refuse a patch whose search text lies in source the port does not compile (or, with found,
    add the message to it)."""
    dead = dead_ranges(text)
    for s, e in spans:
        for a, b, why, recompiled in dead:
            if s < b and a < e:
                mask = transform.code_mask(text)
                heads = [h[1] for h in HEAD.finditer(mask, a, b) if h.start() < e and h[1] not in NOT_FUNCTIONS]
                where = heads[-1] if heads else recompiled or 'file scope'
                line = text.count('\n', 0, a) + 1
                if recompiled:
                    reason = ('the core runs %s from its recompiled asm (INCLUDE_ASM), so this C never '
                              'runs' % recompiled)
                else:
                    reason = 'the port does not compile this branch'
                kind, a0 = patch[0], patch[1]
                what = a0 if kind != 're' else a0[0]
                message = ('dead port patch %d of %s (%s %r) in %s: it changes text under `%s` '
                                '(from line %d), and %s. Patch code the port compiles (finding 165).'
                                % (index, rel, kind, what.splitlines()[0][:60] if what else what, where,
                                   why, line, reason))
                if found is None:
                    raise DeadPatch(message)
                found.append(message)
                return


def apply_patches(rel, text, found=None):
    """The port patches of one src/ unit (rel: its path under src/, '/' separated). A patch in
    source the port does not compile raises DeadPatch, or with found, is listed there."""
    for index, patch in enumerate(patches.PATCHES.get(rel, [])):
        kind, a, b = patch
        if kind == 'text':
            if text.count(a) != 1:
                raise ValueError('patch text found %d times: %r' % (text.count(a), a[:60]))
            s = text.index(a)
            check_live(rel, index, patch, text, [(s, s + len(a))], found)
            text = text.replace(a, keep_lines(a, b))
        elif kind == 're':
            pattern, count = a
            check_live(rel, index, patch, text, [m.span() for m in re.finditer(pattern, text)], found)
            text, n = re.subn(pattern, b, text)
            if n != count:
                raise ValueError('patch pattern found %d times, not %d: %r' % (n, count, pattern))
        else:
            mask = transform.code_mask(text)
            heads = list(re.finditer(r'\b%s\s*\([^;{)]*\)\s*\{' % a, mask))
            if len(heads) != 1:
                raise ValueError('patch: %d definitions of %s' % (len(heads), a))
            s = heads[0].end() - 1
            e = transform.close_paren(mask, s) + 1
            check_live(rel, index, patch, text, [(heads[0].start(), e)], found)
            text = text[:s] + keep_lines(text[s:e], b) + text[e:]
    return text


def dead_patches(src_root):
    """Every dead patch over the src/ tree at src_root, as messages (the guard's survey)."""
    found = []
    for rel in sorted(patches.PATCHES):
        path = os.path.join(src_root, *rel.split('/'))
        if os.path.exists(path):
            apply_patches(rel, open(path, encoding='utf-8', errors='replace').read(), found)
    return found


# A local array, struct, union, class or 128-bit object (a stack slot in the frontend's IR).
AGGREGATE_ALLOCA = re.compile(r'(= alloca (?:\[|\{|<|i128\b|%"?(?:struct|union|class)\.)[^\n]*?, align )(\d+)')


def align_aggregate_allocas(ir):
    """The EE stack keeps such objects 16-byte aligned, and lq/sq/lqc2/sqc2 ignore the low 4
    address bits: recompiled code (recomp.h M128) and the inline asm runtime (ps2asm.c) both
    do, and the EE float rewrite copies them with SIMD. The host stack only aligns them to their
    type, so give every aggregate local 16 bytes, whatever path its address takes to a quadword
    access (finding 021)."""
    return AGGREGATE_ALLOCA.sub(lambda m: m[1] + str(max(16, int(m[2]))), ir)
