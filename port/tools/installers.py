"""The installers for a first install (finding 125), made by package_release.py from the files it has
already checked, beside the zips (which stay: the launcher updates from them, update.rs):

  Windows  DownhillRevived-<version>-win-setup.exe            Inno Setup, per user, no administrator
                                                              (port/release/installer/windows.iss)
  macOS    DownhillRevived-<version>-mac-experimental.dmg     the signed Downhill Revived.app and an
                                                              Applications link, to drag it across
  Linux    DownhillRevived-<version>-linux-experimental.AppImage   x64, with the launcher's WebKitGTK
                                                              inside (a Steam Deck has none of its own)

None of them is signed by a developer certificate: the Windows installer is unsigned (SmartScreen
asks once), the app in the DMG is signed ad hoc as in the zip, the AppImage has no signature.

What is checked, on top of package_release's checks of the files themselves:
- Windows: the setup is compiled from a folder holding exactly the checked files plus installed.txt
  (extra_files, checked with them); the setup file itself (Inno Setup's 32-bit program and the
  compressed payload) goes through the server, builder-folder and retail-bytes scans, and the
  absolute-path scan, where only the paths Inno Setup's own pinned program files hold are allowed
  (its developer's source paths, and code bytes that spell one by chance).
- macOS: the DMG is mounted read-only and every file of its app compared with the checked, signed one;
  the signature is verified on the mounted copy.
- Linux: the AppImage is unpacked again (--appimage-extract) and its launcher and game compared with
  the checked ones; every other file in it goes through the retail-bytes scan, the text files through
  the server and path scans, every ELF library through the glibc version check (no symbol newer than
  glibc 2.35 has), and every bundled library is named with its Ubuntu package, version and licence in
  usr/share/doc/downhill-revived/BUNDLED-LIBRARIES.txt, with the packages' copyright files beside it.
latest.json gets a "linux-appimage" entry: an AppImage updates by replacing the AppImage (update.rs);
the Windows installer and the DMG are only for the first install and aren't in latest.json."""
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'build' / 'release-work'
ISS = ROOT / 'port' / 'release' / 'installer' / 'windows.iss'
LAUNCHER = ROOT / 'port' / 'launcher'
ICONS = LAUNCHER / 'src-tauri' / 'icons'

# game.rs INSTALLED_MARKER: in the install folder, the player's files go to app data.
INSTALLED_MARKER = 'installed.txt'
INSTALLED_TEXT = b"""Downhill Revived was installed in this folder by its installer.

Your settings (dhd.ini), saves and dhd.log are not here: they are in
%APPDATA%\\Downhill Revived (paste that into File Explorer's address bar).

The launcher updates this folder by itself. Uninstall the game from Settings > Apps >
Installed apps: your settings, saves and log stay where they are.

This file tells the launcher that the game is installed; leave it here.
"""


def pr():
    """package_release, for its checks (imported lazily: it imports this module)."""
    import package_release
    return package_release


def extra_files(platform_key):
    """{path in the release folder: content} the installer adds to the zip's files."""
    if platform_key == 'win':
        return {'DownhillRevived/' + INSTALLED_MARKER: INSTALLED_TEXT.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')}
    return {}


def make(platform, version, files, extra, stage, out, executables, servers, elf, linked, manifest_path, base_url, page):
    """The platform's installer from the checked files; [its path]."""
    if platform.key == 'win':
        return [windows_setup(version, files, extra, out.parent, servers, elf)]
    if platform.key == 'mac':
        return [mac_dmg(version, stage, files, out.parent, platform)]
    return [linux_appimage(version, files, stage, out.parent, platform, servers, elf, linked, manifest_path,
                           base_url, page)]


def scan_problems(name, content, servers, elf, libraries=()):
    """package_release's scans of one file: server names, the builder's folders, runs of the retail
    ELF (other than the static libraries' own bytes)."""
    p = pr()
    lowered = content.lower()
    found = ['server host %r' % h.decode('ascii', 'replace') for h in p.other_hosts(content)]
    found += ['string %r' % s.decode('ascii', 'replace') for s in p.FORBIDDEN + sorted(servers) if s.lower() in lowered]
    found += ['%d bytes of the retail ELF at +0x%X' % (n, at) for at, e, n in p.game_data_runs(content, elf, libraries)]
    return found


# ---- Windows ----

def windows_setup(version, files, extra, out_dir, servers, elf):
    """DownhillRevived-<version>-win-setup.exe: ISCC on windows.iss with a folder of exactly the
    checked files plus installed.txt."""
    import innosetup
    source = WORK / 'installer-win'
    if source.exists():
        shutil.rmtree(source)
    source.mkdir(parents=True)
    for path, content in list(files.items()) + list(extra.items()):
        (source / path.split('/', 1)[1]).write_bytes(content)
    name = 'DownhillRevived-%s-win-setup' % version
    setup = out_dir / (name + '.exe')
    cmd = [str(innosetup.iscc()), '/Q', '/DAppVersion=%s' % version, '/DSourceDir=%s' % source,
           '/DOutputDir=%s' % out_dir, '/DOutputName=%s' % name, '/DIconFile=%s' % (ICONS / 'icon.ico'), str(ISS)]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode or not setup.exists():
        sys.exit('ISCC failed:\n%s' % (r.stdout + r.stderr)[-4000:])
    data = setup.read_bytes()
    problems = scan_problems(setup.name, data, servers, elf)
    p = pr()
    own = innosetup.own_paths()
    for text, kind in ((data, 'ASCII'), (p.utf16_strings(data), 'UTF-16')):
        problems += ['%s absolute path %r (not one of Inno Setup\'s own)' % (kind, m.group(0)[:120].decode('ascii', 'replace'))
                     for m in p.ABSOLUTE.finditer(text) if m.group(0) not in own
                     and not (kind == 'ASCII' and p.chance_bytes(text, m))]  # the compressed payload's bytes
    if problems:
        setup.unlink()
        sys.exit('%s is not fit to publish:\n  %s' % (setup.name, '\n  '.join(problems[:30])))
    return setup


# ---- macOS ----

def mac_dmg(version, stage, files, out_dir, platform):
    """DownhillRevived-<version>-mac-experimental.dmg: the signed app (from stage, as zipped) and a
    link to /Applications, compressed (UDZO); checked by mounting it."""
    app_name = next(iter(files)).split('/')[0]
    app = stage / app_name
    folder = WORK / 'dmg'
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    subprocess.run(['ditto', str(app), str(folder / app_name)], check=True)   # keeps the signature
    os.symlink('/Applications', folder / 'Applications')
    dmg = out_dir / ('DownhillRevived-%s-%s.dmg' % (version, platform.zip_tag))
    if dmg.exists():
        dmg.unlink()
    r = subprocess.run(['hdiutil', 'create', '-volname', 'Downhill Revived', '-srcfolder', str(folder), '-fs', 'HFS+',
                        '-format', 'UDZO', '-imagekey', 'zlib-level=9', '-ov', str(dmg)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('hdiutil create failed:\n%s' % (r.stdout + r.stderr)[-2000:])
    mount = WORK / 'dmg-mount'
    mount.mkdir(exist_ok=True)
    r = subprocess.run(['hdiutil', 'attach', '-nobrowse', '-readonly', '-noverify', '-mountpoint', str(mount), str(dmg)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('hdiutil attach failed:\n%s' % r.stderr[-2000:])
    try:
        problems = same_tree(app, mount / app_name)
        if not os.path.islink(mount / 'Applications') or os.readlink(mount / 'Applications') != '/Applications':
            problems.append('no Applications link')
        v = subprocess.run(['codesign', '--verify', '--deep', '--strict', str(mount / app_name)], capture_output=True, text=True)
        if v.returncode:
            problems.append('signature: ' + v.stderr.strip()[-500:])
        extra = sorted(set(os.listdir(mount)) - {app_name, 'Applications', '.fseventsd', '.Trashes', '.DS_Store'})
        problems += ['%s: not ours' % e for e in extra]
    finally:
        subprocess.run(['hdiutil', 'detach', str(mount)], capture_output=True)
    if problems:
        dmg.unlink()
        sys.exit('%s is not fit to publish:\n  %s' % (dmg.name, '\n  '.join(problems[:30])))
    return dmg


def same_tree(a, b):
    """The differences between folders a and b, file by file (bytes, executable bit)."""
    def listing(root):
        return {p.relative_to(root).as_posix(): p for p in root.rglob('*') if p.is_file() and not p.is_symlink()}
    la, lb = listing(a), listing(b)
    problems = ['%s: only in %s' % (n, b if n in lb else a) for n in sorted(set(la) ^ set(lb))]
    for n in sorted(set(la) & set(lb)):
        if la[n].read_bytes() != lb[n].read_bytes():
            problems.append('%s differs' % n)
        elif os.access(la[n], os.X_OK) != os.access(lb[n], os.X_OK):
            problems.append('%s: executable bit differs' % n)
    return problems


# ---- Linux ----

# Files of the AppImage that are neither ours nor a library from a distribution package: the AppImage
# project's AppRun (linuxdeploy, MIT) and linuxdeploy-plugin-gtk's environment hook (MIT).
APPIMAGE_TOOLING = {'AppRun', 'AppRun.wrapped', 'apprun-hooks/linuxdeploy-plugin-gtk.sh'}
ELF_MAGIC = b'\x7fELF'


# The AppImage runtime (the start program every AppImage begins with; AppImage/type2-runtime, MIT),
# pinned: appimagetool would otherwise download the "continuous" build on every run.
RUNTIME_TAG = '20251108'
RUNTIME_URL = 'https://github.com/AppImage/type2-runtime/releases/download/%s/runtime-x86_64' % RUNTIME_TAG
RUNTIME_SHA256 = '2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d'


def appimage_runtime():
    """The pinned runtime, downloaded once into DHD_DEVCACHES/appimage-runtime/<tag>/ and checked."""
    import urllib.request
    cache = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches') / 'appimage-runtime' / RUNTIME_TAG
    runtime = cache / 'runtime-x86_64'
    if not runtime.exists() or hashlib.sha256(runtime.read_bytes()).hexdigest() != RUNTIME_SHA256:
        cache.mkdir(parents=True, exist_ok=True)
        print('appimage: downloading', RUNTIME_URL)
        with urllib.request.urlopen(RUNTIME_URL, timeout=120) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != RUNTIME_SHA256:
            sys.exit('%s has SHA-256 %s, expected %s' % (RUNTIME_URL, hashlib.sha256(data).hexdigest(), RUNTIME_SHA256))
        runtime.write_bytes(data)
    return runtime


def tauri_cache():
    """Where Tauri's bundler keeps the linuxdeploy tools it downloads."""
    base = os.environ.get('XDG_CACHE_HOME') or os.path.join(os.path.expanduser('~'), '.cache')
    return Path(base) / 'tauri'


def debian_owner(paths):
    """{path in the AppDir: (package, version, source, source version)} for the system files they were
    copied from: dpkg -S by file name, the owner whose file ends with the same path tail."""
    names = sorted({Path(p).name for p in paths})
    owners = {}
    for i in range(0, len(names), 200):
        r = subprocess.run(['dpkg', '-S'] + ['*/' + n for n in names[i:i + 200]], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            pkgs, _, path = line.partition(': ')
            for pkg in pkgs.split(', '):
                owners.setdefault(Path(path).name, []).append((pkg.split(':')[0], path))
    out = {}
    for p in paths:
        tail = Path(p).relative_to('usr/lib').parts if p.startswith('usr/lib/') else Path(p).parts
        best = None
        for pkg, path in owners.get(Path(p).name, []):
            parts = Path(path).parts
            score = next((k for k in range(len(tail), 0, -1) if parts[-k:] == tail[-k:]), 0)
            if score and (best is None or score > best[0]):
                best = (score, pkg)
        if best:
            out[p] = best[1]
    info = {}
    for pkg in sorted(set(out.values())):
        r = subprocess.run(['dpkg-query', '-W', '-f', '${Version}\t${source:Package}\t${source:Version}', pkg],
                           capture_output=True, text=True)
        info[pkg] = tuple(r.stdout.split('\t')) if r.returncode == 0 else ('?', pkg, '?')
    return {p: (pkg,) + info[pkg] for p, pkg in out.items()}


def licences_of(copyright_text):
    """The licence names a Debian copyright file states (its DEP-5 License: lines), or a note."""
    # DEP-5 short names ("LGPL-2.1+", "GPL-2+ or MPL-1.1"), not a licence paragraph's first line
    names = sorted({m.strip() for m in re.findall(r'^License:\s*(.+)$', copyright_text, re.M)
                    if len(m.split()) <= 6 and not m.rstrip().endswith((':', ',', '.'))})
    return ', '.join(names) if names else 'see its copyright file'


def bundled_libraries_text(owners, appdir):
    """BUNDLED-LIBRARIES.txt: every file the AppImage carries from Ubuntu, by package."""
    by_pkg = {}
    for path, (pkg, version, source, source_version) in sorted(owners.items()):
        by_pkg.setdefault((pkg, version, source, source_version), []).append(path)
    lines = ['Libraries bundled in the Downhill Revived AppImage',
             '==================================================',
             '',
             'The launcher\'s window is drawn by WebKitGTK and GTK. A Steam Deck and many other systems',
             'lack them, so the AppImage carries them and the libraries they need, copied unmodified from',
             'Ubuntu 22.04\'s packages (the system the Linux release is built on). The game itself (usr/bin/dhd)',
             'uses none of them: it needs only the C library, Vulkan and the system\'s display and sound',
             'libraries, as the zip does.',
             '',
             'Each package\'s licence is in usr/share/doc/<package>/copyright inside the AppImage. The source of',
             'each is Ubuntu\'s source package named below, at',
             'https://launchpad.net/ubuntu/+source/<source package>/<source version>.',
             '',
             'The AppImage\'s start program (AppRun, the AppImage runtime) and its GTK environment script come',
             'from the AppImage project\'s linuxdeploy tools (MIT licence).',
             '']
    for (pkg, version, source, source_version), paths in sorted(by_pkg.items()):
        copyright_file = appdir / 'usr' / 'share' / 'doc' / pkg / 'copyright'
        text = copyright_file.read_text(encoding='utf-8', errors='replace') if copyright_file.exists() else ''
        lines.append('%s %s (source %s %s): %s' % (pkg, version, source, source_version, licences_of(text)))
        lines += ['    ' + p for p in paths]
    return '\n'.join(lines) + '\n'


def linux_appimage(version, files, stage, out_dir, platform, servers, elf, linked, manifest_path, base_url, page):
    """DownhillRevived-<version>-linux-experimental.AppImage: Tauri's AppImage bundle of the checked
    launcher (linuxdeploy adds WebKitGTK and its libraries and points the launcher's RUNPATH at them),
    then the game unchanged, the text files and the bundled libraries' notices added, packed again
    and checked (the module's doc)."""
    import build_launcher
    p = pr()
    folder = next(iter(files)).split('/')[0] + '/'
    launcher_bytes, game_bytes = files[folder + platform.launcher], files[folder + platform.game]
    # Tauri bundles target/release's launcher: it must be the checked one
    built = build_launcher.exe_path('linux')
    if not built.exists() or built.read_bytes() != launcher_bytes:
        built.parent.mkdir(parents=True, exist_ok=True)
        built.write_bytes(launcher_bytes)
        built.chmod(0o755)
    conf = WORK / 'appimage-tauri.json'
    conf.parent.mkdir(parents=True, exist_ok=True)
    conf.write_text('{"version": "%s", "bundle": {"active": true, "targets": ["appimage"], "category": "Game", '
                    '"shortDescription": "Downhill Revived", "linux": {"appimage": {"bundleMediaFramework": false}}}}'
                    % version)
    bundle_dir = built.parent / 'bundle' / 'appimage'
    if bundle_dir.exists():
        shutil.rmtree(bundle_dir)
    # no FUSE needed (WSL, CI); the pinned runtime for both packing steps (linuxdeploy's AppImage plugin)
    env = dict(os.environ, APPIMAGE_EXTRACT_AND_RUN='1', LDAI_RUNTIME_FILE=str(appimage_runtime()))
    tauri = LAUNCHER / 'node_modules' / '.bin' / 'tauri'
    r = subprocess.run([str(tauri), 'bundle', '--bundles', 'appimage', '--config', str(conf)], cwd=str(LAUNCHER),
                       env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    made = bundle_dir / 'Downhill Revived.AppDir'
    if r.returncode or not made.is_dir():
        sys.exit('tauri bundle failed:\n%s' % (r.stdout + r.stderr)[-4000:])

    appdir = WORK / 'appimage' / 'Downhill Revived.AppDir'
    if appdir.parent.exists():
        shutil.rmtree(appdir.parent)
    shutil.copytree(made, appdir, symlinks=True)
    # the game exactly as checked (linuxdeploy would have rewritten its RUNPATH: it uses no bundled library)
    (appdir / 'usr' / 'bin' / platform.game).write_bytes(game_bytes)
    (appdir / 'usr' / 'bin' / platform.game).chmod(0o755)
    doc = appdir / 'usr' / 'share' / 'doc' / 'downhill-revived'
    doc.mkdir(parents=True, exist_ok=True)
    for name in ('README.txt', 'LICENSE.txt', 'THIRD-PARTY-NOTICES.txt'):
        (doc / name).write_bytes(files[folder + name])

    ours = {'usr/bin/' + platform.launcher, 'usr/bin/' + platform.game}
    regular = sorted(q.relative_to(appdir).as_posix() for q in appdir.rglob('*') if q.is_file() and not q.is_symlink())
    elves = [q for q in regular if (appdir / q).read_bytes()[:4] == ELF_MAGIC]
    third = [q for q in elves if q not in ours and q not in APPIMAGE_TOOLING]
    owners = debian_owner(third)
    problems = ['%s: from no Ubuntu package (where is it from?)' % q for q in third if q not in owners]
    for pkg in sorted({o[0] for o in owners.values()}):   # each package's licence, beside its files
        target = appdir / 'usr' / 'share' / 'doc' / pkg / 'copyright'
        source = Path('/usr/share/doc') / pkg / 'copyright'
        if not target.exists():
            if not source.exists():
                problems.append('%s: no copyright file' % pkg)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
    (doc / 'BUNDLED-LIBRARIES.txt').write_text(bundled_libraries_text(owners, appdir), encoding='utf-8')
    regular = sorted(q.relative_to(appdir).as_posix() for q in appdir.rglob('*') if q.is_file() and not q.is_symlink())

    # the checks (the module's doc)
    game_path, launcher_path = folder + platform.game, folder + platform.launcher
    for q in regular:
        data = (appdir / q).read_bytes()
        name = q.rsplit('/', 1)[-1]
        if p.GAME_DATA.search(name):
            problems.append('%s looks like game data' % q)
        if q == 'usr/bin/' + platform.game:
            if data != game_bytes:
                problems.append('%s is not the checked game' % q)
            continue   # checked as the zip's
        libraries = linked.get(launcher_path, []) if q == 'usr/bin/' + platform.launcher else []
        problems += ['%s: %d bytes of the retail ELF at +0x%X' % (q, n, at)
                     for at, e, n in p.game_data_runs(data, elf, libraries)]
        problems += ['%s: server host %r' % (q, h.decode('ascii', 'replace')) for h in p.other_hosts(data)]
        if data[:4] == ELF_MAGIC:
            needs = p.symbol_versions(appdir / q)
            too_new = ['GLIBC_%s' % '.'.join(map(str, v)) for v in sorted(needs.get('GLIBC', ())) if v > p.LINUX_GLIBC]
            problems += ['%s needs %s, newer than glibc %s has' % (q, v, '.'.join(map(str, p.LINUX_GLIBC))) for v in too_new]
        if q in ours or q in APPIMAGE_TOOLING or data[:4] != ELF_MAGIC:
            # ours, and every text file: no builder's folder, no other server
            lowered = data.lower()
            problems += ['%s: string %r' % (q, s.decode('ascii', 'replace')) for s in p.FORBIDDEN + sorted(servers)
                         if s.lower() in lowered and q not in APPIMAGE_TOOLING]
    launcher_in = appdir / 'usr' / 'bin' / platform.launcher
    problems += ['%s %s' % (launcher_in.name, x) for x in build_launcher.problems(launcher_in)]
    problems += ['%s needs %s' % (launcher_in.name, v) for v in p.too_new_symbols(launcher_in)]
    if problems:
        sys.exit('the AppImage is not fit to publish:\n  %s' % '\n  '.join(problems[:40]))

    # packed again with linuxdeploy's AppImage plugin (appimagetool and the AppImage runtime)
    plugin = tauri_cache() / 'linuxdeploy-plugin-appimage.AppImage'
    if not plugin.exists():
        sys.exit('%s: missing (tauri bundle downloads it)' % plugin)
    image = out_dir / ('DownhillRevived-%s-%s.AppImage' % (version, platform.zip_tag))
    if image.exists():
        image.unlink()
    r = subprocess.run([str(plugin), '--appdir', str(appdir)], cwd=str(appdir.parent), capture_output=True, text=True,
                       env=dict(env, ARCH='x86_64', LDAI_OUTPUT=str(image)), encoding='utf-8', errors='replace')
    if r.returncode or not image.exists():
        sys.exit('packing the AppImage failed:\n%s' % (r.stdout + r.stderr)[-3000:])
    # what is inside is what was checked
    unpack = WORK / 'appimage-check'
    if unpack.exists():
        shutil.rmtree(unpack)
    unpack.mkdir()
    r = subprocess.run([str(image), '--appimage-extract'], cwd=str(unpack), capture_output=True, text=True)
    if r.returncode:
        sys.exit('%s --appimage-extract failed: %s' % (image.name, r.stderr[-1000:]))
    differences = same_tree(appdir, unpack / 'squashfs-root')
    shutil.rmtree(unpack)
    if differences:
        image.unlink()
        sys.exit('%s does not hold the checked files:\n  %s' % (image.name, '\n  '.join(differences[:30])))
    if manifest_path:
        p.write_manifest(manifest_path, version, {'linux-appimage': {
            'url': '%s/%s' % (base_url, image.name), 'sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
            'size': image.stat().st_size}}, page)
    print('  AppImage: %d libraries from %d Ubuntu packages bundled (usr/share/doc/downhill-revived/'
          'BUNDLED-LIBRARIES.txt)' % (len(owners), len({o[0] for o in owners.values()})))
    return image
