// Downhill Revived on Android: the first start (finding 143). The app's launcher activity: when the
// copy of the disc image is in the app's files folder it starts the game (DhdActivity) at once;
// otherwise it asks for the player's Downhill Domination (USA) disc image (Android's document
// picker), checks it before copying (an ISO 9660 volume whose SYSTEM.CNF boots SCUS_971.77, as
// host.c's disc_problem does) and that the phone has room for it, copies it with a progress bar,
// offers to delete the original (the copy is the one the game reads), then starts the game.
// android_main.c checks the copy again (and the game's data, in the core), and keeps its own picker
// for a start without this screen (a controller plugged in starts DhdActivity directly).
// Its look follows the launcher's (port/launcher/ui/style.css): the same colours, 8 dp corners.
package app.downhillrevived;

import android.app.Activity;
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
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

public class SetupActivity extends Activity {
    private static final String TAG = "DownhillRevived";
    /** The copy's name in the files folder (android_main.c's DISC_NAME). */
    private static final String DISC_NAME = "SCUS-97177.iso";
    private static final int PICK = 1;
    /** Room kept free besides the copy (saves, dhd.log, the texture cache). */
    private static final long MARGIN = 128L << 20;
    private static final int SECTOR = 2048;

    // the launcher's colours
    private static final int GROUND = 0xFF1A1430, RAISE = 0xFF241C3E, EMBER = 0xFFF07C3A, INK_ON_EMBER = 0xFF2A1206,
            SNOW = 0xFFF5EFF8, LILAC = 0xFFBCAED4, DIM = 0xFF9D8FB8, LINE = 0x29BCAED4, BAD = 0xFFFF6B6B;

    private LinearLayout card;
    private Uri chosen;
    private volatile boolean leaving;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (new File(getFilesDir(), DISC_NAME).isFile()) {
            startGame();
            return;
        }
        new File(getFilesDir(), DISC_NAME + ".part").delete();  // a copy cut short
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
        leaving = true;  // a copy in progress stops and removes its .part
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

    private void showCopying(long size) {
        card.removeAllViews();
        title("Copying the disc image");
        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(1000);
        progressBar.setIndeterminate(size <= 0);
        progressBar.setProgressTintList(ColorStateList.valueOf(EMBER));
        progressBar.setProgressBackgroundTintList(ColorStateList.valueOf(LINE));
        progressBar.setIndeterminateTintList(ColorStateList.valueOf(EMBER));
        LinearLayout.LayoutParams bar = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(10));
        bar.topMargin = dp(18);
        card.addView(progressBar, bar);
        progressText = text("", LILAC, 15);
        text("Keep the app open: this takes a minute or two.", DIM, 14);
    }

    private void showDone(final Uri original, long size) {
        if (original == null || !deletable(original)) {
            startGame();
            return;
        }
        card.removeAllViews();
        title("Ready to ride");
        text("The game has its own copy of the disc now. The file you chose still takes " + gb(size)
                + ": delete it to get that space back?", LILAC, 16);
        buttons(primary("Delete it and play", new View.OnClickListener() {
            @Override public void onClick(View v) {
                try {
                    DocumentsContract.deleteDocument(getContentResolver(), original);
                } catch (Throwable e) {
                    Log.w(TAG, "the original couldn't be deleted: " + e);
                }
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
        if (request != PICK || result != RESULT_OK || data == null || data.getData() == null) return;
        chosen = data.getData();
        Log.i(TAG, "disc image chosen: " + chosen);
        use(chosen);
    }

    /** Checks the chosen file and the room for it, then copies it. */
    private void use(final Uri uri) {
        long size;
        String problem;
        try (ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r")) {
            size = fd.getStatSize();
            problem = checkDisc(new FileInputStream(fd.getFileDescriptor()).getChannel());
        } catch (Throwable e) {
            showIntro("The file can't be opened (" + e.getMessage() + ").");
            return;
        }
        if (problem != null) {
            showIntro(problem);
            return;
        }
        long free = freeBytes();
        if (size > 0 && size + MARGIN > free) {
            showNoRoom(size + MARGIN, free);
            return;
        }
        copy(uri, size);
    }

    private void copy(final Uri uri, final long size) {
        showCopying(size);
        new Thread(new Runnable() {
            @Override public void run() {
                File part = new File(getFilesDir(), DISC_NAME + ".part"), disc = new File(getFilesDir(), DISC_NAME);
                String problem = null;
                long done = 0, shown = 0;
                try (InputStream in = getContentResolver().openInputStream(uri); FileOutputStream out = new FileOutputStream(part)) {
                    byte[] block = new byte[1 << 20];
                    for (int n; (n = in.read(block)) > 0; ) {
                        if (leaving) throw new IOException("the app was closed");
                        out.write(block, 0, n);
                        done += n;
                        if (done - shown >= (16L << 20)) progress(shown = done, size);
                    }
                    out.getFD().sync();
                } catch (Throwable e) {
                    problem = freeBytes() < (64L << 20) ? "The phone ran out of space while copying." : "The copy failed (" + e.getMessage() + ").";
                }
                if (problem == null) {  // the copy itself, checked again (a file with no size given ends here)
                    try (RandomAccessFile f = new RandomAccessFile(part, "r")) {
                        problem = checkDisc(f.getChannel());
                    } catch (Throwable e) {
                        problem = "The copy can't be read (" + e.getMessage() + ").";
                    }
                }
                if (problem == null && !part.renameTo(disc)) problem = "The copy can't be stored.";
                if (problem != null) part.delete();
                final String why = problem;
                final long total = done;
                if (leaving) return;
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        if (why != null) showIntro(why);
                        else showDone(uri, total);
                    }
                });
            }
        }, "disc copy").start();
    }

    private void progress(final long done, final long size) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (progressBar == null) return;
                if (size > 0) {
                    progressBar.setProgress((int) (done * 1000 / size));
                    progressText.setText(gb(done) + " of " + gb(size) + "  \u00B7  " + (done * 100 / size) + "%");
                } else {
                    progressText.setText(gb(done));
                }
            }
        });
    }

    /** Whether file is Downhill Domination (USA): null if it is, else why not (host.c's
     *  disc_problem without the game-data check, which the core makes). */
    static String checkDisc(FileChannel file) throws IOException {
        ByteBuffer sector = ByteBuffer.allocate(SECTOR).order(ByteOrder.LITTLE_ENDIAN);
        if (!read(file, 16, sector) || sector.get(0) != 1 || !ascii(sector, 1, 5).equals("CD001"))
            return "That file isn't a disc image (no ISO 9660 volume in it).";
        // the primary volume descriptor's root directory record (at 156): extent at +2, size at +10
        long dir = sector.getInt(156 + 2) & 0xFFFFFFFFL, dirSize = sector.getInt(156 + 10) & 0xFFFFFFFFL;
        long cnf = 0, cnfSize = 0;
        for (long off = 0; off < dirSize && off < 16 * SECTOR && cnf == 0; off += SECTOR) {
            if (!read(file, dir + off / SECTOR, sector)) break;
            for (int i = 0; i + 33 < SECTOR && sector.get(i) != 0; i += sector.get(i) & 0xFF) {
                int nameLength = sector.get(i + 32) & 0xFF;
                if (nameLength >= 10 && i + 33 + nameLength <= SECTOR
                        && ascii(sector, i + 33, 10).equalsIgnoreCase("SYSTEM.CNF")
                        && (nameLength == 10 || sector.get(i + 33 + 10) == ';')) {
                    cnf = sector.getInt(i + 2) & 0xFFFFFFFFL;
                    cnfSize = sector.getInt(i + 10) & 0xFFFFFFFFL;
                    break;
                }
            }
        }
        if (cnf == 0 || !read(file, cnf, sector)) return "That file has no SYSTEM.CNF, so it isn't a PlayStation 2 game disc.";
        String text = ascii(sector, 0, (int) Math.min(cnfSize, SECTOR));
        if (text.contains("SCUS_971.77")) return null;
        int boot = text.indexOf("BOOT2");
        String id = "";
        if (boot >= 0) {
            String line = text.substring(boot).split("[\r\n]")[0];
            int start = Math.max(line.lastIndexOf('\\'), line.lastIndexOf(':')) + 1, end = line.indexOf(';', start);
            id = line.substring(start, end > start ? end : line.length()).trim();
        }
        return id.isEmpty() ? "That PlayStation 2 disc isn't Downhill Domination (USA), SCUS-97177."
                : "That disc is " + id + ", not Downhill Domination (USA), SCUS-97177.";
    }

    private static boolean read(FileChannel file, long lsn, ByteBuffer sector) throws IOException {
        sector.clear();
        long at = lsn * SECTOR;
        while (sector.hasRemaining()) {
            int n = file.read(sector, at + sector.position());
            if (n <= 0) return false;
        }
        return true;
    }

    private static String ascii(ByteBuffer b, int at, int length) {
        byte[] bytes = new byte[length];
        for (int i = 0; i < length; i++) bytes[i] = b.get(at + i);
        return new String(bytes, StandardCharsets.ISO_8859_1);
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
