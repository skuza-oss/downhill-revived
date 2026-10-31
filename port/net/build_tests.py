"""Build and run the network module's tests as x64 Windows programs (clang-cl and Visual Studio's
x64 libraries). Separate from the port's build. nettool links the WebSocket client (ws.h), the Rust
library rustgs.py builds (dhd_render.lib, which carries dhd_net).
usage: python port/net/build_tests.py [--no-run] [PROGRAM...]"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'port', 'tools'))
OUT = os.path.join(ROOT, 'build', 'port', 'net')
FLAGS = '/nologo /O2 /W4 -Wno-deprecated-declarations /D_CRT_SECURE_NO_WARNINGS /MT'
CLIENT = ['hash.c', 'json.c', 'client.c', 'test_guest.c']  # client.c locks with SDL3 (sdl3.py's x64 build)
RUST = '<rust>'  # the Rust library and the system libraries rustc lists for it
PROGRAMS = {  # name: (sources, libraries, run in the tests, SDL3)
    'test_hash': (['hash.c', 'test_hash.c', 'test_guest.c'], '', True, False),
    'test_json': (['json.c', 'test_json.c'], '', True, False),
    'test_loopback': (CLIENT + ['test_loopback.c'], 'winmm.lib', True, True),
    'test_join': (['join.c', 'test_join.c'], '', True, False),
    'test_session': (CLIENT + ['test_session.c'], 'winmm.lib', True, True),
    'nettool': (CLIENT + ['net.c', 'join.c', 'nettool.c'], 'winmm.lib user32.lib ' + RUST, False, True),
}


def rust_libs():
    # A C-only wire test may reuse an existing renderer archive (no Rust sources change).
    prebuilt = os.environ.get('DHD_NET_TEST_RUST_LIB')
    if prebuilt:
        import check_rust_lib
        return ' '.join(['"%s"' % prebuilt] + sorted(check_rust_lib.allowed_native_libs()))
    import rustgs
    try:
        report = rustgs.build(rustgs.X64)
    except rustgs.Refused as why:
        sys.exit('rustgs: refused: %s' % why)
    return ' '.join(['"%s"' % report['lib']] + report['native_static_libs'])


def main():
    os.makedirs(OUT, exist_ok=True)
    vswhere = r'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    vs = subprocess.run([vswhere, '-latest', '-property', 'installationPath'], capture_output=True,
                        text=True).stdout.strip()
    llvm = os.path.expanduser(r'~\scoop\apps\llvm\current\bin')
    cmd = os.path.join(OUT, 'build.cmd')
    with open(cmd, 'w') as f:
        f.write('@set "PATH=%%PATH%%;%s"\n' % os.path.dirname(vswhere))
        f.write('@call "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat" x64 >nul\n' % vs)
        f.write('@set "PATH=%s;%%PATH%%"\n' % llvm)
        wanted = [a for a in sys.argv[1:] if not a.startswith('-')] or list(PROGRAMS)
        for name in wanted:
            sources, libs, run, sdl = PROGRAMS[name]
            if RUST in libs:
                libs = libs.replace(RUST, rust_libs())
            exe = os.path.join(OUT, name + '.exe')
            files = ' '.join('"%s"' % os.path.join(HERE, s) for s in sources)
            flags = FLAGS
            if sdl:
                import sdl3
                sdl_lib, sdl_include = sdl3.build()
                flags += ' /I"%s"' % sdl_include
                libs += ' "%s" %s' % (sdl_lib, ' '.join(sdl3.SYSTEM_LIBS))
            f.write('clang-cl %s /Fo"%s\\\\" %s /Fe:"%s" /link %s\n' % (flags, OUT, files, exe, libs))
            f.write('@if errorlevel 1 exit /b 1\n')
            if run and '--no-run' not in sys.argv:
                f.write('"%s"\n@if errorlevel 1 exit /b 1\n' % exe)
    return subprocess.run(['cmd', '/c', cmd]).returncode


if __name__ == '__main__':
    sys.exit(main())
