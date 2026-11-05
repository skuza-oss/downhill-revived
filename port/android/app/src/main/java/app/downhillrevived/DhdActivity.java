// Downhill Revived on Android (finding 091): SDL's activity, which loads libSDL3.so and libmain.so
// (the game, port/tools/build_host64.py --target android-arm64) and runs SDL_main (port/host/android_main.c) on its
// own thread. Here only what the game asks of the activity: the screen's shape before that, and for
// the touch controls (finding 141, port/host/android_touch.c, called from SDL's thread): the
// screen's density and its gesture and cutout insets, the vibrator, and the phone's roll for tilt
// steering; and the update notice (finding 143, port/host/android_update.c): one line over the game
// when a newer release has an APK, which opens the release's page in the browser.
package app.downhillrevived;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Rect;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

public class DhdActivity extends SDLActivity {
    /** How long the update notice stays. */
    private static final int NOTICE_MS = 15000;

    private volatile float roll = Float.NaN;
    private SensorEventListener tiltListener;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // How the run before ended, before the native game starts (it quotes dhd-exit.txt into the
        // new dhd.log): once per process, so a start that skips SetupActivity (a USB controller
        // plugged in) writes it too (finding 152). On a thread, waited for 2 s at most: the record's
        // trace can be slow to come, and the UI thread must not freeze; a late record only misses
        // this run's "previous run" lines
        Thread exit = new Thread(() -> LastExit.record(getApplicationContext()), "last exit");
        exit.start();
        try {
            exit.join(2000);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
        super.onCreate(savedInstanceState);
        // The screen's shape (its long side over its short one), for widescreen = 1:
        // host_monitor_aspect reads DHD_MONITOR_ASPECT. Set before SDL_main starts.
        try {
            Rect bounds = getWindowManager().getMaximumWindowMetrics().getBounds();
            int w = Math.max(bounds.width(), bounds.height()), h = Math.min(bounds.width(), bounds.height());
            if (h > 0) nativeSetenv("DHD_MONITOR_ASPECT", Double.toString((double) w / h));
        } catch (Throwable e) {  // the libraries didn't load: SDLActivity shows why
            Log.w("DownhillRevived", "no screen shape: " + e);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (tiltListener != null) ((SensorManager) getSystemService(SENSOR_SERVICE)).unregisterListener(tiltListener);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (tiltListener != null) registerTilt();
    }

    // SDL hides the system bars once, when the game goes full screen. A rotation the activity asks
    // for itself (tilt steering keeps the screen on its landscape side, touchTiltOn) can bring the
    // status bar back on some phones (MIUI on a Redmi 9): hide them again after a rotation and when
    // the window gets its focus back, as SetupActivity does.
    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        rehideBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) rehideBars();
    }

    /** The system bars hidden again (a swipe shows them for a moment), now and once the new layout
     *  is done; only while the game is full screen (SDL's own flag). */
    private void rehideBars() {
        if (!mFullscreenModeActive) return;
        hideBars();
        View decor = getWindow().getDecorView();
        decor.post(new Runnable() {
            @Override public void run() { if (mFullscreenModeActive) hideBars(); }
        });
    }

    private void hideBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    /** The touch controls' screen: xdpi and ydpi times 1000, the system gesture insets (left, top,
     *  right, bottom) and the cutout's safe insets, in pixels. */
    public int[] touchScreen() {
        int[] r = new int[10];
        DisplayMetrics m = getResources().getDisplayMetrics();
        r[0] = Math.round(m.xdpi * 1000);
        r[1] = Math.round(m.ydpi * 1000);
        try {
            WindowInsets in = getWindow().getDecorView().getRootWindowInsets();
            if (in != null) {
                Insets g = in.getInsets(WindowInsets.Type.systemGestures());
                r[2] = g.left; r[3] = g.top; r[4] = g.right; r[5] = g.bottom;
                DisplayCutout c = in.getDisplayCutout();
                if (c != null) {
                    r[6] = c.getSafeInsetLeft(); r[7] = c.getSafeInsetTop(); r[8] = c.getSafeInsetRight(); r[9] = c.getSafeInsetBottom();
                }
            }
        } catch (Throwable e) {
            Log.w("DownhillRevived", "no window insets: " + e);
        }
        return r;
    }

    /** A tick (1), a double tick (2) or a strong tick (3) on the phone's vibrator. */
    public void touchBuzz(int kind) {
        Vibrator v = getSystemService(Vibrator.class);
        if (v == null || !v.hasVibrator()) return;
        int effect = kind == 3 ? VibrationEffect.EFFECT_HEAVY_CLICK : kind == 2 ? VibrationEffect.EFFECT_DOUBLE_CLICK : VibrationEffect.EFFECT_TICK;
        v.vibrate(VibrationEffect.createPredefined(effect));
    }

    /** The phone's roll in degrees (turned right positive, 0 held level in landscape), NaN until known. */
    public float touchTilt() {
        return roll;
    }

    /** Tilt steering on or off: the gravity sensor, and the screen kept on its current landscape side.
     *  Returns whether the phone has a tilt sensor (gravity, else the accelerometer); without one
     *  nothing changes (the game's stick keeps steering). Called from the SDL thread. */
    public boolean touchTiltOn(final boolean on) {
        final boolean sensor = tiltSensor() != null;
        runOnUiThread(new Runnable() {
            @Override public void run() {
                SensorManager sensors = (SensorManager) getSystemService(SENSOR_SERVICE);
                if (on && tiltListener == null && sensor) {
                    tiltListener = new SensorEventListener() {
                        @Override public void onSensorChanged(SensorEvent e) {
                            float x = e.values[0], y = e.values[1], sx, sy;
                            int rotation = getDisplay().getRotation();
                            if (rotation == Surface.ROTATION_90) { sx = -y; sy = x; }
                            else if (rotation == Surface.ROTATION_270) { sx = y; sy = -x; }
                            else if (rotation == Surface.ROTATION_180) { sx = -x; sy = -y; }
                            else { sx = x; sy = y; }
                            roll = (float) Math.toDegrees(Math.atan2(-sx, sy));
                        }
                        @Override public void onAccuracyChanged(Sensor s, int accuracy) {}
                    };
                    registerTilt();
                    // kept on its landscape side (LOCKED would keep a portrait screen when the game
                    // starts from a portrait home screen: the picture then stays in a strip)
                    setRequestedOrientation(getDisplay().getRotation() == Surface.ROTATION_270
                            ? ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
                } else if (!on && tiltListener != null) {
                    sensors.unregisterListener(tiltListener);
                    tiltListener = null;
                    roll = Float.NaN;
                    setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
                }
            }
        });
        return sensor;
    }

    /** The sensor tilt steering reads: gravity, else the accelerometer; null without either. */
    private Sensor tiltSensor() {
        SensorManager sensors = (SensorManager) getSystemService(SENSOR_SERVICE);
        if (sensors == null) return null;
        Sensor s = sensors.getDefaultSensor(Sensor.TYPE_GRAVITY);
        return s != null ? s : sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
    }

    private void registerTilt() {
        Sensor s = tiltSensor();
        if (s != null) ((SensorManager) getSystemService(SENSOR_SERVICE)).registerListener(tiltListener, s, SensorManager.SENSOR_DELAY_GAME);
    }

    /** The update notice: "Downhill Revived VERSION is out - tap for the download page" at the top of
     *  the screen for NOTICE_MS; a tap opens the page in the browser (the releases page unless
     *  asked is one of this repository's). Called once, from the update
     *  check's thread. */
    public void updateNotice(final String version, final String asked) {
        final String page = UpdatePage.safe(asked);   /* this repository's pages only (update_check.c checks it too) */
        runOnUiThread(new Runnable() {
            @Override public void run() {
                final TextView notice = new TextView(DhdActivity.this);
                notice.setText("Downhill Revived " + version + " is out - tap for the download page");
                notice.setTextColor(Color.WHITE);
                notice.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
                notice.setSingleLine(true);
                int pad = dp(10);
                notice.setPadding(2 * pad, pad, 2 * pad, pad);
                GradientDrawable back = new GradientDrawable();
                back.setColor(0xE0202020);
                back.setCornerRadius(dp(8));
                notice.setBackground(back);
                FrameLayout.LayoutParams where = new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                        Gravity.TOP | Gravity.CENTER_HORIZONTAL);
                where.topMargin = dp(12);
                final Runnable remove = new Runnable() {
                    @Override public void run() {
                        ViewGroup parent = (ViewGroup) notice.getParent();
                        if (parent != null) parent.removeView(notice);
                    }
                };
                notice.setOnClickListener(new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        remove.run();
                        try {
                            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(page)));
                        } catch (Throwable e) {
                            Log.w("DownhillRevived", "no browser for " + page + ": " + e);
                        }
                    }
                });
                addContentView(notice, where);
                notice.postDelayed(remove, NOTICE_MS);
            }
        });
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
