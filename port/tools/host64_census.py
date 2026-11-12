#!/usr/bin/env python3
"""Census of the host side of the port for 64-bit Windows (x86_64-pc-windows-msvc).

The host side is what runs natively in the 64-bit build (docs/findings/081-portable-core.md,
"Phase 2 plan"): port/host/*, the host layer in port/hle (host.c, graphics_queue.c, gs_queue.inc,
vif.c's VIF1/VU1 part, the VU1 runtime in vu0_interp.c), port/gs/*, the device and OS files
(gamepad, keyboard, text input, movie + libmpeg2, launcher link, fps), and port/net/*.

Every unit is compiled with:
  clang --target=x86_64-pc-windows-msvc -c -O2 -ffp-contract=off -ftrivial-auto-var-init=zero
        -fno-strict-aliasing -fwrapv -w -I <gen> -I port/runtime -I port/include
  vu0_interp.c gets -DPS2_VU_AOT (as in build_host64.py), with vu1_aot.inc generated into <gen>.
  every unit gets the SDL3 include directory (SDL3's threads, locks and timers are used everywhere).
  libmpeg2 units get only their own directory on the include path.

Two passes per unit:
  plain  the link's flags (-w): every error counts; the census result.
  warn   the same without -w, plus the warnings that hide pointer/int truncation and format
         mismatches (-Wpointer-to-int-cast, -Wint-to-pointer-cast, -Wshorten-64-to-32, ...).
         Warnings are reported per file; they don't make a unit fail.

Errors are grouped by category. Output: build/host64/obj (objects), build/host64/census.json
(every message), build/host64/census.txt (table). Writes only under build/host64/ and reads the
repo (and assets/vutext.bin, for vu1_aot.inc).

Usage: python port/tools/host64_census.py [--jobs N] [--only SUBSTR]
"""
import argparse, json, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'port' / 'tools'))

OUTDIR = ROOT / 'build' / 'host64'
OBJ = OUTDIR / 'obj'
GEN = OUTDIR / 'gen'
TMP = OUTDIR / 'tmp'
RUNTIME = ROOT / 'port' / 'runtime'
INCLUDE = ROOT / 'port' / 'include'
LIBMPEG2 = ROOT / 'port' / 'third_party' / 'libmpeg2'
CLANG = (Path.home() / 'scoop/apps/llvm/current/bin/clang.exe')
TARGET = 'x86_64-pc-windows-msvc'

BASE_FLAGS = ['--target=' + TARGET, '-c', '-O2', '-ffp-contract=off', '-ftrivial-auto-var-init=zero',
              '-fno-strict-aliasing', '-fwrapv']
WARN_FLAGS = ['-Wall', '-Wpointer-to-int-cast', '-Wint-to-pointer-cast', '-Wshorten-64-to-32',
              '-Wincompatible-pointer-types', '-Wint-conversion', '-Wformat', '-Wpointer-sign']
WARN = re.compile(r"\[-W(pointer-to-int-cast|int-to-pointer-cast|shorten-64-to-32|incompatible-pointer-types|"
                  r"int-conversion|format|pointer-sign)\]")
CONFIGS = ('plain', 'warn')

# The host units, in link order. Paths are repo-relative.
HOST_UNITS = [
    'port/host/host_sdl.c', 'port/host/host_win32.c', 'port/host/sound_out_sdl.c', 'port/host/sdl_host.c',
    'port/host/host_end.c',
    'port/runtime/sha256.c',
    'port/hle/host.c', 'port/hle/graphics_queue.c', 'port/hle/vif.c', 'port/runtime/vu0_interp.c',
    'port/gs/memory.c', 'port/gs/gs.c', 'port/gs/display.c', 'port/gs/present.c', 'port/gs/gpu_rust.c', 'port/gs/perf_log.c',
    'port/gs/texpack.c', 'port/gs/font_sharp.c', 'port/gs/button_icons.c', 'port/gs/capture.c', 'port/gs/replay.c',
    'port/hle/gamepad.c', 'port/hle/keyboard.c', 'port/hle/text_input.c', 'port/hle/movie.c',
    'port/hle/touch.c', 'port/host/touch_host.c',
    'port/hle/launcher_link.c', 'port/hle/fps.c',
    'port/net/client.c', 'port/net/hash.c', 'port/net/json.c', 'port/net/net.c',
] + ['port/third_party/libmpeg2/%s.c' % n for n in (
    'alloc', 'cpu_accel', 'cpu_state', 'decode', 'header', 'idct', 'motion_comp', 'slice')]
# port/hle/gs_queue.inc is included by graphics_queue.c; port/gs/gpu_exact.inc, gpu_rust_native.inc
# (with native_core.inc) by gpu_rust.c, which needs port/render/include (dhd_render.h).

# Error classification, first match wins. The groups name what has to change on x64.
X86_HDR = re.compile(r"^(mm_malloc|mmintrin|xmmintrin|emmintrin|pmmintrin|tmmintrin|smmintrin|nmmintrin|"
                     r"wmmintrin|immintrin|x86intrin|intrin|ia32intrin|avxintrin|avx2intrin|fmaintrin|"
                     r"f16cintrin|bmiintrin|popcntintrin|ammintrin|prfchwintrin|adxintrin|lzcntintrin|"
                     r"xopintrin|cpuid)\.h$")
X86_IDENT = re.compile(r"(_mm\d*_\w+|__m\d{2,3}[di]?|__builtin_ia32\w*|_MM_\w+|_control87|_controlfp\w*|"
                       r"_clearfp|_statusfp|_fpreset|__rdtsc|__cpuid\w*|__stosb|__movsb|"
                       r"_rotl\w*|_rotr\w*)")
ASM = re.compile(r"\basm\b|__asm|inline assembly|instruction|operand|register .* for|constraint", re.I)
MSVC_EXT = re.compile(r"__declspec|__stdcall|__cdecl|__fastcall|__forceinline|__int(8|16|32|64)\b|"
                      r"__unaligned|__pragma|__w64|__noop")
CONTEXT_FIELD = re.compile(r"'(?:Eip|Esp|Ebp|Eax|Ebx|Ecx|Edx|Esi|Edi|Eflags|SegCs|SegSs)'|\bCONTEXT\b|NT_TIB|\bNtCurrentTeb\b")
TRUNC = re.compile(r"cast to smaller integer|pointer to integer|integer to pointer|different size|"
                   r"implicit conversion loses|changes value|shorten|incompatible integer|"
                   r"incompatible pointer|format specifies|conversion specifies|expects argument")
DIAG = re.compile(r"^(.+?):(\d+):(\d+): (?:fatal )?error: (.*)$")
DIAG_NOLOC = re.compile(r"^(?:clang[^:]*: )?error: (.*)$")
DIAG_WARN = re.compile(r"^(.+?):(\d+):(\d+): warning: (.*)$")
QUOTED = re.compile(r"'([^']*)'")


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', **kw)


def gen_headers():
    """The census's own gen dir (on the include path) and vu1_aot.inc in it (gs_shader.h went
    with the Direct3D 11 renderer, finding 088 CL2)."""
    GEN.mkdir(parents=True, exist_ok=True)
    import vu1_aot
    vu1_aot.generate(GEN / 'vu1_aot.inc')


def sdl3_include():
    import sdl3  # port/tools/sdl3.py: the x64 SDL3 build (cached under build/port/sdl3)
    return str(sdl3.build()[1])


def classify(msg):
    """(category, detail) for one diagnostic message."""
    m = re.match(r"'([^']+)' file not found", msg)
    if m:
        h = m.group(1)
        if X86_HDR.match(h):
            return 'x86 intrinsic header', h
        return 'missing header', h
    ids = QUOTED.findall(msg)
    m = X86_IDENT.search(msg)
    if m:
        return 'x86 intrinsic / MSVC builtin', m.group(1)
    m = MSVC_EXT.search(msg)
    if m:
        return 'MSVC extension', m.group(0)
    m = CONTEXT_FIELD.search(msg)
    if m:
        return 'x86 CONTEXT / NT_TIB', m.group(0)
    if ASM.search(msg):
        return 'inline asm', msg[:90]
    if TRUNC.search(msg):
        return 'pointer / integer width', msg[:90]
    if re.search(r"undeclared|unknown type name|unknown identifier", msg) and ids and ids[0][:1].isupper():
        return 'Windows-style identifier', ids[0]
    if re.search(r"undeclared|unknown type name", msg) and ids:
        return 'other undeclared', ids[0]
    return 'other', msg[:90]


def parse(stderr, pattern):
    """(location, message) for every line of stderr that matches pattern (DIAG or DIAG_WARN)."""
    out = []
    for line in stderr.splitlines():
        m = pattern.match(line)
        if m:
            out.append(((m.group(1), int(m.group(2))), m.group(4)))
    return out


def parse_errors(stderr):
    errs = []
    for loc, msg in parse(stderr, DIAG):
        cat, detail = classify(msg)
        errs.append({'file': loc[0], 'line': loc[1], 'msg': msg, 'cat': cat, 'detail': detail})
    for line in stderr.splitlines():  # errors without a location (driver, missing files)
        m = DIAG_NOLOC.match(line)
        if m and not DIAG.match(line):
            cat, detail = classify(m.group(1))
            errs.append({'file': None, 'line': 0, 'msg': m.group(1), 'cat': cat, 'detail': detail})
    return errs


def compile_unit(job):
    rel, cfg = job['rel'], job['cfg']
    src = ROOT / rel
    name = rel.replace('/', '_').replace('.c', '') + '.o'
    if cfg == 'plain':
        obj = OBJ / name
    else:
        obj = OBJ / ('warn_' + name)
    obj.parent.mkdir(parents=True, exist_ok=True)
    flags = list(BASE_FLAGS)
    if cfg == 'warn':
        flags += WARN_FLAGS
    else:
        flags += ['-w']
    if rel.startswith('port/third_party/libmpeg2/'):
        incs = ['-I', str(LIBMPEG2)]
    else:
        incs = ['-I', str(GEN), '-I', str(RUNTIME), '-I', str(INCLUDE)]
    extra = ['-I', job['sdl3']]  # SDL3's headers, for every unit
    if rel.endswith('runtime/vu0_interp.c'):
        extra += ['-DPS2_VU_AOT']
    if rel.endswith('gs/gpu_rust.c'):
        extra += ['-I', str(ROOT / 'port' / 'render' / 'include')]  # dhd_render.h
    cmd = [str(CLANG)] + flags + extra + incs + [str(src), '-o', str(obj)]
    env = dict(os.environ, TMP=str(TMP), TEMP=str(TMP), TMPDIR=str(TMP))
    t0 = time.time()
    p = sh(cmd, env=env, timeout=900)
    ok = p.returncode == 0 and obj.exists()
    res = {'rel': rel, 'cfg': cfg, 'ok': ok, 'rc': p.returncode, 'secs': round(time.time() - t0, 2),
           'cmd': ' '.join(cmd)}
    if cfg == 'plain':
        res['errors'] = parse_errors(p.stderr)
        res['first_stderr'] = '' if ok else p.stderr[:1500]
    else:
        res['warnings'] = [{'file': loc[0], 'line': loc[1], 'msg': msg}
                           for loc, msg in parse(p.stderr, DIAG_WARN)
                           if WARN.search(msg) or TRUNC.search(msg)]
        res['errors'] = parse_errors(p.stderr)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jobs', type=int, default=min(16, os.cpu_count() or 4))
    ap.add_argument('--only', default='')
    args = ap.parse_args()

    assert CLANG.exists(), CLANG
    for d in (OBJ, TMP):
        d.mkdir(parents=True, exist_ok=True)
    ver = sh([str(CLANG), '--version']).stdout.splitlines()[0]
    gen_headers()
    sdl = sdl3_include()
    units = [u for u in HOST_UNITS if args.only in u]
    print('units: %d   clang: %s' % (len(units), ver))
    jobs = [{'rel': rel, 'cfg': cfg, 'sdl3': sdl} for rel in units for cfg in CONFIGS]
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(compile_unit, jobs))
    print('compiled %d jobs in %.0fs' % (len(jobs), time.time() - t0))

    by = {}
    for r in results:
        by.setdefault(r['rel'], {})[r['cfg']] = r
    summary = {'clang': ver, 'target': TARGET, 'units': [], 'groups': {}, 'configs': list(CONFIGS)}
    groups = {}
    for rel in units:
        row = {'source': rel}
        for cfg in CONFIGS:
            r = by[rel][cfg]
            row[cfg] = {'ok': r['ok'], 'rc': r['rc'], 'nerr': len(r['errors']),
                        'errors': [[e['file'], e['line'], e['msg'][:220], e['cat']] for e in r['errors']],
                        'groups': sorted({'%s: %s' % (e['cat'], e['detail']) for e in r['errors']}),
                        'first_error': (r['errors'][0]['msg'] if r['errors'] else r.get('first_stderr', '').strip()[:300])}
        row['warn'].update({'nwarn': len(by[rel]['warn'].get('warnings', [])),
                            'warnings': [[w['file'], w['line'], w['msg'][:200]] for w in by[rel]['warn']['warnings']]})
        summary['units'].append(row)
        for e in by[rel]['plain']['errors']:
            groups.setdefault('%s: %s' % (e['cat'], e['detail']), set()).add(rel)
    summary['groups'] = {k: sorted(v) for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))}
    summary['ok_count'] = sum(1 for u in summary['units'] if u['plain']['ok'])
    summary['nerr'] = sum(u['plain']['nerr'] for u in summary['units'])
    summary['nwarn'] = sum(u['warn']['nwarn'] for u in summary['units'])
    (OUTDIR / 'census.json').write_text(json.dumps(summary, indent=1), encoding='utf-8')

    lines = ['%-36s %-6s %6s %6s  %s' % ('unit', 'x64', 'errors', 'warn', 'first error')]
    for u in summary['units']:
        lines.append('%-36s %-6s %6d %6d  %s' % (
            u['source'][:36], 'OK' if u['plain']['ok'] else 'FAIL', u['plain']['nerr'], u['warn']['nwarn'],
            u['plain']['first_error'][:90].replace('\n', ' ')))
    lines.append('')
    lines.append('OK units: %d/%d   errors: %d   warnings (pointer/width/format): %d' % (
        summary['ok_count'], len(units), summary['nerr'], summary['nwarn']))
    lines.append('error groups:')
    for k, v in summary['groups'].items():
        lines.append('  %4d  %s   e.g. %s' % (len(v), k, ', '.join(x.split('/')[-1] for x in v[:4])))
    (OUTDIR / 'census.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines))
    print('json: %s' % (OUTDIR / 'census.json'))


if __name__ == '__main__':
    main()
