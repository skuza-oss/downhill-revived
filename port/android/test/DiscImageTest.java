// Tests of DiscImage (the first start's disc rules, finding 143) on a plain JVM, with synthetic
// ISO 9660 images (no game data):
//
//     python port/android/test_disc_image.py [your.iso]
//
// which compiles DiscImage.java and this file with the JDK's javac (JAVA_HOME) into build/ and
// runs it. Given an .iso (or DHD_TEST_ISO), it also checks the real disc image: the right disc,
// and its declared volume no longer than the file.
package app.downhillrevived;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public class DiscImageTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            System.out.println("FAIL: " + what);
            failures++;
        }
    }

    /** An ISO 9660 image of fileBlocks sectors whose primary volume descriptor declares
     *  volumeBlocks, with SYSTEM.CNF (cnf) in the root directory; pvd false: no descriptor. */
    static byte[] image(int volumeBlocks, int fileBlocks, String cnf, boolean pvd) {
        int sector = DiscImage.SECTOR;
        ByteBuffer b = ByteBuffer.allocate(fileBlocks * sector).order(ByteOrder.LITTLE_ENDIAN);
        if (pvd) {
            int at = 16 * sector;
            b.put(at, (byte) 1);
            b.put(at + 1, "CD001".getBytes(StandardCharsets.US_ASCII), 0, 5);
            b.putInt(at + 80, volumeBlocks);
            b.putShort(at + 128, (short) sector);
            b.putInt(at + 156 + 2, 20);         // the root directory: sector 20, one sector
            b.putInt(at + 156 + 10, sector);
        }
        byte[] name = "SYSTEM.CNF;1".getBytes(StandardCharsets.US_ASCII);
        byte[] text = cnf.getBytes(StandardCharsets.US_ASCII);
        int dir = 20 * sector;
        b.put(dir, (byte) (33 + name.length + 1));  // the record's length (even)
        b.putInt(dir + 2, 21);                     // SYSTEM.CNF at sector 21
        b.putInt(dir + 10, text.length);
        b.put(dir + 32, (byte) name.length);
        b.put(dir + 33, name, 0, name.length);
        b.put(21 * sector, text, 0, text.length);
        return b.array();
    }

    static final String DOWNHILL = "BOOT2 = cdrom0:\\SCUS_971.77;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";

    /** What SetupActivity does with a finished copy: check, then completeness. */
    static String verdict(Path dir, byte[] copied, long size, boolean exact) throws IOException {
        Path file = Files.createTempFile(dir, "copy", ".part");
        Files.write(file, copied);
        try (RandomAccessFile f = new RandomAccessFile(file.toFile(), "r")) {
            String problem = DiscImage.check(f.getChannel());
            return problem != null ? problem : DiscImage.completeness(copied.length, size, exact, DiscImage.volumeBytes(f.getChannel()));
        }
    }

    /** A stream that hands out only the first limit bytes of data, then ends (a provider's stream
     *  cut short). */
    static InputStream cut(byte[] data, int limit) {
        return new ByteArrayInputStream(data, 0, limit);
    }

    public static void main(String[] args) throws Exception {
        Path dir = Files.createTempDirectory("dhd-disc-image-test");
        byte[] good = image(64, 64, DOWNHILL, true);
        long goodSize = good.length;

        // the right disc, complete, whether its size was known or not
        check(verdict(dir, good, goodSize, true) == null, "a complete image of the right disc passes");
        check(verdict(dir, good, 0, false) == null, "a complete copy of a stream with no size passes");
        check(verdict(dir, good, goodSize - 4096, false) == null, "a provider SIZE below the copy (its word) still passes");

        // other files
        check(verdict(dir, new byte[64 * 2048], 0, false).contains("isn't a disc image"), "no ISO 9660 volume");
        check(verdict(dir, image(64, 64, "BOOT2 = cdrom0:\\SLUS_203.12;1\r\n", true), 0, false).contains("SLUS_203.12"),
                "another PS2 disc is named by its serial");
        check(verdict(dir, image(64, 64, "nothing here", true), 0, false).contains("isn't Downhill Domination"),
                "a SYSTEM.CNF without BOOT2");

        // a short copy never passes (the review's case: 49,152 of 131,072 bytes, metadata intact)
        byte[] head = java.util.Arrays.copyOf(good, 49152);
        String known = verdict(dir, head, goodSize, true);
        check(known != null && known.contains("stopped early"), "a short copy of a file of known size: " + known);
        String provider = verdict(dir, head, goodSize, false);
        check(provider != null && provider.contains("stopped early"), "a short copy below the provider's SIZE: " + provider);
        String unknown = verdict(dir, head, 0, false);
        check(unknown != null && unknown.contains("incomplete"), "a short copy of a stream with no size: " + unknown);
        check(verdict(dir, good, goodSize + 2048, true) != null, "an exact size the copy doesn't match");
        // a source whose own size is short of its volume (a truncated dump) fails too
        byte[] truncated = java.util.Arrays.copyOf(image(64, 64, DOWNHILL, true), 40 * 2048);
        check(verdict(dir, truncated, truncated.length, true) != null, "a truncated dump of known size");
        check(DiscImage.completeness(goodSize, goodSize, true, 0) != null, "no volume size: can't be shown complete");

        // the volume's size: blocks times the block size, 0 without a descriptor
        Path img = dir.resolve("good.iso");
        Files.write(img, good);
        try (RandomAccessFile f = new RandomAccessFile(img.toFile(), "r")) {
            check(DiscImage.volumeBytes(f.getChannel()) == 64L * 2048, "volumeBytes");
        }
        Path none = dir.resolve("none.iso");
        Files.write(none, image(64, 64, DOWNHILL, false));
        try (RandomAccessFile f = new RandomAccessFile(none.toFile(), "r")) {
            check(DiscImage.volumeBytes(f.getChannel()) == 0, "volumeBytes without a descriptor");
        }

        // the copy loop: all of a stream, the count it returns, a cut stream's count, cancelling
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        long[] seen = {0};
        long done = DiscImage.copy(new ByteArrayInputStream(good), out, () -> false, n -> seen[0] = n);
        check(done == goodSize && out.size() == good.length, "copy: every byte");
        check(DiscImage.copy(cut(good, 49152), new ByteArrayOutputStream(), () -> false, n -> { }) == 49152,
                "copy: a cut stream's count");
        byte[] big = new byte[3 * DiscImage.BLOCK];
        int[] blocks = {0};
        try {
            DiscImage.copy(new ByteArrayInputStream(big), new ByteArrayOutputStream(), () -> ++blocks[0] > 1, n -> { });
            check(false, "copy: cancelling throws");
        } catch (IOException expected) {
            check(blocks[0] == 2, "copy: cancelled before the second block is written");
        }
        byte[] large = new byte[(int) (DiscImage.PROGRESS_STEP * 2 + 5)];
        long[] last = {0};
        int[] calls = {0};
        DiscImage.copy(new ByteArrayInputStream(large), new ByteArrayOutputStream(), () -> false, n -> { last[0] = n; calls[0]++; });
        check(calls[0] == 2 && last[0] == DiscImage.PROGRESS_STEP * 2, "copy: progress every PROGRESS_STEP");

        // the real disc image, if given: the right disc, its volume within the file
        String real = args.length > 0 ? args[0] : System.getenv("DHD_TEST_ISO");
        if (real != null && !real.isEmpty() && Files.isRegularFile(Path.of(real))) {
            try (RandomAccessFile f = new RandomAccessFile(real, "r")) {
                long length = f.length(), volume = DiscImage.volumeBytes(f.getChannel());
                check(DiscImage.check(f.getChannel()) == null, "the real disc image is Downhill Domination (USA)");
                check(DiscImage.completeness(length, length, true, volume) == null, "the real disc image is complete");
                System.out.println("real disc image: " + length + " bytes, volume " + volume + " bytes");
            }
        }

        for (Path p : Files.newDirectoryStream(dir)) Files.delete(p);
        Files.delete(dir);
        System.out.println(failures == 0 ? "DiscImageTest: all passed" : "DiscImageTest: " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
