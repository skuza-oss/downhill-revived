// Tests of DiscImage (the first start's disc rules, finding 143) on a plain JVM, with synthetic
// ISO 9660 images from port/host/disc_fixtures.py and the production JNI adapter (no game data):
//
//     python port/android/test_disc_image.py [your.iso]
//
// which compiles DiscImage.java and this file with the JDK's javac (JAVA_HOME) into build/ and
// runs it. Given an .iso (or DHD_TEST_ISO), it also checks the real disc image: the right disc,
// and its declared volume no longer than the file.
package app.downhillrevived;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
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

    static byte[] fixture(String name) throws IOException {
        return Files.readAllBytes(Path.of(System.getenv("DHD_DISC_FIXTURES"), name + ".iso"));
    }

    /** What the import (DiscImport) does with a finished copy: DiscImage.checkCopy, the shared
     *  verdict (port/host/disc_import.c: completeness first for a copy cut short, so it says the
     *  copy stopped), then the check's own words. */
    static String verdict(Path dir, byte[] copied, long size, boolean exact) throws IOException {
        Path file = Files.createTempFile(dir, "copy", ".part");
        Files.write(file, copied);
        try (RandomAccessFile f = new RandomAccessFile(file.toFile(), "r")) {
            return DiscImage.checkCopy(f.getChannel(), copied.length, size, exact);
        }
    }

    /** A stream that hands out only the first limit bytes of data, then ends (a provider's stream
     *  cut short). */
    static InputStream cut(byte[] data, int limit) {
        return new ByteArrayInputStream(data, 0, limit);
    }

    public static void main(String[] args) throws Exception {
        Path dir = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "dhd-disc-image-test");
        byte[] good = fixture("good");
        long goodSize = good.length;

        // JNI must leave the provider's sequential-copy position alone.
        Path seek = dir.resolve("seek.iso");
        Files.write(seek, good);
        try (RandomAccessFile f = new RandomAccessFile(seek.toFile(), "r")) {
            f.getChannel().position(123);
            check(DiscImage.check(f.getChannel()) == null && f.getChannel().position() == 123,
                    "JNI positional reads preserve the copy position");
        }
        // the right disc, complete, whether its size was known or not
        check(verdict(dir, good, goodSize, true) == null, "a complete image of the right disc passes");
        check(verdict(dir, good, 0, false) == null, "a complete copy of a stream with no size passes");
        check(verdict(dir, good, goodSize - 4096, false) == null, "a provider SIZE below the copy (its word) still passes");

        for (String name : new String[]{"no-volume", "no-cnf", "other", "europe", "japan", "cut", "tiny", "zero-volume", "empty-boot"}) {
            check(verdict(dir, fixture(name), 0, false) != null, "shared rejection: " + name);
        }
        check(DiscImage.completeness(49152, goodSize, true, goodSize).contains("stopped early"), "copy size rule");
        check(DiscImage.completeness(49152, goodSize, false, goodSize).contains("stopped early"), "provider size rule");
        // other files
        check(verdict(dir, fixture("no-volume"), 0, false).contains("isn't a disc image"), "no ISO 9660 volume");
        check(verdict(dir, fixture("other"), 0, false).contains("SLUS_203.12"),
                "another PS2 disc is named by its serial");
        check(verdict(dir, fixture("no-boot"), 0, false).contains("isn't Downhill Domination"),
                "a SYSTEM.CNF without BOOT2");

        // a short copy never passes (the review's case: 49,152 of 131,072 bytes, metadata intact)
        byte[] head = java.util.Arrays.copyOf(good, 49152);
        String known = verdict(dir, head, goodSize, true);
        check(known != null && known.contains("stopped early"), "a short copy of a file of known size: " + known);
        String provider = verdict(dir, head, goodSize, false);
        check(provider != null && provider.contains("stopped early"), "a short copy below the provider's SIZE: " + provider);
        try (RandomAccessFile f = new RandomAccessFile(dir.resolve("head.iso").toFile(), "rw")) {
            f.write(head);
            check(DiscImage.CUT.equals(DiscImage.check(f.getChannel())), "the shared check refuses a cut image itself");
        }
        String unknown = verdict(dir, head, 0, false);
        check(unknown != null && unknown.contains("incomplete"), "a short copy of a stream with no size: " + unknown);
        check(verdict(dir, good, goodSize + 2048, true) != null, "an exact size the copy doesn't match");
        // a source whose own size is short of its volume (a truncated dump) fails too
        byte[] truncated = java.util.Arrays.copyOf(fixture("good"), 40 * 2048);
        check(verdict(dir, truncated, truncated.length, true) != null, "a truncated dump of known size");
        check(DiscImage.completeness(goodSize, goodSize, true, 0) != null, "no volume size: can't be shown complete");

        // the volume's size: blocks times the block size, 0 without a descriptor
        Path img = dir.resolve("good.iso");
        Files.write(img, good);
        try (RandomAccessFile f = new RandomAccessFile(img.toFile(), "r")) {
            check(DiscImage.volumeBytes(f.getChannel()) == 64L * 2048, "volumeBytes");
        }
        Path none = dir.resolve("none.iso");
        Files.write(none, fixture("no-volume"));
        try (RandomAccessFile f = new RandomAccessFile(none.toFile(), "r")) {
            check(DiscImage.volumeBytes(f.getChannel()) == 0, "volumeBytes without a descriptor");
        }

        // the shared rules' constants and the room (disc_import.h)
        check(DiscImage.BLOCK == 1 << 20 && DiscImage.PROGRESS_STEP == 16L << 20 && DiscImage.MARGIN == 128L << 20
                && DiscImage.LOW == 64L << 20, "the shared constants");
        check(DiscImage.roomNeeded(1000, 1000 + DiscImage.MARGIN) == 0, "room: the source and the margin fit");
        check(DiscImage.roomNeeded(1001, 1000 + DiscImage.MARGIN) == 1001 + DiscImage.MARGIN, "room: one byte short");
        check(DiscImage.roomNeeded(0, 0) == 0, "room: an unknown size asks nothing");

        // the publication: an older pick's copy lands nowhere once a newer pick began
        Path older = dir.resolve("older.part"), newer = dir.resolve("newer.part"), target = dir.resolve("target.iso");
        Files.write(older, new byte[] { 1 });
        Files.write(newer, new byte[] { 2 });
        long first = DiscImage.publishBegin(), second = DiscImage.publishBegin();
        check(DiscImage.publish(first, older.toString(), target.toString()) == 1 && !Files.exists(target),
                "a superseded pick lands nowhere");
        check(DiscImage.publish(second, newer.toString(), target.toString()) == 0 && Files.readAllBytes(target)[0] == 2,
                "the newest pick lands");
        Files.delete(older);
        Files.delete(target);

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
