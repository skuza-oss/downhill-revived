package app.downhillrevived;

import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

/** Synthetic recorded event order, not measurements of the reporter's unidentified pad. */
public final class PadMotionTest {
    static int motions, fallback;
    static View surface = new View(), editor = new View();
    static View focused = surface;
    static View.OnGenericMotionListener listener = (view, event) -> {
        if (view != surface) throw new AssertionError("SDL must receive its surface");
        motions++;
        return true;
    };
    static void motion(int source) {
        MotionEvent event = new MotionEvent(source);
        if (!PadMotion.dispatch(event, surface, listener)) {
            fallback++;
            focused.dispatchGenericMotionEvent(event);
        }
    }
    public static void main(String[] args) {
        surface.setOnGenericMotionListener(listener);
        motion(InputDevice.SOURCE_JOYSTICK); // pad before typing
        focused = editor; // SDL ShowTextInputTask.requestFocus
        motion(InputDevice.SOURCE_JOYSTICK); // left stick
        motion(InputDevice.SOURCE_JOYSTICK); // hat/d-pad
        motion(InputDevice.SOURCE_JOYSTICK); // trigger
        focused = surface; // SDL COMMAND_TEXTEDIT_HIDE.requestFocus
        motion(InputDevice.SOURCE_JOYSTICK);
        focused = editor; // keyboard reopened
        motion(InputDevice.SOURCE_JOYSTICK);
        if (motions != 6) throw new AssertionError("lost pad motion across IME focus: " + motions);
        int before = motions;
        motion(InputDevice.SOURCE_KEYBOARD);
        motion(InputDevice.SOURCE_MOUSE);
        motion(InputDevice.SOURCE_TOUCHSCREEN);
        if (motions != before || fallback != 3) throw new AssertionError("non-controller input intercepted or duplicate dispatch");
        if (PadMotion.dispatch(new MotionEvent(InputDevice.SOURCE_JOYSTICK), null, listener))
            throw new AssertionError("no surface before SDL starts");
        if (PadMotion.dispatch(new MotionEvent(InputDevice.SOURCE_JOYSTICK), surface, (v, e) -> false))
            throw new AssertionError("unhandled SDL motion must fall through");
        System.out.println("PadMotionTest: PASS (surface -> editor -> surface -> editor, motion/fallback)");
    }
}
