// Downhill Revived on Android: the first start's rules for the disc image (finding 143), apart from
// SetupActivity so a plain JVM test runs them (port/android/test/DiscImageTest.java): whether a file
// is Downhill Domination (USA), whether a copy of it is complete, and the copy loop itself. Nothing
// here uses Android.
package app.downhillrevived;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.function.BooleanSupplier;
import java.util.function.LongConsumer;

final class DiscImage {
    static final int SECTOR = 2048;
    /** The copy loop's block; progress is reported every PROGRESS_STEP bytes. */
    static final int BLOCK = 1 << 20;
    static final long PROGRESS_STEP = 16L << 20;

    private DiscImage() {}

    /** Whether file is Downhill Domination (USA): null if it is, else why not (host.c's
     *  disc_problem without the game-data check, which the core makes). Positional reads only:
     *  file must be seekable (a local copy, or a document that is a regular file). */
    static String check(FileChannel file) throws IOException {
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

    /** The bytes the ISO 9660 volume declares: the primary volume descriptor's volume space size
     *  (blocks, at 80) times its logical block size (at 128); 0 when there is no such descriptor.
     *  A dump of the disc is at least this long (the retail disc's is exactly this long). */
    static long volumeBytes(FileChannel file) throws IOException {
        ByteBuffer sector = ByteBuffer.allocate(SECTOR).order(ByteOrder.LITTLE_ENDIAN);
        if (!read(file, 16, sector) || sector.get(0) != 1 || !ascii(sector, 1, 5).equals("CD001")) return 0;
        long blocks = sector.getInt(80) & 0xFFFFFFFFL;
        int blockSize = sector.getShort(128) & 0xFFFF;
        return blocks * blockSize;
    }

    /** Whether a finished copy is complete: null if it is, else why not.
     *  copied: the bytes the copy loop wrote; size: the source's size when known (> 0), exact when
     *  it came from the file itself (stat; a document's SIZE column is the provider's word, so a
     *  copy may run past it but never stop short of it); volume: the copy's volumeBytes. The
     *  copy must hold the whole volume in every case, so a source that ends early never passes,
     *  whether or not its size was known. */
    static String completeness(long copied, long size, boolean exact, long volume) {
        if (size > 0 && copied < size)
            return "The copy stopped early: " + mb(copied) + " of " + mb(size) + " arrived. Try again.";
        if (size > 0 && exact && copied != size)
            return "The copy doesn't match the file: " + mb(copied) + " copied, the file has " + mb(size) + ". Try again.";
        if (volume <= 0)
            return "The copy has no disc volume size, so it can't be checked as complete.";
        if (copied < volume)
            return "The copy is incomplete: the disc image declares " + mb(volume) + ", and only " + mb(copied)
                    + " arrived. Try again, or copy the file onto the phone first.";
        return null;
    }

    /** Copies in to out until in ends; cancelled is asked before every block is written (the copy
     *  then ends with an IOException), progress gets the bytes so far every PROGRESS_STEP.
     *  Returns the bytes copied. */
    static long copy(InputStream in, OutputStream out, BooleanSupplier cancelled, LongConsumer progress) throws IOException {
        byte[] block = new byte[BLOCK];
        long done = 0, shown = 0;
        for (int n; (n = in.read(block)) > 0; ) {  // a short end is caught by completeness
            if (cancelled.getAsBoolean()) throw new IOException("the copy was cancelled");
            out.write(block, 0, n);
            done += n;
            if (done - shown >= PROGRESS_STEP) progress.accept(shown = done);
        }
        return done;
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

    private static String mb(long bytes) {
        return String.format(Locale.US, "%.1f MB", bytes / (double) (1L << 20));
    }
}
