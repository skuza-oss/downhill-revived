"""Run the production Android motion router on the JVM with minimal View/event fixtures."""
from pathlib import Path
import subprocess
from test_disc_image import jdk_tool

HERE = Path(__file__).resolve().parent
OUT = HERE.parents[1] / 'build/android/pad-motion-test'

def main():
    package = OUT / 'android/view'
    package.mkdir(parents=True, exist_ok=True)
    fixtures = {
        'InputDevice': '''public class InputDevice {
 public static final int SOURCE_CLASS_JOYSTICK=0x10, SOURCE_JOYSTICK=0x01000010,
 SOURCE_KEYBOARD=0x101, SOURCE_MOUSE=0x2002, SOURCE_TOUCHSCREEN=0x1002;
}''',
        'MotionEvent': '''public class MotionEvent {
 private final int source; public MotionEvent(int source) { this.source=source; }
 public int getSource() { return source; }
}''',
        'View': '''public class View {
 public interface OnGenericMotionListener { boolean onGenericMotion(View view, MotionEvent event); }
 private OnGenericMotionListener listener;
 public void setOnGenericMotionListener(OnGenericMotionListener listener) { this.listener=listener; }
 public boolean dispatchGenericMotionEvent(MotionEvent event) {
  return listener != null && listener.onGenericMotion(this,event);
 }
}''',
    }
    sources = []
    for name, text in fixtures.items():
        path = package / (name + '.java')
        path.write_text('package android.view;\n' + text, encoding='utf-8')
        sources.append(str(path))
    sources += [str(HERE / 'app/src/main/java/app/downhillrevived/PadMotion.java'), str(HERE / 'test/PadMotionTest.java')]
    subprocess.run([jdk_tool('javac'), '--release', '17', '-d', str(OUT)] + sources, check=True)
    return subprocess.run([jdk_tool('java'), '-cp', str(OUT), 'app.downhillrevived.PadMotionTest']).returncode

if __name__ == '__main__':
    raise SystemExit(main())
