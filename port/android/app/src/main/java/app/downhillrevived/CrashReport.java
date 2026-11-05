// Downhill Revived on Android: the crash report's plain rules (finding 152), without Android's
// classes so a JVM test (port/android/test/CrashReportTest.java) runs them: which ways a run can end
// count as "closed unexpectedly" and the few words a player reads for each, the system's trace of a
// native crash (a tombstone: protobuf since Android 12, text before) as a short readable summary,
// and a log copied into the shared file with its middle left out when it is long.
// LastExit.java reads the system's record (ApplicationExitInfo) and LogShare.java builds the file.
package app.downhillrevived;

import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Pattern;

final class CrashReport {
    private CrashReport() {}

    // ApplicationExitInfo's reasons (stable values of the public API, API 30 to 35)
    static final int REASON_EXIT_SELF = 1, REASON_SIGNALED = 2, REASON_LOW_MEMORY = 3, REASON_CRASH = 4,
            REASON_CRASH_NATIVE = 5, REASON_ANR = 6, REASON_EXCESSIVE_RESOURCE_USAGE = 9;
    private static final String[] REASONS = { "UNKNOWN", "EXIT_SELF", "SIGNALED", "LOW_MEMORY", "CRASH", "CRASH_NATIVE",
            "ANR", "INITIALIZATION_FAILURE", "PERMISSION_CHANGE", "EXCESSIVE_RESOURCE_USAGE", "USER_REQUESTED",
            "USER_STOPPED", "DEPENDENCY_DIED", "OTHER", "FREEZER", "PACKAGE_STATE_CHANGE", "PACKAGE_UPDATED" };
    /** RunningAppProcessInfo.IMPORTANCE_VISIBLE: at most this, the game was on the screen. */
    static final int IMPORTANCE_VISIBLE = 200;
    /** The game's own crash handler's exit code when no system handler came after it (host.c crashed). */
    static final int CRASH_HANDLER_EXIT = 3;
    /** The trace kept in dhd-exit.txt: a tombstone's summary or the start of an ANR's threads. */
    static final int TRACE_MAX = 24 << 10;
    /** A tombstone read whole to be summarised (they are some hundreds of KB with their maps and logs). */
    static final int TOMBSTONE_MAX = 8 << 20;
    private static final int FRAMES_MAX = 48;
    /** A GPU driver's library as a tombstone names it: Vulkan's loader and the vendors' drivers. */
    private static final Pattern DRIVER = Pattern.compile(
            "(?i)(libvulkan|vulkan\\.|/hw/vulkan|libgles|libegl_|mali|adreno|libgsl|libllvm-qcom|qglinternal|pvr|powervr|libimg|libsrv_um|xclipse|libgpudataproducer)");
    private static final Pattern FRAME0 = Pattern.compile("(?m)^\\s*#00 pc [0-9a-fA-F]+\\s+(\\S+)");

    static String reasonName(int reason) {
        return reason >= 0 && reason < REASONS.length ? REASONS[reason] : "REASON_" + reason;
    }

    /** What a player reads after "closed unexpectedly last time", or null when the run ended as runs
     *  do (the game's exit, the player swiping it away or stopping it, Android ending it in the
     *  background). driver: a native crash whose first frame is in a GPU driver. */
    static String why(int reason, int status, int importance, boolean driver) {
        boolean shown = importance > 0 && importance <= IMPORTANCE_VISIBLE;
        switch (reason) {
            case REASON_CRASH:
                return "It crashed.";
            case REASON_CRASH_NATIVE:
                return driver ? "The graphics driver crashed." : "It crashed.";
            case REASON_SIGNALED:  // ended by a crash signal outside the system's crash handling
                return status == 4 || status == 6 || status == 7 || status == 8 || status == 11 ? "It crashed." : null;
            case REASON_EXIT_SELF:
                return status == CRASH_HANDLER_EXIT ? "It crashed." : null;
            case REASON_ANR:
                return "It stopped responding.";
            case REASON_LOW_MEMORY:  // a game left in the background is ended for memory all the time
                return shown ? "It ran out of memory." : null;
            case REASON_EXCESSIVE_RESOURCE_USAGE:
                return shown ? "Android stopped it for using too much of the phone." : null;
            default:
                return null;
        }
    }

    /** Whether a tombstone's summary (or text) has its first frame in a GPU driver. */
    static boolean driverCrash(String summary) {
        if (summary == null) return false;
        java.util.regex.Matcher m = FRAME0.matcher(summary);
        return m.find() && DRIVER.matcher(m.group(1)).find();
    }

    /** A native crash's trace as text: the tombstone's main lines (the crashing thread's frames) from
     *  its protobuf form, or the start of a text one; null when there is none. */
    static String tombstone(byte[] trace) {
        if (trace == null || trace.length == 0) return null;
        if (looksLikeText(trace)) return clip(new String(trace, StandardCharsets.UTF_8));
        try {
            return clip(summary(trace));
        } catch (RuntimeException e) {
            return "(a tombstone of " + trace.length + " bytes that could not be read: " + e.getMessage() + ")";
        }
    }

    /** Any other trace (an ANR's threads): its start. */
    static String text(byte[] trace) {
        return trace == null || trace.length == 0 ? null : clip(new String(trace, StandardCharsets.UTF_8));
    }

    private static boolean looksLikeText(byte[] b) {
        int n = Math.min(b.length, 256), plain = 0;
        for (int i = 0; i < n; i++) {
            int c = b[i] & 0xFF;
            if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F)) plain++;
        }
        return plain == n;
    }

    private static String clip(String s) {
        if (s.length() <= TRACE_MAX) return s;
        int cut = s.lastIndexOf('\n', TRACE_MAX);
        return s.substring(0, cut > 0 ? cut : TRACE_MAX) + "\n(the rest of the trace left out)";
    }

    // ---- the tombstone's protobuf (AOSP system/core/debuggerd/proto/tombstone.proto) ----

    /** One message's fields, read in order. */
    private static final class Proto {
        final byte[] b;
        int at;
        final int end;
        int field, wire;

        Proto(byte[] b, int at, int end) {
            this.b = b;
            this.at = at;
            this.end = end;
        }

        boolean next() {
            if (at >= end) return false;
            long key = varint();
            field = (int) (key >>> 3);
            wire = (int) (key & 7);
            return true;
        }

        /** The field's number and wire type as one value: field << 3 | wire. */
        int key() {
            return field << 3 | wire;
        }

        long varint() {
            long v = 0;
            for (int shift = 0; shift < 64; shift += 7) {
                if (at >= end) throw new IllegalStateException("cut short");
                int c = b[at++] & 0xFF;
                v |= (long) (c & 0x7F) << shift;
                if (c < 0x80) return v;
            }
            throw new IllegalStateException("bad number");
        }

        int length() {
            long n = varint();
            if (n < 0 || n > end - at) throw new IllegalStateException("bad length");
            return (int) n;
        }

        Proto message() {
            int n = length();
            Proto p = new Proto(b, at, at + n);
            at += n;
            return p;
        }

        String string() {
            int n = length();
            String s = new String(b, at, n, StandardCharsets.UTF_8);
            at += n;
            return s;
        }

        void skip() {
            switch (wire) {
                case 0: varint(); break;
                case 1: at += 8; break;
                case 2: { int n = length(); at += n; break; }  // not at += length(): at would be read first
                case 5: at += 4; break;
                default: throw new IllegalStateException("wire type " + wire);
            }
            if (at > end) throw new IllegalStateException("cut short");
        }
    }

    private static final class Frame {
        long relPc, offset;
        String function = "", file = "", buildId = "";
    }

    private static String summary(byte[] b) {
        Proto t = new Proto(b, 0, b.length);
        String fingerprint = "", timestamp = "", abort = "", signal = "";
        long pid = 0, tid = 0;
        List<String> causes = new ArrayList<>();
        String threadName = "";
        List<Frame> frames = new ArrayList<>();
        List<Object[]> threads = new ArrayList<>();  // {id, name, frames}
        boolean cut = false;
        try {  // a tombstone read in part: what came before the cut (its threads come before its maps and logs)
            while (t.next()) {
                switch (t.key()) {
                    case 2 << 3 | 2: fingerprint = t.string(); break;
                    case 4 << 3 | 2: timestamp = t.string(); break;
                    case 5 << 3: pid = t.varint(); break;
                    case 6 << 3: tid = t.varint(); break;
                    case 10 << 3 | 2: signal = signal(t.message()); break;
                    case 14 << 3 | 2: abort = t.string(); break;
                    case 15 << 3 | 2: {  // Cause: human_readable = 1
                        Proto c = t.message();
                        while (c.next()) { if (c.key() == (1 << 3 | 2)) causes.add(c.string()); else c.skip(); }
                        break;
                    }
                    case 16 << 3 | 2: {  // map<uint32, Thread>: key 1, value 2
                        Proto e = t.message();
                        while (e.next()) { if (e.key() == (2 << 3 | 2)) threads.add(thread(e.message())); else e.skip(); }
                        break;
                    }
                    default: t.skip();
                }
            }
        } catch (IllegalStateException e) {
            cut = true;
        }
        for (Object[] th : threads) {
            if ((Long) th[0] == tid) {
                threadName = (String) th[1];
                @SuppressWarnings("unchecked") List<Frame> f = (List<Frame>) th[2];
                frames = f;
            }
        }
        StringBuilder s = new StringBuilder();
        if (!fingerprint.isEmpty()) s.append("Build fingerprint: '").append(fingerprint).append("'\n");
        if (!timestamp.isEmpty()) s.append("Timestamp: ").append(timestamp).append('\n');
        s.append("pid: ").append(pid).append(", tid: ").append(tid).append(", name: ").append(threadName).append('\n');
        if (!signal.isEmpty()) s.append(signal).append('\n');
        if (!abort.isEmpty()) s.append("Abort message: '").append(abort).append("'\n");
        for (String c : causes) s.append("Cause: ").append(c).append('\n');
        s.append("backtrace:\n");
        int n = 0;
        for (Frame f : frames) {
            if (n == FRAMES_MAX) { s.append("      (").append(frames.size() - n).append(" more frames)\n"); break; }
            s.append(String.format(Locale.US, "      #%02d pc %016x  %s", n++, f.relPc, f.file.isEmpty() ? "<unknown>" : f.file));
            if (!f.function.isEmpty()) s.append(" (").append(f.function).append('+').append(f.offset).append(')');
            if (!f.buildId.isEmpty()) s.append(" (BuildId: ").append(f.buildId).append(')');
            s.append('\n');
        }
        if (frames.isEmpty()) s.append("      (no frames for the crashing thread)\n");
        if (cut) s.append("(the tombstone was read in part)\n");
        return s.toString();
    }

    private static String signal(Proto p) {
        long number = 0, code = 0, fault = 0;
        String name = "", codeName = "";
        boolean hasFault = false;
        while (p.next()) {
            switch (p.key()) {
                case 1 << 3: number = p.varint(); break;
                case 2 << 3 | 2: name = p.string(); break;
                case 3 << 3: code = p.varint(); break;
                case 4 << 3 | 2: codeName = p.string(); break;
                case 8 << 3: hasFault = p.varint() != 0; break;
                case 9 << 3: fault = p.varint(); break;
                default: p.skip();
            }
        }
        return String.format(Locale.US, "signal %d (%s), code %d (%s)%s", number, name, (int) code, codeName,
                hasFault ? String.format(Locale.US, ", fault addr 0x%x", fault) : "");
    }

    private static Object[] thread(Proto p) {
        long id = 0;
        String name = "";
        List<Frame> frames = new ArrayList<>();
        while (p.next()) {
            switch (p.key()) {
                case 1 << 3: id = p.varint(); break;
                case 2 << 3 | 2: name = p.string(); break;
                case 4 << 3 | 2: frames.add(frame(p.message())); break;
                default: p.skip();
            }
        }
        return new Object[] { id, name, frames };
    }

    private static Frame frame(Proto p) {
        Frame f = new Frame();
        while (p.next()) {
            switch (p.key()) {
                case 1 << 3: f.relPc = p.varint(); break;
                case 4 << 3 | 2: f.function = p.string(); break;
                case 5 << 3: f.offset = p.varint(); break;
                case 6 << 3 | 2: f.file = p.string(); break;
                case 8 << 3 | 2: f.buildId = p.string(); break;
                default: p.skip();
            }
        }
        return f;
    }

    // ---- the shared file ----

    /** A log for the shared file: whole up to head + tail bytes, else its first head and last tail
     *  bytes (a crash is at the end) with a line saying how much is left out. */
    static void copyLog(File log, OutputStream out, long head, long tail) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(log, "r")) {
            long size = f.length();
            if (size <= head + tail) {
                copy(f, 0, size, out);
                return;
            }
            copy(f, 0, head, out);
            out.write(String.format(Locale.US, "\n[... %d bytes left out ...]\n", size - head - tail).getBytes(StandardCharsets.UTF_8));
            copy(f, size - tail, tail, out);
        }
    }

    private static void copy(RandomAccessFile f, long from, long n, OutputStream out) throws IOException {
        byte[] buf = new byte[64 << 10];
        f.seek(from);
        while (n > 0) {
            int got = f.read(buf, 0, (int) Math.min(buf.length, n));
            if (got <= 0) break;
            out.write(buf, 0, got);
            n -= got;
        }
    }
}
