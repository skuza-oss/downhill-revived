"""Build/run the independent wasm2c x64 runtime test (no game data).

Windows (default on Windows): clang-cl/lld-link with guest_memory_win32.c and
the fibers' Windows x64 switch (fiber_win64.c). Linux (--linux, the default when
run on Linux): clang/ELF with guest_memory_posix.c and the System V x64 or
AArch64 switch. Invoke the Linux mode from inside WSL.
"""
import argparse
import os
import platform
import signal
import sys
from pathlib import Path
import subprocess
from prepare_wasm2c import prepare
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches

ROOT = Path(__file__).resolve().parents[2]
ON_WINDOWS = sys.platform == 'win32'

WIN_WABT = (DEVCACHES / 'wabt/wabt-1.0.42')
WIN_WASI = (DEVCACHES / 'wasi-sdk/wasi-sdk-34.0-x86_64-windows')
WIN_LLVM = (Path.home() / 'scoop/apps/llvm/current/bin')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--optimized', action='store_true',
                   help='validate x86-64-v2, Zen 3 tuning, O3 (without LTO)')
    p.add_argument('--negative-controls', action='store_true',
                   help='also prove omitted translation/SP/depth saves fail the checks')
    p.add_argument('--linux', action='store_true',
                   help='Linux/ELF build (default when run on Linux)')
    p.add_argument('--out', type=Path, default=ROOT / 'build/host64-selftest')
    p.add_argument('--wabt', type=Path, default=None)
    p.add_argument('--wasi', type=Path, default=None)
    p.add_argument('--cc', type=Path, default=None, help='native C compiler')
    p.add_argument('--llvm', type=Path, default=WIN_LLVM)
    a = p.parse_args()
    linux = a.linux or not ON_WINDOWS
    wabt = a.wabt or (Path('/opt/wabt') if linux else WIN_WABT)
    wasi = a.wasi or (Path('/opt/wasi-sdk') if linux else WIN_WASI)
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if ON_WINDOWS and os.environ.get('DHD_SCRATCH'):
        # the compilers' temporary files go to the scratch folder, not the system drive's temp
        scratch = Path(os.environ['DHD_SCRATCH']) / 'host64-selftest'
        scratch.mkdir(parents=True, exist_ok=True)
        env.update(TEMP=str(scratch), TMP=str(scratch))
    exe_suffix = '.exe' if linux is False else ''
    def run(args):
        print(' '.join(map(str, args)), flush=True)
        subprocess.run(list(map(str, args)), cwd=out, env=env, check=True, timeout=180)
    source = ROOT / 'port/host64'
    rt = wabt / 'share/wabt/wasm2c'
    wasi_clang = wasi/('bin/clang.exe' if not linux else 'bin/clang')
    run([wasi_clang, '-O2', '-nostdlib', '-ffp-contract=off',
         '-Wl,--no-entry', '-Wl,--initial-memory=2147483648',
         '-Wl,--stack-first', '-Wl,--global-base=1880096768',  # HOST64_MODULE_BASE (memory_layout.h), as W5
         '-Wl,--max-memory=2147614720', '-Wl,--export=__stack_pointer',
         '-Wl,-z,stack-size=1048576', source/'selftest_core.c', '-o', out/'core.wasm'])
    run([wabt/('bin/wasm2c.exe' if not linux else 'bin/wasm2c'),
         out/'core.wasm', '-n', 'core', '-o', out/'core.c'])
    unprepared = (out/'core.c').read_text()
    prepare(out/'core.c')
    prepared = (out/'core.c').read_text()
    prepare(out/'core.c')
    assert (out/'core.c').read_text() == prepared
    if linux:
        cc = str(a.cc or 'clang')
        # Explicit bounds checks: wasm-rt.h would otherwise default Linux to
        # guard-page mode, whose checks compile out and rely on WABT's own
        # mmap allocator and signal handler. Our mirror allocator replaces
        # both, so every generated OOB access must trap in the code itself.
        common = [cc, '-O2', '-ffp-model=strict', '-ffp-contract=off',
                  '-ftrivial-auto-var-init=zero', '-fno-strict-aliasing', '-fwrapv',
                  '-DWASM_RT_STACK_DEPTH_COUNT=1', '-DWASM_RT_MAX_CALL_STACK_DEPTH=512',
                  '-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1',
                  '-I'+str(wabt/'include'), '-I'+str(rt), '-I'+str(source), '-I'+str(out)]
        if a.optimized:
            common += ['-O3', '-march=x86-64-v2', '-mtune=znver3']
        switch = 'fiber_arm64.c' if platform.machine() in ('aarch64', 'arm64') else 'fiber_sysv64.c'
        os_sources = ('guest_memory.c', 'guest_memory_posix.c', 'fiber.c', switch,
                      'fp_mode.c', 'selftest_host.c')
    else:
        msvc = Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231')
        sdk = Path('C:/Program Files (x86)/Windows Kits/10')
        version = '10.0.26100.0'
        common = [a.llvm/'clang-cl.exe', '--target=x86_64-pc-windows-msvc', '/O2',
                  '/clang:-ffp-model=strict', '/clang:-ffp-contract=off',
                  '-DWASM_RT_STACK_DEPTH_COUNT=1', '-DWASM_RT_MAX_CALL_STACK_DEPTH=512',
                  '-I'+str(wabt/'include'), '-I'+str(rt), '-I'+str(source), '-I'+str(out),
                  '/imsvc'+str(msvc/'include')]
        common += ['/imsvc'+str(sdk/'Include'/version/x) for x in ('ucrt','um','shared')]
        if a.optimized:
            common += ['/clang:-O3', '/clang:-march=x86-64-v2',
                       '/clang:-mtune=znver3']
        os_sources = ('guest_memory.c', 'guest_memory_win32.c', 'fiber.c', 'fiber_win64.c',
                      'fp_mode.c', 'selftest_host.c')
    objects = []
    obj_suffix = '.o' if linux else '.obj'
    for f in [out/'core.c', rt/'wasm-rt-impl.c'] + [source/n for n in os_sources]:
        obj = out/(f.stem+obj_suffix); objects.append(obj)
        if linux:
            run(common+['-c', f, '-o', obj])
        else:
            run(common+['/c', f, '/Fo'+str(obj)])
    exe = out/('selftest'+exe_suffix)
    if linux:
        run([cc, *objects, '-o', exe, '-lm', '-rdynamic'])
    else:
        link = [a.llvm/'clang-cl.exe', *objects, '/Fe'+str(exe), '/link',
             '/libpath:'+str(msvc/'lib/x64'),
             '/libpath:'+str(sdk/'Lib'/version/'ucrt/x64'),
             '/libpath:'+str(sdk/'Lib'/version/'um/x64'), 'onecore.lib', '/SUBSYSTEM:CONSOLE']
        run(link)
    run([exe])
    if linux:
        for offset in ('0x80000000', '0x100000000', '0x1ffffffff'):
            result = subprocess.run([str(exe), '--guard', offset],
                                    cwd=out, env=env, timeout=30)
            if result.returncode != -signal.SIGSEGV:
                raise RuntimeError(f'guard {offset}: expected SIGSEGV, got {result.returncode}')
        print('PASS: 3 guard reads die with SIGSEGV', flush=True)
    else:
        for offset in ('0x80000000', '0x100000000', '0x1ffffffff'):
            result = subprocess.run([str(exe), '--guard', offset],
                                    cwd=out, env=env, timeout=30)
            if result.returncode & 0xffffffff != 0xc0000005:
                raise RuntimeError(f'guard {offset}: expected access violation, got {result.returncode}')
        print('PASS: 3 guard reads fault with 0xC0000005', flush=True)
    if a.negative_controls:
        # Same /O2 module and physical OS mirrors; omit only host translation.
        broken = out/'no-mirror-fix.c'
        broken.write_text(unprepared)
        if linux:
            obj = out/'no-mirror-fix.o'
            run(common+['-c', broken, '-o', obj])
            neg = out/'no-mirror-fix'
            run([cc, obj, *[x for x in objects if x.name!='core.o'], '-o', neg,
                 '-lm', '-rdynamic'])
        else:
            obj = out/'no-mirror-fix.obj'
            run(common+['/c', broken, '/Fo'+str(obj)])
            neg = out/'no-mirror-fix.exe'
            run([str(obj) if str(x)==str(out/'core.obj') else
                 '/Fe'+str(neg) if str(x).startswith('/Fe') else x for x in link])
        result = subprocess.run([str(neg)], cwd=out, env=env,
                                capture_output=True, text=True, timeout=30)
        (out/'no-mirror-fix.log').write_text(result.stdout+result.stderr)
        if result.returncode!=1 or 'w2c_core_memory_test(&core)==0' not in result.stderr:
            raise RuntimeError(f'no-mirror-fix: expected mirror assertion missing: {result.stderr}')
        print('PASS negative control no-mirror-fix: /O2 mirror assertion caught omission', flush=True)
        fiber_source = source/'fiber.c'
        original = fiber_source.read_text()
        for name, needle, diagnostic in (
            ('no-sp', '*module_sp=fibers[to].sp;', 'address>=top-0x40000'),
            ('no-depth', 'wasm_rt_call_stack_depth=fibers[to].depth;',
             'wasm_rt_call_stack_depth==182-depth'),
        ):
            assert original.count(needle)==1
            broken = out/(name+'.c')
            broken.write_text(original.replace(needle, '/* negative control */'))
            if linux:
                obj = out/(name+'.o')
                run(common+['-c', broken, '-o', obj])
                neg = out/name
                run([cc, obj, *[x for x in objects if x.name!=fiber_source.stem+'.o'],
                     '-o', neg, '-lm', '-rdynamic'])
            else:
                obj = out/(name+'.obj')
                run(common+['/c', broken, '/Fo'+str(obj)])
                neg = out/(name+'.exe')
                run([str(obj) if str(x)==str(out/'fiber.obj') else
                     '/Fe'+str(neg) if str(x).startswith('/Fe') else x for x in link])
            result = subprocess.run([str(neg)], cwd=out, env=env,
                                    capture_output=True, text=True, timeout=30)
            (out/(name+'.log')).write_text(result.stdout+result.stderr)
            if result.returncode!=1 or diagnostic not in result.stderr:
                raise RuntimeError(f'{name}: expected assertion missing: {result.stderr}')
            print(f'PASS negative control {name}: required assertion caught omission', flush=True)

if __name__ == '__main__':
    main()
