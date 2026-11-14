// Downhill Revived on Android: the rules for the disc image (findings 143, 225, 234, 242), apart from
// the launcher's import (DiscImport) so a plain JVM test runs them (port/android/test/DiscImageTest.java):
// whether a file is Downhill Domination (USA) (port/host/disc_check.c), whether a copy of it is
// complete, how much room an import needs and which import may land (port/host/disc_import.c, the
// iOS launcher's too), all through the natives of port/host/disc_check_jni.c; and the copy loop
// over the document's stream, the one part that is Android's own I/O. The words a player reads for
// a refused disc are Android's (check). Nothing here uses Android.
package app.downhillrevived;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.function.BooleanSupplier;
import java.util.function.LongConsumer;

final class DiscImage {
    /** The copy loop's block, its progress step, the room kept free besides the copy, and the free
     *  space under which a failed copy is "out of space": disc_import.h's. */
    static final int BLOCK;
    static final long PROGRESS_STEP, MARGIN, LOW;

    static {
        Loader.load();
        long[] r = rules();
        BLOCK = (int) r[0];
        PROGRESS_STEP = r[1];
        MARGIN = r[2];
        LOW = r[3];
    }

    private DiscImage() {}

    /** check's answer for the right disc in a file shorter than its volume (DISC_CUT). */
    static final String CUT = "The disc image is incomplete: it does not hold its whole declared volume.";
    /** disc_check.h's kinds. */
    private static final int GAME = 0, NOT_ISO = 1, NO_CNF = 2, CUT_KIND = 4;

    /** What disc_check_image says of a file: its kind, another game's boot file, the declared volume. */
    private static final class Answer {
        final int kind;
        final String id;
        final long volume;

        Answer(String text) {
            String[] a = text.split("\n", 3);
            kind = Integer.parseInt(a[0]);
            id = a.length > 1 ? a[1] : "";
            volume = a.length > 2 ? Long.parseLong(a[2]) : 0;
        }
    }

    private static Answer answer(FileChannel file) throws IOException {
        return new Answer(checkNative(file, file.size()));
    }

    /** Whether file is Downhill Domination (USA): null if it is, else why not (host.c's
     *  disc_problem without the game-data check, which the core makes). Positional reads only:
     *  file must be seekable (a local copy, or a document that is a regular file). */
    static String check(FileChannel file) throws IOException {
        return words(answer(file));
    }

    private static String words(Answer a) {
        switch (a.kind) {
            case GAME: return null;
            case NOT_ISO: return "That file isn't a disc image (no ISO 9660 volume in it).";
            case NO_CNF: return "That file has no SYSTEM.CNF, so it isn't a PlayStation 2 game disc.";
            case CUT_KIND: return CUT;
            default:
                return a.id.isEmpty() ? "That PlayStation 2 disc isn't Downhill Domination (USA), SCUS-97177."
                        : "That disc is " + a.id + ", not Downhill Domination (USA), SCUS-97177.";
        }
    }

    /** A finished copy's verdict: null when it is the game and complete, else why not. A copy cut
     *  short says why (the copy stopped: try again) before the image's own word (finding 234).
     *  copied: the bytes the copy loop wrote; size: the source's size when known (> 0), exact when
     *  it came from the file itself (stat; a document's SIZE column is the provider's word, so a
     *  copy may run past it but never stop short of it). */
    static String checkCopy(FileChannel copy, long copied, long size, boolean exact) throws IOException {
        Answer a = answer(copy);
        String whole = verdictNative(a.kind, copied, size, exact, a.volume);
        return whole != null ? whole : words(a);
    }

    /** Whether a copy of the game is complete (disc_completeness): null if it is, else why not. */
    static String completeness(long copied, long size, boolean exact, long volume) {
        return verdictNative(GAME, copied, size, exact, volume);
    }

    /** The bytes the ISO 9660 volume declares, 0 when there is no primary volume descriptor. */
    static long volumeBytes(FileChannel file) throws IOException {
        return answer(file).volume;
    }

    /** 0 when free bytes hold a copy of a source of size bytes (0: unknown) and the margin, else
     *  the bytes needed. */
    static native long roomNeeded(long size, long free);
    /** A pick's generation: every older import is superseded from here (disc_publish_begin). */
    static native long publishBegin();
    /** from renamed over to while generation is the newest: 0 done, 1 superseded, -1 failed. */
    static native int publish(long generation, String from, String to);

    private static final class Loader {
        static { System.loadLibrary("main"); }
        static void load() {}
    }
    private static native String checkNative(FileChannel file, long size) throws IOException;
    private static native long[] rules();
    private static native String verdictNative(int kind, long copied, long size, boolean exact, long volume);

    /** JNI's reader: positional reads keep a document provider's copy stream at zero. */
    private static boolean readAt(FileChannel file, long offset, ByteBuffer bytes) throws IOException {
        while (bytes.hasRemaining()) {
            if (file.read(bytes, offset + bytes.position()) <= 0) return false;
        }
        return true;
    }

    /** Copies in to out until in ends; cancelled is asked before every block is written (the copy
     *  then ends with an IOException), progress gets the bytes so far every PROGRESS_STEP.
     *  Returns the bytes copied. */
    static long copy(InputStream in, OutputStream out, BooleanSupplier cancelled, LongConsumer progress) throws IOException {
        byte[] block = new byte[BLOCK];
        long done = 0, shown = 0;
        for (int n; (n = in.read(block)) > 0; ) {  // a short end is caught by checkCopy
            if (cancelled.getAsBoolean()) throw new IOException("the copy was cancelled");
            out.write(block, 0, n);
            done += n;
            if (done - shown >= PROGRESS_STEP) progress.accept(shown = done);
        }
        return done;
    }
}
