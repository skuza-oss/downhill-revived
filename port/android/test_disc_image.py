"""Run the Android side's plain-JVM tests (finding 143): the first start's disc rules
(port/android/test/DiscImageTest.java), the update notice's page (UpdatePageTest.java) and the crash
report's rules (CrashReportTest.java, finding 152), then the controller motion route
across text-editor focus (test_pad_motion.py, finding 245).

    python port/android/test_disc_image.py [your.iso]

Compiles app/src/main/java/app/downhillrevived/DiscImage.java, UpdatePage.java, CrashReport.java and
the tests with the JDK in JAVA_HOME (the one the Android build uses; javac and java on PATH
otherwise) into build/android/disc-image-test, then runs them. With an .iso (or DHD_TEST_ISO set) the disc test also
checks that disc image. Needs a host C compiler and JDK JNI headers (DHD_JNI_HEADERS overrides the include folder).
No Android SDK or Gradle needed.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'build' / 'android' / 'disc-image-test'


def jdk_tool(name):
    home = os.environ.get('JAVA_HOME')
    if home:
        for suffix in ('.exe', ''):
            tool = Path(home) / 'bin' / (name + suffix)
            if tool.is_file():
                return str(tool)
    found = shutil.which(name)
    if not found:
        sys.exit('test_disc_image: no %s (set JAVA_HOME to a JDK 17+)' % name)
    return found


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    sys.path.insert(0, str(ROOT / 'port' / 'host'))
    from disc_fixtures import write
    write(OUT / 'fixtures')
    os.environ['DHD_DISC_FIXTURES'] = str(OUT / 'fixtures')
    # Compile the production JNI adapter against the desktop JVM as well as the Android build.
    jdk = Path(jdk_tool('javac')).parent.parent
    sys.path.insert(0, str(ROOT / 'port' / 'tools'))
    import testprog
    suffix = 'win32' if os.name == 'nt' else ('darwin' if sys.platform == 'darwin' else 'linux')
    library = OUT / ('main.dll' if os.name == 'nt' else ('libmain.dylib' if sys.platform == 'darwin' else 'libmain.so'))
    headers = Path(os.environ.get('DHD_JNI_HEADERS', jdk / 'include'))
    cmd = [testprog.CLANG, '-shared', '-I', str(headers), '-I', str(headers / suffix),
           str(ROOT / 'port/host/disc_check.c'), str(ROOT / 'port/host/disc_import.c'),
           str(ROOT / 'port/host/disc_check_jni.c'), '-o', str(library)]
    env = os.environ.copy()
    if os.name != 'nt':
        cmd += ['-fPIC']
    subprocess.run(cmd, env=env, check=True)
    java = HERE / 'app' / 'src' / 'main' / 'java' / 'app' / 'downhillrevived'
    sources = [java / 'DiscImage.java', java / 'UpdatePage.java', java / 'CrashReport.java',
               HERE / 'test' / 'DiscImageTest.java', HERE / 'test' / 'UpdatePageTest.java',
               HERE / 'test' / 'CrashReportTest.java']
    r = subprocess.run([jdk_tool('javac'), '--release', '17', '-d', str(OUT)] + [str(s) for s in sources])
    if r.returncode:
        sys.exit('test_disc_image: javac failed')
    r = subprocess.run([jdk_tool('java'), '-Djava.library.path=' + str(OUT), '-Djava.io.tmpdir=' + str(OUT), '-cp', str(OUT), 'app.downhillrevived.UpdatePageTest'])
    failed = r.returncode
    r = subprocess.run([jdk_tool('java'), '-Djava.library.path=' + str(OUT), '-Djava.io.tmpdir=' + str(OUT), '-cp', str(OUT), 'app.downhillrevived.CrashReportTest'])
    failed = failed or r.returncode
    r = subprocess.run([jdk_tool('java'), '-Djava.library.path=' + str(OUT), '-Djava.io.tmpdir=' + str(OUT), '-cp', str(OUT), 'app.downhillrevived.DiscImageTest'] + sys.argv[1:2])
    failed = failed or r.returncode
    # The game activity's focus-independent SDL controller-motion route (245).
    r = subprocess.run([sys.executable, str(HERE / 'test_pad_motion.py')])
    sys.exit(failed or r.returncode)


if __name__ == '__main__':
    main()
