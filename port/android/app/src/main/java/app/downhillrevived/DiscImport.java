// Downhill Revived on Android: the disc image's import (findings 143, 225). The launcher's page asks
// for the player's Downhill Domination (USA) disc image; the document picker gives a content URI,
// and a job here checks it before copying when the document is a file (an ISO 9660 volume whose
// SYSTEM.CNF boots SCUS_971.77, as host.c's disc_problem does: DiscImage), checks that the device
// has room for it, copies it into the app's files folder with progress, checks the copy (the same,
// and complete) and only then puts it in place as SCUS-97177.iso. Its steps reach the page as
// "disc-import" events (ui/disc_flow.js): checking, copying {done, total}, done {deletable, size} or
// problem {problem}.
// Everything that may block (opening the document, which a cloud provider may download first, its
// reads, the copy) runs on the job's own thread. Each job writes its own temporary file and only it
// renames or deletes that file; a newer job cancels the one before.
package app.downhillrevived;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;

final class DiscImport {
    private static final String TAG = "DownhillRevived";
    /** The copy's name in the files folder (android_main.c's DISC_NAME). */
    static final String DISC_NAME = "SCUS-97177.iso";
    /** Room kept free besides the copy (saves, dhd.log, the texture cache). */
    static final long MARGIN = 128L << 20;
    /** The temporary files of the jobs running in this process: a clean-up leaves them alone.
     *  Guarded by JOBS, as each job's last steps are. */
    private static final Object JOBS = new Object();
    private static final Set<String> running = new HashSet<>();
    private static final AtomicInteger numbers = new AtomicInteger();

    /** What a job tells the page; called on the job's thread. */
    interface Events {
        void send(JSONObject event);
    }

    private DiscImport() {}

    static File disc(Context context) {
        return new File(context.getFilesDir(), DISC_NAME);
    }

    static long freeBytes(Context context) {
        return new StatFs(context.getFilesDir().getPath()).getAvailableBytes();
    }

    /** Deletes the temporary files of copies cut short (the app closed or killed during one, the
     *  name of an older version's, SCUS-97177.iso.part, too), but none a running job writes. */
    static void removeStaleCopies(Context context) {
        File[] files = context.getFilesDir().listFiles();
        if (files == null) return;
        synchronized (JOBS) {
            for (File f : files) {
                String name = f.getName();
                if (name.startsWith(DISC_NAME + ".") && name.endsWith(".part") && !running.contains(name)) f.delete();
            }
        }
    }

    /** One pick's check and copy, on its own thread. It owns temp (a name no other job has) and is
     *  the only one to rename or delete it; once cancelled (the launcher went away, or another pick
     *  replaced it) it neither publishes the copy nor sends anything, and removes temp. */
    static final class Job implements Runnable {
        final int number = numbers.incrementAndGet();
        final Uri uri;
        private final Context context;
        private final Events events;
        private final File temp;
        private volatile boolean cancelled;

        Job(Context context, Uri uri, Events events) {
            this.context = context.getApplicationContext();
            this.uri = uri;
            this.events = events;
            temp = new File(this.context.getFilesDir(), DISC_NAME + "." + Long.toHexString(System.currentTimeMillis()) + "-"
                    + number + ".part");
        }

        void start() {
            new Thread(this, "disc copy").start();
        }

        /** After this returns the job can't publish (publish checks under the same lock). */
        void cancel() {
            synchronized (JOBS) {
                cancelled = true;
            }
        }

        @Override public void run() {
            synchronized (JOBS) {
                running.add(temp.getName());
            }
            try {
                send("checking", null);
                work();
            } catch (Throwable e) {
                Log.w(TAG, "disc copy: " + e);
                problem(coded("disc.problem.copyFailed", "detail", String.valueOf(e.getMessage())));
            } finally {
                temp.delete();  // its own file only; gone already once renamed
                synchronized (JOBS) {
                    running.remove(temp.getName());
                }
            }
        }

        private void work() throws IOException {
            ParcelFileDescriptor fd;
            try {
                fd = context.getContentResolver().openFileDescriptor(uri, "r");
                if (fd == null) throw new IOException("the provider gave no file");
            } catch (Throwable e) {
                problem(coded("disc.problem.openFailed", "detail", String.valueOf(e.getMessage())));
                return;
            }
            try (ParcelFileDescriptor.AutoCloseInputStream in = new ParcelFileDescriptor.AutoCloseInputStream(fd)) {
                // a regular file has its size and can be read anywhere; a pipe or socket (a provider
                // streaming the document) has no size here and reads once, in order: its copy is
                // checked instead, with the size the provider gives (OpenableColumns.SIZE) if any
                long stat = fd.getStatSize();
                boolean exact = stat > 0;
                long size = exact ? stat : documentSize();
                if (exact) {
                    String problem = null;
                    try {
                        problem = DiscImage.check(in.getChannel());  // positional reads: the stream stays at 0
                    } catch (IOException e) {
                        Log.i(TAG, "the file can't be checked before copying (" + e + "); its copy is checked");
                    }
                    if (problem != null) {
                        problem(problem);
                        return;
                    }
                }
                if (cancelled) return;
                long free = freeBytes(context);
                if (size > 0 && size + MARGIN > free) {
                    problem(coded("disc.problem.noRoom", "need", gb(size + MARGIN), "free", gb(free)));
                    return;
                }
                final long total = size;
                progress(0, total);
                long done;
                try (FileOutputStream out = new FileOutputStream(temp)) {
                    done = DiscImage.copy(in, out, () -> cancelled, soFar -> progress(soFar, total));
                    out.getFD().sync();
                } catch (Throwable e) {
                    if (!cancelled)
                        problem(freeBytes(context) < (64L << 20) ? coded("disc.problem.outOfSpace")
                                : coded("disc.problem.copyFailed", "detail", String.valueOf(e.getMessage())));
                    return;
                }
                if (cancelled) return;
                String problem;  // the copy itself: the same disc, and complete
                try (RandomAccessFile f = new RandomAccessFile(temp, "r")) {
                    problem = DiscImage.check(f.getChannel());
                    if (problem == null) problem = DiscImage.completeness(done, size, exact, DiscImage.volumeBytes(f.getChannel()));
                } catch (Throwable e) {
                    problem = "The copy can't be read (" + e.getMessage() + ").";
                }
                if (problem != null) {
                    Log.w(TAG, "disc copy: " + problem + " (" + done + " bytes, size " + size + (exact ? "" : " from the provider") + ")");
                    problem(problem);
                    return;
                }
                File disc = disc(context);
                synchronized (JOBS) {  // cancel() can't slip in between this check and the rename
                    if (cancelled) return;
                    // a replaced disc: rename(2) puts the new copy over the old one in one step, so
                    // there is always a whole copy in place
                    if (!temp.renameTo(disc)) problem = "The copy can't be stored.";
                }
                if (problem != null) {
                    problem(problem);
                    return;
                }
                Log.i(TAG, "disc image copied: " + done + " bytes, checked");
                new File(context.getFilesDir(), "iso-check.txt").delete();  // the online check's answer was the old file's
                JSONObject e = event("done");
                put(e, "deletable", deletable());
                put(e, "size", done);
                events.send(e);
            }
        }

        /** Deletes the document chosen (the copy is the one the game reads), when its provider allows. */
        boolean deleteOriginal() {
            try {
                return DocumentsContract.deleteDocument(context.getContentResolver(), uri);
            } catch (Throwable e) {
                Log.w(TAG, "the original couldn't be deleted: " + e);
                return false;
            }
        }

        /** The document's size as its provider gives it (OpenableColumns.SIZE), 0 when it doesn't. */
        private long documentSize() {
            try (Cursor c = context.getContentResolver().query(uri, new String[] { OpenableColumns.SIZE }, null, null, null)) {
                return c != null && c.moveToFirst() && !c.isNull(0) ? Math.max(0, c.getLong(0)) : 0;
            } catch (Throwable e) {
                return 0;
            }
        }

        private boolean deletable() {
            ContentResolver resolver = context.getContentResolver();
            try (Cursor c = resolver.query(uri, new String[] { DocumentsContract.Document.COLUMN_FLAGS }, null, null, null)) {
                return c != null && c.moveToFirst() && (c.getInt(0) & DocumentsContract.Document.FLAG_SUPPORTS_DELETE) != 0;
            } catch (Throwable e) {
                return false;
            }
        }

        private JSONObject event(String stage) {
            JSONObject e = new JSONObject();
            put(e, "job", number);
            put(e, "stage", stage);
            return e;
        }

        private void send(String stage, JSONObject extra) {
            if (cancelled) return;
            events.send(extra != null ? extra : event(stage));
        }

        private void progress(long done, long total) {
            if (cancelled) return;
            JSONObject e = event("copying");
            put(e, "done", done);
            put(e, "total", total);
            events.send(e);
        }

        /** problem: the player's words (DiscImage's), or a coded one (coded). */
        private void problem(Object problem) {
            if (cancelled) return;
            JSONObject e = event("problem");
            put(e, "problem", problem);
            events.send(e);
        }
    }

    /** A problem the page words itself: its string table key and the values for its placeholders. */
    static JSONObject coded(String code, String... params) {
        JSONObject o = new JSONObject(), p = new JSONObject();
        put(o, "code", code);
        for (int i = 0; i + 1 < params.length; i += 2) put(p, params[i], params[i + 1]);
        put(o, "params", p);
        return o;
    }

    static void put(JSONObject o, String key, Object value) {
        try {
            o.put(key, value);
        } catch (JSONException e) {
            throw new IllegalArgumentException(e);
        }
    }

    static String gb(long bytes) {
        return String.format(Locale.US, "%.1f GB", bytes / (double) (1L << 30));  // no line break inside
    }
}
