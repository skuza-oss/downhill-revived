// Downhill Revived on Android: the few screen calls whose API changed in Android 11 (API 30), for
// both activities (finding 191). The APK's minimum is Android 10 (API 29): there the system bars
// are hidden with the older system UI flags (as SDL's own full screen does at every level), the
// screen's size comes from the display's real metrics and its rotation from the default display.
// Android 11 and later take the same calls as before.
package app.downhillrevived;

import android.app.Activity;
import android.graphics.Insets;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.Build;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

final class Screen {
    private Screen() {}

    /** The status and navigation bars hidden; a swipe shows them for a moment. Call after the
     *  window has its content (before it, Android 11's getInsetsController is null or throws). */
    @SuppressWarnings("deprecation")
    static void hideBars(Window window) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController c = window.getInsetsController();
            if (c == null) return;
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        } else {
            window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    /** The whole screen's size in pixels (the largest window the activity can have), {w, h}. */
    @SuppressWarnings("deprecation")
    static int[] size(Activity activity) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Rect bounds = activity.getWindowManager().getMaximumWindowMetrics().getBounds();
            return new int[] { bounds.width(), bounds.height() };
        }
        Point real = new Point();
        activity.getWindowManager().getDefaultDisplay().getRealSize(real);
        return new int[] { real.x, real.y };
    }

    /** The display's rotation (Surface.ROTATION_*). */
    @SuppressWarnings("deprecation")
    static int rotation(Activity activity) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return activity.getDisplay().getRotation();
        return activity.getWindowManager().getDefaultDisplay().getRotation();
    }

    /** The system gesture insets (left, top, right, bottom) in pixels. */
    @SuppressWarnings("deprecation")
    static Insets gestureInsets(WindowInsets in) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return in.getInsets(WindowInsets.Type.systemGestures());
        return in.getSystemGestureInsets();  // API 29
    }

    /** Android 10: the window drawn under a camera cutout on the screen's short edges (landscape: the
     *  sides), as SDL's full screen asks for itself from Android 11 (LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS);
     *  without it Android 10 leaves a black strip beside the cutout. */
    static void underCutout(Window window) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return;
        WindowManager.LayoutParams p = window.getAttributes();
        p.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setAttributes(p);
    }
}
