// Downhill Revived on Android: Share log (finding 152). dhd.log lives in the app's private files
// folder, which no other app and no file manager can open, so a player whose game crashes could send
// nothing. This copies the logs into one text file (named for the version and the day) and opens
// Android's share sheet with it, served by LogProvider; nothing leaves the phone unless the player
// picks an app there. Reached from the dialog after a run that closed unexpectedly and from the
// icon's long-press shortcut (SetupActivity).
// The file: the phone, how the last run ended (LastExit's dhd-exit.txt), the log of the run that
// ended, then the other one. Which is which: a dhd.log written since this process started is the
// game running now (host.c moved the one before to dhd-previous.log); otherwise dhd.log is the run
// that ended. A long log keeps its start and its end (CrashReport.copyLog).
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

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

final class LogShare {
    private static final String TAG = "DownhillRevived";
    /** Each log's start and end kept in the shared file: some MB in all, which chat apps take. */
    private static final long HEAD = 256 << 10, TAIL = 1536 << 10;

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
        File log = new File(files, "dhd.log"), previous = new File(files, "dhd-previous.log");
        String exit = LastExit.text(activity);
        long started = System.currentTimeMillis() - (SystemClock.elapsedRealtime() - Process.getStartElapsedRealtime());
        boolean running = log.isFile() && log.lastModified() >= started;
        File ended = running ? previous : log, other = running ? log : previous;
        if (!ended.isFile() && !other.isFile() && exit == null) return "There is no log yet.";
        String version = version(activity);
        Date now = new Date();
        File dir = folder(activity);
        dir.mkdirs();
        File[] old = dir.listFiles();
        if (old != null) for (File f : old) f.delete();
        File out = new File(dir, "downhill-revived-" + version + "-log-"
                + new SimpleDateFormat("yyyy-MM-dd", Locale.US).format(now) + ".txt");
        try (OutputStream o = new BufferedOutputStream(new FileOutputStream(out))) {
            line(o, "Downhill Revived " + version + " log, shared " + LastExit.date(now.getTime()));
            line(o, "phone: " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE
                    + " (API " + Build.VERSION.SDK_INT + ")");
            if (exit != null) {
                line(o, "\n==== how the last run ended (Android's record) ====");
                o.write(exit.getBytes(StandardCharsets.UTF_8));
            }
            section(o, ended, "the last run (" + ended.getName() + ")");
            section(o, other, (running ? "this run, still going (" : "the run before it (") + other.getName() + ")");
        } catch (IOException e) {
            Log.w(TAG, "the shared log can't be written: " + e);
            return "The log couldn't be copied (" + e.getMessage() + ").";
        }
        Uri uri = LogProvider.uri(out);
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_STREAM, uri);
        send.putExtra(Intent.EXTRA_SUBJECT, "Downhill Revived " + version + " log");
        send.setClipData(ClipData.newRawUri(out.getName(), uri));  // the grant reaches the app chosen
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return Intent.createChooser(send, "Share log");
    }

    private static void section(OutputStream o, File log, String title) throws IOException {
        if (!log.isFile()) return;
        line(o, "\n==== " + title + ", last written " + LastExit.date(log.lastModified()) + " ====");
        CrashReport.copyLog(log, o, HEAD, TAIL);
    }

    private static void line(OutputStream o, String s) throws IOException {
        o.write((s + "\n").getBytes(StandardCharsets.UTF_8));
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
