"""Run the first start's disc rules test (port/android/test/DiscImageTest.java, finding 143) on a plain JVM.

    python port/android/test_disc_image.py [your.iso]

Compiles app/src/main/java/app/downhillrevived/DiscImage.java and the test with the JDK in
JAVA_HOME (the one the Android build uses; javac and java on PATH otherwise) into
build/android/disc-image-test, then runs it. With an .iso (or DHD_TEST_ISO set) it also checks that
disc image. No Android SDK or Gradle needed.
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
    sources = [HERE / 'app' / 'src' / 'main' / 'java' / 'app' / 'downhillrevived' / 'DiscImage.java',
               HERE / 'test' / 'DiscImageTest.java']
    r = subprocess.run([jdk_tool('javac'), '--release', '17', '-d', str(OUT)] + [str(s) for s in sources])
    if r.returncode:
        sys.exit('test_disc_image: javac failed')
    r = subprocess.run([jdk_tool('java'), '-cp', str(OUT), 'app.downhillrevived.DiscImageTest'] + sys.argv[1:2])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
