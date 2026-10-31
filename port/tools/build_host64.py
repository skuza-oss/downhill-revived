"""Build the game for one platform: the wasm2c module, the native host and the Rust renderer.

    python port/tools/build_host64.py build/wasm/dhd_core.wasm                   # win-x64: dhd64.exe
    python3 port/tools/build_host64.py --target linux-x64 MODULE                 # Linux or WSL: dhd64 (ELF)
    python3 port/tools/build_host64.py --target macos-arm64 MODULE               # a Mac: dhd64 (Mach-O)
    python port/tools/build_host64.py --target android-arm64 MODULE --jobs 2     # libmain.so (finding 091)
    python port/tools/build_host64.py [--target T] --fiber-test                  # the EE fibers alone

MODULE is a .wasm or WABT .c units (module name must be core; a Mac without wasm2c takes the C that
wasm2c wrote elsewhere, with --wabt naming a folder that holds WABT's include/ and share/). --fake
builds the independent integration fixture with wasi-sdk (win-x64). No force-link or missing stubs.
All generated C is prepared before compilation; all output stays in build/.

One host on every platform (finding 099): the units of host64_census.HOST_UNITS, with POSIX_UNITS
for the Win32 ones off Windows (host_posix.c) and Android's start
(android_main.c); the glue of port/host64 (guest memory, fibers, the FP mode, the launcher). SDL3 is
static (sdl3.py) except on Android, where SDL's Java side loads libSDL3.so. The GS renderer is the
Rust one everywhere: port/gs/gpu_rust.c over the dhd_render library (port/render, built by
port/tools/rustgs.py, release profile; wgpu on DX12 or Vulkan, Metal, Vulkan), with dhd_net, and
the libraries rustc lists for it (--print native-static-libs, checked by rustgs). What differs per
target is in TARGETS: the compiler and its flags, the linker, SDL3's build, the Rust triple, the
output and the fibers' register switch (fiber.c is shared; fiber_win64.c, fiber_sysv64.c or
fiber_arm64.c per CPU and ABI). Bounds are checked by guard pages on every target (guest_memory.c reserves 8 GiB: every
wasm32 address plus offset), and WABT's call-depth counter is a plain global (the module runs on one
OS thread), as dhd64.exe has them since finding 082 W7e.

Toolchains: win-x64 and android-arm64 build on Windows (clang-cl, MSVC's headers and lld-link; the
NDK's clang: sdl3.android_ndk()). linux-x64 builds on Linux or in WSL with clang and lld; its Rust
library comes from rustgs.py on the same machine when it has cargo, else from the Windows side
through WSL's interop (python.exe). macos-arm64 builds on a Mac with Xcode's command-line tools
(Apple clang and ld) for macOS 11 and later. Objects go through the shared object cache (objcache.py)
where it has a folder.

Output (never committed): win-x64 build/host64-exe/dhd64.exe (link.rsp and dhd64.map beside it);
linux-x64 build/host64-linux/dhd64; macos-arm64 build/host64-macos/dhd64; android-arm64
build/android/lib/arm64-v8a/libmain.so (stripped) and libSDL3.so, the two libraries the APK carries
(port/android/build_apk.py), and libmain.debug.so, the same library with its symbols
(llvm-addr2line -e build/android/libmain.debug.so 0xOFFSET for a crash report's frames).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_host64_imports import generate  # noqa: E402
from host64_census import HOST_UNITS  # noqa: E402
import objcache  # noqa: E402
from prepare_wasm2c import check_launcher_exports, module_units, prepare_units  # noqa: E402
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches

ROOT = Path(__file__).resolve().parents[2]
WABT = (DEVCACHES / 'wabt/wabt-1.0.42')
WASI = (DEVCACHES / 'wasi-sdk/wasi-sdk-34.0-x86_64-windows')
LLVM = (Path.home() / 'scoop/apps/llvm/current/bin')
MSVC = Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231')
SDK = Path('C:/Program Files (x86)/Windows Kits/10')
VERSION = '10.0.26100.0'
SOURCE = ROOT / 'port/host64'


class Target:
    def __init__(self, key, rust, out, exe, fiber, jobs=8, outputs=1, flags=(), wabt=WABT, simde=None):
        self.key, self.rust, self.out, self.exe, self.jobs, self.outputs = key, rust, out, exe, jobs, outputs
        self.fiber = fiber  # the fiber switch's adapter, port/host64/fiber_<fiber>.c (the CPU and its ABI)
        self.flags, self.wabt, self.simde = list(flags), Path(os.environ.get('WABT_DIR') or wabt), simde
        self.windows, self.android, self.mac = key == 'win-x64', key == 'android-arm64', key == 'macos-arm64'
        self.posix = not self.windows


MAC_MIN = '11.0'  # the first macOS on Apple silicon (package_release.py's LSMinimumSystemVersion)
ANDROID_API = 30  # sdl3.ANDROID_API: Android 11
TARGETS = {t.key: t for t in (
    Target('win-x64', 'x86_64-pc-windows-msvc', 'build/host64-exe', 'dhd64.exe', 'win64'),
    Target('linux-x64', 'x86_64-unknown-linux-gnu', 'build/host64-linux', 'dhd64', 'sysv64',
           flags=['-march=x86-64-v2', '-fPIC'], wabt='/opt/wabt', simde='/opt/simde-src'),
    # M1 is every Apple silicon Mac's floor
    Target('macos-arm64', 'aarch64-apple-darwin', 'build/host64-macos', 'dhd64', 'arm64', jobs=4,
           flags=['-arch', 'arm64', '-mmacosx-version-min=' + MAC_MIN, '-mcpu=apple-m1'],
           wabt='/opt/homebrew/opt/wabt', simde=Path.home() / 'DevCaches/simde-src'),
    # -march=armv8.2-a: the Snapdragon 888's cores and every arm64 phone since 2018; tuned for its
    # Cortex-X1 and A78s (finding 091)
    Target('android-arm64', 'aarch64-linux-android', 'build/android', 'libmain.so', 'arm64', jobs=2, outputs=8,
           flags=['--target=aarch64-linux-android%d' % ANDROID_API, '-march=armv8.2-a', '-mtune=cortex-a78',
                  '-fPIC', '-fomit-frame-pointer']),
)}
# Off Windows: the POSIX host instead of the Win32 one
POSIX_UNITS = {'port/host/host_win32.c': 'port/host/host_posix.c'}
WINDOWS_ONLY = ('port/gs/replay.c',)  # Win32 calls (GS replay)
ANDROID_UNITS = ['port/host/android_main.c', 'port/host/android_perf.c', 'port/host/touch.c']
# Android: only SDL's entry leaves the library; everything else may be garbage-collected
# (--gc-sections), as /opt:ref does for dhd64.exe.
ANDROID_EXPORTS = '{\n  global: SDL_main;\n  local: *;\n};\n'


def host_units(t):
    if t.windows:
        return list(HOST_UNITS)
    return [POSIX_UNITS.get(u, u) for u in HOST_UNITS if u not in WINDOWS_ONLY] + (ANDROID_UNITS if t.android else [])


def glue(t):
    return [SOURCE / n for n in ('guest_memory.c', 'guest_memory_%s.c' % ('win32' if t.windows else 'posix'),
                                 'fiber.c', 'fiber_%s.c' % t.fiber, 'fp_mode.c', 'bridge.c', 'diagnostics.c',
                                 'launcher.c')]


def fiber_test(t, a, cc, wabt, out, env):
    """--fiber-test: port/host64/selftest_fiber.c with the target's compiler, guest memory and fiber
    adapter (glue's first five units). Run here with its two guard-fault children (android-arm64:
    built only)."""
    import signal
    sources = [SOURCE/'selftest_fiber.c'] + glue(t)[:5]
    flags = ['-DWASM_RT_STACK_DEPTH_COUNT=1', '-I'+str(wabt/'include'), '-I'+str(SOURCE)]
    exe = out/('selftest_fiber.exe' if t.windows else 'selftest_fiber')
    if t.windows:
        cmd = [a.llvm/'clang-cl.exe', '--target=x86_64-pc-windows-msvc', '/nologo', '/O2', '/MT', *flags,
               '/imsvc'+str(MSVC/'include'), *['/imsvc'+str(SDK/'Include'/VERSION/x) for x in ('ucrt','um','shared')],
               *sources, '/Fe'+str(exe), '/Fo'+str(out)+os.sep, '/link', '/libpath:'+str(MSVC/'lib/x64'),
               '/libpath:'+str(SDK/'Lib'/VERSION/'ucrt/x64'), '/libpath:'+str(SDK/'Lib'/VERSION/'um/x64'),
               'onecore.lib', '/subsystem:console']
    else:
        cmd = [cc, *t.flags, '-O2', '-pthread', *flags, *sources, '-o', exe] + (['-rdynamic'] if not t.mac else [])
    r = subprocess.run(list(map(str, cmd)), cwd=out, env=env, capture_output=True, text=True, timeout=300)
    if r.returncode:
        raise RuntimeError(' '.join(map(str, cmd)) + '\n' + r.stdout + r.stderr)
    if t.android:
        print('PASS: built %s (run it on a device: adb push, then --guest-guard and --native-guard)' % exe)
        return 0
    r = subprocess.run([str(exe)], cwd=out, env=env, capture_output=True, text=True, timeout=60)
    print(r.stdout + r.stderr, end='')
    if r.returncode or 'PASS' not in r.stdout:
        raise RuntimeError('selftest_fiber failed (%d)' % r.returncode)
    for flag, report in (('--guest-guard', 'uncommitted guest address 0x006000000'),
                         ('--native-guard', 'native stack: 8 MiB less 64 KiB written')):
        r = subprocess.run([str(exe), flag], cwd=out, env=env, capture_output=True, text=True, timeout=60)
        code = r.returncode & 0xffffffff if t.windows else r.returncode
        faulted = code in (0xc0000005, 0xc00000fd) if t.windows else code in (-signal.SIGSEGV, -signal.SIGBUS)
        if not faulted or report not in r.stderr:
            raise RuntimeError('selftest_fiber %s: expected a fault after %r, got %#x:\n%s' % (flag, report, code, r.stderr))
        print('PASS: %s faults (%#x)' % (flag, code))
    return 0


def rust_library(t, work):
    """The Rust library's report (rustgs.build): here, or for linux-x64 in WSL without cargo, from
    rustgs.py on the Windows side (WSL's interop, python.exe)."""
    import rustgs  # cargo through rustgs only (its flags, version gate, checks and cache)
    if t.key != 'linux-x64' or shutil.which('cargo') or not shutil.which('python.exe'):
        try:
            return rustgs.build(t.rust)
        except rustgs.Refused as why:
            raise RuntimeError('rustgs: refused: %s' % why)

    def wslpath(flag, path):
        return subprocess.run(['wslpath', flag, str(path)], capture_output=True, text=True, check=True).stdout.strip()
    report = work / 'rust.json'
    r = subprocess.run(['python.exe', wslpath('-w', ROOT / 'port/tools/rustgs.py'), 'build', '--target', t.rust,
                        '--report', wslpath('-w', report)], capture_output=True, text=True, encoding='utf-8',
                       errors='replace')
    if r.returncode or not report.exists():
        raise RuntimeError('rustgs.py (Windows) failed:\n' + (r.stdout + r.stderr)[-4000:])
    rep = json.loads(report.read_text())
    rep['lib'] = wslpath('-u', rep['lib'])
    return rep


def sdl(t, a):
    """(SDL3's library, its include directory, the libraries it needs at link time)."""
    import sdl3
    if t.android:
        lib, include, _java = sdl3.android_build()
        return lib, include, []
    if a.sdl_lib and not a.sdl_include:
        raise ValueError('--sdl-lib needs --sdl-include')
    if t.windows:
        lib, include = (a.sdl_lib, a.sdl_include) if a.sdl_lib else sdl3.build()
        libs = list(sdl3.SYSTEM_LIBS)
    elif a.sdl_lib and a.sdl_libs is not None:
        lib, include, libs = a.sdl_lib, a.sdl_include, []
    else:
        lib, include, libs = sdl3.posix_build()  # cached: built once
        if a.sdl_lib:
            lib, include = a.sdl_lib, a.sdl_include
    if a.sdl_libs is not None:
        libs = shlex.split(a.sdl_libs)
    return Path(lib).resolve(), Path(include).resolve(), libs


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('module', type=Path, nargs='*')
    p.add_argument('--target', choices=sorted(TARGETS), default='win-x64')
    p.add_argument('--fake', action='store_true', help='the integration fixture (win-x64)')
    p.add_argument('--run', action='store_true', help='bounded headless execution (not android-arm64)')
    p.add_argument('--fiber-test', action='store_true',
                   help="build and run port/host64/selftest_fiber.c with the target's fiber switch (no module)")
    p.add_argument('--out', type=Path, help="default the target's folder under build/")
    p.add_argument('--sdl-lib', type=Path, help="SDL3 static library (default: sdl3.py's for the target)")
    p.add_argument('--sdl-include', type=Path, help='its include directory (with --sdl-lib)')
    p.add_argument('--sdl-libs', metavar='LIBS', help='the libraries the static SDL3 needs, one quoted string '
                   "(default: sdl3.py's for the target)")
    p.add_argument('--host-library', type=Path, help='reuse an explicitly supplied host library')
    p.add_argument('--jobs', type=int, help="parallel compiles (default: the target's)")
    p.add_argument('--num-outputs', type=int, help='WABT split C output count for .wasm input')
    p.add_argument('--wabt', type=Path, help="WABT (wasm2c and its runtime; default WABT_DIR, else the target's)")
    p.add_argument('--simde', type=Path, help='SIMDe headers (the wasm SIMD shim; default SIMDE_DIR, else the '
                   "target's)")
    p.add_argument('--wasi', type=Path, default=WASI)
    p.add_argument('--llvm', type=Path, default=LLVM)
    p.add_argument('--debug', action='store_true',
                   help='line tables (win-x64: and a PDB, for sample_profile.py); code generation is unchanged')
    pgo = p.add_mutually_exclusive_group()
    pgo.add_argument('--pgo-generate', action='store_true',
                     help='instrumented build (clang targets): an Android run writes files/dhd-<pid>.profraw at its exit')
    pgo.add_argument('--pgo-use', type=Path, metavar='PROFDATA',
                     help='optimize with this profile (llvm-profdata merge of the .profraw files)')
    a = p.parse_args()
    t = TARGETS[a.target]
    if not a.fiber_test and bool(a.module) == a.fake:
        p.error('provide --fake or one .wasm / a set of wasm2c .c files')
    if a.fake and not t.windows:
        p.error('--fake builds the win-x64 fixture')
    if t.windows and (a.pgo_generate or a.pgo_use):
        p.error('--pgo-* are for the clang targets')
    jobs, outputs = a.jobs or t.jobs, a.num_outputs or t.outputs
    wabt = (a.wabt or t.wabt).resolve()
    simde = a.simde or Path(os.environ.get('SIMDE_DIR') or t.simde or str(DEVCACHES / 'simde-src'))
    out = (a.out or ROOT / t.out).resolve()
    if out.drive.upper() == 'C:' or str(out).startswith('/mnt/c/'):
        p.error('build output must stay off C:')
    work = out / 'work' if t.android else out  # Android: out holds the APK's libraries only
    scratch = work / 'tmp'
    for d in (out, work, work / 'obj', scratch):
        d.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMP=str(scratch), TEMP=str(scratch), TMPDIR=str(scratch))
    commands = []

    def run(cmd, timeout=180):
        cmd = list(map(str, cmd)); commands.append(cmd)
        r = subprocess.run(cmd, cwd=work, env=env, capture_output=True, text=True,
                           encoding='utf-8', errors='replace', timeout=timeout)
        if r.returncode:
            raise RuntimeError(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        return r.stdout + r.stderr
    rt = wabt / 'share/wabt/wasm2c'
    if t.android:
        import sdl3
        ndk = sdl3.android_ndk()
        tools = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
        cc, ar = tools / 'clang.exe', tools / 'llvm-ar.exe'
        if not cc.exists():
            p.error('no NDK clang at %s' % cc)
    elif t.posix:
        ndk, tools, cc, ar = None, None, 'clang', 'ar'
    if a.fiber_test:
        return fiber_test(t, a, None if t.windows else cc, wabt, work, env)
    try:
        # SDL3 and the Rust library first: their checks are quick to fail, the module's C is not
        sdl_lib, sdl_include, sdl_libs = sdl(t, a)
        report = rust_library(t, work)
        if a.fake:
            wasm = work / 'fake.wasm'
            run([a.wasi/'bin/clang.exe', '-O2', '-nostdlib', '-ffp-contract=off',
                 '-Wl,--no-entry', '-Wl,--allow-undefined', '-Wl,--export-table',
                 '-Wl,--export-memory', '-Wl,--export=__stack_pointer',
                 '-Wl,--initial-memory=2147483648', '-Wl,--max-memory=2147483648',
                 '-Wl,--stack-first', '-Wl,--global-base=1880096768',  # HOST64_MODULE_BASE (memory_layout.h), as W5
                 SOURCE/'fake_module.c', '-o', wasm])
        elif len(a.module) == 1 and a.module[0].suffix == '.wasm':
            wasm = a.module[0].resolve()
        else:
            wasm = None
        wasm2c = wabt / 'bin' / ('wasm2c.exe' if os.name == 'nt' else 'wasm2c')
        units = module_units(work, run, wasm2c, wasm, a.module, outputs)
        rows, exports = generate(work/'core.h', work, linux=t.posix)
        check_launcher_exports(exports)
        prepare_units(units, work, exact_f32=True)
        # The module runs on one OS thread only (its fibers; posted jobs and callbacks are drained
        # there, finding 082 W7e), as its shared __stack_pointer global already requires. WABT's
        # call-depth counter is thread-local, which costs a TLS lookup on every wasm call and
        # return; a copy of wasm-rt.h that lets the build choose the storage class makes it a
        # plain global. The depth check itself (WASM_RT_STACK_DEPTH_COUNT, limit 512) is unchanged.
        header = (wabt/'include/wasm-rt.h').read_text()
        block = '#ifdef WASM_RT_C11_AVAILABLE\n#define WASM_RT_THREAD_LOCAL _Thread_local\n'
        if header.count(block) != 1:
            raise ValueError('wasm-rt.h: WASM_RT_THREAD_LOCAL block not found (WABT version changed?)')
        start = header.index(block); end = header.index('#endif\n', start) + len('#endif\n')
        header = (header[:start] + '#ifndef WASM_RT_THREAD_LOCAL\n' + header[start:end] + '#endif\n' +
                  header[end:])
        if t.windows:
            # Guard pages: WABT keeps every load alive with GNU asm, which it gates on __GNUC__, and
            # supports guard pages on every 64-bit GNU host; clang-cl has the same asm but does not
            # define __GNUC__.
            force_read = '#elif defined(__GNUC__) && WASM_RT_MEMCHECK_GUARD_PAGES'
            for source in units + list(work.glob('*-impl.h')):
                text = source.read_text()
                if force_read in text:
                    source.write_text(text.replace(force_read, '#elif (defined(__GNUC__) || defined(__clang__)) && '
                                                               'WASM_RT_MEMCHECK_GUARD_PAGES'))
            supported = '(WASM_RT_NONCONFORMING_ALLOW_OOB_READ_ELIMINATION || defined(__GNUC__))'
            if header.count(supported) != 1:
                raise ValueError('wasm-rt.h: guard-page support test not found (WABT version changed?)')
            header = header.replace(supported, supported[:-1] + ' || defined(__clang__))')
        (work/'wasm-rt.h').write_text(header)
        # Bounds checks by guard pages: what lies beyond the memory's size is reserved, never
        # committed, so an out-of-bounds access faults and its reporter (guest_memory_*.c) names the
        # guest address and ends the process, as the trap did. WABT's own fault-to-trap handler stays
        # off (WASM_RT_SKIP_SIGNAL_RECOVERY): ours handle faults. Traps can come from any EE fiber:
        # report and end (launcher.c).
        defines = ['-DWASM_RT_STACK_DEPTH_COUNT=1', '-DWASM_RT_MAX_CALL_STACK_DEPTH=512', '-DWASM_RT_THREAD_LOCAL=',
                   '-DWASM_RT_MEMCHECK_GUARD_PAGES=1', '-DWASM_RT_SKIP_SIGNAL_RECOVERY=1']
        defines += ['-DHOST64_FAKE_MODULE=1'] if a.fake else ['-DWASM_RT_TRAP_HANDLER=host64_trap']
        includes = ['-I'+str(work), '-I'+str(wabt/'include'), '-I'+str(rt), '-I'+str(SOURCE),
                    '-I'+str(ROOT/'port/runtime'), '-I'+str(ROOT/'port/include'), '-I'+str(ROOT/'port/host'),
                    '-I'+str(simde)]
        # __FILE__ (and assert's) would carry this checkout's and the toolchain's folders into the
        # game; map them so a packaged build names no builder path.
        maps = [(ROOT, 'src'), (out, 'gen'), (wabt, 'wabt')]
        maps += [(a.llvm, 'llvm')] if t.windows else [(ndk, 'ndk')] if t.android else [(simde, 'simde')]
        prefix_maps = ['-ffile-prefix-map=%s=%s' % (spelling, new) for old, new in maps
                       for spelling in dict.fromkeys((str(old), str(old).replace('\\', '/')))]  # a set's order varies
        pgo_flags = []
        if a.pgo_generate:
            pgo_flags = ['-fprofile-generate']
        elif a.pgo_use:  # copied under its hash, so the object cache keys follow its content
            data = a.pgo_use.read_bytes()
            profile = work / ('pgo-%s.profdata' % hashlib.sha256(data).hexdigest()[:16])
            profile.write_bytes(data)
            pgo_flags = ['-fprofile-use=' + str(profile)]
        if t.windows:
            common = [a.llvm/'clang-cl.exe', '--target=x86_64-pc-windows-msvc', '/nologo',
                      '/O2', '/clang:-O3', '/clang:-march=x86-64-v2', '/clang:-mtune=znver3',
                      '/clang:-ffp-model=strict', '/clang:-ffp-contract=off',
                      '/clang:-ftrivial-auto-var-init=zero', '/clang:-fno-strict-aliasing',
                      '/clang:-fwrapv', '/Gy', '/Gw', '/MT', '/w', *defines, *includes,
                      '/imsvc'+str(MSVC/'include')]
            common += ['/imsvc'+str(SDK/'Include'/VERSION/x) for x in ('ucrt','um','shared')]
            common += ['/clang:' + m for m in prefix_maps]
            if a.debug:
                common += ['/clang:-gline-tables-only', '/clang:-gcodeview']
        else:
            # one section per function and datum, as the Windows build's /Gy: the link
            # garbage-collects what nothing references (/opt:ref there)
            common = [cc, *t.flags, '-c', '-O3', '-ffp-model=strict', '-ffp-contract=off',
                      '-ftrivial-auto-var-init=zero', '-fno-strict-aliasing', '-ffunction-sections',
                      '-fdata-sections', '-fwrapv', '-pthread', '-w', *pgo_flags, *defines, *includes,
                      *prefix_maps] + (['-gline-tables-only'] if a.debug else [])
        objdir = work/'obj'
        suffix = '.obj' if t.windows else '.o'

        def compile_one(source, timeout=1800):
            source = Path(source)
            rel = source.relative_to(ROOT).as_posix() if source.is_relative_to(ROOT) else source.name
            extra = ['-I'+str(Path(sdl_include).resolve())] if rel.startswith('port/') else []  # threads, locks, timers
            if rel.endswith('vu0_interp.c'):
                extra += ['-DPS2_VU_AOT']
            if rel.endswith('gs/gpu_rust.c'):
                extra += ['-I'+str(ROOT/'port/render/include')]  # dhd_render.h
            if rel.endswith('host64/launcher.c') and t.android:
                extra += ['-Dmain=dhd_launcher_main']  # SDL_main (android_main.c) calls it
            if 'libmpeg2/' in rel:
                extra += ['-I'+str(ROOT/'port/third_party/libmpeg2')]
            obj = objdir/(rel.replace('/', '_') + suffix)
            cmd = common + extra + (['/c', source, '/Fo'+str(obj)] if t.windows else [source, '-o', obj])
            # the shared object cache (objcache.py); this output folder is a token
            objcache.compile(list(map(str, cmd)), str(obj), lambda c: run(c, timeout=timeout) and None,
                             roots=[(work, 'out')], cwd=work, env=env, hit='')
            return obj
        # VU1's straight-line blocks as C (vu1_aot.inc and vu1_native_clip.inc, from the game's
        # .vutext: DHD_VUTEXT or assets/vutext.bin); vu0_interp.c runs them
        import vu1_aot
        vu1_aot.generate(work/'vu1_aot.inc')
        # the window's icon where the program carries none of its own (present.c's window_icon)
        icon = (ROOT/'port/res/icon.png').read_bytes()
        (work/'dhd_icon.inc').write_text('static const unsigned char dhd_icon_png[] = {%s};\n' % ','.join(map(str, icon)))
        if a.host_library:
            library = a.host_library.resolve()
            if not library.exists():
                raise ValueError(f'host library does not exist: {library}')
            host_objects = []
        else:
            with ThreadPoolExecutor(jobs) as pool:
                host_objects = list(pool.map(lambda u: compile_one(ROOT/u, 900), host_units(t)))
            library = work/('host64.lib' if t.windows else 'libhost64.a')
            library.unlink(missing_ok=True)  # ar adds to an archive that is there: a stale member could win
            run([a.llvm/'llvm-lib.exe', '/out:'+str(library), *host_objects] if t.windows else
                [ar, 'rcs', library, *host_objects])
        sources = units + [rt/'wasm-rt-impl.c', work/'imports_gen.c'] + glue(t)
        with ThreadPoolExecutor(jobs) as pool:
            objects = list(pool.map(compile_one, sources))
        # rustc's list for the library (checked by rustgs against the target's allow-list)
        rust_libs = [x for x in report['native_static_libs'] if x != '-lc']
        exe = out/t.exe
        if t.windows:
            # dxgi.lib: gpu_rust.c's adapter choice and video memory (CreateDXGIFactory1)
            libs = ['onecore.lib','user32.lib','gdi32.lib',
                    'dxgi.lib','ole32.lib','shell32.lib','advapi32.lib',
                    'winmm.lib','ws2_32.lib','uuid.lib']
            libs += [str(sdl_lib)] + [x for x in sdl_libs if x not in libs]
            # rustc's list: import libs and /defaultlib:libcmt, which /MT already names; raw-dylib
            # imports (d3d12, dxgi, ...) need no lib. dxguid.lib: gpu_rust.c's IID_IDXGIFactory1 and
            # IID_IDXGIDevice
            libs += [report['lib']] + [x for x in report['native_static_libs'] + ['dxguid.lib'] if x not in libs]
            # the icon (port/res/dhd.rc), shown by Explorer, the taskbar and the game's windows
            res = work/'dhd.res'
            run([a.llvm/'llvm-rc.exe', '/fo', str(res), str(ROOT/'port/res/dhd.rc')])
            rsp = work/'link.rsp'
            rsp.write_text('\n'.join('"'+str(x)+'"' for x in objects+[library,res]+libs))
            link = [a.llvm/'lld-link.exe', '@'+str(rsp), '/out:'+str(exe),
                    '/libpath:'+str(MSVC/'lib/x64'),
                    '/libpath:'+str(SDK/'Lib'/VERSION/'ucrt/x64'),
                    '/libpath:'+str(SDK/'Lib'/VERSION/'um/x64'),
                    '/subsystem:windows','/entry:mainCRTStartup','/opt:ref','/opt:icf','/dynamicbase','/nxcompat',  # windowed: no console on a double-click, the log goes to dhd.log (host_console_init)
                    '/map:'+str(out/'dhd64.map'), '/errorlimit:0']
            if a.debug:
                link += ['/debug', '/pdb:'+str(out/'dhd64.pdb')]
            (work/'link.log').write_text(run(link))
        elif t.android:
            exports_map = work/'exports.map'
            exports_map.write_text(ANDROID_EXPORTS)
            lib_dir = out/'lib'/'arm64-v8a'
            lib_dir.mkdir(parents=True, exist_ok=True)
            debug = out/'libmain.debug.so'
            rsp = work/'link.rsp'
            rsp.write_text('\n'.join('"%s"' % str(x).replace('\\', '/') for x in
                                     objects + [library, report['lib'], sdl_lib]))
            run([cc, t.flags[0], '-shared', *pgo_flags[:1], '-o', debug, '@'+str(rsp),
                 '-Wl,-soname,libmain.so', '-Wl,--version-script='+str(exports_map),
                 '-Wl,--gc-sections', '-Wl,--no-undefined', '-Wl,--build-id=sha1',
                 '-Wl,-z,max-page-size=16384', '-Wl,-z,noexecstack', '-Wl,-z,relro', '-Wl,-z,now',
                 '-landroid', *rust_libs, '-lm'], timeout=1800)
            exe = lib_dir/t.exe
            run([tools/'llvm-strip.exe', '--strip-unneeded', '-o', exe, debug])
            run([tools/'llvm-strip.exe', '--strip-unneeded', '-o', lib_dir/'libSDL3.so', sdl_lib])
        elif t.mac:
            # ld64: -dead_strip as --gc-sections; no -rdynamic (exporting every symbol keeps the
            # module-side code of fps.c alive, which names guest data the host does not have); -S: no
            # debug map (its STABS name every object and archive by its path on this machine)
            run([cc, *t.flags[:3], *pgo_flags[:1], '-Wl,-dead_strip', '-Wl,-S', *objects, library, report['lib'], sdl_lib,
                 *[w for x in sdl_libs for w in x.split()], *[w for x in rust_libs for w in x.split()],
                 '-o', exe], timeout=900)
        else:
            # LLVM's lld: GNU ld on a tree under /mnt (WSL's 9p) took minutes, past the timeout;
            # lld links the same objects there in about 2 s
            run([cc, t.flags[0], '-pthread', '-rdynamic', '-fuse-ld=lld', '-Wl,--gc-sections', *pgo_flags[:1],
                 *objects, library, report['lib'], sdl_lib, *sdl_libs, '-o', exe, *rust_libs, '-ldl', '-lm'],
                timeout=900)
        (work/'failure.log').unlink(missing_ok=True)
        host_summary = 'reused host library' if a.host_library else f'{len(host_objects)} {t.key} host objects'
        print(f'PASS: {len(rows)} generated imports; {host_summary}; {exe} ({exe.stat().st_size / 1048576:.1f} MB)',
              flush=True)
        if a.run and not t.android:
            if a.fake:
                (work/'fixture.txt').write_bytes(b'host64-proof')
            env.update(DHD_HEADLESS='1', DHD_SND_OFF='1')
            result = run([exe], timeout=60)
            if a.fake:
                for test in range(1,12):
                    result += run([exe,'--negative',test],timeout=30)
            (work/'run.log').write_text(result)
            print(result, end='')
    except (RuntimeError, ValueError) as error:
        (work/'failure.log').write_text(str(error))
        raise SystemExit(str(error)[-6000:])
    finally:
        (work/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')


if __name__ == '__main__':
    main()
