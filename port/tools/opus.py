"""libopus for the PC build: the voice chat's codec (port/host/voice.c, finding 131).

    python port/tools/opus.py          prints the static library it built (or had built)

build_host64.py calls build() for the win-x64 target, as it calls sdl3.build(). It downloads
libopus's release source once (pinned version, SHA-256 checked against xiph.org's SHA256SUMS.txt;
nothing else is accepted), unpacks it under build/port/opus and builds a static library with CMake
and Ninja for 64-bit Windows (x86_64-pc-windows-msvc, clang, the static C runtime that dhd64.exe
uses), as sdl3.py builds SDL3. The download is kept in build/downloads, so a rebuild needs no
network. libopus is BSD-3-Clause (port/release/THIRD-PARTY-NOTICES.txt). wasm_objects() builds the
same source for the browser's network module (port/web/build_net.py). The other native targets have
no voice chat yet (voice.c builds without it)."""
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sdl3  # noqa: E402  the same download, tool and target helpers

VERSION = '1.5.2'
URL = 'https://downloads.xiph.org/releases/opus/opus-%s.tar.gz' % VERSION
SHA256 = '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1'

ROOT = Path(__file__).resolve().parents[2]
DOWNLOADS = ROOT / 'build' / 'downloads'
OUT = ROOT / 'build' / 'port' / 'opus'

# The codec alone: no programs, tests, docs or the deep-learning extras (1.5's DRED and OSCE, which
# need model files); float API on; intrinsics as CMake finds them for the target.
OPTIONS = dict(OPUS_BUILD_SHARED_LIBRARY='OFF', OPUS_BUILD_TESTING='OFF', OPUS_BUILD_PROGRAMS='OFF',
               OPUS_INSTALL_PKG_CONFIG_MODULE='OFF', OPUS_INSTALL_CMAKE_CONFIG_MODULE='OFF',
               OPUS_DRED='OFF', OPUS_OSCE='OFF', OPUS_DEEP_PLC='OFF', BUILD_TESTING='OFF')


def fetch():
    tarball = DOWNLOADS / ('opus-%s.tar.gz' % VERSION)
    if not tarball.exists() or sdl3.sha256(tarball) != SHA256:
        DOWNLOADS.mkdir(parents=True, exist_ok=True)
        part = tarball.with_suffix('.part')
        print('opus: downloading', URL)
        with urllib.request.urlopen(URL, timeout=120) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f)
        got = sdl3.sha256(part)
        if got != SHA256:
            part.unlink()
            sys.exit('opus: %s has SHA-256 %s, expected %s' % (URL, got, SHA256))
        os.replace(part, tarball)
    return tarball


def build(target=sdl3.X64_TARGET):
    """The static library and its include directory (holding opus/opus.h); built once per version
    and target, under build/port/opus/<version>-<arch>."""
    base = OUT / ('%s-%s' % (VERSION, target.split('-')[0]))
    lib = base / 'build' / 'opus.lib'
    include = base / 'src' / ('opus-%s' % VERSION) / 'include'
    stamp = base / 'built.txt'
    recipe = repr(sorted(OPTIONS.items())) + target
    if lib.exists() and stamp.exists() and stamp.read_text() == recipe:
        return lib, include
    tarball = fetch()
    src_parent = base / 'src'
    if src_parent.exists():
        shutil.rmtree(src_parent)
    src_parent.mkdir(parents=True)
    with tarfile.open(tarball) as t:
        t.extractall(src_parent, filter='data')
    src = src_parent / ('opus-%s' % VERSION)
    bld = base / 'build'
    if bld.exists():
        shutil.rmtree(bld)
    import ninja
    cmake = sdl3.tool('cmake', sdl3.CMAKE_DEFAULT)
    clang = sdl3.tool('clang')
    cmd = [cmake, '-S', str(src), '-B', str(bld), '-G', 'Ninja',
           '-DCMAKE_MAKE_PROGRAM=' + os.path.join(ninja.BIN_DIR, 'ninja.exe'),
           '-DCMAKE_C_COMPILER=' + clang, '-DCMAKE_C_COMPILER_TARGET=' + target,
           '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
           '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW']
    cmd += ['-D%s=%s' % kv for kv in sorted(OPTIONS.items())]
    for step in (cmd, [cmake, '--build', str(bld)]):
        p = subprocess.run(step, capture_output=True, text=True)
        if p.returncode:
            sys.exit('opus build failed:\n' + (p.stdout + p.stderr)[-4000:])
    if not lib.exists():
        found = sorted(bld.glob('*.lib'))
        if not found:
            sys.exit('opus: no library in %s after the build' % bld)
        lib = found[0]
    stamp.write_text(recipe)
    return lib, include


def wasm_objects(clang, sysroot):
    """libopus for the browser's network module (port/web/build_net.py): the same pinned source,
    its own source lists (celt_sources.mk, silk_sources.mk, opus_sources.mk: the float build, no
    intrinsics, none of the deep-learning extras) compiled by wasi-sdk's clang for wasm32-wasip1.
    wasi-sdk has no archiver, so this returns the object files (and the include directory) for the
    module's link, compiled once per version under build/port/opus/<version>-wasm32."""
    base = OUT / ('%s-wasm32' % VERSION)
    include = base / 'src' / ('opus-%s' % VERSION) / 'include'
    objdir = base / 'obj'
    stamp = base / 'built.txt'
    recipe = 'wasm32 ' + str(clang)
    if stamp.exists() and stamp.read_text() == recipe:
        return sorted(objdir.glob('*.o')), include
    tarball = fetch()
    if base.exists():
        shutil.rmtree(base)
    (base / 'src').mkdir(parents=True)
    with tarfile.open(tarball) as t:
        t.extractall(base / 'src', filter='data')
    src = base / 'src' / ('opus-%s' % VERSION)
    lists = {}
    for mk in ('celt_sources.mk', 'silk_sources.mk', 'opus_sources.mk'):
        name = None
        for line in (src / mk).read_text().splitlines():
            if '=' in line and not line.startswith(('\t', ' ')):
                name, _, line = line.partition('=')
                name = name.strip()
                lists[name] = []
            if name:
                lists[name] += [w for w in line.replace('\\', ' ').split() if w.endswith('.c')]
            if not line.rstrip().endswith('\\'):
                name = None
    files = []
    for key in ('CELT_SOURCES', 'SILK_SOURCES', 'SILK_SOURCES_FLOAT', 'OPUS_SOURCES', 'OPUS_SOURCES_FLOAT'):
        files += lists[key]
    objdir.mkdir()
    flags = ['--target=wasm32-wasip1', '--sysroot=' + str(sysroot), '-O2', '-DOPUS_BUILD', '-DVAR_ARRAYS',
             '-DHAVE_LRINTF', '-DHAVE_LRINT', '-DNDEBUG', '-w'] + ['-I' + str(src / d) for d in ('include', 'celt', 'silk', 'silk/float', '.')]
    for f in files:
        out = objdir / (f.replace('/', '_')[:-2] + '.o')
        p = subprocess.run([str(clang), *flags, '-c', str(src / f), '-o', str(out)], capture_output=True, text=True)
        if p.returncode:
            sys.exit('opus (wasm32): %s failed:\n%s' % (f, (p.stdout + p.stderr)[-4000:]))
    stamp.write_text(recipe)
    return sorted(objdir.glob('*.o')), include


if __name__ == '__main__':
    print(build()[0])
