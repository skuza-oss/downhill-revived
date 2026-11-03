"""build_apk.py's release check (check_release, --check; finding 143) with apksigner and aapt
replaced by canned answers: no Android SDK, APK or key needed.

    python port/android/test_build_apk.py
"""
import importlib.util
from pathlib import Path
import sys
import types

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


def tools(digest, badging):
    """apksigner and aapt answering as for an APK signed by digest with this badging."""
    def run(cmd, capture_output=True, text=True):
        if 'apksigner' in str(cmd[0]):
            return Answer('Verified using v2 scheme (APK Signature Scheme v2): true\n'
                          'Verified using v3 scheme (APK Signature Scheme v3): true\n'
                          'Signer #1 certificate DN: CN=Downhill Revived\n'
                          'Signer #1 certificate SHA-256 digest: %s\n' % digest)
        return Answer(badging)
    build_apk.build_tool = lambda name: Path(name + '.bat') if name in ('apksigner', 'aapt') else None
    build_apk.subprocess = types.SimpleNamespace(run=run)


GOOD = "package: name='app.downhillrevived' versionCode='708' versionName='0.7.8'\nnative-code: 'arm64-v8a'\n"
OTHER = "package: name='com.example.other' versionCode='1' versionName='1'\nnative-code: 'arm64-v8a'\n"


def main():
    # the release APK passes, on the packaging path (a version) and on --check's (none)
    tools(RELEASE, GOOD)
    check(build_apk.check_release('x.apk', '0.7.8', 708)[0] == [], 'the release APK passes with its version')
    check(build_apk.check_release('x.apk', None, None)[0] == [], 'the release APK passes --check')
    # another package: refused with one line on both paths, never a traceback (--check passes no version)
    tools(RELEASE, OTHER)
    try:
        problems, _ = build_apk.check_release('x.apk', None, None)
        check(any("not app.downhillrevived" in p and 'com.example.other' in p for p in problems),
              '--check names the wrong package: %r' % problems)
        problems, _ = build_apk.check_release('x.apk', '0.7.8', 708)
        check(any('not app.downhillrevived 708 0.7.8' in p for p in problems), 'packaging names the wanted version: %r' % problems)
    except Exception as e:  # noqa: BLE001 (the bug this test is for)
        check(False, 'check_release raised %s: %s' % (type(e).__name__, e))
    # no package line at all
    tools(RELEASE, "native-code: 'arm64-v8a'\n")
    try:
        problems, _ = build_apk.check_release('x.apk', None, None)
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
    check(any('not app.downhillrevived 709 0.7.9' in p for p in build_apk.check_release('x.apk', '0.7.9', 709)[0]),
          'a version mismatch is refused')
    if failures:
        sys.exit('test_build_apk: %d failed' % len(failures))
    print('test_build_apk: all passed')


if __name__ == '__main__':
    main()
