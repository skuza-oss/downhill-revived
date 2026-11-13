"""check_rust_lib.py: the refusals rustgs.py applies to every Rust library it builds (plan section 3.1).

    python port/tools/check_rust_lib.py LIB [LIB ...] [--tree DIR ...] [--extra DIR ...]
                                        [--native-static-libs "kernel32.lib ..."]

LIB is the x64 static library (dhd_render.lib, an archive of COFF objects), the Android or Linux one
(libdhd_render.a, ELF objects), the macOS one (libdhd_render.a, Mach-O objects, read whole), a .wasm
module or the wasm-bindgen glue. Exit 0 when nothing is found; otherwise each problem is printed and the exit is 1.

Refused:
- an absolute drive path (package_release.ABSOLUTE, as the release check reads it: ASCII in the data
  sections, minus the chance bytes package_release.chance_bytes skips; UTF-16 everywhere) in any
  archive member's non-code sections, or a wasm module's non-code sections;
- a marker anywhere, in any case: \\Users\\ and /Users/, this account's user name, DevCaches (the
  cargo home, rustup home and target dirs all live there), the worktree path(s) (--tree; default
  this repository) and any --extra folder, each with and without its drive, in either slash;
- `/defaultlib:msvcrt` (the dynamic C runtime; the x64 exe links libcmt);
- a native-static-libs entry (as rustc printed it) outside the list spike S1 recorded
  (port/render/native-static-libs-x86_64.txt; Android's: native-static-libs-aarch64-linux-android.txt).
The remapped dependency paths (cargo\\registry\\src\\..., rustup\\...) and std's /rustc/<hash>/library/...
hold no drive and no marker, so they pass; so do drive paths under a build-machine folder of the
toolchain's own std libraries (compiler_builtins' C objects name Rust CI's C:\\a\\rust\\rust\\...;
see toolchain_roots), as package_release allows wgpu-native's.
"""
import argparse
import functools
import hashlib
import os
import re
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parents[1]
ALLOWED_LIBS_FILE = ROOT / 'port' / 'render' / 'native-static-libs-x86_64.txt'
ALLOWED_LIBS_FILES = {'aarch64-linux-android': ROOT / 'port' / 'render' / 'native-static-libs-aarch64-linux-android.txt',
                      'x86_64-unknown-linux-gnu': ROOT / 'port' / 'render' / 'native-static-libs-x86_64-unknown-linux-gnu.txt',
                      'aarch64-apple-darwin': ROOT / 'port' / 'render' / 'native-static-libs-aarch64-apple-darwin.txt',
                      'aarch64-apple-ios': ROOT / 'port' / 'render' / 'native-static-libs-aarch64-apple-ios.txt',
                      'aarch64-apple-ios-sim': ROOT / 'port' / 'render' / 'native-static-libs-aarch64-apple-ios.txt'}
DYNAMIC_CRT = re.compile(rb'[/-]defaultlib:\s*"?msvcrt', re.I)
SCN_CODE = 0x00000020
SCN_EXECUTE = 0x20000000


@functools.lru_cache(maxsize=None)
def _release():
    sys.path.insert(0, str(TOOLS))
    import package_release
    return package_release


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def allowed_libs_file(triple=None):
    return ALLOWED_LIBS_FILES.get(triple, ALLOWED_LIBS_FILE)


def allowed_native_libs(triple=None):
    words = []
    for line in allowed_libs_file(triple).read_text(encoding='utf-8').splitlines():
        if line.strip() and not line.lstrip().startswith('#'):
            words += line.split()
    return {w.lower() for w in words}


def path_markers(folder):
    p = os.path.normpath(str(folder))
    out = set()
    for s in (p, p[2:] if re.match(r'^[A-Za-z]:', p) else p):
        out |= {s, s.replace('\\', '/')}
    return {s for s in out if len(s.strip('\\/')) > 2}


def markers(trees, extra):
    found = {'\\Users\\', '/Users/', 'DevCaches'}
    for var in ('USERNAME', 'USER'):
        name = os.environ.get(var, '')
        if len(name) > 2:
            found.add(name)
    for folder in list(trees) + list(extra):
        found |= path_markers(folder)
    return sorted(found)


# ---- containers -----------------------------------------------------------------------------------

def ar_members(data):
    """[(name, bytes)] of a COFF archive (!<arch>)."""
    out, pos, longnames = [], 8, b''
    while pos + 60 <= len(data):
        head = data[pos:pos + 60]
        if head[58:60] != b'`\n':
            raise ValueError('bad archive member header at %d' % pos)
        name = head[:16].rstrip()
        size = int(head[48:58].strip() or b'0')
        body = data[pos + 60:pos + 60 + size]
        if name == b'//':
            longnames = body
        elif name.startswith(b'/') and name[1:].isdigit() and longnames:
            off = int(name[1:])
            end = longnames.find(b'\0', off)
            name = longnames[off:end if end >= 0 else None].rstrip(b'/\n')
        out.append((name.decode('latin-1'), body))
        pos += 60 + size + (size & 1)
    return out


def coff_sections(obj):
    """[(name, characteristics, bytes)] of a COFF object (regular or bigobj); None if it is not one."""
    if len(obj) < 20:
        return None
    sig1, sig2 = struct.unpack_from('<HH', obj, 0)
    if sig1 == 0 and sig2 == 0xFFFF:
        version = struct.unpack_from('<H', obj, 4)[0]
        if version < 2 or len(obj) < 56:
            return None          # a short import object: names only
        count = struct.unpack_from('<I', obj, 44)[0]
        table = 56
    else:
        machine, count = struct.unpack_from('<HH', obj, 0)
        if machine not in (0x8664, 0x14c, 0xaa64, 0x1c4):
            return None
        table = 20 + struct.unpack_from('<H', obj, 16)[0]
    out = []
    for k in range(count):
        h = table + 40 * k
        if h + 40 > len(obj):
            return None
        name = obj[h:h + 8].rstrip(b'\0').decode('latin-1')
        size, ptr = struct.unpack_from('<II', obj, h + 16)
        flags = struct.unpack_from('<I', obj, h + 36)[0]
        out.append((name, flags, obj[ptr:ptr + size] if ptr else b''))
    return out


SHF_EXECINSTR = 0x4


def elf_sections(obj):
    """[(name, flags, bytes)] of an ELF64 little-endian object; None if it is not one."""
    if len(obj) < 64 or obj[:4] != b'\x7fELF' or obj[4] != 2 or obj[5] != 1:
        return None
    shoff = struct.unpack_from('<Q', obj, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', obj, 0x3A)
    if not shoff or shentsize < 64 or shoff + shnum * shentsize > len(obj):
        return None
    heads = [struct.unpack_from('<IIQQQQ', obj, shoff + k * shentsize) for k in range(shnum)]
    strtab = heads[shstrndx] if shstrndx < shnum else None
    out = []
    for name_off, kind, flags, _addr, offset, size in heads:
        name = ''
        if strtab:
            start = strtab[4] + name_off
            end = obj.find(b'\0', start)
            name = obj[start:end].decode('latin-1')
        out.append((name, flags, obj[offset:offset + size] if kind != 8 else b''))  # 8: SHT_NOBITS
    return out


def leb(data, pos):
    value = shift = 0
    while True:
        b = data[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return value, pos


def wasm_sections(data):
    """[(id, bytes)] of a wasm module."""
    out, pos = [], 8
    while pos < len(data):
        sid = data[pos]
        size, pos = leb(data, pos + 1)
        out.append((sid, data[pos:pos + size]))
        pos += size
    return out


def units(path):
    """[(label, all bytes, non-code bytes)] of a library, module or text file."""
    data = Path(path).read_bytes()
    if data[:8] == b'!<arch>\n':
        out = []
        for name, body in ar_members(data):
            secs = coff_sections(body)
            elf = elf_sections(body) if secs is None else None
            if secs is not None:
                plain = b'\0'.join(b for _, f, b in secs if not f & (SCN_CODE | SCN_EXECUTE))
                out.append((name, body, plain))
            elif elf is not None:  # the Android library's members
                plain = b'\0'.join(b for _, f, b in elf if not f & SHF_EXECINSTR)
                out.append((name, body, plain))
            else:
                out.append((name, body, body))
        return out
    if data[:4] == b'\0asm':
        return [('wasm', data, b'\0'.join(b for sid, b in wasm_sections(data) if sid != 10))]
    return [('file', data, data)]


# ---- checks ---------------------------------------------------------------------------------------

@functools.lru_cache(maxsize=None)
def toolchain_roots(sysroot_lib):
    """The build-machine folders in the toolchain's own std libraries (rustup ships compiler_builtins'
    C objects with Rust CI's paths, C:\\a\\rust\\rust\\...): package_release.library_roots of each
    rlib in sysroot_lib (<sysroot>/lib/rustlib/<triple>/lib). Paths under them are the toolchain's,
    not ours, as wgpu-native's are in package_release."""
    roots = set()
    for rlib in sorted(Path(sysroot_lib).glob('*.rlib')):
        roots |= _release().library_roots(str(rlib))
    return tuple(sorted(roots))


# A POSIX build machine's folder in a library's bytes: rustup's std for macOS is built on Rust CI's
# Mac runners, so its compiler_builtins objects name /Users/runner/work/rust/... (the /Users/ marker).
POSIX_ROOT = re.compile(rb'(?<![A-Za-z0-9])/(?:Users|home)/[\x21-\x7e]+')


@functools.lru_cache(maxsize=None)
def toolchain_posix_roots(sysroot_lib):
    """toolchain_roots' POSIX counterpart: the first four levels of each /Users/... or /home/... path
    in the toolchain's own rlibs (/Users/runner/work/rust/). Paths under them are the toolchain's."""
    roots = set()
    for rlib in sorted(Path(sysroot_lib).glob('*.rlib')):
        for m in POSIX_ROOT.finditer(rlib.read_bytes()):
            cuts = [i for i, c in enumerate(m.group(0)) if c == ord('/')]
            if len(cuts) >= 5:
                roots.add(m.group(0)[:cuts[4] + 1])
    return tuple(sorted(roots))


def sysroot_lib(triple, cwd=None):
    import subprocess
    r = subprocess.run(['rustc', '--print', 'sysroot'], cwd=str(cwd or ROOT / 'port' / 'render'),
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('check_rust_lib: rustc --print sysroot failed: %s' % r.stderr.strip())
    return Path(r.stdout.strip()) / 'lib' / 'rustlib' / triple / 'lib'


def problems(path, trees=(ROOT,), extra=(), native_static_libs=None, sysroot=None, triple=None):
    """The problems of one library, module or glue file. sysroot: <sysroot>/lib/rustlib/<triple>/lib
    of the toolchain that built it (its std's build-machine paths are allowed); None allows none."""
    pr = _release()
    found = []
    allowed = toolchain_roots(str(sysroot)) if sysroot else ()
    posix_allowed = toolchain_posix_roots(str(sysroot)) if sysroot else ()
    marks = [(m, m.lower().encode()) for m in markers(trees, extra)]
    for label, data, plain in units(path):
        where = '%s(%s)' % (Path(path).name, label)
        for text, kind in ((plain, 'ASCII'), (pr.utf16_strings(data), 'UTF-16')):
            for m in pr.ABSOLUTE.finditer(text):
                if kind == 'ASCII' and pr.chance_bytes(text, m):
                    continue
                if m.group(0).startswith(allowed):
                    continue
                found.append('%s: %s absolute path %r' % (where, kind, m.group(0)[:120].decode('ascii', 'replace')))
        ours = data
        for root in posix_allowed:  # the toolchain's own folders hold no marker of ours
            ours = ours.replace(root, b'')
        lowered = ours.lower()
        utf16 = pr.utf16_strings(data).lower()
        for m, low in marks:
            if low in lowered or low in utf16:
                found.append('%s: marker %r' % (where, m))
        if DYNAMIC_CRT.search(data):
            found.append('%s: /defaultlib:msvcrt (the dynamic C runtime)' % where)
    if native_static_libs is not None:
        known = allowed_native_libs(triple)
        extra_libs = [l for l in native_static_libs if l.lower() not in known]
        if extra_libs:
            found.append('native-static-libs outside the S1 list (%s): %s' % (
                allowed_libs_file(triple).relative_to(ROOT).as_posix(), ' '.join(extra_libs)))
    return list(dict.fromkeys(found))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('libs', nargs='+', type=Path)
    ap.add_argument('--tree', action='append', type=Path, help='a worktree whose path must not appear (default: this one)')
    ap.add_argument('--extra', action='append', type=Path, default=[], help='another folder that must not appear')
    ap.add_argument('--native-static-libs', help="rustc's native-static-libs line (space separated)")
    ap.add_argument('--target', default='x86_64-pc-windows-msvc', help='the triple LIB was built for')
    ap.add_argument('--sysroot-lib', type=Path, help='<sysroot>/lib/rustlib/<triple>/lib (default: rustc --print sysroot)')
    a = ap.parse_args()
    sysroot = a.sysroot_lib or sysroot_lib(a.target)
    nsl = a.native_static_libs.split() if a.native_static_libs is not None else None
    bad = []
    for lib in a.libs:
        bad += problems(lib, a.tree or [ROOT], a.extra, nsl, sysroot)
        nsl = None
    for p in bad:
        print('check_rust_lib: ' + p)
    print('check_rust_lib: %s' % ('%d problems' % len(bad) if bad else 'ok'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
