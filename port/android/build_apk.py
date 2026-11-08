"""Build Downhill Revived's APK for Android arm64: a debug APK (finding 091) or the release APK (finding 143).

    python port/android/build_apk.py [--wasm build/wasm/dhd_core.wasm] [--jobs N] [--offline]
    python port/android/build_apk.py --release [--wasm ...]   # signed with the release key (below)
    python port/android/build_apk.py --check APK                # only check_release on a built APK

1. The native libraries: build/android/lib/arm64-v8a/libmain.so and libSDL3.so, built by
   port/tools/build_host64.py --target android-arm64 from the module (--wasm, default build/wasm/dhd_core.wasm,
   when given or when the libraries are missing).
2. The Gradle project in build/android/project: this folder's files (the Gradle scripts, the
   manifest, DhdActivity.java, the strings and theme), SDL's Java sources of the same release
   (org.libsdl.app, from the SDL source sdl3.py unpacked), the icon (port/res/icon.png, scaled per
   density), the two libraries (src/main/jniLibs/arm64-v8a) and the Gradle wrapper (SDL's
   gradle-wrapper.jar, Gradle WRAPPER_GRADLE: the version already in GRADLE_USER_HOME).
3. gradlew assembleDebug (no daemon), signed with the debug key ANDROID_USER_HOME holds
   (DHD_DEVCACHES\\AndroidUser\\debug.keystore, made on first use). Output:
   build/android/downhill-revived-<version>-debug.apk.
   With --release: gradlew assembleRelease, not debuggable, no R8 (SDL's Java is called from C
   through JNI by name), signed with the release key the environment names: DHD_ANDROID_KEYSTORE
   (the keystore file, outside the checkout), DHD_ANDROID_KEYSTORE_PASS, DHD_ANDROID_KEY_ALIAS,
   DHD_ANDROID_KEY_PASS (app/build.gradle reads them; nothing is written down here). The key is kept
   by the maintainer; an APK signed with another key can't update an installed one. Then the APK is
   checked (check_release: apksigner verify with one signer, the release certificate
   RELEASE_CERT_SHA256; aapt's badging: not debuggable, the version, arm64-v8a only) and written as build/android/DownhillRevived-<version>-android-experimental.apk, the name
   of the release asset (port/tools/package_release.py --platform android).

The release certificate is pinned: an APK signed with any other key (a new keystore made by
mistake, a second DHD_ANDROID_KEYSTORE) can't update the installed app, so check_release refuses
it. A deliberate key change (a rotation with scheme v3's lineage, or a new app) passes only with
--new-signing-key (package_release.py has it too), which says so in its output; then
RELEASE_CERT_SHA256 changes in the commit of the release that starts using the key.

versionCode is MAJOR * 10000 + MINOR * 100 + PATCH (0.7.7: 707), so it grows with every release
while MINOR and PATCH stay under 100 (version() refuses others): Android installs an APK over an
installed one only with a higher or equal versionCode.

Needs: the Android SDK (ANDROID_HOME; platform android-35, build-tools), a JDK 17+ (JAVA_HOME),
GRADLE_USER_HOME on F: (Gradle, the Android Gradle Plugin and its libraries are fetched there on
the first build unless already cached; --offline uses only the cache). No Android Studio. Nothing
here goes into git: build/ is ignored.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'port' / 'tools'))
import sdl3  # noqa: E402

OUT = ROOT / 'build' / 'android'
PROJECT = OUT / 'project'
WRAPPER_GRADLE = '8.10.2'   # AGP 8.7.0 (build.gradle) needs Gradle 8.9 or later
ICON_SIZES = {'mdpi': 48, 'hdpi': 72, 'xhdpi': 96, 'xxhdpi': 144, 'xxxhdpi': 192}
TEMPLATE = ('settings.gradle', 'build.gradle', 'gradle.properties', 'app/build.gradle', 'app/src/main')


RELEASE_ENV = ('DHD_ANDROID_KEYSTORE', 'DHD_ANDROID_KEYSTORE_PASS', 'DHD_ANDROID_KEY_ALIAS', 'DHD_ANDROID_KEY_PASS')
PACKAGE = 'app.downhillrevived'
# The release key's certificate (public; finding 143): every release APK is signed with it.
RELEASE_CERT_SHA256 = 'D7:61:3E:E0:F0:93:95:22:D8:3B:06:CE:8A:05:ED:9D:03:E4:A9:96:8E:EA:2F:9C:83:98:58:94:14:56:AE:1B'
NEW_SIGNER_NOTE = ("NOTE: --new-signing-key: signed by %s, not the release key RELEASE_CERT_SHA256; installed copies "
                   "won't take it as an update unless the key was rotated (scheme v3 lineage)")


def version():
    text = (ROOT / 'port' / 'include' / 'version.h').read_text()
    name = re.search(r'#define DHD_VERSION "([0-9.]+)"', text).group(1)
    major, minor, patch = (int(x) for x in name.split('.'))
    if minor > 99 or patch > 99:
        sys.exit('build_apk: version %s: versionCode MAJOR*10000+MINOR*100+PATCH needs MINOR and PATCH under 100' % name)
    return name, major * 10000 + minor * 100 + patch


def release_apk_name(name):
    """The release asset's name, in package_release.py's style (DownhillRevived-<version>-<platform>...)."""
    return 'DownhillRevived-%s-android-experimental.apk' % name


def build_tool(name):
    """name (apksigner, aapt or aapt2) from the newest build-tools of the Android SDK that has it."""
    sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or '')
    suffixes = ('.bat', '.exe', '') if os.name == 'nt' else ('',)
    def key(folder):
        return [int(x) if x.isdigit() else 0 for x in re.split(r'[.-]', folder.name)]
    for folder in sorted((sdk / 'build-tools').glob('*'), key=key, reverse=True):
        for suffix in suffixes:
            if (folder / (name + suffix)).is_file():
                return folder / (name + suffix)
    return None


def cert_digest(text):
    """A certificate SHA-256 as apksigner prints it: 64 lower-case hex digits, no colons."""
    return re.sub(r'[^0-9a-f]', '', text.lower())


def check_release(apk, name, code, new_signing_key=False):
    """What is wrong with a release APK (a list of problems), and what it is (a dict): its signature
    (apksigner verify --print-certs: one signer, the release certificate RELEASE_CERT_SHA256 unless
    new_signing_key, the schemes) and aapt's badging: the package, versionCode and versionName (when
    name is not None), not debuggable, arm64-v8a its only native code. facts['new_signer'] is true
    when new_signing_key let another (non-debug) certificate through."""
    problems, facts = [], {}
    apksigner, aapt = build_tool('apksigner'), build_tool('aapt') or build_tool('aapt2')
    if not apksigner or not aapt:
        return ["no apksigner or aapt in ANDROID_HOME's build-tools"], facts
    r = subprocess.run([str(apksigner), 'verify', '--verbose', '--print-certs', str(apk)], capture_output=True, text=True)
    if r.returncode:
        problems.append('apksigner verify failed: %s' % (r.stderr or r.stdout).strip()[-500:])
    out = r.stdout
    digests = re.findall(r'certificate SHA-256 digest: ([0-9a-f]{64})', out)
    facts['signer_sha256'] = digests[0] if digests else ''
    facts['signer_dn'] = (re.findall(r'certificate DN: (.*)', out) or [''])[0].strip()
    facts['schemes'] = [s for s in ('v1', 'v2', 'v3', 'v3.1', 'v4')
                        if re.search(r'Verified using %s scheme[^:]*: true' % re.escape(s), out)]
    if len(set(digests)) != 1:
        problems.append('signed by %d certificates, not one' % len(set(digests)))
    if 'Android Debug' in out:
        problems.append('signed with a debug key')
    facts['new_signer'] = False
    if digests and digests[0] != cert_digest(RELEASE_CERT_SHA256):
        if new_signing_key and 'Android Debug' not in out:
            facts['new_signer'] = True
        else:
            problems.append('signed by certificate SHA-256 %s, not the release key %s (an installed copy refuses it as '
                            'an update; --new-signing-key only for a deliberate key change)'
                            % (digests[0], cert_digest(RELEASE_CERT_SHA256)))
    if not {'v2', 'v3'} & set(facts['schemes']):
        problems.append('no APK signature scheme v2 or v3')
    r = subprocess.run([str(aapt), 'dump', 'badging', str(apk)], capture_output=True, text=True)
    badging = r.stdout
    if r.returncode or not badging:
        return problems + ['aapt dump badging failed: %s' % r.stderr.strip()[-300:]], facts
    package = re.search(r"package: name='([^']*)' versionCode='(\d+)' versionName='([^']*)'", badging)
    facts['badging'] = package.group(0) if package else ''
    if not package or package.group(1) != PACKAGE or (name is not None and package.groups() != (PACKAGE, str(code), name)):
        wanted = PACKAGE if name is None else '%s %d %s' % (PACKAGE, code, name)   # --check: no version to compare
        problems.append('package line %r, not %s' % (facts['badging'], wanted))
    native = re.search(r'native-code: (.*)', badging)
    facts['native_code'] = native.group(1).strip() if native else ''
    if facts['native_code'] != "'arm64-v8a'":
        problems.append('native code %r, not arm64-v8a only' % facts['native_code'])
    try:  # a profile-collecting library (--pgo-generate) has the counters' section; a release never does
        import zipfile
        if b'__llvm_prf_cnts' in zipfile.ZipFile(apk).read('lib/arm64-v8a/libmain.so'):
            problems.append('libmain.so is the profile-collecting build (--pgo-generate)')
        with zipfile.ZipFile(apk) as z:
            split = z.read('assets/dhd-split.txt').decode().strip() if 'assets/dhd-split.txt' in z.namelist() else 'unknown'
        if split not in ('one', 'wasm2c'):  # finding 151: the development chunks cost the game ~1.5 %
            problems.append('libmain.so was built from %s C, not a release build (build_host64.py --release)' % split)
    except (OSError, KeyError, zipfile.BadZipFile) as e:
        problems.append('libmain.so not readable in the APK: %s' % e)
    facts['debuggable'] = 'application-debuggable' in badging
    if facts['debuggable']:
        problems.append('debuggable')
    return problems, facts


def release_env():
    """The release key's variables, checked: all set, the keystore a file outside the checkout."""
    missing = [v for v in RELEASE_ENV if not os.environ.get(v)]
    if missing:
        sys.exit('build_apk: --release needs %s set (the release key; docs/BUILDING.md)' % ', '.join(missing))
    keystore = Path(os.environ['DHD_ANDROID_KEYSTORE']).resolve()
    if not keystore.is_file():
        sys.exit('build_apk: DHD_ANDROID_KEYSTORE names no file')
    if ROOT in keystore.parents:
        sys.exit('build_apk: the release keystore must be outside the checkout (it is never committed)')


def icons(res):
    source = ROOT / 'port' / 'res' / 'icon.png'
    try:
        from PIL import Image
    except ImportError:  # one icon, scaled by Android
        (res / 'mipmap-xxxhdpi').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, res / 'mipmap-xxxhdpi' / 'ic_launcher.png')
        return
    image = Image.open(source).convert('RGBA')
    for density, size in ICON_SIZES.items():
        (res / ('mipmap-' + density)).mkdir(parents=True, exist_ok=True)
        image.resize((size, size), Image.LANCZOS).save(res / ('mipmap-' + density) / 'ic_launcher.png')


def project(libs, assets=None):
    if PROJECT.exists():
        shutil.rmtree(PROJECT)
    for rel in TEMPLATE:
        src, dst = HERE / rel, PROJECT / rel
        if src.is_dir():
            shutil.copytree(src, dst)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
    _lib, _include, java = sdl3.android_build()
    shutil.copytree(Path(java) / 'org', PROJECT / 'app' / 'src' / 'main' / 'java' / 'org')
    icons(PROJECT / 'app' / 'src' / 'main' / 'res')
    if assets:  # files the APK carries as they are (package_release.py: the licence texts)
        shutil.copytree(assets, PROJECT / 'app' / 'src' / 'main' / 'assets')
    # which C libmain.so was built from (build_host64's split.txt: one, wasm2c, chunks), so a release
    # check of any APK can refuse the development chunks (finding 151)
    split = OUT / 'split.txt'
    if split.exists():
        (PROJECT / 'app' / 'src' / 'main' / 'assets').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(split, PROJECT / 'app' / 'src' / 'main' / 'assets' / 'dhd-split.txt')
    jni = PROJECT / 'app' / 'src' / 'main' / 'jniLibs' / 'arm64-v8a'
    jni.mkdir(parents=True)
    for lib in libs:
        shutil.copyfile(lib, jni / lib.name)
    template = Path(java).parents[3]  # SDL's android-project
    for rel in ('gradlew', 'gradlew.bat', 'gradle/wrapper/gradle-wrapper.jar'):
        (PROJECT / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(template / rel, PROJECT / rel)
    (PROJECT / 'gradle' / 'wrapper' / 'gradle-wrapper.properties').write_text(
        'distributionBase=GRADLE_USER_HOME\ndistributionPath=wrapper/dists\n'
        'distributionUrl=https\\://services.gradle.org/distributions/gradle-%s-all.zip\n'
        'zipStoreBase=GRADLE_USER_HOME\nzipStorePath=wrapper/dists\n' % WRAPPER_GRADLE)
    sdk = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or str(Path.home() / 'Android/Sdk')
    (PROJECT / 'local.properties').write_text('sdk.dir=%s\n' % sdk.replace('\\', '/'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--wasm', type=Path, help='rebuild the libraries from this module first')
    ap.add_argument('--jobs', type=int, help="parallel compiles (default: build_host64.py's for the target)")
    ap.add_argument('--line-tables', action='store_true',
                    help="libmain.debug.so with line tables (build_host64's --debug, for simpleperf); code unchanged")
    pgo = ap.add_mutually_exclusive_group()
    pgo.add_argument('--pgo-generate', action='store_true',
                     help="build_host64's: an instrumented build; a run writes files/dhd-<pid>.profraw at its exit")
    pgo.add_argument('--pgo-use', type=Path, metavar='PROFDATA', help="build_host64's: optimise with this merged profile")
    ap.add_argument('--offline', action='store_true', help="Gradle from GRADLE_USER_HOME's cache only")
    ap.add_argument('--release', action='store_true', help='the release APK, signed with the key DHD_ANDROID_* name')
    ap.add_argument('--new-signing-key', action='store_true',
                    help='a deliberate key change: accept a release signer other than RELEASE_CERT_SHA256')
    ap.add_argument('--check', type=Path, metavar='APK', help='only check this APK as a release one (check_release)')
    ap.add_argument('--assets', type=Path, help="a folder whose files go into the APK's assets (the licence texts)")
    ap.add_argument('--extra-lib', type=Path, action='append', default=[],
                    help='another arm64 .so to pack (e.g. libVkLayer_khronos_validation.so for DHD_WGPU_DEBUG)')
    a = ap.parse_args()
    if a.check:
        return check_only(a.check, a.new_signing_key)
    for var in ('GRADLE_USER_HOME', 'ANDROID_USER_HOME'):
        value = os.environ.get(var, '')
        if not value or os.path.splitdrive(os.path.abspath(value))[0].upper() == 'C:':
            sys.exit('build_apk: %s must be set, off C: (%r)' % (var, value))
    if a.release:
        release_env()
    libs = [OUT / 'lib' / 'arm64-v8a' / n for n in ('libmain.so', 'libSDL3.so')]
    wasm = a.wasm or (ROOT / 'build' / 'wasm' / 'dhd_core.wasm')
    # an explicit native option or a release always builds: an earlier build's libraries may lack it
    # (a release: wasm2c's own split parts, build_host64 --release)
    if a.wasm or a.release or a.pgo_generate or a.pgo_use or a.line_tables or not all(p.exists() for p in libs):
        r = subprocess.run([sys.executable, str(ROOT / 'port' / 'tools' / 'build_host64.py'), '--target', 'android-arm64',
                            str(wasm)] + (['--jobs', str(a.jobs)] if a.jobs else []) + (['--debug'] if a.line_tables else []) + (['--release'] if a.release else []) +
                           (['--pgo-generate'] if a.pgo_generate else []) +
                           (['--pgo-use', str(a.pgo_use.resolve())] if a.pgo_use else []))
        if r.returncode:
            sys.exit('build_apk: build_host64.py --target android-arm64 failed')
    project(libs + a.extra_lib, a.assets)
    name, code = version()
    gradlew = PROJECT / ('gradlew.bat' if os.name == 'nt' else 'gradlew')
    kind = 'release' if a.release else 'debug'
    cmd = [str(gradlew), '--no-daemon', '--console=plain', '-PdhdVersionName=' + name, '-PdhdVersionCode=%d' % code,
           'assembleRelease' if a.release else 'assembleDebug'] + (['--offline'] if a.offline else [])
    print('build_apk: ' + ' '.join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=str(PROJECT))
    if r.returncode:
        sys.exit('build_apk: Gradle failed (exit %d)' % r.returncode)
    built = PROJECT / 'app' / 'build' / 'outputs' / 'apk' / kind / ('app-%s.apk' % kind)
    if not a.release:
        apk = OUT / ('downhill-revived-%s-debug.apk' % name)
        shutil.copyfile(built, apk)
        print('PASS: %s (%.1f MB), %s %s (%d), arm64-v8a, debug-signed' % (
            apk, apk.stat().st_size / 1048576, PACKAGE, name, code), flush=True)
        return
    problems, facts = check_release(built, name, code, a.new_signing_key)
    if problems:
        sys.exit('build_apk: the release APK is not fit to publish:\n  ' + '\n  '.join(problems))
    apk = OUT / release_apk_name(name)
    shutil.copyfile(built, apk)
    print('PASS: %s (%.1f MB), %s; signed by %s (certificate SHA-256 %s; schemes %s), not debuggable, '
          'native code %s' % (apk, apk.stat().st_size / 1048576, facts['badging'], facts['signer_dn'],
                              facts['signer_sha256'], ' '.join(facts['schemes']), facts['native_code']), flush=True)
    if facts['new_signer']:
        print(NEW_SIGNER_NOTE % facts['signer_sha256'], flush=True)


def check_only(apk, new_signing_key):
    """--check APK: check_release on a built APK (its version is not compared with version.h)."""
    if not apk.is_file():
        sys.exit('build_apk: %s: no such APK' % apk)
    problems, facts = check_release(apk, None, None, new_signing_key)
    if problems:
        sys.exit('build_apk: %s is not fit to publish:\n  %s' % (apk, '\n  '.join(problems)))
    print('PASS: %s, %s; signed by %s (certificate SHA-256 %s; schemes %s), not debuggable, native code %s'
          % (apk, facts['badging'], facts['signer_dn'], facts['signer_sha256'], ' '.join(facts['schemes']),
             facts['native_code']), flush=True)
    if facts['new_signer']:
        print(NEW_SIGNER_NOTE % facts['signer_sha256'], flush=True)


if __name__ == '__main__':
    main()
