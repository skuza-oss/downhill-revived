"""Build Downhill Revived's APK for Android arm64: a debug APK (finding 091) or the release APK (finding 143).

    python port/android/build_apk.py [--wasm build/wasm/dhd_core.wasm] [--jobs 2] [--offline]
    python port/android/build_apk.py --release [--wasm ...]   # signed with the release key (below)

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
   checked (check_release: apksigner verify, aapt's badging: not debuggable, the version, arm64-v8a
   only) and written as build/android/DownhillRevived-<version>-android-experimental.apk, the name
   of the release asset (port/tools/package_release.py --platform android).

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


def check_release(apk, name, code):
    """What is wrong with a release APK (a list of problems), and what it is (a dict): its signature
    (apksigner verify --print-certs: the signer's SHA-256, the schemes) and aapt's badging: the
    package, versionCode and versionName, not debuggable, arm64-v8a its only native code."""
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
    if not {'v2', 'v3'} & set(facts['schemes']):
        problems.append('no APK signature scheme v2 or v3')
    r = subprocess.run([str(aapt), 'dump', 'badging', str(apk)], capture_output=True, text=True)
    badging = r.stdout
    if r.returncode or not badging:
        return problems + ['aapt dump badging failed: %s' % r.stderr.strip()[-300:]], facts
    package = re.search(r"package: name='([^']*)' versionCode='(\d+)' versionName='([^']*)'", badging)
    facts['badging'] = package.group(0) if package else ''
    if not package or package.groups() != (PACKAGE, str(code), name):
        problems.append('package line %r, not %s %d %s' % (facts['badging'], PACKAGE, code, name))
    native = re.search(r'native-code: (.*)', badging)
    facts['native_code'] = native.group(1).strip() if native else ''
    if facts['native_code'] != "'arm64-v8a'":
        problems.append('native code %r, not arm64-v8a only' % facts['native_code'])
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
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--offline', action='store_true', help="Gradle from GRADLE_USER_HOME's cache only")
    ap.add_argument('--release', action='store_true', help='the release APK, signed with the key DHD_ANDROID_* name')
    ap.add_argument('--assets', type=Path, help="a folder whose files go into the APK's assets (the licence texts)")
    ap.add_argument('--extra-lib', type=Path, action='append', default=[],
                    help='another arm64 .so to pack (e.g. libVkLayer_khronos_validation.so for DHD_WGPU_DEBUG)')
    a = ap.parse_args()
    for var in ('GRADLE_USER_HOME', 'ANDROID_USER_HOME'):
        value = os.environ.get(var, '')
        if not value or os.path.splitdrive(os.path.abspath(value))[0].upper() == 'C:':
            sys.exit('build_apk: %s must be set, off C: (%r)' % (var, value))
    if a.release:
        release_env()
    libs = [OUT / 'lib' / 'arm64-v8a' / n for n in ('libmain.so', 'libSDL3.so')]
    wasm = a.wasm or (ROOT / 'build' / 'wasm' / 'dhd_core.wasm')
    if a.wasm or not all(p.exists() for p in libs):
        r = subprocess.run([sys.executable, str(ROOT / 'port' / 'tools' / 'build_host64.py'), '--target', 'android-arm64',
                            str(wasm), '--jobs', str(a.jobs)])
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
    problems, facts = check_release(built, name, code)
    if problems:
        sys.exit('build_apk: the release APK is not fit to publish:\n  ' + '\n  '.join(problems))
    apk = OUT / release_apk_name(name)
    shutil.copyfile(built, apk)
    print('PASS: %s (%.1f MB), %s; signed by %s (certificate SHA-256 %s; schemes %s), not debuggable, '
          'native code %s' % (apk, apk.stat().st_size / 1048576, facts['badging'], facts['signer_dn'],
                              facts['signer_sha256'], ' '.join(facts['schemes']), facts['native_code']), flush=True)


if __name__ == '__main__':
    main()
