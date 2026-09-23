package dev.siggemckvack.dualscreen;

import android.app.Activity;
import android.util.Log;
import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebStorage;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.RelativeLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/**
 * In-app browser for grabbing a walkthrough page.
 *
 * <p>Why a WebView: the sites worth reading sit behind bot protection that
 * answers a plain HTTP client with 403 (zeldadungeon.net refuses even
 * robots.txt). A WebView is a real browser, so it is served normally. No
 * User-Agent is spoofed here.
 *
 * <p>Why an overlay rather than its own Activity: starting an Activity sends
 * the game activity through onPause, which dismisses the second-screen
 * Presentation (see CompanionManager) underneath a render worker that may be
 * presenting to it. Adding the WebView to the existing Activity keeps the game
 * running, the second display attached, and SDL out of its pause/resume path.
 */
public final class GuideBrowser {

    private static final String TAG = "dusk-guide";

    private static FrameLayout sRoot;
    private static final java.util.ArrayList<String> sQueue = new java.util.ArrayList<>();
    private static int sQueueAt = -1;  // -1 = not crawling
    // See onPageFinished: guards against parallel settle chains.
    private static int sSettleGen = 0;
    private static int sSaved;
    // A walkthrough is tens of pages, not hundreds; the cap stops a bad link
    // filter from walking an entire site.
    private static final int MAX_PAGES = 60;
    private static String sAssetStem;   // <stem>_files dir for the page being saved

    /**
     * Side channel for image bytes.
     *
     * <p>They must NOT ride inside the DOM: evaluateJavascript returns its
     * result as one JSON string, and Android truncates results as large as a
     * chapter with inlined photos, cutting the saved page short. Each image is
     * handed over separately here and written to <stem>_files, the layout a
     * browser "save page" produces and the importer already reads.
     */
    public static final class Bridge {
        private final Activity host;
        Bridge(Activity h) { host = h; }

        @JavascriptInterface
        public void image(String name, String dataUrl) {
            try {
                int comma = dataUrl.indexOf(',');
                if (comma < 0 || sAssetStem == null) {
                    return;
                }
                // `name` arrives from JavaScript, and addJavascriptInterface
                // exposes this to EVERY frame of every page loaded — including
                // third-party iframes. Unsanitised it is an arbitrary-file-write
                // primitive: new File(dir, "../../../x") resolves normally, and
                // this app holds MANAGE_EXTERNAL_STORAGE. Whitelist the shape
                // the injector actually produces and nothing else.
                if (!name.matches("^img[0-9]{1,4}\\.jpg$")) {
                    Log.e(TAG, "rejected image name: " + name);
                    return;
                }
                byte[] bytes = android.util.Base64.decode(
                        dataUrl.substring(comma + 1), android.util.Base64.DEFAULT);
                // Bound it too: JS can hand over unlimited base64 in a loop.
                if (bytes.length > 4 * 1024 * 1024) {
                    return;
                }
                File dir = new File(guidesImportDir(host), sAssetStem + "_files");
                if (!dir.exists() && !dir.mkdirs()) {
                    return;
                }
                final File out = new File(dir, name);
                if (!out.getCanonicalPath().startsWith(dir.getCanonicalPath())) {
                    Log.e(TAG, "rejected out-of-tree image path");
                    return;
                }
                try (FileOutputStream f = new FileOutputStream(out)) {
                    f.write(bytes);
                }
            } catch (Throwable t) {
                Log.e(TAG, "image() failed", t);
            }
        }
    }
    private static WebView sWebView;
    private static TextView sStatus;

    private GuideBrowser() {}

    /**
     * MUST be called when the mod shuts down (CompanionManager.stop): sRoot/sWebView are static
     * and hold the Activity, so without this a destroyed Activity leaks and
     * show() sees a non-null sRoot on relaunch into the same process and
     * returns "already open" forever.
     */
    public static void shutdown() {
        teardown();  // no reapplyImmersive: the Activity is going away
    }

    /**
     * Absolute path of the guide store, as the NATIVE side computes it.
     *
     * <p>Not derived here, so pages are always written where the importer looks;
     * the local fallback in guidesImportDir is only for when the native call fails.
     */
    private static native String nativeGuidesRoot();

    /** Asks the game to scan the import folder. Safe to call more than once. */
    private static native void nativeGuidesImport();

    private static void requestImport() {
        try {
            nativeGuidesImport();
        } catch (Throwable t) {
            Log.e(TAG, "could not request an import", t);
        }
    }

    private static File guidesImportDir(Activity host) {
        try {
            final String root = nativeGuidesRoot();
            if (root != null && !root.isEmpty()) {
                return new File(root, "import");
            }
        } catch (Throwable t) {
            Log.e(TAG, "nativeGuidesRoot unavailable, using the default location", t);
        }
        return new File(host.getExternalFilesDir(null), "guides/import");
    }

    /** Called from native (jni_bridge.cpp, open_guide_browser). Safe from any thread. */
    public static void open(final Activity host, final String startUrl) {
        if (host == null) {
            return;
        }
        Log.i(TAG, "open() called from native");
        host.runOnUiThread(() -> {
            try {
                show(host, startUrl);
            } catch (Throwable t) {
                // A throw here would otherwise vanish onto the UI thread and
                // look exactly like "nothing happened".
                Log.e(TAG, "show() failed", t);
            }
        });
    }

    /**
     * Re-applies immersive fullscreen.
     *
     * <p>Adding a focusable WebView makes the system bars — the gesture pill —
     * reappear, and SDL only re-hides through its own private handler, which
     * this overlay sits outside of. Both showing and closing must put the
     * flags back or the pill is left stranded over the game.
     */
    private static void reapplyImmersive(final Activity host) {
        if (host == null) {
            return;
        }
        host.runOnUiThread(() -> {
            try {
                // The version-aware implementation (pre-R setSystemUiVisibility
                // flags alone are a no-op at targetSdk 30+).
                CompanionPresentation.applyImmersive(host.getWindow());
            } catch (Throwable t) {
                Log.e(TAG, "reapplyImmersive failed", t);
            }
        });
    }

    private static void show(Activity host, String startUrl) {
        Log.i(TAG, "show() on UI thread, url=" + startUrl);
        if (sRoot != null) {
            Log.i(TAG, "already open");
            return;
        }
        FrameLayout root = new FrameLayout(host);
        root.setBackgroundColor(Color.BLACK);
        // Swallow touches that miss a child, so taps never fall through to the
        // game surface underneath.
        root.setClickable(true);

        LinearLayout column = new LinearLayout(host);
        column.setOrientation(LinearLayout.VERTICAL);

        LinearLayout bar = new LinearLayout(host);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackgroundColor(0xFF1A1814);
        bar.setGravity(Gravity.CENTER_VERTICAL);

        Button all = new Button(host);
        all.setText("Save whole guide");
        all.setOnClickListener(v -> saveWholeGuide(host));
        bar.addView(all);

        Button close = new Button(host);
        close.setText("Close");
        close.setOnClickListener(v -> close(host));
        bar.addView(close);

        sStatus = new TextView(host);
        sStatus.setTextColor(0xFFF0E8D0);
        sStatus.setPadding(16, 0, 16, 0);
        bar.addView(sStatus);

        column.addView(bar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        WebView web = new WebView(host);
        // Start every session with an empty cookie jar. The WebView shares the
        // app-wide persistent jar, and crawls accumulate cookies across runs
        // until the site answers "400 Request Header Or Cookie Too Large",
        // which the user cannot clear without wiping app data. Nothing here
        // needs a session (there is no login).
        try {
            CookieManager cookies = CookieManager.getInstance();
            cookies.removeAllCookies(null);
            cookies.setAcceptThirdPartyCookies(web, false);  // ads/analytics are the bulk of it
            cookies.flush();
            WebStorage.getInstance().deleteAllData();
        } catch (Throwable t) {
            Log.e(TAG, "could not clear browser storage", t);
        }
        web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setDomStorageEnabled(true);
        web.getSettings().setLoadWithOverviewMode(true);
        web.getSettings().setUseWideViewPort(true);
        web.getSettings().setBuiltInZoomControls(true);
        web.getSettings().setDisplayZoomControls(false);
        web.addJavascriptInterface(new Bridge(host), "DuskBridge");
        // Keep navigation in this view; handing off to an external browser
        // would put the page somewhere we cannot read the DOM from.
        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onReceivedError(WebView view, android.webkit.WebResourceRequest req,
                    android.webkit.WebResourceError err) {
                // Without this a dead host stalls the crawl permanently: no
                // onPageFinished ever arrives, sQueueAt stays >= 0, and even
                // Save is blocked by its own in-progress guard.
                if (sQueueAt >= 0 && req != null && req.isForMainFrame()) {
                    Log.e(TAG, "page failed, skipping: " + req.getUrl());
                    advanceCrawl();
                }
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                // Only meaningful mid-crawl: each finished page is saved, then
                // the next is loaded. Chaining off the load callback is what
                // makes a multi-page guide one button instead of N.
                if (sQueueAt >= 0) {
                    // onPageFinished is not once-per-page: a redirect or an
                    // in-page navigation fires it again. Parallel settle chains
                    // would each save the page and advance the crawl, skipping
                    // the next page; the token keeps only the newest alive.
                    sSettleGen++;
                    // Not a fixed delay: onPageFinished fires before this site
                    // has built its article body. Wait for the DOM to stop
                    // growing instead.
                    waitForSettle(host, 0, 0, sSettleGen);
                }
            }
        });
        column.addView(web, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0f));

        root.addView(column, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // Added to SDL's own layout, where SDL overlays its text-input view the
        // same way, so it reliably draws above the game surface. addContentView
        // (the fallback) can leave the overlay invisible while it still
        // swallows touch.
        // Reflection: SDLActivity is in the app, not on this DEX's compile classpath (it resolves
        // at runtime through the app class loader this DEX is parented to).
        View sdlLayout = null;
        try {
            sdlLayout = (View) Class.forName("org.libsdl.app.SDLActivity")
                    .getMethod("getContentView").invoke(null);
        } catch (ReflectiveOperationException | ClassCastException e) {
            Log.w(TAG, "SDLActivity.getContentView unavailable", e);
        }
        if (sdlLayout instanceof ViewGroup) {
            ((ViewGroup) sdlLayout).addView(root, new RelativeLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            Log.i(TAG, "overlay attached to SDL layout");
        } else {
            host.addContentView(root, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            Log.i(TAG, "overlay attached via addContentView (fallback)");
        }
        root.bringToFront();

        sRoot = root;
        sWebView = web;
        // The WebView has just taken focus; put the bars back.
        root.postDelayed(() -> reapplyImmersive(host), 100);
        web.loadUrl(startUrl == null || startUrl.isEmpty()
                ? "https://www.zeldadungeon.net/twilight-princess-walkthrough/"
                : startUrl);
    }

    private static void close(Activity host) {
        if (sRoot == null) {
            return;
        }
        teardown();
        reapplyImmersive(host);
    }

    /**
     * Drops the overlay and every piece of static state behind it. Shared by
     * close() and shutdown() so the two cannot drift.
     */
    private static void teardown() {
        // Also on the way out: a single page saved without a crawl never
        // reaches advanceCrawl, and closing is the point the user expects it
        // to have arrived.
        requestImport();
        try {
            if (sRoot != null) {
                ViewGroup parent = (ViewGroup) sRoot.getParent();
                if (parent != null) {
                    parent.removeView(sRoot);
                }
            }
            if (sWebView != null) {
                sWebView.destroy();
            }
        } catch (Throwable t) {
            Log.e(TAG, "teardown failed", t);
        }
        sWebView = null;
        sRoot = null;
        sStatus = null;
        sAssetStem = null;
        sQueueAt = -1;
        sSettleGen = 0;
        sSaved = 0;
        sQueue.clear();
    }

    /**
     * Collects every link under the current page's own path and saves them all.
     *
     * <p>Filtered by path prefix rather than a per-site rule: a walkthrough
     * index at /twilight-princess-walkthrough/ links its chapters at
     * /twilight-princess-walkthrough/chapter-N, so "same origin, under my
     * directory" picks up the guide and leaves the rest of the site alone.
     */
    private static void saveWholeGuide(final Activity host) {
        if (sWebView == null || sQueueAt >= 0) {
            return;
        }
        final String js =
            "(function(){"
            + "var base=location.origin+location.pathname.replace(/[^/]*$/,'');"
            + "var out=[location.href.split('#')[0]];"
            + "var seen={};seen[out[0]]=1;"
            + "var a=document.querySelectorAll('a[href]');"
            + "for(var i=0;i<a.length;i++){var h=a[i].href.split('#')[0];"
            + "if(h.indexOf(base)===0&&!seen[h]){seen[h]=1;out.push(h);}}"
            + "return out.join('\\n');})();";
        sWebView.evaluateJavascript(js, value -> {
            if (sWebView == null || sStatus == null) {
                return;  // Close raced the JS result
            }
            String list = unquote(value);
            sQueue.clear();
            for (String u : list.split("\n")) {
                if (!u.trim().isEmpty() && sQueue.size() < MAX_PAGES) {
                    sQueue.add(u.trim());
                }
            }
            if (sQueue.isEmpty()) {
                sStatus.setText("No linked pages found here.");
                return;
            }
            sQueueAt = 0;
            sSaved = 0;
            Log.i(TAG, "crawl: " + sQueue.size() + " pages");
            sStatus.setText("Saving 1/" + sQueue.size() + "...");
            sWebView.loadUrl(sQueue.get(0));
        });
    }

    private static void savePage(final Activity host) {
        savePage(host, false);
    }

    private static void savePage(final Activity host, final boolean crawling) {
        if (sWebView == null) {
            return;
        }
        final String pageUrl = sWebView.getUrl();
        final String title = sWebView.getTitle();
        // Hand each image to the Bridge and leave only a filename in the DOM.
        // The site answers 403 to non-browser clients for images too, so only
        // this browser can read them; a capped canvas (same-origin) also
        // downscales. The bytes leave via DuskBridge, not the returned DOM.
        sAssetStem = safeName(title, pageUrl);
        final String inline =
            "(function(){"
            + "var imgs=document.images,n=0;"
            + "for(var i=0;i<imgs.length;i++){var im=imgs[i];"
            + "try{"
            + "if(!im.naturalWidth)continue;"
            + "var w=im.naturalWidth,h=im.naturalHeight;"
            + "if(w<64||h<64)continue;"          // skip logos/spacers
            + "var m=512,s=Math.min(1,m/Math.max(w,h));"
            + "var c=document.createElement('canvas');"
            + "c.width=Math.max(1,Math.round(w*s));c.height=Math.max(1,Math.round(h*s));"
            + "c.getContext('2d').drawImage(im,0,0,c.width,c.height);"
            + "var nm='img'+i+'.jpg';"
            + "DuskBridge.image(nm,c.toDataURL('image/jpeg',0.8));"
            + "im.setAttribute('src',nm);n++;"
            + "}catch(e){}}"
            + "return ''+n;})();";
        sWebView.evaluateJavascript(inline, count -> {
            Log.i(TAG, "inlined " + count + " images");
            grabDom(host, pageUrl, title, crawling);
        });
    }

    private static void grabDom(final Activity host, final String pageUrl,
            final String title, final boolean crawling) {
        if (sWebView == null) {
            return;
        }
        // The RENDERED DOM, not the original response: scripts have run and
        // lazy content is present, which a plain fetch could never have got.
        sWebView.evaluateJavascript(
                "(function(){return document.documentElement.outerHTML;})();",
                value -> {
                    writeHtml(host, unquote(value), pageUrl, title);
                    if (crawling) {
                        advanceCrawl();
                    }
                });
    }

    /**
     * Polls until the document stops growing, then saves.
     *
     * <p>Generic on purpose: "content has arrived" is not something a fixed
     * timeout can know, and every site defers differently. Two consecutive
     * identical sizes means it has settled; the attempt cap stops a page that
     * animates forever from hanging the crawl.
     */
    private static void waitForSettle(final Activity host, final int attempt, final int lastLen,
            final int gen) {
        if (sWebView == null || sQueueAt < 0 || gen != sSettleGen) {
            return;  // closed, or superseded by a newer load of this page
        }
        if (attempt > 25) {  // ~10s, then take what we have
            savePage(host, true);
            return;
        }
        sWebView.evaluateJavascript(
                "(function(){return ''+(document.body?document.body.innerHTML.length:0);})();",
                value -> {
                    int len = 0;
                    try {
                        len = Integer.parseInt(unquote(value).trim());
                    } catch (Exception e) {
                        len = 0;
                    }
                    // Settled AND non-trivial: a shell page reports a stable
                    // small size immediately, which is exactly what we must
                    // not accept.
                    if (gen != sSettleGen) {
                        return;  // a newer load superseded this chain
                    }
                    if (len > 20000 && len == lastLen) {
                        savePage(host, true);
                        return;
                    }
                    final int captured = len;
                    if (sWebView == null) {
                        return;  // closed while polling
                    }
                    sWebView.postDelayed(
                            () -> waitForSettle(host, attempt + 1, captured, gen), 400);
                });
    }

    private static void advanceCrawl() {
        if (sQueueAt < 0 || sWebView == null) {
            return;
        }
        sSaved++;
        sQueueAt++;
        if (sQueueAt >= sQueue.size()) {
            final int n = sSaved;
            sQueueAt = -1;
            sQueue.clear();
            // Tell the game to import them NOW. It has no other way to learn
            // that the folder grew, and by this point the user considers the
            // job done.
            requestImport();
            if (sStatus != null) {
                sStatus.setText("Saved " + n + " pages. Close this to read them.");
            }
            Log.i(TAG, "crawl done, " + n + " pages");
            return;
        }
        if (sStatus != null) {
            sStatus.setText("Saving " + (sQueueAt + 1) + "/" + sQueue.size() + "...");
        }
        sWebView.loadUrl(sQueue.get(sQueueAt));
    }

    /** evaluateJavascript returns a JSON string literal, not raw HTML. */
    private static String unquote(String jsonString) {
        if (jsonString == null || jsonString.length() < 2) {
            return "";
        }
        String s = jsonString;
        if (s.charAt(0) == '"' && s.charAt(s.length() - 1) == '"') {
            s = s.substring(1, s.length() - 1);
        }
        StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c != '\\' || i + 1 >= s.length()) {
                out.append(c);
                continue;
            }
            char n = s.charAt(++i);
            switch (n) {
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case '"': out.append('"'); break;
                case '\\': out.append('\\'); break;
                case 'u':
                    if (i + 4 < s.length()) {
                        out.append((char) Integer.parseInt(s.substring(i + 1, i + 5), 16));
                        i += 4;
                    }
                    break;
                default: out.append(n); break;
            }
        }
        return out.toString();
    }

    private static void writeHtml(Activity host, String html, String pageUrl, String title) {
        if (sStatus == null) {
            return;
        }
        if (html == null || html.isEmpty()) {
            sStatus.setText("Nothing to save yet.");
            return;
        }
        try {
            File dir = guidesImportDir(host);
            if (!dir.exists() && !dir.mkdirs()) {
                sStatus.setText("Could not create the guides folder.");
                return;
            }
            File out = new File(dir, safeName(title, pageUrl) + ".html");
            try (FileOutputStream f = new FileOutputStream(out)) {
                // Source URL recorded so the importer can resolve relative
                // image links; a rendered DOM carries no other record of it.
                if (pageUrl != null && !pageUrl.isEmpty()) {
                    f.write(("<!-- dusk-source: " + pageUrl + " -->\n")
                            .getBytes(StandardCharsets.UTF_8));
                }
                f.write(html.getBytes(StandardCharsets.UTF_8));
            }
            if (sQueueAt < 0) {
                sStatus.setText("Saved. Close this to read it.");
            }
        } catch (Exception e) {
            sStatus.setText("Save failed: " + e.getMessage());
        }
    }

    private static String safeName(String title, String url) {
        String base = title != null && !title.isEmpty() ? title : url;
        if (base == null || base.isEmpty()) {
            base = "guide";
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < base.length() && sb.length() < 60; i++) {
            char c = base.charAt(i);
            boolean ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '-';
            sb.append(ok ? c : '-');
        }
        String s = sb.toString().replaceAll("-+", "-").replaceAll("^-|-$", "");
        return s.isEmpty() ? "guide" : s;
    }
}
