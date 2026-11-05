// Downhill Revived on Android: how the run before ended (finding 152). Android keeps a record of
// each process's end (ActivityManager.getHistoricalProcessExitReasons, API 30, our minSdk): the
// reason, a description, the time and, for a native crash, the system's trace (a tombstone). Read
// once per process, before any native code runs (SetupActivity, and DhdActivity for a start that
// skips it), and written to files/dhd-exit.txt, which android_main.c quotes into the new dhd.log
// ("previous run: ...") and LogShare puts at the top of a shared log. The words a player reads and
// the trace's summary: CrashReport.java.
package app.downhillrevived;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

final class LastExit {
    static final String FILE = "dhd-exit.txt";
    private static final String TAG = "DownhillRevived";
    /** A record this much older than the last dhd.log's last write is not about that run. */
    private static final long STALE_MS = 5000;

    /** When the run ended (System.currentTimeMillis), 0 when Android has no record. */
    final long time;
    /** The few words for the dialog ("It crashed."), null when the run ended as runs do. */
    final String why;
    /** dhd-exit.txt's text. */
    final String text;

    private LastExit(long time, String why, String text) {
        this.time = time;
        this.why = why;
        this.text = text;
    }

    private static LastExit known;
    private static boolean read;

    /** The run before this process, read and written to dhd-exit.txt on the first call in a process
     *  (any later one: the same answer; the game's own run is still going). null when there was none. */
    static synchronized LastExit record(Context context) {
        if (read) return known;
        read = true;
        File out = new File(context.getFilesDir(), FILE);
        try {
            known = find(context);
        } catch (Throwable e) {  // never in the way of the game's start
            Log.w(TAG, "no exit record: " + e);
            known = null;
        }
        try {
            if (known == null) {
                out.delete();
            } else {
                try (FileOutputStream f = new FileOutputStream(out)) {
                    f.write(known.text.getBytes(StandardCharsets.UTF_8));
                }
            }
        } catch (IOException e) {
            Log.w(TAG, FILE + " can't be written: " + e);
        }
        return known;
    }

    /** dhd-exit.txt's text as this process wrote it, null when there is none. */
    static String text(Context context) {
        LastExit e = record(context);
        return e == null ? null : e.text;
    }

    /** True the first time it is asked for this exit (the dialog shows once per exit). */
    static boolean firstAsk(Context context, LastExit exit) {
        SharedPreferences p = context.getSharedPreferences("crash-log", Context.MODE_PRIVATE);
        if (p.getLong("shown", 0) == exit.time) return false;
        p.edit().putLong("shown", exit.time).commit();
        return true;
    }

    private static LastExit find(Context context) throws IOException {
        File log = new File(context.getFilesDir(), "dhd.log");
        ActivityManager am = context.getSystemService(ActivityManager.class);
        List<ApplicationExitInfo> list = am == null ? null : am.getHistoricalProcessExitReasons(context.getPackageName(), 0, 1);
        if (list == null || list.isEmpty()) {
            return log.isFile() ? new LastExit(0, null, "how it ended: Android kept no record\n") : null;
        }
        ApplicationExitInfo e = list.get(0);
        if (log.isFile() && e.getTimestamp() < log.lastModified() - STALE_MS)
            return new LastExit(0, null, "how it ended: Android kept no record (its newest is from "
                    + date(e.getTimestamp()) + ", before that run's log)\n");
        String trace = null, kind = null;
        if (e.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
            trace = CrashReport.tombstone(readTrace(e, CrashReport.TOMBSTONE_MAX));
            kind = "tombstone";
        } else if (e.getReason() == ApplicationExitInfo.REASON_ANR) {
            trace = CrashReport.text(readTrace(e, CrashReport.TRACE_MAX));
            kind = "trace";
        }
        String why = CrashReport.why(e.getReason(), e.getStatus(), e.getImportance(), CrashReport.driverCrash(trace));
        StringBuilder s = new StringBuilder();
        s.append("how it ended: ").append(why == null ? "normally" : "closed unexpectedly, "
                + Character.toLowerCase(why.charAt(0)) + why.substring(1, why.length() - 1))
                .append(" (").append(date(e.getTimestamp())).append(")\n");
        s.append("system record: ").append(CrashReport.reasonName(e.getReason())).append(", status ").append(e.getStatus())
                .append(", importance ").append(e.getImportance())
                .append(e.getImportance() <= CrashReport.IMPORTANCE_VISIBLE ? " (on the screen)" : " (in the background)")
                .append(", process ").append(e.getPid()).append('\n');
        if (e.getDescription() != null && !e.getDescription().isEmpty())
            s.append("description: ").append(e.getDescription()).append('\n');
        s.append(String.format(Locale.US, "memory: %.0f MB used (PSS), %.0f MB resident\n", e.getPss() / 1024.0, e.getRss() / 1024.0));
        if (trace != null) {
            s.append(kind).append(":\n");
            for (String line : trace.split("\n")) s.append("  ").append(line).append('\n');
        }
        return new LastExit(e.getTimestamp(), why, s.toString());
    }

    /** The record's trace, at most max bytes, null when it has none (a native crash's only from Android 12). */
    private static byte[] readTrace(ApplicationExitInfo e, int max) {
        try (InputStream in = e.getTraceInputStream()) {
            if (in == null) return null;
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[64 << 10];
            for (int n; out.size() < max && (n = in.read(buf, 0, Math.min(buf.length, max - out.size()))) > 0;) out.write(buf, 0, n);
            return out.toByteArray();
        } catch (Throwable t) {
            Log.w(TAG, "the exit's trace can't be read: " + t);
            return null;
        }
    }

    static String date(long millis) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date(millis));
    }
}
