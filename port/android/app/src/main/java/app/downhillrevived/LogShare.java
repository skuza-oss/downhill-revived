// Downhill Revived on Android: Share log (finding 152). dhd.log lives in the app's private files
// folder, which no other app and no file manager can open, so a player whose game crashes could send
// nothing. This copies the logs into one text file (named for the version and the day) and opens
// Android's share sheet with it, served by LogProvider; nothing leaves the phone unless the player
// picks an app there. Reached from the launcher's note after a run that closed unexpectedly, its Help
// page and the icon's long-press shortcut (LauncherActivity; finding 225).
// The file is written by the shared C the iOS launcher uses too (port/host/log_bundle.c, finding
// 242): the device, how the last run ended (LastExit's dhd-exit.txt), launcher.log (the disc
// import's steps), the log of the run that ended, then the other one; a long log keeps its start and
// its end. What only Android knows stays here: the phone's words, and which log is which (a dhd.log
// written since this process started is the game running now: host.c moved the one before to
// dhd-previous.log).
package app.downhillrevived;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;

final class LogShare {
    private static final String TAG = "DownhillRevived";

    private LogShare() {}

    /** The folder LogProvider serves: only the shared file is in it. */
    static File folder(Context context) {
        return new File(context.getCacheDir(), "share");
    }

    /** Writes the shared file on a thread of its own (the logs are megabytes and the exit record's
     *  trace may be slow to come: never on the UI thread, which would freeze the screen), then
     *  opens the share sheet on the UI thread (its result comes to activity's onActivityResult
     *  with request). When there is nothing to share or the file can't be written it says so and
     *  runs failed (may be null). */
    static void start(Activity activity, int request, Runnable failed) {
        Context app = activity.getApplicationContext();
        new Thread(() -> {
            Object made;  // the share Intent, or the message for the player
            try {
                made = prepare(app);
            } catch (Throwable e) {
                made = "The log couldn't be copied (" + e.getMessage() + ").";
            }
            final Object result = made;
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed()) return;
                if (result instanceof Intent) {
                    try {
                        activity.startActivityForResult((Intent) result, request);
                        return;
                    } catch (ActivityNotFoundException e) {
                        Toast.makeText(activity, "No app on this phone can share a file.", Toast.LENGTH_LONG).show();
                    }
                } else {
                    Toast.makeText(activity, (String) result, Toast.LENGTH_LONG).show();
                }
                if (failed != null) failed.run();
            });
        }, "share log").start();
    }

    /** The shared file written, and the chooser's Intent for it; or the message for the player. */
    private static Object prepare(Context activity) {
        File files = activity.getFilesDir();
        File log = new File(files, "dhd.log");
        LastExit.record(activity);  // dhd-exit.txt, written once by this process
        long started = System.currentTimeMillis() - (SystemClock.elapsedRealtime() - Process.getStartElapsedRealtime());
        boolean running = log.isFile() && log.lastModified() >= started;
        String version = version(activity);
        File dir = folder(activity);
        dir.mkdirs();
        File[] old = dir.listFiles();
        if (old != null) for (File f : old) f.delete();
        String made;
        try {
            made = Native.logBundle(files.getPath(), dir.getPath(), version, Build.MANUFACTURER + " " + Build.MODEL
                    + ", Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")", running);
        } catch (IOException e) {
            Log.w(TAG, "the shared log can't be written: " + e);
            return "The log couldn't be copied (" + e.getMessage() + ").";
        }
        if (made == null) return "There is no log yet.";
        File out = new File(made);
        Uri uri = LogProvider.uri(out);
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_STREAM, uri);
        send.putExtra(Intent.EXTRA_SUBJECT, "Downhill Revived " + version + " log");
        send.setClipData(ClipData.newRawUri(out.getName(), uri));  // the grant reaches the app chosen
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return Intent.createChooser(send, "Share log");
    }

    private static String version(Context context) {
        try {
            String v = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName;
            return v != null ? v : "unknown";
        } catch (Throwable e) {
            return "unknown";
        }
    }
}
