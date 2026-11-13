// Downhill Revived on Android: the launcher page's calls (finding 225; the list and their results:
// port/launcher/BRIDGE.md, the same names the desktop launcher's Tauri commands have). A call
// arrives as {id, call, args} on the UI thread, runs on the activity's worker threads and is
// answered {id, ok: true, value} or {id, ok: false, error}; an error is the page's string table key
// with its values ({code, params}) or, for the disc checks' own words, a string.
package app.downhillrevived;

import android.net.Uri;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

final class LauncherBridge {
    /** What this launcher can do (BRIDGE.md, "Capabilities"). */
    static final String[] CAPS = { "touch-settings", "disc-import", "direct-start", "share-log", "help", "touch-editor" };
    /** The largest resolution scale a phone's renderer draws (gpu_rust_shared.inc NATIVE_PHONE_SCALE_MAX). */
    static final int PHONE_SCALE_MAX = 6;
    /** The project's pages, the only ones a link opens (the desktop launcher's list, main.rs open_link). */
    static final String DISCORD = "https://discord.gg/XrSeTyzfr2", KOFI = "https://ko-fi.com/skuza";
    static final String[] ALLOWED = { "https://discord.gg/", "https://ko-fi.com/skuza", "https://github.com/skuza-oss/",
            "https://downhill-revived.pages.dev/" };

    private final LauncherActivity activity;
    private final String ini;
    /** The page's pick_disc waiting for the picker, and the import jobs by number. */
    private volatile Reply picking;
    private volatile DiscImport.Job job;
    private volatile boolean closed;

    /** The answer to one call. */
    interface Reply {
        void value(Object value);
        void error(Object error);
    }

    LauncherBridge(LauncherActivity activity) {
        this.activity = activity;
        ini = new File(activity.files(), "dhd.ini").getPath();
    }

    /** The setting "launcher" is off: the game starts at once, when it can (its disc is in place). */
    boolean startsDirectly() {
        try {
            String v = Native.read(ini, "launcher");
            return v != null && (v.trim().equals("0") || v.trim().equalsIgnoreCase("off")) && discPath() != null
                    && Native.discProblem(discPath()) == null;
        } catch (Throwable e) {
            Log.w(LauncherActivity.TAG, "launcher: the setting can't be read: " + e);
            return false;
        }
    }

    /** A message from the page. */
    void receive(String text) {
        final int id;
        final String call;
        final JSONObject args;
        try {
            JSONObject m = new JSONObject(text);
            id = m.getInt("id");
            call = m.getString("call");
            args = m.optJSONObject("args") != null ? m.optJSONObject("args") : new JSONObject();
        } catch (JSONException e) {
            return;
        }
        Reply reply = new Reply() {
            @Override public void value(Object value) { answer(id, true, value); }
            @Override public void error(Object error) { answer(id, false, error); }
        };
        try {
            activity.work.execute(() -> {
                try {
                    handle(call, args, reply);
                } catch (Throwable e) {
                    Log.w(LauncherActivity.TAG, "launcher: " + call + ": " + e);
                    reply.error(String.valueOf(e.getMessage()));
                }
            });
        } catch (java.util.concurrent.RejectedExecutionException e) {
            // the activity is ending
        }
    }

    private void answer(int id, boolean ok, Object value) {
        JSONObject m = new JSONObject();
        try {
            m.put("id", id);
            m.put("ok", ok);
            m.put(ok ? "value" : "error", value == null ? JSONObject.NULL : value);
        } catch (JSONException e) {
            return;
        }
        activity.post(m.toString());
    }

    private void handle(String call, JSONObject args, Reply reply) throws Exception {
        switch (call) {
            case "snapshot": reply.value(snapshot()); return;
            case "settings_schema": reply.value(new JSONArray(Native.schema())); return;
            case "save_setting": {
                if (!Native.save(ini, args.getString("key"), args.getString("value"))) reply.error("can't save dhd.ini");
                else reply.value(null);
                return;
            }
            case "rider_name": reply.value(Native.riderName(args.optString("text"))); return;
            case "check_update": checkUpdate(reply); return;
            case "check_disc": checkDisc(reply); return;
            case "pick_disc": pickDisc(reply); return;
            case "retry_import": {
                DiscImport.Job before = job;
                if (before == null) { reply.value(null); return; }
                reply.value(startImport(before.uri));
                return;
            }
            case "delete_original": {
                DiscImport.Job j = job;
                if (j == null || j.number != args.optInt("job")) { reply.value(false); return; }
                reply.value(j.deleteOriginal());
                return;
            }
            case "play": {
                boolean edit = args.optBoolean("edit_touch_layout");
                activity.runOnUiThread(() -> activity.play(edit, true));
                reply.value(null);
                return;
            }
            case "open_link": {
                String url = args.optString("url");
                url = url.equals("discord") ? DISCORD : url.equals("kofi") ? KOFI : url;
                boolean allowed = false;
                for (String a : ALLOWED) allowed |= url.startsWith(a);
                if (!allowed) { reply.error("not opening " + url); return; }
                final String page = url.startsWith(UpdatePage.REPOSITORY) ? UpdatePage.safe(url) : url;
                activity.runOnUiThread(() -> activity.open(page));
                reply.value(null);
                return;
            }
            case "share_log": activity.runOnUiThread(activity::shareLog); reply.value(null); return;
            case "licences": reply.value(licences()); return;
            default: reply.error("no such call: " + call);
        }
    }

    /** The copy of the disc image, when it is there. */
    private String discPath() {
        File disc = DiscImport.disc(activity);
        return disc.isFile() ? disc.getPath() : null;
    }

    private JSONObject snapshot() throws JSONException {
        String disc = discPath();
        JSONObject settings = new JSONObject(Native.values(ini));
        // the game reads its copy (android_main.c names it in dhd.ini on its first start); a launcher
        // that made the copy names it too
        if (disc != null && !disc.equals(settings.optString("iso"))) {
            Native.save(ini, "iso", disc);
            settings.put("iso", disc);
        } else if (disc == null) {
            settings.put("iso", "");
        }
        JSONObject s = new JSONObject();
        s.put("version", Native.version());
        s.put("caps", new JSONArray(CAPS));
        s.put("native_window_controls", false);
        s.put("settings", settings);
        s.put("game_found", true);
        s.put("running", false);
        String problem = disc != null ? Native.discProblem(disc) : null;
        s.put("disc_problem", problem == null ? JSONObject.NULL : problem);
        s.put("texture_pack", false);
        s.put("played", 0);
        s.put("scale", JSONObject.NULL);
        s.put("scale_max", PHONE_SCALE_MAX);
        s.put("disc_size", disc != null ? new File(disc).length() : 0);
        s.put("free_space", DiscImport.freeBytes(activity));
        String why = activity.crashWhy;
        if (why != null) {
            JSONObject exit = new JSONObject();
            exit.put("why", why);
            s.put("last_exit", exit);
            activity.crashWhy = null;  // said once
        } else {
            s.put("last_exit", JSONObject.NULL);
        }
        return s;
    }

    /** The newest release with an Android build: {release: {version, page}, newer}. */
    private void checkUpdate(Reply reply) throws JSONException {
        String version = Native.version();
        String found = Native.update();
        if (found == null) {
            reply.error(DiscImport.coded("footer.checkFailed", "version", version));
            return;
        }
        JSONObject release = new JSONObject();
        JSONObject r = new JSONObject();
        boolean newer = !found.isEmpty();
        String[] parts = found.split("\n", 2);
        r.put("version", newer ? parts[0] : version);
        r.put("page", newer && parts.length > 1 ? UpdatePage.safe(parts[1]) : UpdatePage.RELEASES);
        release.put("release", r);
        release.put("newer", newer);
        reply.value(release);
    }

    /** The online check of the disc's copy (finding 098): the USA disc's game files, or not. */
    private void checkDisc(Reply reply) throws JSONException {
        String disc = discPath();
        if (disc == null) { reply.error("No disc image is chosen."); return; }
        String answer = Native.onlineCheck(disc, new File(activity.files(), "iso-check.txt").getPath());
        String[] parts = answer.split("\n", 2);
        int code = Integer.parseInt(parts[0].trim());
        String note = parts.length > 1 ? parts[1] : "";
        if (code < 0) {
            reply.error("The disc image couldn't be read for the online check: " + note);
            return;
        }
        JSONObject r = new JSONObject();
        r.put("path", disc);
        r.put("usa", note.isEmpty());
        reply.value(r);
    }

    /** The document picker; the answer comes with picked. */
    private void pickDisc(Reply reply) {
        Reply before = picking;
        if (before != null) before.value(null);
        picking = reply;
        activity.runOnUiThread(() -> {
            if (!activity.pick()) {
                picking = null;
                reply.error(DiscImport.coded("disc.problem.noPicker"));
            }
        });
    }

    /** The picker's answer: the document (null: cancelled). */
    void picked(Uri uri) {
        Reply reply = picking;
        picking = null;
        if (reply == null) return;
        if (uri == null) { reply.value(null); return; }
        Log.i(LauncherActivity.TAG, "disc image chosen: " + uri);
        try {
            reply.value(startImport(uri));
        } catch (JSONException e) {
            reply.error(String.valueOf(e.getMessage()));
        }
    }

    private JSONObject startImport(Uri uri) throws JSONException {
        DiscImport.Job before = job;
        if (before != null) before.cancel();
        DiscImport.Job j = new DiscImport.Job(activity, uri, e -> { if (!closed) activity.emit("disc-import", e); });
        job = j;
        JSONObject r = new JSONObject();
        r.put("job", j.number);
        j.start();
        return r;
    }

    /** The licence texts the APK carries (a release's: LICENSE.txt, THIRD-PARTY-NOTICES.txt). */
    private String licences() {
        StringBuilder s = new StringBuilder();
        for (String name : new String[] { "LICENSE.txt", "THIRD-PARTY-NOTICES.txt" }) {
            try (InputStream in = activity.getAssets().open(name)) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] b = new byte[64 << 10];
                for (int n; (n = in.read(b)) > 0;) out.write(b, 0, n);
                if (s.length() > 0) s.append("\n\n");
                s.append(new String(out.toByteArray(), StandardCharsets.UTF_8));
            } catch (IOException ignored) {
                // a development build carries none
            }
        }
        return s.length() > 0 ? s.toString()
                : "Downhill Revived is free software under the GNU General Public License, version 3 or later. "
                  + "This build carries no licence texts; a release has them.";
    }

    void close() {
        closed = true;
        DiscImport.Job j = job;
        if (j != null) j.cancel();
        Reply r = picking;
        picking = null;
        if (r != null) r.value(null);
    }
}
