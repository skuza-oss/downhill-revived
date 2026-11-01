"""Inno Setup's compiler (ISCC.exe) for the Windows installer (port/release/installer/windows.iss,
finding 125): download, SHA-256 and signature check, portable unpack.

    python port/tools/innosetup.py      prints ISCC.exe's path

Inno Setup's own installer is pinned by version and SHA-256 (nothing else is accepted), and its
Authenticode signature must be valid. It is run silently in portable mode (/PORTABLE=1: no registry
entry, no uninstaller, no shortcuts) into <cache>/<VERSION>, where the cache is %DHD_INNOSETUP_CACHE%
if set, else DHD_DEVCACHES/innosetup. Windows only: the installer is made on the machine that
packages the Windows release. Inno Setup is free software under its own permissive licence
(port/release/THIRD-PARTY-NOTICES.txt); the installers it makes carry its setup program."""
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')

VERSION = '7.1.1'
SETUP = 'innosetup-%s-x64.exe' % VERSION
URL = 'https://github.com/jrsoftware/issrc/releases/download/is-%s/%s' % (VERSION.replace('.', '_'), SETUP)
SHA256 = '8a5a6711c3766a6e5fb74f280ba0e3551e719ce94538d901fdbd1f8f9920b3f0'


def cache():
    return Path(os.environ.get('DHD_INNOSETUP_CACHE') or DEVCACHES / 'innosetup')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def signature_ok(path):
    """Windows says the file's Authenticode signature is valid."""
    r = subprocess.run(['powershell', '-NoProfile', '-Command',
                        "(Get-AuthenticodeSignature -LiteralPath '%s').Status" % path], capture_output=True, text=True)
    return r.stdout.strip() == 'Valid'


def iscc():
    """ISCC.exe, installing Inno Setup once."""
    if os.name != 'nt':
        sys.exit('innosetup: the Windows installer is made on Windows')
    base = cache() / VERSION
    compiler = base / 'ISCC.exe'
    stamp = base / 'sha256.txt'
    if compiler.exists() and stamp.exists() and stamp.read_text().strip() == SHA256:
        return compiler
    setup = cache() / 'downloads' / SETUP
    if not setup.exists() or sha256(setup) != SHA256:
        setup.parent.mkdir(parents=True, exist_ok=True)
        part = setup.with_suffix('.part')
        print('innosetup: downloading', URL)
        with urllib.request.urlopen(URL, timeout=300) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f)
        got = sha256(part)
        if got != SHA256:
            part.unlink()
            sys.exit('innosetup: %s has SHA-256 %s, expected %s' % (URL, got, SHA256))
        os.replace(part, setup)
    if not signature_ok(setup):
        sys.exit('innosetup: %s has no valid signature' % setup)
    if base.exists():
        shutil.rmtree(base)
    r = subprocess.run([str(setup), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', '/PORTABLE=1',
                        '/NOICONS', '/DIR=%s' % base])
    if r.returncode or not compiler.exists():
        sys.exit('innosetup: the portable install into %s failed (exit %d)' % (base, r.returncode))
    stamp.write_text(SHA256 + '\n')
    return compiler


def own_paths():
    """The absolute drive paths (as package_release.ABSOLUTE finds them, ASCII and UTF-16) in Inno
    Setup's own setup programs (*.e32, *.e64): what a setup it makes may hold that isn't ours."""
    import package_release
    found = set()
    for f in sorted(iscc().parent.glob('*.e[36][24]')):
        data = f.read_bytes()
        for text in (data, package_release.utf16_strings(data)):
            found |= {m.group(0) for m in package_release.ABSOLUTE.finditer(text)}
    return found


if __name__ == '__main__':
    print(iscc())
