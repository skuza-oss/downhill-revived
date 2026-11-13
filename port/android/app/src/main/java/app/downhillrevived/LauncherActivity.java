// Downhill Revived on Android: the launcher (finding 225), the app's entry activity. It shows the
// launcher's page, the desktop launcher's own (port/launcher/ui, packed into the APK's assets under
// launcher/), in the system WebView, and answers the page's calls (port/launcher/BRIDGE.md) here:
// the settings through the game's settings code (Native), the disc import (DiscImport), the update
// check, Share log (LogShare), links. PLAY starts the game (DhdActivity) and this activity ends; the
// game's EXIT GAME ends the process (host_end, finding 210), so the next start opens the launcher
// again. "Edit the touch layout" starts the game's layout editor alone and this activity stays below
// it: the editor's Done ends the game's process the same way, and Android starts this activity again
// from its saved state (play, finding 228).
//
// The page is the app's own and nothing else: the WebView loads only
// https://appassets.androidplatform.net/launcher/... (a name Android reserves for an app's own
// files), served from the assets here with a strict content security policy; every other request is
// refused and no navigation leaves the page. The bridge is a MessagePort posted to that origin only
// (postWebMessage's target origin), once the page has loaded: no JavaScript interface is added to
// the WebView, so no other page could reach it.
//
// Before the page (finding 152): how the run before ended (LastExit, before any native code), for
// the page's "closed unexpectedly" note. The icon's long-press shortcuts: Share log (the share
// sheet only) and Launcher (the page even when the game is set to start directly).
package app.downhillrevived;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.webkit.WebMessage;
import android.webkit.WebMessagePort;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.FrameLayout;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class LauncherActivity extends Activity {
    static final String TAG = "DownhillRevived";
    /** The shortcut's actions: share the log (no page, no game); open the page whatever the setting. */
    static final String SHARE_LOG = "app.downhillrevived.SHARE_LOG";
    static final String OPEN = "app.downhillrevived.OPEN_LAUNCHER";
    /** Started by the game when its disc is missing (DhdActivity.discMissing). */
    static final String DISC = "app.downhillrevived.CHOOSE_DISC";
    /** The page's origin and folder (Android reserves this host name for an app's own files). */
    static final String ORIGIN = "https://appassets.androidplatform.net";
    static final String BASE = ORIGIN + "/launcher/";
    /** The page's content security policy: its own files only, no network, no inline script. */
    static final String CSP = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
            + "font-src 'self'; connect-src 'self'; frame-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'";
    static final int PICK = 1, SHARE = 2;
    private static final int GROUND = 0xFF1A1430;
    /** The saved state's key: this activity started the layout editor alone (play). */
    static final String BACK_FROM_EDITOR = "back_from_editor";

    private WebView web;
    private WebMessagePort port;
    private boolean pageLoaded, sharingOnly, paused;
    /** The layout editor alone was started from here (saved with the state). */
    private boolean editing;
    /** Started again after the editor alone ended the game: the page opens at its row (snapshot's
     *  "reopen", once). */
    volatile boolean backFromEditor;
    private final Handler ui = new Handler(Looper.getMainLooper());
    /** The page's calls run here: some wait (the update check, the online check's hashing, a picker). */
    final ExecutorService work = Executors.newCachedThreadPool();
    LauncherBridge bridge;
    /** How the run before ended, when it closed unexpectedly and the page hasn't said so yet. */
    volatile String crashWhy;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        backdrop();
        String action = getIntent().getAction();
        if (SHARE_LOG.equals(action)) {
            sharingOnly = true;
            LogShare.start(this, SHARE, this::finish);
            return;
        }
        // the game runs in this process already (it starts once per process): back to it
        if (DhdActivity.started) {
            startActivity(new Intent(this, DhdActivity.class));
            finish();
            return;
        }
        DiscImport.removeStaleCopies(this);
        bridge = new LauncherBridge(this);
        // The exit record is read off the UI thread: a native crash's trace can be megabytes and slow
        // to come, and a frozen first screen would be Android's "not responding"
        Context app = getApplicationContext();
        // started again by Android with its saved state (after the layout editor alone ended the game,
        // play): the page, never a direct start
        boolean restored = savedInstanceState != null;
        backFromEditor = restored && savedInstanceState.getBoolean(BACK_FROM_EDITOR);
        if (backFromEditor) Log.i(TAG, "launcher: back from the layout editor alone");
        boolean asked = OPEN.equals(action) || DISC.equals(action) || restored;
        new Thread(() -> {
            LastExit exit = LastExit.record(app);
            String why = exit != null && exit.why != null && LastExit.firstAsk(app, exit) ? exit.why : null;
            boolean direct = !asked && why == null && bridge.startsDirectly();
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                crashWhy = why;
                if (direct) play(false, false);  // the game at once (the setting "launcher", off)
                else showPage();
            });
        }, "last exit").start();
    }

    /** The launcher's page in the WebView. */
    private void showPage() {
        web = new WebView(this);
        web.setBackgroundColor(GROUND);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setDomStorageEnabled(false);
        s.setDatabaseEnabled(false);
        s.setGeolocationEnabled(false);
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);
        s.setMediaPlaybackRequiresUserGesture(true);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        s.setCacheMode(WebSettings.LOAD_NO_CACHE);
        s.setTextZoom(100);  // the page sizes its own text; a large system font still applies to the game
        web.setVerticalScrollBarEnabled(false);
        web.setOverScrollMode(View.OVER_SCROLL_NEVER);
        web.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                return serve(request.getUrl());
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                return true;  // nothing navigates away from the page; links go through the bridge
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                if (url != null && url.startsWith(BASE) && !pageLoaded) {
                    pageLoaded = true;
                    connect();
                }
            }
        });
        setContentView(web);
        hideBars();
        web.loadUrl(BASE + "index.html");
    }

    /** One of the page's files from the APK's assets (launcher/...), else a refusal. */
    private WebResourceResponse serve(Uri url) {
        String path = url.getPath();
        if (!ORIGIN.equals(url.getScheme() + "://" + url.getHost()) || url.getPort() != -1 || path == null
                || !path.startsWith("/launcher/") || path.contains("..")) {
            return refused();
        }
        String file = path.substring(1);
        String type = mime(file);
        if (type == null) return refused();
        try {
            InputStream in = getAssets().open(file);
            Map<String, String> headers = new HashMap<>();
            headers.put("Content-Security-Policy", CSP);
            headers.put("X-Content-Type-Options", "nosniff");
            headers.put("Cache-Control", "no-store");
            return new WebResourceResponse(type, type.startsWith("text/") || type.endsWith("json") || type.endsWith("javascript")
                    ? "utf-8" : null, 200, "OK", headers, in);
        } catch (IOException e) {
            return refused();
        }
    }

    private static WebResourceResponse refused() {
        return new WebResourceResponse("text/plain", "utf-8", 403, "Forbidden", new HashMap<>(), new ByteArrayInputStream(new byte[0]));
    }

    private static String mime(String file) {
        if (file.endsWith(".html")) return "text/html";
        if (file.endsWith(".js")) return "text/javascript";
        if (file.endsWith(".css")) return "text/css";
        if (file.endsWith(".json")) return "application/json";
        if (file.endsWith(".woff2")) return "font/woff2";
        if (file.endsWith(".png")) return "image/png";
        if (file.endsWith(".svg")) return "image/svg+xml";
        return null;
    }

    /** The bridge's port, posted to the page's origin only. */
    private void connect() {
        WebMessagePort[] channel = web.createWebMessageChannel();
        port = channel[0];
        port.setWebMessageCallback(new WebMessagePort.WebMessageCallback() {
            @Override
            public void onMessage(WebMessagePort p, WebMessage message) {
                bridge.receive(message.getData());
            }
        }, ui);
        web.postWebMessage(new WebMessage("dhd-bridge", new WebMessagePort[] { channel[1] }), Uri.parse(ORIGIN));
    }

    /** A message to the page (any thread). */
    void post(String text) {
        ui.post(() -> {
            if (port != null && !isDestroyed()) port.postMessage(new WebMessage(text));
        });
    }

    void emit(String event, JSONObject payload) {
        JSONObject m = new JSONObject();
        try {
            m.put("event", event);
            m.put("payload", payload != null ? payload : new JSONObject());
        } catch (JSONException e) {
            return;
        }
        post(m.toString());
    }

    /** PLAY: the game, and this activity ends. checked: the page showed the update line, so the game
     *  doesn't check again.
     *  edit: the touch layout editor alone (findings 226, 228): the game opens only the editor and
     *  ends after Done (host_end, as EXIT GAME). This activity then stays below it, stopped, with its
     *  state saved: when the game's process ends, Android takes the dead game's activity away and
     *  starts the one under it again in a new process, so the player is back here (onCreate with a
     *  saved state: BACK_FROM_EDITOR) and the next PLAY starts a new game. */
    void play(boolean edit, boolean checked) {
        Intent game = new Intent(this, DhdActivity.class);
        if (edit) game.putExtra(DhdActivity.EDIT_TOUCH_LAYOUT, true);
        if (checked) game.putExtra(DhdActivity.LAUNCHER_CHECKED, true);
        editing = edit;
        startActivity(game);
        if (!edit) finish();
        overridePendingTransition(0, 0);
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putBoolean(BACK_FROM_EDITOR, editing);
    }

    /** Android's document picker for the disc image. */
    boolean pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");  // .iso files have no MIME type everywhere
        try {
            startActivityForResult(intent, PICK);
            return true;
        } catch (ActivityNotFoundException e) {
            return false;
        }
    }

    void shareLog() {
        LogShare.start(this, SHARE, null);
    }

    boolean open(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            return true;
        } catch (Throwable e) {
            Log.w(TAG, "no app opens " + url + ": " + e);
            return false;
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == SHARE) {  // back from the share sheet: the shortcut's start ends
            if (sharingOnly) finish();
            return;
        }
        if (request == PICK && bridge != null)
            bridge.picked(result == RESULT_OK && data != null ? data.getData() : null);
    }

    @Override
    protected void onPause() {
        super.onPause();
        paused = true;
    }

    @Override
    protected void onResume() {
        super.onResume();
        // back from the share sheet, the picker or a browser: the page reads the settings again
        if (paused && port != null) emit("resumed", null);
        paused = false;
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideBars();
    }

    @Override
    protected void onDestroy() {
        if (bridge != null) bridge.close();
        work.shutdownNow();
        if (web != null) {
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }

    /** A plain screen of the launcher's colour until the page (and behind the share sheet). It also
     *  gives the window its decor view, which getInsetsController needs (finding 152). */
    private void backdrop() {
        Window w = getWindow();
        w.setBackgroundDrawableResource(R.color.ground);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(GROUND);
        setContentView(root);
        hideBars();
    }

    private void hideBars() {
        Screen.hideBars(getWindow());
    }

    File files() {
        return getFilesDir();
    }
}
