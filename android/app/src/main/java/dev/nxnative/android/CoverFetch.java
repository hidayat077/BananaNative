package dev.nxnative.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Automatic cover art fetcher for games whose dump has no local icon and no player-picked cover.
 * Hybrid lookup without any API key:
 *   1. Direct 1:1 icon by 16-hex Title ID (tinfoil.media, covers Switch and Switch 2 titles).
 *   2. Fallback search by game title on Nintendo Europe's public search endpoint (1:1 square art).
 * Fetched covers are saved through {@link Covers#save} so subsequent opens load offline from disk
 * and never overwrite a cover the player already set.
 */
final class CoverFetch {
    private CoverFetch() {}

    interface Callback { void onCover(Bitmap b); }

    /** Public facts about a game from Nintendo Europe's search (title, publisher, release date, description, categories) */
    static final class Info {
        final String title, publisher, released, about, categories;
        Info(String title, String publisher, String released, String about, String categories) {
            this.title = title; this.publisher = publisher; this.released = released; this.about = about; this.categories = categories;
        }
    }

    /** Looks a game up by title ID (lower case, as the API holds it), then by name; done is called on the main thread, null when offline or not found. */
    static void info(String titleId, String name, Consumer<Info> done) {
        String key = titleId == null ? name : titleId;
        synchronized (infos) {
            if (infos.containsKey(key)) { MAIN.post(() -> done.accept(infos.get(key))); return; }
            if (infoMissed.contains(key)) { MAIN.post(() -> done.accept(null)); return; }
        }
        POOL.execute(() -> {
            Info info = null;
            try {
                JSONObject doc = searchNintendo(titleId, name);
                if (doc != null) {
                    JSONArray cats = doc.optJSONArray("pretty_game_categories_txt");
                    StringBuilder cs = new StringBuilder();
                    if (cats != null) for (int i = 0; i < cats.length(); i++) { if (cs.length() > 0) cs.append(", "); cs.append(cats.optString(i)); }
                    info = new Info(doc.optString("title", ""), doc.optString("publisher", ""), doc.optString("pretty_date_s", ""),
                            doc.optString("excerpt", ""), cs.toString());
                }
            } catch (Throwable ignored) {
                info = null;
            }
            synchronized (infos) { if (info != null) infos.put(key, info); else infoMissed.add(key); }
            Info ready = info;
            MAIN.post(() -> done.accept(ready));
        });
    }

    private static JSONObject searchNintendo(String titleId, String name) {
        try {
            JSONObject doc = null;
            if (isTitleId(titleId))
                doc = search("https://searching.nintendo-europe.com/en/select?q=*:*&fq=type:GAME&fq=application_id_s:" + titleId.toLowerCase(Locale.US) + "&rows=1&wt=json");
            String clean = cleanName(name);
            if (doc == null && !clean.isEmpty())
                doc = search("https://searching.nintendo-europe.com/en/select?q=" + URLEncoder.encode(clean, "UTF-8") + "&fq=type:GAME&rows=1&wt=json");
            return doc;
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static boolean isTitleId(String titleId) {
        if (titleId == null || titleId.length() != 16) return false;
        for (int i = 0; i < 16; i++) {
            char ch = titleId.charAt(i);
            boolean hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
            if (!hex) return false;
        }
        return true;
    }

    private static String cleanName(String name) {
        if (name == null) return "";
        return name.replace("™", "").replace("®", "").replace("©", "").trim();
    }

    private static JSONObject search(String url) {
        byte[] bytes = httpGet(url, MAX_JSON_BYTES);
        if (bytes == null) return null;
        try {
            JSONObject root = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
            JSONObject resp = root.optJSONObject("response");
            JSONArray docs = resp != null ? resp.optJSONArray("docs") : null;
            return docs != null && docs.length() > 0 ? docs.optJSONObject(0) : null;
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static final int CONNECT_MS = 8000;
    private static final int READ_MS = 10000;
    private static final int MAX_IMAGE_BYTES = 4 * 1024 * 1024;
    private static final int MAX_JSON_BYTES = 256 * 1024;
    private static final String USER_AGENT = "Polar/1.0";

    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final ExecutorService POOL = Executors.newFixedThreadPool(2, r -> {
        Thread t = new Thread(r, "polar-cover-fetch");
        t.setDaemon(true);
        return t;
    });

    private static final Map<String, List<Callback>> inFlight = new HashMap<>();
    private static final Set<String> missed = new HashSet<>();
    private static final Map<String, List<Callback>> heroInFlight = new HashMap<>();
    private static final Set<String> heroMissed = new HashSet<>();
    private static final LruCache<String, Bitmap> heroMem = new LruCache<>(8);
    private static final Map<String, Info> infos = new HashMap<>();
    private static final Set<String> infoMissed = new HashSet<>();

    /**
     * Fetches a landscape hero banner (from disk cache first, then Nintendo Europe's 2:1/16:9
     * wishlist/share banners, with a Steam storefront header fallback) and invokes {@code cb} on
     * the main thread. Does nothing when offline or no landscape banner is found.
     */
    static void hero(Context c, String titleId, String name, Callback cb) {
        if (c == null || cb == null) return;
        String key = heroKey(titleId, name);
        if (key.isEmpty()) return;
        Bitmap cached = heroMem.get(key);
        if (cached != null && !cached.isRecycled()) { cb.onCover(cached); return; }
        Context app = c.getApplicationContext();
        synchronized (heroInFlight) {
            if (heroMissed.contains(key)) return;
            List<Callback> waiters = heroInFlight.get(key);
            if (waiters != null) { waiters.add(cb); return; }
            waiters = new ArrayList<>(1);
            waiters.add(cb);
            heroInFlight.put(key, waiters);
        }
        POOL.execute(() -> {
            Bitmap bmp = null;
            try {
                File file = new File(new File(app.getCacheDir(), "heroes"), key + ".jpg");
                if (file.isFile() && file.length() > 128) {
                    bmp = decodeLandscape(BitmapFactory.decodeFile(file.getAbsolutePath()));
                }
                if (bmp == null) {
                    bmp = fetchLandscapeBanner(titleId, name, file);
                }
                if (bmp != null) heroMem.put(key, bmp);
            } catch (Throwable ignored) {
                bmp = null;
            }
            List<Callback> waiters;
            synchronized (heroInFlight) {
                waiters = heroInFlight.remove(key);
                if (bmp == null) heroMissed.add(key);
            }
            if (bmp != null && waiters != null && !waiters.isEmpty()) {
                Bitmap ready = bmp;
                MAIN.post(() -> {
                    for (Callback w : waiters) w.onCover(ready);
                });
            }
        });
    }

    private static String heroKey(String titleId, String name) {
        if (isTitleId(titleId)) return titleId.toLowerCase(Locale.US);
        String clean = cleanName(name).toLowerCase(Locale.US);
        return clean.isEmpty() ? "" : Integer.toHexString(clean.hashCode());
    }

    private static Bitmap fetchLandscapeBanner(String titleId, String name, File cacheFile) {
        String[] urls = {
                nintendoBannerUrl(titleId, name),
                steamBannerUrl(name)
        };
        for (String url : urls) {
            if (url == null || url.isEmpty()) continue;
            if (url.startsWith("//")) url = "https:" + url;
            byte[] data = httpGet(url, MAX_IMAGE_BYTES);
            if (data == null || data.length < 128) continue;
            Bitmap bmp = decodeLandscape(BitmapFactory.decodeByteArray(data, 0, data.length));
            if (bmp != null) {
                saveHeroCache(cacheFile, bmp);
                return bmp;
            }
        }
        return null;
    }

    private static String nintendoBannerUrl(String titleId, String name) {
        JSONObject doc = searchNintendo(titleId, name);
        if (doc == null) return null;
        String[] fields = {
                "wishlist_email_banner640w_image_url_s",
                "image_url_h2x1_s",
                "wishlist_email_banner460w_image_url_s"
        };
        for (String f : fields) {
            String u = doc.optString(f, "");
            if (!u.isEmpty()) return u;
        }
        return null;
    }

    private static String steamBannerUrl(String name) {
        String clean = cleanName(name);
        if (clean.isEmpty()) return null;
        try {
            String url = "https://store.steampowered.com/api/storesearch/?term="
                    + URLEncoder.encode(clean, "UTF-8") + "&l=english&cc=US";
            byte[] bytes = httpGet(url, MAX_JSON_BYTES);
            if (bytes == null) return null;
            JSONObject root = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
            JSONArray items = root.optJSONArray("items");
            if (items == null || items.length() == 0) return null;
            String needle = normTitle(clean);
            for (int i = 0; i < Math.min(items.length(), 3); i++) {
                JSONObject item = items.optJSONObject(i);
                if (item == null) continue;
                String itemNorm = normTitle(item.optString("name", ""));
                if (itemNorm.isEmpty()) continue;
                if (itemNorm.equals(needle) || itemNorm.contains(needle) || needle.contains(itemNorm)) {
                    int appId = item.optInt("id", 0);
                    if (appId > 0)
                        return "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/" + appId + "/header.jpg";
                }
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private static String normTitle(String s) {
        return s.toLowerCase(Locale.US).replaceAll("[^a-z0-9]", "");
    }

    private static Bitmap decodeLandscape(Bitmap raw) {
        if (raw == null) return null;
        int w = raw.getWidth(), h = raw.getHeight();
        if (w < 200 || h < 100 || w <= h) {
            raw.recycle();
            return null;
        }
        return raw;
    }

    private static void saveHeroCache(File dst, Bitmap bmp) {
        File dir = dst.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) return;
        File tmp = new File(dst.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            if (bmp.compress(Bitmap.CompressFormat.JPEG, 90, out)) {
                out.flush();
                if (!tmp.renameTo(dst)) tmp.delete();
            } else {
                tmp.delete();
            }
        } catch (IOException ignored) {
            tmp.delete();
        }
    }

    /** Fetches a cover in the background if the game has none yet; invokes cb on the main thread when saved. */
    static void fetchIfMissing(Context c, Game g, AtomicBoolean hasLocalIcon, Callback cb) {
        if (c == null || g == null) return;
        Context app = c.getApplicationContext();
        String id = g.orderId();
        if (Covers.has(app, g) || (hasLocalIcon != null && hasLocalIcon.get())) return;
        synchronized (inFlight) {
            if (missed.contains(id)) return;
            List<Callback> waiters = inFlight.get(id);
            if (waiters != null) {
                if (cb != null) waiters.add(cb);
                return;
            }
            waiters = new ArrayList<>(1);
            if (cb != null) waiters.add(cb);
            inFlight.put(id, waiters);
        }
        POOL.execute(() -> {
            Bitmap bmp = null;
            try {
                if (!Covers.has(app, g) && (hasLocalIcon == null || !hasLocalIcon.get())) {
                    bmp = fetchByTitleId(g.titleId);
                    if (bmp == null) bmp = fetchByName(g.name);
                    if (bmp != null && !Covers.has(app, g) && (hasLocalIcon == null || !hasLocalIcon.get())) {
                        if (!Covers.save(app, id, bmp)) bmp = null;
                    } else if (bmp != null) {
                        bmp.recycle();
                        bmp = null;
                    }
                }
            } catch (Throwable ignored) {
                bmp = null;
            }
            List<Callback> waiters;
            synchronized (inFlight) {
                waiters = inFlight.remove(id);
                if (bmp == null && (hasLocalIcon == null || !hasLocalIcon.get())) missed.add(id);
            }
            if (bmp != null && waiters != null && !waiters.isEmpty()) {
                Bitmap ready = bmp;
                MAIN.post(() -> {
                    for (Callback w : waiters) w.onCover(ready);
                });
            }
        });
    }

    /** Tier 1: exact 1:1 icon lookup by 16-hex Title ID. */
    private static Bitmap fetchByTitleId(String titleId) {
        if (titleId == null || titleId.length() != 16) return null;
        for (int i = 0; i < 16; i++) {
            char ch = titleId.charAt(i);
            boolean hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
            if (!hex) return null;
        }
        String url = "https://tinfoil.media/ti/" + titleId.toUpperCase(Locale.US) + "/512/512/";
        byte[] data = httpGet(url, MAX_IMAGE_BYTES);
        return decodeSquare(data);
    }

    /** Tier 2: fallback search by game name via Nintendo Europe's public search (no API key). */
    private static Bitmap fetchByName(String name) {
        if (name == null) return null;
        String clean = name.replace("™", "").replace("®", "").replace("©", "").trim();
        if (clean.isEmpty()) return null;
        try {
            String url = "https://searching.nintendo-europe.com/en/select?q="
                    + URLEncoder.encode(clean, "UTF-8")
                    + "&fq=type:GAME&rows=3&wt=json";
            byte[] jsonBytes = httpGet(url, MAX_JSON_BYTES);
            if (jsonBytes == null) return null;
            JSONObject root = new JSONObject(new String(jsonBytes, StandardCharsets.UTF_8));
            JSONObject resp = root.optJSONObject("response");
            JSONArray docs = resp != null ? resp.optJSONArray("docs") : null;
            if (docs == null || docs.length() == 0) return null;

            String imgUrl = null;
            for (int i = 0; i < docs.length(); i++) {
                JSONObject doc = docs.optJSONObject(i);
                if (doc == null) continue;
                String sq = doc.optString("image_url_sq_s", "");
                if (!sq.isEmpty()) { imgUrl = sq; break; }
            }
            if (imgUrl == null) {
                for (int i = 0; i < docs.length(); i++) {
                    JSONObject doc = docs.optJSONObject(i);
                    if (doc == null) continue;
                    String wide = doc.optString("image_url_h2x1_s", "");
                    if (!wide.isEmpty()) { imgUrl = wide; break; }
                }
            }
            if (imgUrl == null) return null;
            if (imgUrl.startsWith("//")) imgUrl = "https:" + imgUrl;
            byte[] imgBytes = httpGet(imgUrl, MAX_IMAGE_BYTES);
            return decodeSquare(imgBytes);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static byte[] httpGet(String urlStr, int maxBytes) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(urlStr).openConnection();
            conn.setInstanceFollowRedirects(true);
            conn.setConnectTimeout(CONNECT_MS);
            conn.setReadTimeout(READ_MS);
            conn.setRequestProperty("User-Agent", USER_AGENT);
            if (conn.getResponseCode() != HttpURLConnection.HTTP_OK) return null;
            int len = conn.getContentLength();
            if (len > maxBytes) return null;
            try (InputStream in = conn.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream(len > 0 ? len : 16384);
                byte[] buf = new byte[8192];
                int total = 0, n;
                while ((n = in.read(buf)) > 0) {
                    total += n;
                    if (total > maxBytes) return null;
                    out.write(buf, 0, n);
                }
                return out.toByteArray();
            }
        } catch (IOException ignored) {
            return null;
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    /** Decodes image bytes, center-crops to square if needed, and caps size at {@link Covers#MAX}. */
    private static Bitmap decodeSquare(byte[] data) {
        if (data == null || data.length < 128) return null;
        Bitmap raw = BitmapFactory.decodeByteArray(data, 0, data.length);
        if (raw == null) return null;
        int w = raw.getWidth(), h = raw.getHeight();
        if (w < 64 || h < 64) { raw.recycle(); return null; }
        Bitmap sq = raw;
        if (w != h) {
            int side = Math.min(w, h);
            sq = Bitmap.createBitmap(raw, (w - side) / 2, (h - side) / 2, side, side);
            if (sq != raw) raw.recycle();
        }
        int side = sq.getWidth();
        if (side > Covers.MAX) {
            Bitmap scaled = Bitmap.createScaledBitmap(sq, Covers.MAX, Covers.MAX, true);
            if (scaled != sq) sq.recycle();
            sq = scaled;
        }
        return sq;
    }
}
