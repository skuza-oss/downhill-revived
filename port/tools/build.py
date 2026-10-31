"""Build the game from source in one command: the wasm core first (build_wasm.py), then one of

    python port/tools/build.py                      # the native game (build_host64.py): build/host64-exe/dhd64.exe
    python port/tools/build.py --target linux-x64   # the same on Linux or in WSL (any build_host64.py target)
    python port/tools/build.py --package            # the release zip (package_release.py, which builds the game)
    python port/tools/build.py --web                # the browser build (port/web/build.py): build/web

Arguments after -- go to that last step, e.g. `build.py -- --debug` or `build.py --package -- --jobs 8`.
--no-core skips build_wasm.py when build/wasm/dhd_core.wasm is known to be current. Every step's own
output is shown; the first one that fails stops the build."""
import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'port' / 'tools'
WASM = ROOT / 'build' / 'wasm' / 'dhd_core.wasm'


def step(name, cmd):
    print(f'== {name}: {" ".join(map(str, cmd[1:]))}', flush=True)
    start = time.time()
    if subprocess.run(list(map(str, cmd)), cwd=ROOT).returncode:
        sys.exit(f'build.py: {name} failed')
    print(f'== {name}: done in {time.time() - start:.0f} s', flush=True)


def main():
    argv = sys.argv[1:]
    rest = argv[argv.index('--') + 1:] if '--' in argv else []
    argv = argv[:argv.index('--')] if '--' in argv else argv
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    last = p.add_mutually_exclusive_group()
    last.add_argument('--package', action='store_true', help='the release zip for this platform')
    last.add_argument('--web', action='store_true', help='the browser build')
    p.add_argument('--target', help="build_host64.py's target (default its own: win-x64)")
    p.add_argument('--no-core', action='store_true', help='reuse build/wasm/dhd_core.wasm as it is')
    a = p.parse_args(argv)
    if (a.package or a.web) and a.target:
        p.error('--target is for the native game (package_release.py takes --platform after --)')
    py = sys.executable
    if not a.no_core:
        step('core', [py, TOOLS / 'build_wasm.py'])
    elif not WASM.exists():
        sys.exit(f'build.py: --no-core, but there is no {WASM.relative_to(ROOT)}')
    if a.package:
        step('package', [py, TOOLS / 'package_release.py', *rest])
    elif a.web:
        step('web', [py, ROOT / 'port' / 'web' / 'build.py', *rest])
    else:
        step('game', [py, TOOLS / 'build_host64.py', WASM, *(['--target', a.target] if a.target else []), *rest])


if __name__ == '__main__':
    main()
