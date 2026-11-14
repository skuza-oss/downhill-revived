package app.downhillrevived;

import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

/** Forward controller motion before focus dispatch: SDL's editor has no motion listener. */
final class PadMotion {
    private PadMotion() {}
    static boolean dispatch(MotionEvent event, View surface, View.OnGenericMotionListener listener) {
        return (event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0
                && surface != null && listener != null && listener.onGenericMotion(surface, event);
    }
}
