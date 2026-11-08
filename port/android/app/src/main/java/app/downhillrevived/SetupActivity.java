// Downhill Revived on Android: the first start (finding 143). The app's launcher activity: when the
// copy of the disc image is in the app's files folder it starts the game (DhdActivity) at once;
// otherwise it asks for the player's Downhill Domination (USA) disc image (Android's document
// picker), checks it before copying when the document is a file (an ISO 9660 volume whose
// SYSTEM.CNF boots SCUS_971.77, as host.c's disc_problem does) and that the phone has room for it,
// copies it with a progress bar, checks the copy (the same, and complete: DiscImage), offers to
// delete the original (the copy is the one the game reads), then starts the game.
// Everything that may block (opening the document, which a cloud provider may download first, its
// reads, the copy) runs on a copy job's thread (Copy); the UI thread only shows the screens. Each
// job writes its own temporary file and only it renames or deletes that file.
// android_main.c checks the copy again (and the game's data, in the core), and keeps its own picker
// for a start without this screen (a controller plugged in starts DhdActivity directly).
// Its look follows the launcher's (port/launcher/ui/style.css): the same colours, 8 dp corners.
// Before all that (finding 152): how the run before ended (LastExit, before any native code), and
// when it closed unexpectedly, once per such end, a dialog that offers its log (Share log, LogShare)
// before Play. The icon's long-press shortcut "Share log" (res/xml/shortcuts.xml, action SHARE_LOG)
// starts this activity only to share the log, without the game.
package app.downhillrevived;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.atomic.AtomicLong;

public class SetupActivity extends Activity {
    private static final String TAG = "DownhillRevived";
    /** The copy's name in the files folder (android_main.c's DISC_NAME). */
    private static final String DISC_NAME = "SCUS-97177.iso";
    private static final int PICK = 1, SHARE = 2;
    /** The shortcut's action: share the log, no game. */
    static final String SHARE_LOG = "app.downhillrevived.SHARE_LOG";
    /** Room kept free besides the copy (saves, dhd.log, the texture cache). */
    private static final long MARGIN = 128L << 20;
    /** The temporary files of the copy jobs running in this process (Copy.temp's names): a new
     *  screen's clean-up leaves them alone. Guarded by JOBS, as each job's last steps are. */
    private static final Object JOBS = new Object();
    private static final Set<String> running = new HashSet<>();
    private static final AtomicLong jobs = new AtomicLong();

    // the launcher's colours
    private static final int GROUND = 0xFF1A1430, RAISE = 0xFF241C3E, EMBER = 0xFFF07C3A, INK_ON_EMBER = 0xFF2A1206,
            SNOW = 0xFFF5EFF8, LILAC = 0xFFBCAED4, DIM = 0xFF9D8FB8, LINE = 0x29BCAED4, BAD = 0xFFFF6B6B;

    private LinearLayout card;
    private Uri chosen;
    private volatile boolean leaving;
    /** The copy job this screen shows, null before the first; only its posts reach the screen. */
    private volatile Copy job;

    /** Started by the shortcut: the share sheet only, then this activity ends. */
    private boolean sharingOnly;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        backdrop();
        if (SHARE_LOG.equals(getIntent().getAction())) {
            sharingOnly = true;
            LogShare.start(this, SHARE, this::finish);
            return;
        }
        // The exit record is read off the UI thread: a native crash's trace can be megabytes and
        // slow to come, and a frozen first screen would be Android's "not responding"
        Context app = getApplicationContext();
        new Thread(() -> {
            LastExit exit = LastExit.record(app);
            boolean ask = exit != null && exit.why != null && LastExit.firstAsk(app, exit);
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (ask) showClosedUnexpectedly(exit.why);
                else begin();
            });
        }, "last exit").start();
    }

    /** After a run that closed unexpectedly: what happened in a few words, Share log (the dialog
     *  stays for Play after the share sheet) and Play; Back plays too. */
    private void showClosedUnexpectedly(String why) {
        final AlertDialog dialog = new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
                .setTitle("Downhill Revived closed unexpectedly last time")
                .setMessage(why + "\n\nSharing the log helps get it fixed: it says what happened.")
                .setPositiveButton("Play", null)
                .setNegativeButton("Share log", null)
                .create();
        dialog.setCanceledOnTouchOutside(false);
        dialog.setOnCancelListener(d -> begin());
        dialog.setOnShowListener(d -> {
            dialog.getButton(Dialog.BUTTON_POSITIVE).setTextColor(EMBER);
            dialog.getButton(Dialog.BUTTON_NEGATIVE).setTextColor(LILAC);
            dialog.getButton(Dialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                dialog.dismiss();
                begin();
            });
            dialog.getButton(Dialog.BUTTON_NEGATIVE).setOnClickListener(v -> LogShare.start(this, SHARE, null));
        });
        dialog.show();
    }

    /** The first start's screens, or the game when the disc's copy is there. */
    private void begin() {
        if (new File(getFilesDir(), DISC_NAME).isFile()) {
            startGame();
            return;
        }
        removeStaleCopies();
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(GROUND);
        card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(32), dp(28), dp(32), dp(28));
        card.setBackground(rounded(RAISE, 0));
        FrameLayout centre = new FrameLayout(this);  // the card in the middle, scrolling on a small screen
        centre.setPadding(dp(16), dp(16), dp(16), dp(16));
        centre.addView(card, new FrameLayout.LayoutParams(dp(560), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(centre, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(scroll);
        setContentView(root);
        hideBars();
        showIntro(null);
    }

    @Override
    protected void onDestroy() {
        leaving = true;
        Copy copy = job;
        if (copy != null) copy.cancel();  // it stops and removes its own temporary file
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideBars();
    }

    // ---- the screens ----

    private void showIntro(String problem) {
        card.removeAllViews();
        title("Downhill Revived");
        text("To start, choose your Downhill Domination (USA) disc image: an .iso file of your own disc "
                + "(SCUS-97177).", LILAC, 16);
        if (problem != null) text(problem, BAD, 15);
        text("It is copied into the app once and needs about 2.5 GB of free space. Free now: "
                + gb(freeBytes()) + ".", DIM, 14);
        buttons(primary(problem == null ? "Choose disc image" : "Choose another file", new View.OnClickListener() {
            @Override public void onClick(View v) { pick(); }
        }), null);
    }

    private void showNoRoom(final long need, long free) {
        card.removeAllViews();
        title("Not enough space");
        text("This disc image needs " + gb(need) + " of free space, and the phone has " + gb(free) + ".", LILAC, 16);
        text("Free up some space (Settings > Storage, or remove apps, videos or downloads you don't need), "
                + "then tap Try again. Once the game has its copy, you can delete the original file.", DIM, 14);
        buttons(primary("Try again", new View.OnClickListener() {
            @Override public void onClick(View v) { use(chosen); }
        }), secondary("Choose another file", new View.OnClickListener() {
            @Override public void onClick(View v) { pick(); }
        }));
    }

    private TextView progressText;
    private ProgressBar progressBar;

    private void showChecking() {
        card.removeAllViews();
        progressBar = null;
        progressText = null;
        title("Checking the file\u2026");
        bar(true);
        text("A file on a cloud drive is downloaded first: this can take a few minutes.", DIM, 14);
    }

    private void showCopying(long size) {
        card.removeAllViews();
        title("Copying the disc image");
        bar(size <= 0);
        progressText = text("", LILAC, 15);
        text("Keep the app open: this takes a minute or two.", DIM, 14);
    }

    private void bar(boolean indeterminate) {
        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(1000);
        progressBar.setIndeterminate(indeterminate);
        progressBar.setProgressTintList(ColorStateList.valueOf(EMBER));
        progressBar.setProgressBackgroundTintList(ColorStateList.valueOf(LINE));
        progressBar.setIndeterminateTintList(ColorStateList.valueOf(EMBER));
        LinearLayout.LayoutParams bar = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(10));
        bar.topMargin = dp(18);
        card.addView(progressBar, bar);
    }

    /** After the copy is complete, checked and in place: offers to delete the original when its
     *  provider allows it (deletable, asked on the copy's thread). */
    private void showDone(final Uri original, long size, boolean deletable) {
        if (original == null || !deletable) {
            startGame();
            return;
        }
        card.removeAllViews();
        title("Ready to ride");
        text("The game has its own copy of the disc now. The file you chose still takes " + gb(size)
                + ": delete it to get that space back?", LILAC, 16);
        buttons(primary("Delete it and play", new View.OnClickListener() {
            @Override public void onClick(View v) {
                final ContentResolver resolver = getContentResolver();
                new Thread(new Runnable() {  // the provider's work, off the UI thread; the game starts meanwhile
                    @Override public void run() {
                        try {
                            DocumentsContract.deleteDocument(resolver, original);
                        } catch (Throwable e) {
                            Log.w(TAG, "the original couldn't be deleted: " + e);
                        }
                    }
                }, "disc delete").start();
                startGame();
            }
        }), secondary("Keep it and play", new View.OnClickListener() {
            @Override public void onClick(View v) { startGame(); }
        }));
    }

    // ---- picking, checking, copying ----

    private void pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");  // .iso files have no MIME type everywhere
        try {
            startActivityForResult(intent, PICK);
        } catch (Throwable e) {
            showIntro("This phone has no file picker (" + e.getMessage() + ").");
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == SHARE) {  // back from the share sheet: the shortcut's start ends, the dialog stays
            if (sharingOnly) finish();
            return;
        }
        if (request != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        chosen = data.getData();
        Log.i(TAG, "disc image chosen: " + chosen);
        use(chosen);
    }

    /** Checks the chosen file and the room for it, then copies it: all on a new copy job's thread
     *  (the one before, if any, is cancelled). */
    private void use(final Uri uri) {
        Copy before = job;
        if (before != null) before.cancel();
        Copy copy = new Copy(uri);
        job = copy;
        showChecking();
        new Thread(copy, "disc copy").start();
    }

    /** Deletes the temporary files of copies cut short (the app closed or killed during one, the
     *  name of an older version's, SCUS-97177.iso.part, too), but none a running job writes. */
    private void removeStaleCopies() {
        File[] files = getFilesDir().listFiles();
        if (files == null) return;
        synchronized (JOBS) {
            for (File f : files) {
                String name = f.getName();
                if (name.startsWith(DISC_NAME + ".") && name.endsWith(".part") && !running.contains(name)) f.delete();
            }
        }
    }

    /** One pick's check and copy, on its own thread. It owns temp (a name no other job has) and is
     *  the only one to rename or delete it; once cancelled (the screen went away, or another pick
     *  replaced it) it neither publishes the copy nor shows anything, and removes temp. */
    private final class Copy implements Runnable {
        final Uri uri;
        final File temp;
        private volatile boolean cancelled;

        Copy(Uri uri) {
            this.uri = uri;
            temp = new File(getFilesDir(), DISC_NAME + "." + Long.toHexString(System.currentTimeMillis()) + "-"
                    + jobs.incrementAndGet() + ".part");
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
                work();
            } catch (Throwable e) {
                Log.w(TAG, "disc copy: " + e);
                showProblem("The copy failed (" + e.getMessage() + ").");
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
                fd = getContentResolver().openFileDescriptor(uri, "r");
                if (fd == null) throw new IOException("the provider gave no file");
            } catch (Throwable e) {
                showProblem("The file can't be opened (" + e.getMessage() + ").");
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
                        showProblem(problem);
                        return;
                    }
                }
                if (cancelled) return;
                final long free = freeBytes();
                if (size > 0 && size + MARGIN > free) {
                    final long need = size + MARGIN;
                    post(new Runnable() {
                        @Override public void run() { showNoRoom(need, free); }
                    });
                    return;
                }
                final long total = size;
                post(new Runnable() {
                    @Override public void run() { showCopying(total); }
                });
                long done;
                try (FileOutputStream out = new FileOutputStream(temp)) {
                    done = DiscImage.copy(in, out, () -> cancelled, soFar -> progress(Copy.this, soFar, total));
                    out.getFD().sync();
                } catch (Throwable e) {
                    if (!cancelled)
                        showProblem(freeBytes() < (64L << 20) ? "The phone ran out of space while copying."
                                : "The copy failed (" + e.getMessage() + ").");
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
                    showProblem(problem);
                    return;
                }
                File disc = new File(getFilesDir(), DISC_NAME);
                synchronized (JOBS) {  // cancel() can't slip in between this check and the rename
                    if (cancelled) return;
                    if (!disc.isFile() && !temp.renameTo(disc)) problem = "The copy can't be stored.";
                }
                if (problem != null) {
                    showProblem(problem);
                    return;
                }
                Log.i(TAG, "disc image copied: " + done + " bytes, checked");
                final boolean deletable = deletable(uri);
                final long copied = done;
                post(new Runnable() {
                    @Override public void run() { showDone(uri, copied, deletable); }
                });
            }
        }

        /** The document's size as its provider gives it (OpenableColumns.SIZE), 0 when it doesn't. */
        private long documentSize() {
            try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.SIZE }, null, null, null)) {
                return c != null && c.moveToFirst() && !c.isNull(0) ? Math.max(0, c.getLong(0)) : 0;
            } catch (Throwable e) {
                return 0;
            }
        }

        private void showProblem(final String problem) {
            post(new Runnable() {
                @Override public void run() { showIntro(problem); }
            });
        }

        /** screen on the UI thread, if this job is still the one shown. */
        private void post(final Runnable screen) {
            runOnUiThread(new Runnable() {
                @Override public void run() {
                    if (job == Copy.this && !cancelled && !leaving) screen.run();
                }
            });
        }
    }

    private void progress(final Copy copy, final long done, final long size) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (job != copy || leaving || progressBar == null || progressText == null) return;
                if (size > 0) {
                    progressBar.setProgress((int) (Math.min(done, size) * 1000 / size));
                    progressText.setText(gb(done) + " of " + gb(size) + "  \u00B7  " + (Math.min(done, size) * 100 / size) + "%");
                } else {
                    progressText.setText(gb(done));
                }
            }
        });
    }

    private boolean deletable(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { DocumentsContract.Document.COLUMN_FLAGS }, null, null, null)) {
            return c != null && c.moveToFirst() && (c.getInt(0) & DocumentsContract.Document.FLAG_SUPPORTS_DELETE) != 0;
        } catch (Throwable e) {
            return false;
        }
    }

    private long freeBytes() {
        return new StatFs(getFilesDir().getPath()).getAvailableBytes();
    }

    private void startGame() {
        startActivity(new Intent(this, DhdActivity.class));
        finish();
        overridePendingTransition(0, 0);
    }

    // ---- the look ----

    private TextView title(String s) {
        TextView t = text(s, SNOW, 26);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        ((LinearLayout.LayoutParams) t.getLayoutParams()).topMargin = 0;
        return t;
    }

    private TextView text(String s, int colour, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(colour);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setLineSpacing(0, 1.15f);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = dp(12);
        card.addView(t, p);
        return t;
    }

    private Button primary(String label, View.OnClickListener click) {
        Button b = button(label, click);
        b.setTextColor(INK_ON_EMBER);
        b.setBackground(rounded(EMBER, 0));
        return b;
    }

    private Button secondary(String label, View.OnClickListener click) {
        Button b = button(label, click);
        b.setTextColor(LILAC);
        b.setBackground(rounded(Color.TRANSPARENT, LINE));
        return b;
    }

    private Button button(String label, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        b.setStateListAnimator(null);
        b.setPadding(dp(22), 0, dp(22), 0);
        b.setMinHeight(dp(48));
        b.setOnClickListener(click);
        return b;
    }

    private void buttons(Button first, Button second) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.END);
        if (second != null) {
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(48));
            p.rightMargin = dp(12);
            row.addView(second, p);
        }
        row.addView(first, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(48)));
        LinearLayout.LayoutParams where = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        where.topMargin = dp(24);
        card.addView(row, where);
        first.requestFocus();  // a controller's buttons work here too
    }

    private GradientDrawable rounded(int fill, int border) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(8));
        if (border != 0) d.setStroke(dp(1), border);
        return d;
    }

    /** A plain screen behind the dialog or the share sheet. It also gives the window its decor view,
     *  which getInsetsController needs: before any setContentView it is null and hideBars throws
     *  (Android 13, Mi 11i; finding 152). */
    private void backdrop() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(GROUND);
        setContentView(root);
        hideBars();
    }

    private void hideBars() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(WindowInsets.Type.systemBars());
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    private static String gb(long bytes) {
        return String.format(Locale.US, "%.1f\u00A0GB", bytes / (double) (1L << 30));  // no line break inside
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
