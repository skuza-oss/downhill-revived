"""The player launcher (port/launcher, finding 093), built for a release.

    python port/tools/build_launcher.py [--platform win|mac|linux]   # prints the executable's path

`npx tauri build --no-bundle` in port/launcher (npm install first when node_modules is missing),
with the renderer's flags (port/tools/rustgs.py): on Windows the static C runtime, and everywhere
CARGO_HOME / RUSTUP_HOME remapped to cargo / rustup (and the repository to dhd, for dhd_settings
and dhd_disc) so the crates' panic locations name no folder of this machine. The launcher is
built for the machine it runs on: DownhillRevived.exe on Windows,
DownhillRevived on macOS (Apple Silicon) and Linux (x64; WebKitGTK 4.1's development files are
needed). package_release.py puts it in the platform's package. It is refused if it still holds one
of those folders or the repository's path, or wasn't built with port/include/version.h's version.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / 'port' / 'launcher'
RELEASE_DIR = LAUNCHER / 'src-tauri' / 'target' / 'release'
VERSION_H = ROOT / 'port' / 'include' / 'version.h'


def host_platform():
    return 'win' if sys.platform == 'win32' else 'mac' if sys.platform == 'darwin' else 'linux'


def exe_path(platform):
    return RELEASE_DIR / ('DownhillRevived.exe' if platform == 'win' else 'DownhillRevived')


def spellings(path):
    p = os.path.normpath(str(path))
    return list(dict.fromkeys((p, p.replace('\\', '/'))))


def homes():
    out = {}
    for var, name in (('CARGO_HOME', 'cargo'), ('RUSTUP_HOME', 'rustup')):
        value = os.environ.get(var)
        if os.name == 'nt' and (not value or os.path.splitdrive(os.path.normpath(value))[0].upper() == 'C:'):
            sys.exit('%s must be set, off C: (the Rust toolchain lives on F:)' % var)
        out[name] = value or os.path.join(os.path.expanduser('~'), '.' + name)  # rustup's own defaults
    return out


def rustflags():
    flags = ['-Ctarget-feature=+crt-static'] if os.name == 'nt' else []
    for name, value in homes().items():
        flags += ['--remap-path-prefix=%s=%s' % (s, name) for s in spellings(value)]
    # dhd_settings and dhd_disc (port/render/settings, disc) are outside the launcher's workspace:
    # cargo names their sources by their full paths
    flags += ['--remap-path-prefix=%s=dhd' % s for s in spellings(ROOT)]
    return flags


def version():
    m = re.search(r'#define DHD_VERSION "([0-9.]+(?:-[0-9A-Za-z.-]+)?)"', VERSION_H.read_text(encoding='utf-8'))
    return m.group(1)


def problems(exe):
    """Local folders or a wrong version in the exe."""
    data = exe.read_bytes()
    out = []
    folders = [s for v in homes().values() for s in spellings(v)] + spellings(ROOT)
    for folder in folders:
        for enc in ('utf-8', 'utf-16-le'):
            if folder.encode(enc).lower() in data.lower():
                out.append('names %s (%s)' % (folder, enc))
    if ('DownhillRevived-launcher/%s' % version()).encode() not in data:
        out.append('was not built with version %s (port/include/version.h)' % version())
    return out


def native_libraries(exe):
    """The C libraries the crates' build scripts compiled for exe (ring's core, beside it under build/),
    linked into it: package_release.game_data_runs takes their bytes as theirs."""
    return sorted(p for pattern in ('*/out/*.lib', '*/out/*.a') for p in (exe.parent / 'build').glob(pattern))


def build(platform=None):
    platform = platform or host_platform()
    if platform != host_platform():
        sys.exit('the %s launcher is built on %s itself (this is %s)' % (platform, platform, host_platform()))
    npx = shutil.which('npx') or sys.exit('npx (Node.js) is needed to build the launcher')
    if not (LAUNCHER / 'node_modules' / '.bin').is_dir():
        subprocess.run([shutil.which('npm'), 'install', '--no-audit', '--no-fund'], cwd=str(LAUNCHER), check=True)
    env = {k: v for k, v in os.environ.items() if k not in ('RUSTFLAGS', 'CARGO_BUILD_RUSTFLAGS', 'CARGO_TARGET_DIR')}
    env['CARGO_ENCODED_RUSTFLAGS'] = '\x1f'.join(rustflags())
    if os.name != 'nt':
        env['RUSTC_WRAPPER'] = ''  # the repository's sccache wrapper (.cargo/config.toml) is a Windows program
    print('building the launcher (port/launcher)', flush=True)
    r = subprocess.run([npx, 'tauri', 'build', '--no-bundle'], cwd=str(LAUNCHER), env=env,
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        sys.exit('the launcher build failed:\n' + (r.stdout + r.stderr)[-4000:])
    exe = exe_path(platform)
    found = problems(exe)
    if found:
        sys.exit('%s is not fit to publish:\n  %s' % (exe, '\n  '.join(found)))
    return exe


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--platform', choices=('win', 'mac', 'linux'), default=host_platform())
    print(build(ap.parse_args().platform))
