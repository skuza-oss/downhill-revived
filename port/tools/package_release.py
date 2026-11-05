"""The public release for one platform, built from the 64-bit game on the Rust renderer:
build/release/DownhillRevived-<version>-<platform>.zip (and the same files unzipped beside it), and
latest.json. macOS and Linux are experimental (EXPERIMENTAL): their zips are named
DownhillRevived-<version>-mac-experimental.zip and -linux-experimental.zip, and latest.json's
"platforms" entries name them, so their launchers update from them (update.rs).

    python port/tools/build_wasm.py                      # the game core, build/wasm/dhd_core.wasm
    python port/tools/package_release.py                 # dhd.exe from it, checked, zipped
    python port/tools/package_release.py --wasm PATH     # from another dhd_core.wasm
    python port/tools/package_release.py --exe PATH      # a ready build_host64.py dhd64.exe (link.rsp beside it)
    python port/tools/package_release.py --launcher PATH # a ready build_launcher.py DownhillRevived.exe
    python port/tools/package_release.py --platform mac --game PATH [--launcher PATH]
    python port/tools/package_release.py --merge OTHER/latest.json   # add another machine's platform
    python port/tools/package_release.py --platform android         # the APK (signing key: BUILDING.md)
    python port/tools/package_release.py --sums                     # only SHA256SUMS again

--platform is win, mac (Apple Silicon), linux (x64) or android (arm64); the default is the host's.
The launcher is built for the host only (build_launcher.py), so each platform is packaged on its own machine (a
mac or linux layout can be looked at from Windows with --launcher and --game). mac and linux take
the game as --game PATH (build_host64's targets for them); --exe is the same option.

Windows (-win.zip). dhd.exe is build_host64.py's dhd64.exe (wasm2c, clang-cl, lld-link; the Rust renderer,
port/render, linked in as dhd_render.lib) under build/release-work/, with an x64 SDL3 static
library (sdl3.py's by default, or --sdl-lib/--sdl-include). In the package it is named dhd.exe:
the name every earlier release used, so the updater of a 32-bit release (0.3.1 and before) puts it
in place of the old one (finding 088, CL3).

The version is port/include/version.h's DHD_VERSION, the one the exe was built with (--version,
if given, must be the same). Beside the zip goes latest.json, the update manifest the launcher
reads (port/launcher/src-tauri/src/update.rs, finding 054): {"version", "url", "sha256", "size"
(the Windows zip's: what 0.6.x launchers read), "page", "platforms": {"win"|"mac"|"linux": {"url"
(the zip as a GitHub release asset of tag v<version>, or --base-url + the zip's name), "sha256"
(the zip's), "size"}}}. Each run adds its platform to a latest.json of the same version already
there (--merge adds the entries of another machine's). Publish the zips and latest.json, with every
platform in it, as assets of the GitHub release tagged v<version>. Every run (and --merge) also
writes SHA256SUMS beside them: the SHA-256 of each release file there (DownhillRevived-<version>-*
but the link map, and latest.json), as sha256sum prints them; after copying another machine's
files in, --sums writes it again.

Android (finding 143, experimental): --platform android builds the release APK with
port/android/build_apk.py --release from --wasm (signed with the key the DHD_ANDROID_* variables
name; never in the repository) or takes a ready one (--apk), and puts it in the release folder as
DownhillRevived-<version>-android-experimental.apk, with latest.json's "android" entry (the game's
update notice reads it, port/host/android_update.c; launchers ignore the key). The APK carries
LICENSE.txt and THIRD-PARTY-NOTICES.txt (with the Rust crates linked into libmain.so) as assets.
Checked: build_apk.check_release (one signer, the release certificate build_apk.RELEASE_CERT_SHA256
unless --new-signing-key, not debuggable, version, arm64-v8a only), every file in
it as the zips' are (game data names and bytes, servers, local paths), libmain.so's version and
its absolute paths and servers as the game's.

The Windows zip holds one folder, DownhillRevived-<version>, with:
  DownhillRevived.exe      the launcher (port/launcher, finding 093; built by build_launcher.py): settings,
                           rider name, updates, Discord; it starts dhd.exe, and dhd.exe started straight
                           opens it (port/hle/launcher_link.c). It imports only Windows' own DLLs (checked)
  dhd.exe                  the 64-bit game; it imports only Windows' own DLLs (checked)
  README.txt               how to play (port/release/README.txt)
  LICENSE.txt              the GPL-3.0 text (LICENSE)
  THIRD-PARTY-NOTICES.txt  port/release/THIRD-PARTY-NOTICES.txt, then the licences of the Rust
                           crates linked into dhd.exe and DownhillRevived.exe, generated from
                           port/render/Cargo.lock, port/launcher/src-tauri/Cargo.lock and the
                           crates' own licence files (rust_notices)
  dxcompiler.dll           Microsoft's DirectX Shader Compiler, x64, unchanged (port/tools/dxc.py):
                           the renderer compiles its Direct3D 12 shaders with it (finding 084 §14)

The package is refused if:
  - port/ or src/ has uncommitted changes to tracked files (a release is built from committed source);
  - dhd.exe is not an x64 executable linked with exactly one dhd_render.lib (link.rsp beside it), or
    that library is unfit (rust_lib_problems: it names a folder of this machine, or
    check_rust_lib.problems finds one of its refusals), or dhd.exe wasn't built with the version;
  - dhd.exe holds an absolute path (a builder's folder, C:\\Users\\..., the repository's own path), in
    ASCII or UTF-16 (a drive path "X:\\..." in ASCII: in the data sections, where strings are; in
    machine code such bytes are an instruction's operand). SDL's own list of USB vibration device
    paths (SDL_OWN_PATHS) is the one exception;
  - dhd.exe holds a server address other than the official one (dhd.skuza.app, the built-in
    default): another workers.dev host, DHD_NET_SERVER when set, or the server line of a dhd.ini
    named by --settings;
  - dhd.exe or DownhillRevived.exe imports a DLL that is not Windows' own, or the launcher holds a
    folder of this machine or another version (build_launcher.problems);
  - a packaged file name looks like game data, or any packaged file holds a run of 64 bytes or more
    of the retail ELF (port/tools/scan_retail.py) other than the string literals and tables the
    decompiled source itself holds (scan_retail.SOURCE) and the Rust library's own bytes. The
    game's data comes from the player's disc (port/hle/game_data.c);
  - a Rust crate linked in has a licence other than the permissive ones in LICENCES, or no text
    for the one it is used under.
A third-party binary (third_party in main) is checked by its SHA-256 instead: it must be exactly the file
its publisher released (pinned in its fetch script), so it can hold neither game data nor a path or
server of ours. The byte scans would only find coincidences in it (dxcompiler.dll holds the words
0..14, as a run of the ELF's .data does, and the text "\\temp").

macOS (-mac-experimental.zip): Downhill Revived.app, with Contents/Info.plist (bundle id and name from
tauri.conf.json, the version, LSMinimumSystemVersion 11.0, NSHighResolutionCapable),
Contents/MacOS/DownhillRevived (the launcher, the bundle's executable) and Contents/MacOS/dhd (the
game, a helper started by it), Contents/Resources/icon.icns (the launcher's icon) and the three text
files. On a Mac the bundle is signed ad hoc (codesign --force --deep -s -; no Apple developer
account: players allow it once under Privacy & Security). Elsewhere it isn't, and latest.json is
left alone: such a zip only shows the layout. Linux (-linux-experimental.zip): the Windows layout's folder with
DownhillRevived, dhd and the text files. A zip on both, so the launcher has one extractor; the
executables keep their 0755 mode (unix entries).

The checks are the same on every platform: the version, absolute paths, server names and the
retail-bytes scan in every file, the crates' licences (for the platform's target). The Windows DLL
imports become the libraries a Mach-O or ELF binary needs (otool -L or llvm-objdump; readelf or
llvm-readobj), each on an allow-list: the system's own (/usr/lib, /System/Library) on macOS, the C
runtime (and the launcher's WebKitGTK stack) on Linux, where neither binary may need a symbol
version newer than glibc 2.35 has (LINUX_SYMBOLS: built on Ubuntu 22.04). The Rust renderer library's checks
(rust_lib_problems) read the Windows link.rsp; on mac and linux they are skipped with a note, and
--rust-lib names the library whose bytes the retail scan may find in the game.
"""
import argparse
import functools
import hashlib
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_launcher  # noqa: E402
import dxc  # noqa: E402
import installers  # noqa: E402
import scan_retail  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RELEASE = ROOT / 'port' / 'release'
RENDER = ROOT / 'port' / 'render'
LAUNCHER_CRATE = ROOT / 'port' / 'launcher' / 'src-tauri'
VERSION_H = ROOT / 'port' / 'include' / 'version.h'
WORK = ROOT / 'build' / 'release-work'
# Where players get releases: latest.json names the zip as an asset of the release v<version>.
RELEASES = 'https://github.com/skuza-oss/downhill-revived/releases'


def header_version():
    """DHD_VERSION from port/include/version.h: the one place the release's version is written."""
    m = re.search(r'#define\s+DHD_VERSION\s+"([^"]+)"', VERSION_H.read_text(encoding='utf-8'))
    if not m:
        sys.exit('%s: no DHD_VERSION' % VERSION_H)
    return m.group(1)


GAME_DATA = re.compile(r'\.(iso|bin|img|elf|irx|mc2|ps2|raw|vpk|vag|pss)$|^SCUS_|bios', re.I)
# an absolute Windows path: a drive letter, a separator, then at least two printable characters
ABSOLUTE = re.compile(rb'(?<![A-Za-z0-9])[A-Za-z]:[\\/][\x20-\x7e]{2,}')
# The official public race server (port/hle/host.c): the one server address a release may name.
OFFICIAL_HOST = b'dhd.skuza.app'
# any workers.dev host (other_hosts allows only the official one)
WORKERS_HOST = re.compile(rb'[A-Za-z0-9.-]*workers\.dev', re.I)
# user folders, and the builder's own account name and checkout folders. A build as root (WSL's
# default) looks for root's home instead: "root" is a word every program holds (roots, has_root).
FORBIDDEN = [b'\\Users\\', b'/Users/'] + [
    (b'/root/' if s == 'root' else s.encode())
    for s in {os.environ.get('USERNAME', ''), os.environ.get('USER', '')} if len(s) > 2] + [
    p.encode() for folder in (ROOT, ROOT.parent) if len(folder.parts) > 1
    for p in (str(folder), str(folder).replace('\\', '/'), str(folder)[2:], str(folder)[2:].replace('\\', '/'))]
# SDL_windows.c's list of USB vibration device paths: SDL's own text, not a builder's folder.
SDL_OWN_PATHS = ('C:/Windows/USB_Vibration', 'C:/Windows/USB Vibration')
# Windows' own DLLs: dhd.exe may import nothing else (no DLL is shipped beside it to be imported;
# dxcompiler.dll is loaded by the renderer, not imported).
SYSTEM_DLL = re.compile(r'^(kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|ws2_32|winmm|winhttp|'
                        r'comdlg32|dxgi|api-ms-win-.*|ext-ms-.*|ucrtbase|onecoreuap|'
                        r'bcrypt|imm32|version|setupapi|cfgmgr32|combase|uxtheme|dwmapi|hid|hidapi|'
                        r'shcore|msvcrt|mfplat|xinput1_4|xinput9_1_0|dinput8|wintrust|crypt32|'
                        r'ntdll|bcryptprimitives)\.dll$', re.I)  # the last two: Rust std's
# The launcher's webview shell also uses these (Windows' own as well).
LAUNCHER_DLL = re.compile(r'^(comctl32|secur32|shlwapi)\.dll$', re.I)
RUST_TRIPLE = 'x86_64-pc-windows-msvc'   # the Rust renderer's native target
RUST_LIB = 'dhd_render.lib'              # build_host64.py links it (rustgs.py builds it)
# The libraries a macOS binary may need: the system's own.
MAC_LIB = re.compile(r'^(/usr/lib/|/System/Library/)')
# Linux: the C runtime; the launcher also WebKitGTK and the GTK stack it is drawn with (the
# distribution's packages, README.txt).
LINUX_LIB = re.compile(r'^(libc|libm|libdl|libpthread|librt|libgcc_s|ld-linux-x86-64)\.so\.\d+$')
LINUX_LAUNCHER_LIB = re.compile(r'^lib(webkit2gtk-4\.1|javascriptcoregtk-4\.1|gtk-3|gdk-3|gdk_pixbuf-2\.0|cairo|'
                                r'cairo-gobject|pango-1\.0|pangocairo-1\.0|harfbuzz|soup-3\.0|glib-2\.0|gobject-2\.0|'
                                r'gio-2\.0|atk-1\.0|xdo)\.so\.\d+$')
# The oldest Linux the package starts on: glibc 2.35 (Ubuntu 22.04, Debian 12, Fedora 36). A binary
# linked on a newer system binds the newer symbol versions its headers name (C23's __isoc23_* and
# strlcpy: 2.38; pidfd_spawnp: 2.39; acosf and six other float functions: 2.43), which an older
# loader refuses at start, so release builds are linked on Ubuntu 22.04 (docs/BUILDING.md) and every
# version a binary needs is checked: GLIBC up to LINUX_GLIBC; libgcc_s's unwinder (GCC_) up to
# GCC_4.2.0, in every distribution since 2007; nothing else (no libstdc++: GLIBCXX, CXXABI).
LINUX_GLIBC = (2, 35)
LINUX_SYMBOLS = {'GLIBC': LINUX_GLIBC, 'GCC': (4, 2, 0)}


# The platforms whose builds are experimental (0.7.0: macOS and Linux): "-experimental" after the
# platform in the zip's name. latest.json's key stays the platform's (update.rs PLATFORM).
EXPERIMENTAL = ('mac', 'linux', 'android')


class Platform:
    """What differs between the platforms' packages."""
    def __init__(self, key, triple, exe_suffix, kind, name):
        self.key, self.triple, self.kind, self.name = key, triple, kind, name
        self.launcher, self.game = 'DownhillRevived' + exe_suffix, 'dhd' + exe_suffix
        self.zip_tag = key + ('-experimental' if key in EXPERIMENTAL else '')   # in the zip's name


PLATFORMS = {
    'win': Platform('win', RUST_TRIPLE, '.exe', 'PE x64', 'Windows'),
    'mac': Platform('mac', 'aarch64-apple-darwin', '', 'Mach-O arm64', 'macOS'),
    'linux': Platform('linux', 'x86_64-unknown-linux-gnu', '', 'ELF x86-64', 'Linux'),
    'android': Platform('android', 'aarch64-linux-android', '', 'ELF arm64', 'Android'),
}
PLATFORMS['android'].game, PLATFORMS['android'].launcher = 'libmain.so', None   # no launcher: the APK


def binary_kind(data):
    """'PE x64', 'Mach-O arm64', 'ELF x86-64', 'ELF arm64', or None."""
    if data[:2] == b'MZ' and len(data) > 0x40:
        pe = int.from_bytes(data[0x3C:0x40], 'little')
        if data[pe:pe + 4] == b'PE\0\0' and data[pe + 4:pe + 6] == b'\x64\x86':  # IMAGE_FILE_MACHINE_AMD64
            return 'PE x64'
    if data[:4] == b'\xcf\xfa\xed\xfe' and int.from_bytes(data[4:8], 'little') == 0x0100000C:  # CPU_TYPE_ARM64
        return 'Mach-O arm64'
    if data[:5] == b'\x7fELF\x02' and int.from_bytes(data[18:20], 'little') == 62:  # ELFCLASS64, EM_X86_64
        return 'ELF x86-64'
    if data[:5] == b'\x7fELF\x02' and int.from_bytes(data[18:20], 'little') == 183:  # ELFCLASS64, EM_AARCH64
        return 'ELF arm64'
    return None


def utf16_strings(data):
    """UTF-16LE text in data, as bytes (each character's low byte), for the same checks."""
    return b'\n'.join(m.group(0)[::2] for m in re.finditer(rb'(?:[\x20-\x7e]\x00){4,}', data))


def server_strings(settings):
    found = [os.environ.get('DHD_NET_SERVER', '')]
    if settings:
        for line in open(settings, encoding='utf-8-sig'):
            key, _, value = line.partition('=')
            if key.strip() in ('server', 'DHD_NET_SERVER'):
                found.append(value.split(';')[0].strip().strip('"'))
    out = set()
    for s in filter(None, found):
        host = re.sub(r'^\w+://', '', s).split('/')[0]
        if host.encode().lower() == OFFICIAL_HOST:
            continue  # the official server may be named
        out.add(s.encode())
        if host:
            out.add(host.encode())  # the host name alone, too
    return out


HOST_CHARS = frozenset(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-')


# WORKERS_HOST's own domain part: searched alone, then walked back over the host's characters
WORKERS_SUFFIX = re.compile(rb'workers\.dev')


def other_hosts(text):
    """The hosts WORKERS_HOST matches in text, other than the official server: each WORKERS_SUFFIX
    with the run of host characters before it. Found from the suffix backwards, so the time is
    linear: the regex alone retries every start of a long run of letters and digits, which took
    tens of minutes on the AppImage's bundled libraries."""
    lowered, found = text.lower(), set()
    for m in WORKERS_SUFFIX.finditer(lowered):
        start = m.start()
        while start > 0 and text[start - 1] in HOST_CHARS:
            start -= 1
        found.add(lowered[start:m.end()])
    return sorted(found - {OFFICIAL_HOST})


def code_ranges(data):
    """(offset, size) of the executable's code sections: PE, Mach-O 64 or ELF64."""
    kind = binary_kind(data)
    if kind == 'PE x64':
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        count, optional = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
        for k in range(count):
            header = pe + 24 + optional + 40 * k
            size, offset = struct.unpack_from('<II', data, header + 16)
            if struct.unpack_from('<I', data, header + 36)[0] & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
                yield offset, size
    elif kind == 'Mach-O arm64':
        count, at = struct.unpack_from('<I', data, 16)[0], 32
        for _ in range(count):
            cmd, cmd_size = struct.unpack_from('<II', data, at)
            if cmd == 0x19:  # LC_SEGMENT_64, then its section_64 headers
                for k in range(struct.unpack_from('<I', data, at + 64)[0]):
                    section = at + 72 + 80 * k
                    size, offset = struct.unpack_from('<QI', data, section + 40)
                    if struct.unpack_from('<I', data, section + 64)[0] & 0x80000400:  # S_ATTR_*_INSTRUCTIONS
                        yield offset, size
            at += cmd_size
    elif kind in ('ELF x86-64', 'ELF arm64'):
        table, entry, count = struct.unpack_from('<Q', data, 0x28)[0], *struct.unpack_from('<HH', data, 0x3A)
        for k in range(count):
            kind_, flags, _, offset, size = struct.unpack_from('<IQQQQ', data, table + entry * k + 4)
            if flags & 0x4 and kind_ != 8:  # SHF_EXECINSTR, not SHT_NOBITS
                yield offset, size
    else:
        raise ValueError('not a PE, Mach-O or ELF executable')


def without_code(data):
    """data with the bytes of the executable's code sections zeroed. The compiler puts every string in a
    data section, never in code; in code a 4-byte immediate after an opcode can spell a drive path by
    chance (push 0x407c2f3a is 'h:/|@'), depending only on where the linker placed things."""
    out = bytearray(data)
    for offset, size in code_ranges(data):
        out[offset:offset + size] = bytes(size)
    return bytes(out)


def chance_bytes(data, m):
    """True for an ASCII drive-path match that can't be a string: a C string literal ends in a NUL,
    and a real path is long. A short run (under 16 bytes) followed by another byte, or right after a
    byte that is not ASCII, is numbers that spell a path by chance, like the float table in 0.2.8's
    .rdata (3.4421 is the bytes 76 3A 5C 40) or the C runtime's double table "\\xe5Y:/(A6>\\0"."""
    if len(m.group(0)) >= 16:
        return False
    return (m.end() < len(data) and data[m.end()] != 0) or (m.start() > 0 and data[m.start() - 1] >= 0x80)


@functools.lru_cache(maxsize=None)
def library_roots(library):
    """The folders of the machine that built a static library (its first three levels, like
    C:\\Users\\runneradmin\\.cargo\\), as absolute paths in the library's own bytes show them."""
    roots = set()
    for m in ABSOLUTE.finditer(Path(library).read_bytes()):
        cuts = [i for i, c in enumerate(m.group(0)) if c in b'\\/']
        if len(cuts) >= 4:
            roots.add(m.group(0)[:cuts[3] + 1])
    return frozenset(roots)


def game_data_runs(data, elf, libraries=()):
    """The runs of the retail ELF in data (scan_retail.scan) that are neither the source's own
    (scan_retail.allowed) nor bytes of a static library linked in."""
    blobs = [Path(library).read_bytes() for library in libraries]
    return [(at, e, n) for at, e, n in scan_retail.scan(data, elf)
            if not scan_retail.allowed(elf, e, n) and not any(data[at:at + n] in b for b in blobs)]


def rust_lib_problems(rust_lib):
    """What makes the Rust renderer's library (dhd_render.lib, built by rustgs.py) unfit to link into a
    package. Its build-machine folders must be none of ours: library_roots(rust_lib) may hold only the
    Rust toolchain's own (rustup's std ships compiler_builtins with Rust CI's C:\\a\\rust\\rust\\...;
    check_rust_lib.toolchain_roots), never a folder of this machine (rustgs remaps CARGO_HOME and
    RUSTUP_HOME; an unremapped library names them). check_rust_lib.problems (rustgs's own refusals:
    paths, markers, the dynamic C runtime) must find nothing either."""
    import check_rust_lib  # noqa: E402  (imports this module back, lazily)
    if not Path(rust_lib).is_file():
        return ['%s: no such library' % rust_lib]
    sysroot = check_rust_lib.sysroot_lib(RUST_TRIPLE)
    toolchain = set(check_rust_lib.toolchain_roots(str(sysroot)))
    problems = ['%s: build-machine folder %r (not the Rust toolchain\'s): built without rustgs.py\'s path remap?'
                % (Path(rust_lib).name, root.decode('ascii', 'replace'))
                for root in sorted(library_roots(str(rust_lib)) - toolchain)]
    return problems + check_rust_lib.problems(str(rust_lib), sysroot=sysroot)


def exe_problems(exe, servers):
    """What makes exe unfit to publish (see the module's doc). The Rust library's paths are never
    exempt: none of them may reach the exe (rust_lib_problems)."""
    data = exe.read_bytes()
    problems = []
    for text, kind in ((data, 'ASCII'), (utf16_strings(data), 'UTF-16')):
        # drive paths: in the data sections only (see without_code); every other check reads it all
        scanned = without_code(text) if kind == 'ASCII' else text
        for m in ABSOLUTE.finditer(scanned):
            if kind == 'ASCII' and chance_bytes(scanned, m):
                continue
            problems.append('%s absolute path %r' % (kind, m.group(0)[:120].decode('ascii', 'replace')))
        for host in other_hosts(text):
            problems.append('%s server host %r' % (kind, host.decode('ascii', 'replace')))
        lowered = text.lower()
        for s in FORBIDDEN + sorted(servers):
            if s.lower() in lowered:
                problems.append('%s string %r' % (kind, s.decode('ascii', 'replace')))
    return [p for p in problems if not any(s in p for s in SDL_OWN_PATHS)]


def linked_rust_lib(exe):
    """The one dhd_render.lib build_host64.py linked into exe, from the link.rsp it wrote beside it."""
    rsp = exe.with_name('link.rsp')
    if not rsp.exists():
        sys.exit('%s: no link.rsp beside it (build_host64.py writes one); not packaged' % exe)
    entries = [Path(line.strip().strip('"')) for line in rsp.read_text(encoding='utf-8').splitlines() if line.strip()]
    gs = [e for e in entries if e.name.lower() in ('wgpu_native.lib', RUST_LIB)]
    if len(gs) != 1 or gs[0].name.lower() != RUST_LIB:
        sys.exit('%s: link.rsp names %s, not exactly one %s: not built on the Rust renderer'
                 % (exe, ', '.join(map(str, gs)) or 'no GS library', RUST_LIB))
    problems = rust_lib_problems(gs[0])
    if problems:
        sys.exit('%s is not fit to link into a package:\n  %s' % (gs[0], '\n  '.join(problems[:30])))
    return gs[0]


def tool_output(commands, path):
    """The output of the first of commands (each a list, the file appended) found on PATH."""
    for cmd in commands:
        if shutil.which(cmd[0]):
            out = subprocess.run(cmd + [str(path)], capture_output=True, text=True, encoding='utf-8', errors='replace')
            if out.returncode:
                sys.exit('%s failed on %s: %s' % (cmd[0], path, out.stderr[-2000:]))
            return out.stdout
    sys.exit('none of %s is on PATH (LLVM, or the system\'s own tools)' % ', '.join(c[0] for c in commands))


def imported_dlls(exe):
    """The DLL names in the executable's import table (llvm-readobj, LLVM on PATH)."""
    out = tool_output([['llvm-readobj', '--coff-imports']], exe)
    return sorted(set(m.group(1) for m in re.finditer(r'Name:\s*(\S+\.dll)', out, re.I)))


def needed_libraries(path, kind):
    """The shared libraries a Mach-O (its load commands) or ELF (its NEEDED entries) binary needs."""
    if kind == 'Mach-O arm64':
        out = tool_output([['otool', '-L'], ['llvm-objdump', '--macho', '--dylibs-used']], path)
        return sorted(set(line.split()[0] for line in out.splitlines()[1:] if line[:1].isspace() and line.strip()))
    out = tool_output([['llvm-readobj', '--needed-libs'], ['readelf', '-d']], path)
    return sorted(set(re.findall(r'(?<![\w.+-])((?:lib|ld-)[\w.+-]*\.so(?:\.\d+)*)', out)))


def foreign_libraries(path, data, platform, launcher=False):
    """What path needs beyond the platform's allow-list (SYSTEM_DLL, MAC_LIB, LINUX_LIB)."""
    if platform.key == 'win':
        return [d for d in imported_dlls(path) if not SYSTEM_DLL.match(d) and not (launcher and LAUNCHER_DLL.match(d))]
    allowed = [MAC_LIB] if platform.key == 'mac' else [LINUX_LIB] + [LINUX_LAUNCHER_LIB] * launcher
    return [d for d in needed_libraries(path, binary_kind(data)) if not any(a.match(d) for a in allowed)]


def symbol_versions(path):
    """The symbol versions an ELF binary needs from its libraries (.gnu.version_r): {'GLIBC': {(2, 34), ...}, ...}."""
    out = tool_output([['llvm-readelf', '-V'], ['readelf', '-V']], path)
    found = {}
    for name, number in re.findall(r'Name:\s*([A-Z]+)_([0-9][0-9.]*)\b', out):
        found.setdefault(name, set()).add(tuple(int(n) for n in number.split('.')))
    return found


def too_new_symbols(path):
    """The symbol versions a Linux binary needs beyond LINUX_SYMBOLS (none for a package that starts on
    every system with LINUX_GLIBC)."""
    return ['%s_%s' % (name, '.'.join(map(str, v)))
            for name, versions in sorted(symbol_versions(path).items()) for v in sorted(versions)
            if name not in LINUX_SYMBOLS or v > LINUX_SYMBOLS[name]]


# ---- the Rust crates' licences ----

# The licences a linked crate may be used under, in the order one is chosen from an "A OR B";
# the value names the crate's licence file (its name holds the word), when it has several.
LICENCES = {'MIT': 'mit', 'Zlib': 'zlib', 'ISC': 'isc', 'BSD-2-Clause': 'bsd', 'BSD-3-Clause': 'bsd',
            'Apache-2.0': 'apache', 'Unicode-3.0': 'unicode', 'CC0-1.0': 'cc0',
            'MPL-2.0': 'mpl',  # MPL-2.0: the launcher's HTML parser crates (Tauri's), unmodified
            # webpki-roots' data (Mozilla's root certificates, port/render/net): a permissive data licence
            # whose one condition is that its text travels with the data (it does, below)
            'CDLA-Permissive-2.0': 'cdla'}
LICENCE_FILE = re.compile(r'^(licen[cs]e|copying|copyright|notice|unlicense)', re.I)
MIT_TEMPLATE = """Copyright (c) %s

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE."""


def linked_crates(folder=RENDER, root_name='dhd_render', features=('native',), triple=RUST_TRIPLE):
    """The crates linked into a crate of ours (dhd_render.lib; the launcher): cargo metadata (offline,
    --locked: Cargo.lock's versions) with its features for the platform's target, walked from it through
    normal dependencies. Proc-macro crates run only inside the compiler: neither they nor what only
    they use reach the binary. Path crates (no registry source: dhd_net, port/render/net) are this
    project's own, under its licence."""
    cmd = ['cargo', 'metadata', '--offline', '--locked', '--format-version', '1', '--filter-platform', triple]
    if features:
        cmd += ['--features', ','.join(features)]
    # off Windows without the repository's sccache wrapper (.cargo/config.toml), a Windows program
    env = dict(os.environ, **({} if os.name == 'nt' else {'RUSTC_WRAPPER': ''}))
    r = subprocess.run(cmd, cwd=str(folder), env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        sys.exit('cargo metadata failed in %s:\n%s' % (folder, r.stderr[-2000:]))
    meta = json.loads(r.stdout)
    packages = {p['id']: p for p in meta['packages']}
    nodes = {n['id']: n for n in meta['resolve']['nodes']}
    root = next(i for i, p in packages.items() if p['name'] == root_name)
    seen, todo, ours = set(), [root], set()
    while todo:
        i = todo.pop()
        if i in seen or any('proc-macro' in t['kind'] for t in packages[i]['targets']):
            continue
        seen.add(i)
        if packages[i].get('source') is None:  # ours: the root and its path crates
            ours.add(i)
        todo += [d['pkg'] for d in nodes[i]['deps'] if any(k['kind'] is None for k in d['dep_kinds'])]
    seen -= ours
    return sorted((packages[i] for i in seen), key=lambda p: (p['name'], p['version']))


def crate_licence(package):
    """[(licence, text or None)] a crate is used under: one licence of each "AND" part (the first in
    LICENCES of its "OR" choices), with the crate's licence file for it. None: no file of its own
    (Apache-2.0: the text at the end; CC0-1.0: none needed; MIT: the MIT text with the authors;
    MPL-2.0: the same text another crate ships, rust_notices)."""
    expression = (package.get('license') or '').replace('/', ' OR ')
    if not expression:
        sys.exit('Rust crate %s %s names no licence; not packaged' % (package['name'], package['version']))
    folder = Path(package['manifest_path']).parent
    files = sorted(f for f in folder.iterdir() if f.is_file() and LICENCE_FILE.match(f.name))
    out = []
    for part in re.split(r'\s+AND\s+', expression.replace('(', ' ').replace(')', ' ').strip()):
        choices = [c.strip() for c in re.split(r'\s+OR\s+', part)]
        chosen = next((l for l in LICENCES if l in choices), None)
        if not chosen:
            sys.exit('Rust crate %s %s: licence %r has none of %s; not packaged'
                     % (package['name'], package['version'], expression, ', '.join(LICENCES)))
        named = [f for f in files if LICENCES[chosen] in f.name.lower()]
        plain = [f for f in files if not re.search(r'mit|apache|zlib|bsd|isc|unicode|cc0', f.name, re.I)
                 and not f.name.lower().startswith(('notice', 'copyright'))]
        # a single licence with plain files: all of them (ring's LICENSE points to its two others)
        sources = named[:1] if named else plain if plain and len(choices) == 1 else []
        text = '\n\n'.join(f.read_text(encoding='utf-8', errors='replace').strip() for f in sources) or None
        if text is None and chosen == 'MIT':
            text = MIT_TEMPLATE % ', '.join(re.sub(r'\s*<[^>]*>', '', a) for a in package.get('authors') or
                                            ['the %s developers' % package['name']])
        if text is None and chosen not in ('Apache-2.0', 'CC0-1.0', 'MPL-2.0'):
            sys.exit('Rust crate %s %s: no %s licence file in %s; not packaged'
                     % (package['name'], package['version'], chosen, folder))
        if chosen == 'Apache-2.0':  # its NOTICE file travels with it (Apache-2.0 section 4d)
            notices = [f for f in files if f.name.lower().startswith('notice')]
            text = '\n\n'.join(f.read_text(encoding='utf-8', errors='replace').strip() for f in notices) or None
        out.append((chosen, text))
    return out


def indent(text):
    return '\n'.join(('  ' + line).rstrip() for line in text.replace('\r\n', '\n').split('\n'))


def rust_notices(platform=PLATFORMS['win']):
    """The notices of the Rust crates linked into the platform's game and launcher (linked_crates, for its
    target), as text: a list, then each licence text once with the crates it covers, then the Apache
    License 2.0 text (also the WABT runtime's, THIRD-PARTY-NOTICES.txt)."""
    launcher = linked_crates(LAUNCHER_CRATE, 'dhd_launcher', (), platform.triple) if platform.launcher else []
    both = {(p['name'], p['version']): p for p in linked_crates(triple=platform.triple) + launcher}
    crates = [both[k] for k in sorted(both)]
    rows, texts, apache, mpl_without = [], {}, None, []
    for p in crates:
        used = crate_licence(p)
        rows.append('  %s %s  %s%s  %s' % (p['name'], p['version'], p.get('license'),
                                           '' if len(used) == 1 and used[0][0] == p.get('license') else
                                           ' (used under %s)' % ' AND '.join(l for l, _ in used),
                                           p.get('repository') or ''))
        for licence, text in used:
            if text:
                key = ' '.join(text.split())
                texts.setdefault(key, (licence, text, []))[2].append('%s %s' % (p['name'], p['version']))
            elif licence == 'MPL-2.0':
                mpl_without.append('%s %s' % (p['name'], p['version']))
        folder = Path(p['manifest_path']).parent
        if apache is None:
            apache = next((f.read_text(encoding='utf-8', errors='replace').strip() for f in sorted(folder.iterdir())
                           if LICENCE_FILE.match(f.name) and 'apache' in f.name.lower()
                           and 'Apache License' in f.read_text(encoding='utf-8', errors='replace')[:400]), None)
    if apache is None:
        sys.exit('no Apache License 2.0 text among the Rust crates\' licence files; not packaged')
    if mpl_without:  # the MPL-2.0 text is the same for every crate: listed under one that ships it
        mpl = next((t for t in texts.values() if t[0] == 'MPL-2.0' and 'Mozilla Public License' in t[1]), None)
        if mpl is None:
            sys.exit('no MPL-2.0 text for %s; not packaged' % ', '.join(mpl_without))
        mpl[2].extend(mpl_without)
    title = ('Rust crates linked into %s and %s' % (platform.game, platform.launcher) if platform.launcher
             else 'Rust crates linked into %s' % platform.game)
    out = [title,
           '=' * len(title),
           '',
           'The graphics renderer (port/render, with the network client port/render/net) and the launcher',
           '(port/launcher), parts of this project',
           '(GPL-3.0-or-later), are written in Rust and linked with these crates (versions from',
           'port/render/Cargo.lock and port/launcher/src-tauri/Cargo.lock). A crate offered under several',
           'licences is used under the first of MIT, Zlib, ISC, BSD, Apache-2.0. The crates under MPL-2.0 are',
           'used unmodified; their source is on crates.io under the name and version listed. Their licence',
           'texts follow the list; the Apache License 2.0 text is at the end.',
           ''] + rows
    for licence, text, names in texts.values():
        out += ['', '%s: %s' % (licence, ', '.join(names)), '', indent(text)]
    out += ['', 'Apache License 2.0 (the crates above used under it, and the WABT runtime)', '', indent(apache)]
    return '\n'.join(out) + '\n'


# ---- building and packaging ----

def crlf(data):
    if isinstance(data, Path):
        data = data.read_bytes()
    return data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')


def source_changes():
    """Uncommitted changes to tracked files under port/ and src/ (git status lines)."""
    r = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no', '--', 'port', 'src'], cwd=str(ROOT),
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('git status failed: %s' % r.stderr.strip())
    return r.stdout.splitlines()


def require_release_split(out):
    """Stop unless build_host64.py built the game in out with --release: wasm2c's own split parts.
    Its development chunks (finding 151) cost the game ~1.5 % on a phone; split.txt says which."""
    split = out / 'split.txt'
    kind = split.read_text().strip() if split.exists() else 'unknown (no split.txt)'
    if kind != 'wasm2c':
        sys.exit("%s: the game was built with %s C, not a release build: build_host64.py --release "
                 "(wasm2c's own parts); not packaged" % (out, kind))


def build_exe(wasm, sdl_lib, sdl_include, jobs):
    """dhd64.exe from the module, by build_host64.py on the Rust renderer, under WORK."""
    cmd = [sys.executable, str(ROOT / 'port' / 'tools' / 'build_host64.py'), str(wasm),
           '--num-outputs', '8', '--release', '--sdl-lib', str(sdl_lib), '--sdl-include', str(sdl_include),
           '--out', str(WORK), '--jobs', str(jobs)]
    print('building: ' + ' '.join(cmd), flush=True)
    result = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        sys.exit('build_host64.py failed:\n' + (result.stdout + result.stderr)[-4000:])
    print(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else 'built')
    return WORK / 'dhd64.exe'


def lf(data):
    if isinstance(data, Path):
        data = data.read_bytes()
    return data.replace(b'\r\n', b'\n')


def launcher_conf():
    return json.loads((LAUNCHER_CRATE / 'tauri.conf.json').read_text(encoding='utf-8'))


def mac_info_plist(version):
    """Contents/Info.plist of the app bundle: the launcher's name and bundle id (tauri.conf.json)."""
    conf = launcher_conf()
    return plistlib.dumps({
        'CFBundleDevelopmentRegion': 'en',
        'CFBundleDisplayName': conf['productName'],
        'CFBundleExecutable': PLATFORMS['mac'].launcher,
        'CFBundleIconFile': 'icon.icns',
        'CFBundleIdentifier': conf['identifier'],
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': conf['productName'],
        'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': version,
        'CFBundleVersion': version,
        'LSApplicationCategoryType': 'public.app-category.games',
        'LSMinimumSystemVersion': '11.0',  # the first macOS on Apple Silicon
        'NSHighResolutionCapable': True,
    }, sort_keys=True)


def layout(platform, version, launcher, game, notices):
    """{path in the zip: content}, in the zip's order, and the paths of the game and the launcher."""
    text = crlf if platform.key == 'win' else lf
    texts = {
        'README.txt': text(RELEASE / 'README.txt'),
        'LICENSE.txt': text(ROOT / 'LICENSE'),
        'THIRD-PARTY-NOTICES.txt': text((RELEASE / 'THIRD-PARTY-NOTICES.txt').read_bytes().rstrip() + b'\n\n'
                                        + notices.encode('utf-8')),
    }
    if platform.key == 'mac':
        contents = launcher_conf()['productName'] + '.app/Contents/'
        files = {contents + 'Info.plist': mac_info_plist(version),
                 contents + 'MacOS/' + platform.launcher: launcher,
                 contents + 'MacOS/' + platform.game: game,
                 contents + 'Resources/icon.icns': (LAUNCHER_CRATE / 'icons' / 'icon.icns').read_bytes()}
        files.update({contents + 'Resources/' + n: t for n, t in texts.items()})
        return files, contents + 'MacOS/' + platform.game, contents + 'MacOS/' + platform.launcher
    folder = 'DownhillRevived-%s/' % version
    files = {folder + platform.launcher: launcher, folder + platform.game: game}
    files.update({folder + n: t for n, t in texts.items()})
    if platform.key == 'win':
        files[folder + 'dxcompiler.dll'] = dxc.dll('x64').read_bytes()
    return files, folder + platform.game, folder + platform.launcher


def sign_app(app):
    """Ad-hoc signature of the app bundle (codesign, on a Mac only); whether it was signed."""
    if sys.platform != 'darwin':
        print('note: %s is NOT signed (codesign runs on a Mac): this zip only shows the layout, and '
              'latest.json is left alone' % app.name)
        return False
    subprocess.run(['xattr', '-cr', str(app)], check=True)  # codesign refuses Finder info and the like
    for cmd in (['codesign', '--force', '--deep', '-s', '-', str(app)],
                ['codesign', '--verify', '--deep', '--strict', str(app)]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit('%s failed:\n%s' % (' '.join(cmd[:2]), r.stderr[-2000:]))
    print('signed ad hoc: %s' % app)
    return True


def names_server_or_path(content, settings):
    """True when a packaged file (not the game, which exe_problems checks) names a server other than the
    official one, or a local path. The text files may show the official server and example.org addresses,
    nothing else. In the launcher the Rust string constants sit back to back
    ("wss://dhd.skuza.appwss://ws://"); no host holds a scheme, so each scheme starts a new address. A
    bare "ws://" constant may also run into the next strings (Linux: "ws://lobbiesminVersion..."): a
    server address has a dot or a port, or is localhost, so a run of letters without either is not
    one. The official address may run into the next constant the same way ("wss://dhd.skuza.appnamedhd":
    dhd_settings' keys): its host followed by letters with no dot or port is still the official one."""
    split = re.sub(rb'(?=(?:wss?|https?)://)', b'\0', content)
    hosts = set(h.lower().rstrip(b'.') for h in re.findall(rb'wss?://([A-Za-z0-9.:-]+)', split))
    hosts = {OFFICIAL_HOST if h.startswith(OFFICIAL_HOST) and re.fullmatch(rb'[a-z0-9]*', h[len(OFFICIAL_HOST):])
             else h for h in hosts} - {b'races.example.org', OFFICIAL_HOST}
    hosts = {h for h in hosts if b'.' in h or b':' in h or h.startswith(b'localhost')}
    return bool(hosts or other_hosts(content) or any(
        s.lower() in content.lower() for s in FORBIDDEN + sorted(server_strings(settings))))


def write_sums(folder, version):
    """SHA256SUMS in folder: every release file of version there (DownhillRevived-<version>-*, but the
    link map, which is kept, not published) and latest.json, as `sha256sum` prints them."""
    names = sorted(p.name for p in folder.glob('DownhillRevived-%s-*' % version)
                   if p.is_file() and p.suffix != '.map') + (['latest.json'] if (folder / 'latest.json').exists() else [])
    (folder / 'SHA256SUMS').write_text(''.join('%s  %s\n' % (hashlib.sha256((folder / n).read_bytes()).hexdigest(), n)
                                               for n in names), encoding='utf-8', newline='\n')
    return names


def android_release(a, platform, out, manifest_path, page):
    """--platform android: the release APK into out (see the module's doc); returns 0."""
    sys.path.insert(0, str(ROOT / 'port' / 'android'))
    import build_apk  # noqa: E402
    name, code = build_apk.version()
    if name != a.version:
        sys.exit('build_apk.py reads version %s, not %s' % (name, a.version))
    if a.apk:
        apk = a.apk.resolve()
    else:
        notices = rust_notices(platform)
        texts = WORK / 'android-assets'
        if texts.exists():
            shutil.rmtree(texts)
        texts.mkdir(parents=True)
        (texts / 'LICENSE.txt').write_bytes(lf(ROOT / 'LICENSE'))
        (texts / 'THIRD-PARTY-NOTICES.txt').write_bytes(lf((RELEASE / 'THIRD-PARTY-NOTICES.txt').read_bytes().rstrip()
                                                          + b'\n\n' + notices.encode('utf-8')))
        cmd = [sys.executable, str(ROOT / 'port' / 'android' / 'build_apk.py'), '--release', '--wasm', str(a.wasm.resolve()),
               '--jobs', str(a.jobs), '--assets', str(texts)] + (['--new-signing-key'] if a.new_signing_key else [])
        print('building: ' + ' '.join(cmd), flush=True)
        if subprocess.run(cmd).returncode:
            sys.exit('build_apk.py --release failed')
        apk = build_apk.OUT / build_apk.release_apk_name(name)
    if not apk.is_file():
        sys.exit('%s: no such APK' % apk)
    require_release_split(build_apk.OUT)
    problems, facts = build_apk.check_release(apk, name, code, a.new_signing_key)
    elf = a.elf.read_bytes()
    rsp = build_apk.OUT / 'work' / 'link.rsp'   # build_host64.py's: the Rust library linked into libmain.so
    rust_libs = [Path(x.strip().strip('"')) for x in rsp.read_text(encoding='utf-8').splitlines()
                 if x.strip().strip('"').endswith('libdhd_render.a')] if rsp.exists() else []
    if not rust_libs:
        print('note: no Rust library named in %s: its bytes are not exempt from the retail scan' % rsp)
    work = WORK / 'android-check'
    work.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(apk) as z:
        entries = z.namelist()
        for need in ('lib/arm64-v8a/libmain.so', 'lib/arm64-v8a/libSDL3.so', 'assets/LICENSE.txt',
                     'assets/THIRD-PARTY-NOTICES.txt', 'classes.dex', 'AndroidManifest.xml'):
            if need not in entries:
                problems.append('no %s in the APK' % need)
        for path in entries:
            content, file_name = z.read(path), path.rsplit('/', 1)[-1]
            if GAME_DATA.search(file_name):
                problems.append('%s looks like game data' % path)
            if path.startswith('lib/'):
                if not path.startswith('lib/arm64-v8a/') or binary_kind(content) != platform.kind:
                    problems.append('%s is not an arm64 library' % path)
            if path == 'lib/arm64-v8a/libmain.so':
                if b'dhd-port %s ' % a.version.encode() not in content:
                    problems.append('libmain.so was not built with version %s' % a.version)
                (work / 'libmain.so').write_bytes(content)
                problems += ['libmain.so: ' + p for p in exe_problems(work / 'libmain.so', server_strings(a.settings))]
            elif names_server_or_path(content, a.settings):
                problems.append('%s names a server or a local path' % path)
            runs = game_data_runs(content, elf, rust_libs if path.endswith('libmain.so') else [])
            problems += ['%s holds game data at +0x%X: %d bytes = %s' % (path, at, n, scan_retail.elf_place(elf, e))
                         for at, e, n in runs[:20]]
    if problems:
        sys.exit('%s is not fit to publish:\n  %s' % (apk, '\n  '.join(problems[:40])))
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(apk, out)
    apk_hash = hashlib.sha256(out.read_bytes()).hexdigest()
    url = (a.base_url.rstrip('/') + '/' + out.name if a.base_url
           else '%s/download/v%s/%s' % (RELEASES, a.version, out.name))
    done = write_manifest(manifest_path, a.version, {'android': {'url': url, 'sha256': apk_hash, 'size': out.stat().st_size}},
                          page)
    sums = write_sums(out.parent, a.version)
    print('%s (%d KB) sha256 %s' % (out, out.stat().st_size // 1024, apk_hash))
    print('  %s; signed by %s, certificate SHA-256 %s (schemes %s); native code %s; not debuggable'
          % (facts['badging'], facts['signer_dn'], facts['signer_sha256'], ' '.join(facts['schemes']), facts['native_code']))
    if facts['new_signer']:
        print(build_apk.NEW_SIGNER_NOTE % facts['signer_sha256'])
    print('  %s: version %s, %s; platforms %s' % (manifest_path, a.version, url, ', '.join(done)))
    print('  %s: %s' % (out.parent / 'SHA256SUMS', ', '.join(sums)))
    return 0


def write_manifest(path, version, entries, page):
    """latest.json with entries ({platform: {url, sha256, size}}) added to those of a latest.json of the
    same version already there. The top level's url, sha256 and size are the Windows zip's: what the
    0.6.x launchers read (update.rs)."""
    old = json.loads(path.read_text(encoding='utf-8')) if path.exists() else {}
    platforms = dict(old.get('platforms', {})) if old.get('version') == version else {}
    platforms.update(entries)
    manifest = {'name': 'Downhill Revived', 'version': version}
    manifest.update(platforms.get('win', {}))
    manifest['page'] = page
    manifest['platforms'] = dict(sorted(platforms.items()))
    path.write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    return sorted(platforms)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--platform', choices=sorted(PLATFORMS), default=build_launcher.host_platform(),
                    help='win, mac (arm64) or linux (x64); default: this machine\'s')
    source = ap.add_mutually_exclusive_group()
    source.add_argument('--wasm', type=Path, default=ROOT / 'build' / 'wasm' / 'dhd_core.wasm',
                        help='the dhd_core.wasm to build dhd.exe from (default build/wasm/dhd_core.wasm; Windows)')
    source.add_argument('--game', '--exe', dest='game', type=Path,
                        help="the ready game: build_host64.py's dhd64.exe (its link.rsp beside it), or the mac/linux "
                             "build's dhd")
    ap.add_argument('--launcher', type=Path, help='a ready build_launcher.py launcher (default: build it)')
    ap.add_argument('--rust-lib', type=Path, help='mac/linux: the Rust renderer library linked into --game, whose '
                    'bytes the retail scan may find in it')
    ap.add_argument('--sdl-lib', type=Path, help='x64 SDL3 static library (default: sdl3.py\'s x64 build)')
    ap.add_argument('--sdl-include', type=Path, help='its include directory (with --sdl-lib)')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--version', help="must equal port/include/version.h's DHD_VERSION (the default)")
    ap.add_argument('--out', type=Path, help='default build/release/DownhillRevived-<version>-<platform>.zip')
    ap.add_argument('--base-url', help="latest.json's zip address is this + the zip's name (default: the GitHub "
                    "release asset of tag v<version>), for a self-hosted update source")
    ap.add_argument('--merge', type=Path, nargs='+', metavar='LATEST_JSON',
                    help="only add these latest.json files' platforms (same version) to the one beside --out")
    ap.add_argument('--settings', type=Path, help="a dhd.ini whose server line must not be in the package")
    ap.add_argument('--elf', type=Path, default=ROOT / 'iso' / 'SCUS_971.77', help='your retail ELF, for the game-data scan')
    ap.add_argument('--no-installer', action='store_true', help='only the zip: no Windows installer, AppImage or DMG')
    ap.add_argument('--apk', type=Path, help='android: a ready build_apk.py --release APK (default: build it)')
    ap.add_argument('--new-signing-key', action='store_true',
                    help='android: a deliberate key change: accept a signer other than build_apk.RELEASE_CERT_SHA256')
    ap.add_argument('--sums', action='store_true', help='only write SHA256SUMS again in the release folder')
    a = ap.parse_args()
    platform = PLATFORMS[a.platform]
    version = header_version()
    if a.version and a.version != version:
        sys.exit('--version %s, but port/include/version.h says %s (the exe reports that one); bump the header '
                 'and build again' % (a.version, version))
    a.version = version
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', a.version):
        sys.exit('version %r: MAJOR.MINOR.PATCH digits only (the updater and the server compare them)' % a.version)
    name = 'DownhillRevived-%s' % a.version
    suffix = '.apk' if platform.key == 'android' else '.zip'
    out = (a.out or ROOT / 'build' / 'release' / ('%s-%s%s' % (name, platform.zip_tag, suffix))).resolve()
    if (ROOT / 'build').resolve() not in out.parents or out.drive.upper() == 'C:':
        sys.exit('the zip goes under build/ (never tracked, never on C:)')
    manifest_path = out.with_name('latest.json')
    page = '%s/tag/v%s' % (RELEASES, a.version)
    if a.merge:
        for other in a.merge:
            m = json.loads(other.read_text(encoding='utf-8'))
            if m.get('version') != a.version or not m.get('platforms'):
                sys.exit('%s: version %s with platforms needed (it has %s)' % (other, a.version, m.get('version')))
            done = write_manifest(manifest_path, a.version, m['platforms'], page)
        print('%s: version %s, %s' % (manifest_path, a.version, ', '.join(done)))
        print('%s: %s' % (out.with_name('SHA256SUMS'), ', '.join(write_sums(out.parent, a.version))))
        return 0
    if a.sums:
        print('%s: %s' % (out.with_name('SHA256SUMS'), ', '.join(write_sums(out.parent, a.version))))
        return 0
    changes = source_changes()
    if changes:
        sys.exit('port/ or src/ has uncommitted changes; a release is built from committed source:\n  '
                 + '\n  '.join(changes[:20]))
    if not a.elf.exists():
        sys.exit('%s: the retail ELF is needed to check that no game data is packaged' % a.elf)
    if platform.key == 'android':
        return android_release(a, platform, out, manifest_path, page)
    notices = rust_notices(platform)  # before the build: a crate with an unusable licence stops it early

    if a.game:
        exe = a.game.resolve()
        if not exe.exists():
            sys.exit('%s: no such file' % exe)
    elif platform.key != 'win':
        sys.exit('the %s game is built by build_host64\'s %s target: pass it as --game PATH' % (platform.name, platform.key))
    else:
        wasm = a.wasm.resolve()
        if not wasm.exists():
            sys.exit('%s: no such module (python port/tools/build_wasm.py writes it)' % wasm)
        print('module: %s (%d bytes, sha256 %s)' % (wasm, wasm.stat().st_size, hashlib.sha256(wasm.read_bytes()).hexdigest()))
        if a.sdl_lib:
            if not a.sdl_include:
                sys.exit('--sdl-lib needs --sdl-include')
            sdl_lib, sdl_include = a.sdl_lib, a.sdl_include
        else:
            import sdl3  # noqa: E402
            sdl_lib, sdl_include = sdl3.build(sdl3.X64_TARGET)
        exe = build_exe(wasm, Path(sdl_lib).resolve(), Path(sdl_include).resolve(), a.jobs)
    require_release_split(exe.parent)
    data = exe.read_bytes()
    if binary_kind(data) != platform.kind:
        sys.exit('%s is not a %s executable (%s)' % (exe, platform.kind, binary_kind(data) or 'unknown'))
    if platform.key == 'win':
        rust_libs = [linked_rust_lib(exe)]
    else:
        rust_libs = [a.rust_lib.resolve()] if a.rust_lib else []
        print('note: the Rust library\'s own checks (rust_lib_problems) read the Windows link.rsp: skipped for %s'
              % platform.name)
    if b'dhd-port %s ' % a.version.encode() not in data:
        sys.exit('%s was not built with version %s (port/include/version.h); build it again' % (exe, a.version))
    problems = exe_problems(exe, server_strings(a.settings))
    if problems:
        sys.exit('%s is not fit to publish:\n  ' % platform.game + '\n  '.join(problems[:30]))
    libraries = foreign_libraries(exe, data, platform)
    if libraries:
        sys.exit('%s needs libraries that are not %s\'s own: %s' % (platform.game, platform.name, ', '.join(libraries)))
    if platform.key == 'linux' and too_new_symbols(exe):
        sys.exit('%s needs symbols newer than glibc %s has: %s (link it on Ubuntu 22.04)'
                 % (platform.game, '.'.join(map(str, LINUX_GLIBC)), ', '.join(too_new_symbols(exe))))
    launcher = a.launcher.resolve() if a.launcher else build_launcher.build(platform.key)
    if not launcher.exists():
        sys.exit('%s: no such file' % launcher)
    launcher_data = launcher.read_bytes()
    found = build_launcher.problems(launcher)
    if binary_kind(launcher_data) != platform.kind:
        found.append('not a %s executable' % platform.kind)
    else:
        found += ['needs %s, not %s\'s own' % (d, platform.name)
                  for d in foreign_libraries(launcher, launcher_data, platform, launcher=True)]
        if platform.key == 'linux':
            found += ['needs %s, newer than glibc %s has' % (v, '.'.join(map(str, LINUX_GLIBC)))
                      for v in too_new_symbols(launcher)]
    if found:
        sys.exit('%s is not fit to publish:\n  %s' % (launcher, '\n  '.join(found)))

    files, game_path, launcher_path = layout(platform, a.version, launcher_data, data, notices)
    executables = {game_path, launcher_path}
    # Files only the installer adds (Windows: installed.txt), checked as the package's own are.
    extra = installers.extra_files(platform.key)
    # Static libraries linked in: their bytes may match the ELF by chance (game_data_runs).
    linked = {game_path: rust_libs, launcher_path: build_launcher.native_libraries(launcher)}
    # Third-party binaries: the publisher's exact bytes (see the module's doc).
    third_party = {p: dxc.DLL_SHA256['x64'] for p in files if p.endswith('/dxcompiler.dll')}
    elf = a.elf.read_bytes()
    for path, content in list(files.items()) + list(extra.items()):
        file_name = path.rsplit('/', 1)[-1]
        if GAME_DATA.search(file_name):
            sys.exit('%s looks like game data; not packaged' % file_name)
        if path in third_party:
            if hashlib.sha256(content).hexdigest() != third_party[path]:
                sys.exit('%s is not the pinned third-party file; not packaged' % file_name)
            continue
        runs = game_data_runs(content, elf, linked.get(path, []))
        if runs:
            sys.exit('%s holds game data (runs of the retail ELF):\n  %s' % (file_name, '\n  '.join(
                '+0x%X: %d bytes = %s' % (at, n, scan_retail.elf_place(elf, e)) for at, e, n in runs[:20])))
        if path != game_path and names_server_or_path(content, a.settings):
            sys.exit('%s names a server or a local path; not packaged' % file_name)

    # The same files unzipped, to try before publishing: Windows' in build/release/<name>/ as always.
    stage = out.parent if platform.key == 'win' else out.with_suffix('')
    for top in sorted({p.split('/')[0] for p in files}):
        if (stage / top).exists():
            shutil.rmtree(stage / top)
    for path, content in files.items():
        (stage / path).parent.mkdir(parents=True, exist_ok=True)
        (stage / path).write_bytes(content)
        if path in executables and os.name != 'nt':
            (stage / path).chmod(0o755)
    signed = True
    entries = list(files)
    if platform.key == 'mac':
        app = stage / entries[0].split('/')[0]
        signed = sign_app(app)
        entries += sorted(p.relative_to(stage).as_posix() for p in app.rglob('*')
                          if p.is_file() and p.relative_to(stage).as_posix() not in files)  # the signature
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for path in entries:
            info_entry = zipfile.ZipInfo(path, date_time=(2026, 1, 1, 0, 0, 0))
            info_entry.compress_type = zipfile.ZIP_DEFLATED
            if platform.key == 'win':
                info_entry.create_system = 0  # as made on Windows, from any machine
                info_entry.external_attr = 0o644 << 16
            else:  # unix entries: unzip and the launcher's updater keep the mode
                info_entry.create_system = 3
                info_entry.external_attr = (0o100755 if path in executables else 0o100644) << 16
            z.writestr(info_entry, (stage / path).read_bytes())
    zip_hash = hashlib.sha256(out.read_bytes()).hexdigest()
    # The launcher's update manifest (update.rs): published beside the zip as a release asset.
    zip_url = (a.base_url.rstrip('/') + '/' + out.name if a.base_url
               else '%s/download/v%s/%s' % (RELEASES, a.version, out.name))
    commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=str(ROOT), capture_output=True, text=True).stdout.strip()
    print('%s (%d KB)' % (out, out.stat().st_size // 1024))
    print('  zip sha256 %s; unzipped in %s' % (zip_hash, stage / entries[0].split('/')[0]))
    if signed:
        done = write_manifest(manifest_path, a.version, {platform.key: {
            'url': zip_url, 'sha256': zip_hash, 'size': out.stat().st_size}}, page)
        print('  %s: version %s, %s; platforms %s' % (manifest_path, a.version, zip_url, ', '.join(done)))
    print('  %s (%s, Rust renderer %s) sha256 %s, source %s' % (
        platform.game, platform.kind, ', '.join(map(str, rust_libs)) or 'not named', hashlib.sha256(data).hexdigest(),
        commit[:9]))
    if platform.key == 'win':
        print('  imports (all Windows DLLs): %s' % ', '.join(imported_dlls(exe)))
        # dhd.log's crash stacks are rvas into this exe: its link map names them (crash_stack.py)
        link_map = exe.with_name('dhd64.map')
        if link_map.exists():
            shutil.copyfile(link_map, out.with_suffix('.map'))
            print('  link map %s (keep it: crash_stack.py reads players\' crash stacks with it)' % out.with_suffix('.map'))
        else:
            print('  note: no %s; players\' crash stacks of this build can\'t be read' % link_map)
    else:
        print('  libraries (all %s\'s own): %s' % (platform.name, ', '.join(needed_libraries(exe, platform.kind))))
    if platform.key == 'linux':
        for label, path in ((platform.game, exe), (platform.launcher, launcher)):
            print('  %s needs %s' % (label, ', '.join('%s_%s' % (n, '.'.join(map(str, max(v))))
                                                     for n, v in sorted(symbol_versions(path).items()))))
    # The installer for a first install (finding 125): the same checked files (installers.py)
    if not a.no_installer:
        if platform.key == 'mac' and not signed:
            print('note: no DMG: the app is not signed (a DMG is made on a Mac)')
        else:
            made = installers.make(platform, a.version, files, extra, stage, out, executables,
                                   server_strings(a.settings), elf, linked, manifest_path if signed else None,
                                   zip_url.rsplit('/', 1)[0], page)
            for path in made:
                print('  installer %s (%d KB) sha256 %s' % (path, path.stat().st_size // 1024,
                                                            hashlib.sha256(path.read_bytes()).hexdigest()))
    print('  %s: %s' % (out.with_name('SHA256SUMS'), ', '.join(write_sums(out.parent, a.version))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
