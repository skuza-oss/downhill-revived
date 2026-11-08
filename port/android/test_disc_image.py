"""Run the Android side's plain-JVM tests (finding 143): the first start's disc rules
(port/android/test/DiscImageTest.java), the update notice's page (UpdatePageTest.java) and the crash
report's rules (CrashReportTest.java, finding 152).

    python port/android/test_disc_image.py [your.iso]

Compiles app/src/main/java/app/downhillrevived/DiscImage.java, UpdatePage.java, CrashReport.java and
the tests with the JDK in JAVA_HOME (the one the Android build uses; javac and java on PATH
otherwise) into build/android/disc-image-test, then runs them. With an .iso (or DHD_TEST_ISO set) the disc test also
checks that disc image. No Android SDK or Gradle needed.
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
    java = HERE / 'app' / 'src' / 'main' / 'java' / 'app' / 'downhillrevived'
    sources = [java / 'DiscImage.java', java / 'UpdatePage.java', java / 'CrashReport.java',
               HERE / 'test' / 'DiscImageTest.java', HERE / 'test' / 'UpdatePageTest.java',
               HERE / 'test' / 'CrashReportTest.java']
    r = subprocess.run([jdk_tool('javac'), '--release', '17', '-d', str(OUT)] + [str(s) for s in sources])
    if r.returncode:
        sys.exit('test_disc_image: javac failed')
    r = subprocess.run([jdk_tool('java'), '-cp', str(OUT), 'app.downhillrevived.UpdatePageTest'])
    failed = r.returncode
    r = subprocess.run([jdk_tool('java'), '-cp', str(OUT), 'app.downhillrevived.CrashReportTest'])
    failed = failed or r.returncode
    r = subprocess.run([jdk_tool('java'), '-cp', str(OUT), 'app.downhillrevived.DiscImageTest'] + sys.argv[1:2])
    sys.exit(failed or r.returncode)


if __name__ == '__main__':
    main()
