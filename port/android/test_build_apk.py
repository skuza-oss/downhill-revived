"""build_apk.py's release check (check_release, --check; finding 143) with apksigner and aapt
replaced by canned answers: no Android SDK, APK or key needed.

    python port/android/test_build_apk.py
"""
import atexit
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile
import types
import zipfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('build_apk', HERE / 'build_apk.py')
build_apk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_apk)

RELEASE = build_apk.cert_digest(build_apk.RELEASE_CERT_SHA256)
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print('FAIL:', what)


class Answer:
    def __init__(self, stdout, returncode=0, stderr=''):
        self.stdout, self.returncode, self.stderr = stdout, returncode, stderr


def tools(digest, badging, schemes=('v2', 'v3')):
    """apksigner and aapt answering as for an APK signed by digest with these schemes and this badging."""
    def run(cmd, capture_output=True, text=True):
        if 'apksigner' in str(cmd[0]):
            asked.append(cmd)
            # as apksigner: only the scheme the asked range verifies with is true (v2 below API 28)
            used = 'v2' if '--max-sdk-version' in cmd else 'v3'
            return Answer(''.join('Verified using %s scheme (APK Signature Scheme %s): %s\n'
                                  % (s, s, 'true' if s == used and s in schemes else 'false') for s in ('v2', 'v3'))
                          + 'Signer #1 certificate DN: CN=Downhill Revived\n'
                          'Signer #1 certificate SHA-256 digest: %s\n' % digest)
        return Answer(badging)
    build_apk.build_tool = lambda name: Path(name + '.bat') if name in ('apksigner', 'aapt') else None
    build_apk.subprocess = types.SimpleNamespace(run=run)


asked = []  # the apksigner command lines
# check_release also reads libmain.so and the build kind from the APK itself (finding 151): a
# zip with a release build's marker and a library without profile counters
FAKE_DIR = tempfile.mkdtemp(prefix='dhd-apk-test-')
atexit.register(shutil.rmtree, FAKE_DIR, True)
FAKE_APK = str(Path(FAKE_DIR) / 'x.apk')
with zipfile.ZipFile(FAKE_APK, 'w') as z:
    z.writestr('lib/arm64-v8a/libmain.so', b'\x7fELF not really')
    z.writestr('assets/dhd-split.txt', 'one\n')
    z.writestr('assets/launcher/index.html', '<!doctype html>')  # the launcher's page (finding 225)
NO_PAGE_APK = str(Path(FAKE_DIR) / 'no-page.apk')
with zipfile.ZipFile(NO_PAGE_APK, 'w') as z:
    z.writestr('lib/arm64-v8a/libmain.so', b'\x7fELF not really')
    z.writestr('assets/dhd-split.txt', 'one\n')
SDK = "sdkVersion:'%d'\n" % build_apk.min_sdk()
GOOD = "package: name='app.downhillrevived' versionCode='708' versionName='0.7.8'\n" + SDK + "native-code: 'arm64-v8a'\n"
OTHER = "package: name='com.example.other' versionCode='1' versionName='1'\n" + SDK + "native-code: 'arm64-v8a'\n"


def main():
    # the release APK passes, on the packaging path (a version) and on --check's (none)
    tools(RELEASE, GOOD)
    check(build_apk.check_release(FAKE_APK, '0.7.8', 708)[0] == [], 'the release APK passes with its version')
    check(build_apk.check_release(FAKE_APK, None, None)[0] == [], 'the release APK passes --check')
    check(build_apk.check_release(NO_PAGE_APK, '0.7.8', 708)[0] == ['no launcher page (assets/launcher/index.html)'],
          'an APK without the launcher page is refused')
    check(build_apk.min_sdk() == 29, 'the minimum is Android 10 (finding 191): %d' % build_apk.min_sdk())
    main_verify = [c for c in asked if '--print-certs' in c][-1:]
    check(main_verify and main_verify[0][main_verify[0].index('--min-sdk-version') + 1] == '29'
          and '--max-sdk-version' not in main_verify[0], 'apksigner verifies from the minimum API level: %r' % main_verify)
    # finding 191: v2 and v3 both, and the minimum API level of app/build.gradle
    tools(RELEASE, GOOD, schemes=('v3',))
    check(build_apk.check_release(FAKE_APK, '0.7.8', 708)[0] == ['no APK signature scheme v2'], 'a v3-only APK is refused')
    tools(RELEASE, GOOD, schemes=('v2',))
    check(build_apk.check_release(FAKE_APK, '0.7.8', 708)[0] == ['no APK signature scheme v3'], 'a v2-only APK is refused')
    tools(RELEASE, GOOD.replace(SDK, "sdkVersion:'30'\n"))
    check(any('minimum API level 30, not 29' in p for p in build_apk.check_release(FAKE_APK, '0.7.8', 708)[0]),
          'an APK for Android 11 and later is refused')
    # another package: refused with one line on both paths, never a traceback (--check passes no version)
    tools(RELEASE, OTHER)
    try:
        problems, _ = build_apk.check_release(FAKE_APK, None, None)
        check(any("not app.downhillrevived" in p and 'com.example.other' in p for p in problems),
              '--check names the wrong package: %r' % problems)
        problems, _ = build_apk.check_release(FAKE_APK, '0.7.8', 708)
        check(any('not app.downhillrevived 708 0.7.8' in p for p in problems), 'packaging names the wanted version: %r' % problems)
    except Exception as e:  # noqa: BLE001 (the bug this test is for)
        check(False, 'check_release raised %s: %s' % (type(e).__name__, e))
    # no package line at all
    tools(RELEASE, "native-code: 'arm64-v8a'\n")
    try:
        problems, _ = build_apk.check_release(FAKE_APK, None, None)
        check(any("package line ''" in p for p in problems), 'a missing package line is refused: %r' % problems)
    except Exception as e:  # noqa: BLE001
        check(False, 'check_release raised %s: %s' % (type(e).__name__, e))
    # --check's own entry: the refusal is its exit message
    tools(RELEASE, OTHER)
    apk = HERE / 'test_build_apk.py'   # any file: the tools are canned
    try:
        build_apk.check_only(apk, False)
        check(False, '--check passed a wrong-package APK')
    except SystemExit as e:
        check(isinstance(e.code, str) and 'not fit to publish' in e.code and 'com.example.other' in e.code,
              '--check refuses with a message: %r' % e.code)
    # a version mismatch on the packaging path
    tools(RELEASE, GOOD)
    check(any('not app.downhillrevived 709 0.7.9' in p for p in build_apk.check_release(FAKE_APK, '0.7.9', 709)[0]),
          'a version mismatch is refused')
    if failures:
        sys.exit('test_build_apk: %d failed' % len(failures))
    print('test_build_apk: all passed')


if __name__ == '__main__':
    main()
