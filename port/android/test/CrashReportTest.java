// The crash report's plain rules (CrashReport.java, finding 152) on the JVM: which ends count as
// "closed unexpectedly" and their words, a tombstone's summary from its protobuf form (built here
// field by field after AOSP's tombstone.proto), a tombstone read in part, a text one, and the log
// copy that leaves out a long log's middle. Run by port/android/test_disc_image.py.
package app.downhillrevived;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

public class CrashReportTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            failures++;
            System.out.println("FAIL: " + what);
        }
    }

    // ---- a protobuf writer for the test's tombstones ----

    private static void varint(ByteArrayOutputStream o, long v) {
        while ((v & ~0x7FL) != 0) {
            o.write((int) ((v & 0x7F) | 0x80));
            v >>>= 7;
        }
        o.write((int) v);
    }

    private static void number(ByteArrayOutputStream o, int field, long v) {
        varint(o, field << 3);
        varint(o, v);
    }

    private static void bytes(ByteArrayOutputStream o, int field, byte[] b) {
        varint(o, field << 3 | 2);
        varint(o, b.length);
        o.write(b, 0, b.length);
    }

    private static void string(ByteArrayOutputStream o, int field, String s) {
        bytes(o, field, s.getBytes(StandardCharsets.UTF_8));
    }

    private static byte[] frame(long relPc, String function, long offset, String file, String buildId) {
        ByteArrayOutputStream f = new ByteArrayOutputStream();
        number(f, 1, relPc);
        number(f, 2, relPc + 0x7000000000L);  // pc: not printed
        string(f, 4, function);
        number(f, 5, offset);
        string(f, 6, file);
        string(f, 8, buildId);
        return f.toByteArray();
    }

    private static byte[] thread(int id, String name, byte[]... frames) {
        ByteArrayOutputStream t = new ByteArrayOutputStream();
        number(t, 1, id);
        string(t, 2, name);
        for (byte[] f : frames) bytes(t, 4, f);
        return t.toByteArray();
    }

    private static byte[] threadEntry(int id, byte[] thread) {
        ByteArrayOutputStream e = new ByteArrayOutputStream();
        number(e, 1, id);
        bytes(e, 2, thread);
        return e.toByteArray();
    }

    private static byte[] tombstone(String driverFile) {
        ByteArrayOutputStream t = new ByteArrayOutputStream();
        number(t, 1, 1);  // arch: arm64
        string(t, 2, "samsung/a07/a07:15/AP3A/123:user/release-keys");
        string(t, 4, "2026-11-05 14:03:12.123456789+0100");
        number(t, 5, 4321);
        number(t, 6, 4400);
        number(t, 7, 10234);
        ByteArrayOutputStream signal = new ByteArrayOutputStream();
        number(signal, 1, 11);
        string(signal, 2, "SIGSEGV");
        number(signal, 3, 1);
        string(signal, 4, "SEGV_MAPERR");
        number(signal, 8, 1);
        number(signal, 9, 0x18);
        bytes(t, 10, signal.toByteArray());
        ByteArrayOutputStream cause = new ByteArrayOutputStream();
        string(cause, 1, "null pointer dereference");
        bytes(t, 15, cause.toByteArray());
        bytes(t, 16, threadEntry(4321, thread(4321, "app.downhillrevived",
                frame(0x99, "__futex_wait", 4, "/apex/com.android.runtime/lib64/bionic/libc.so", "aa"))));
        bytes(t, 16, threadEntry(4400, thread(4400, "SDLThread",
                frame(0x1234, "vkQueueSubmit", 16, driverFile, "abc123"),
                frame(0x5678, "", 0, "/data/app/~~x/app.downhillrevived-y/lib/arm64/libmain.so", "def456"))));
        ByteArrayOutputStream map = new ByteArrayOutputStream();  // a memory mapping: after the threads
        string(map, 7, "/system/lib64/libc.so".repeat(40));
        bytes(t, 17, map.toByteArray());
        return t.toByteArray();
    }

    public static void main(String[] args) throws IOException {
        // the words, and which ends count
        check("It crashed.".equals(CrashReport.why(CrashReport.REASON_CRASH, 0, 100, false)), "a Java crash");
        check("It crashed.".equals(CrashReport.why(CrashReport.REASON_CRASH_NATIVE, 11, 100, false)), "a native crash");
        check("The graphics driver crashed.".equals(CrashReport.why(CrashReport.REASON_CRASH_NATIVE, 11, 100, true)), "a driver crash");
        check("It crashed.".equals(CrashReport.why(CrashReport.REASON_EXIT_SELF, 3, 100, false)), "the game's crash handler's exit");
        check(CrashReport.why(CrashReport.REASON_EXIT_SELF, 0, 100, false) == null, "the game's own exit is normal");
        check("It crashed.".equals(CrashReport.why(CrashReport.REASON_SIGNALED, 11, 100, false)), "killed by SIGSEGV");
        check(CrashReport.why(CrashReport.REASON_SIGNALED, 9, 100, false) == null, "SIGKILL is not a crash");
        check("It stopped responding.".equals(CrashReport.why(CrashReport.REASON_ANR, 0, 400, false)), "an ANR");
        check("It ran out of memory.".equals(CrashReport.why(CrashReport.REASON_LOW_MEMORY, 0, 100, false)), "low memory on screen");
        check(CrashReport.why(CrashReport.REASON_LOW_MEMORY, 0, 400, false) == null, "low memory in the background is normal");
        check(CrashReport.why(10, 0, 100, false) == null, "the player's swipe (USER_REQUESTED) is normal");
        check(CrashReport.why(11, 0, 100, false) == null, "a stop from Settings (USER_STOPPED) is normal");
        check("CRASH_NATIVE".equals(CrashReport.reasonName(5)) && "REASON_99".equals(CrashReport.reasonName(99)), "reason names");

        // a protobuf tombstone: the crashing thread's frames, the signal, the cause
        String s = CrashReport.tombstone(tombstone("/vendor/lib64/hw/vulkan.mali.so"));
        check(s.contains("signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x18"), "the signal line: " + s);
        check(s.contains("pid: 4321, tid: 4400, name: SDLThread"), "the crashing thread: " + s);
        check(s.contains("#00 pc 0000000000001234  /vendor/lib64/hw/vulkan.mali.so (vkQueueSubmit+16) (BuildId: abc123)"), "frame 0: " + s);
        check(s.contains("#01 pc 0000000000005678  /data/app/~~x/app.downhillrevived-y/lib/arm64/libmain.so (BuildId: def456)"), "frame 1: " + s);
        check(s.contains("Cause: null pointer dereference"), "the cause: " + s);
        check(!s.contains("__futex_wait"), "only the crashing thread: " + s);
        check(!s.contains("read in part"), "a whole tombstone: " + s);
        check(CrashReport.driverCrash(s), "a crash in the Mali driver is the driver's");
        check(!CrashReport.driverCrash(CrashReport.tombstone(tombstone("/data/app/x/lib/arm64/libmain.so"))), "a crash in libmain.so is not");

        // read in part (the maps cut off): the threads are still there
        byte[] whole = tombstone("/vendor/lib64/egl/libGLES_mali.so");
        String part = CrashReport.tombstone(Arrays.copyOf(whole, whole.length - 300));
        check(part.contains("#00 pc 0000000000001234") && part.contains("read in part"), "a cut tombstone: " + part);
        check(CrashReport.driverCrash(part), "libGLES_mali.so is a driver");

        // not a tombstone at all
        String bad = CrashReport.tombstone(new byte[] { (byte) 0xFF, (byte) 0xFF, (byte) 0xFF, 0x01, 0x02 });
        check(bad.contains("could not be read") || bad.contains("no frames"), "garbage: " + bad);

        // a text tombstone (before Android 12): as it is
        String text = "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\nbacktrace:\n"
                + "      #00 pc 000000000004f3a8  /vendor/lib64/libgsl.so (oops+164)\n";
        String t = CrashReport.tombstone(text.getBytes(StandardCharsets.UTF_8));
        check(t.equals(text), "a text tombstone as it is");
        check(CrashReport.driverCrash(t), "Adreno's libgsl.so is a driver");
        check(CrashReport.tombstone(new byte[0]) == null && CrashReport.tombstone(null) == null, "no trace");

        // a long trace is clipped at a line
        String big = CrashReport.text("line\n".repeat(20000).getBytes(StandardCharsets.UTF_8));
        check(big.length() < CrashReport.TRACE_MAX + 100 && big.endsWith("(the rest of the trace left out)"), "a long trace is clipped");

        // the shared file's copy of a log
        File log = File.createTempFile("dhd", ".log");
        try {
            byte[] content = new byte[100];
            for (int i = 0; i < content.length; i++) content[i] = (byte) ('a' + i % 26);
            Files.write(log.toPath(), content);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            CrashReport.copyLog(log, out, 200, 200);
            check(Arrays.equals(out.toByteArray(), content), "a short log whole");
            out.reset();
            CrashReport.copyLog(log, out, 10, 20);
            String cut = out.toString(StandardCharsets.UTF_8);
            check(cut.startsWith(new String(content, 0, 10, StandardCharsets.US_ASCII) + "\n[... 70 bytes left out ...]\n")
                    && cut.endsWith(new String(content, 80, 20, StandardCharsets.US_ASCII)), "a long log's start and end: " + cut);
        } finally {
            log.delete();
        }

        System.out.println(failures == 0 ? "CrashReportTest: PASS" : "CrashReportTest: " + failures + " failure(s)");
        System.exit(failures == 0 ? 0 : 1);
    }
}
