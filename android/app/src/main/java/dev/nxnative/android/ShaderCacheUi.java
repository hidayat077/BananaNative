package dev.nxnative.android;

import android.app.ProgressDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.widget.LinearLayout;
import android.widget.Toast;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Per-game Misc tab section for managing compiled shader caches (vk_gpl_cache_<id>_*.bin and prewarm .keys/.bin files):
 * Export (.zip), Import (.zip or raw .bin), and Delete.
 */
final class ShaderCacheUi {
    static final int PICK_EXPORT = 80, PICK_IMPORT = 81;
    private static final int GPLC_MAGIC = 0x434C5047;   // "GPLC" little-endian (vka_gpl_cache.h)
    private static final int GPLC_HDR_SIZE = 80;
    private static final int GPLC_F_COMPLETE = 1;
    private static Game pendingFor;

    private ShaderCacheUi() {}

    static File dir(MainActivity a, String titleId) {
        return new File(a.getExternalFilesDir(null), "temp/" + titleId);
    }

    private static boolean isShaderCacheFile(String name, String titleId) {
        if (name == null) return false;
        String gplPrefix = "vk_gpl_cache_" + titleId + "_";
        String retypePrefix = "vk_gpl_retype_cache_" + titleId + "_";
        String monoPrefix = "vk_monolithic_cache_" + titleId + "_";
        String svarPrefix = "vk_shader_variants_" + titleId + "_";
        if (name.startsWith(gplPrefix) && (name.endsWith(".bin") || name.endsWith(".bin.tmp"))) return true;
        if (name.startsWith(retypePrefix) && (name.endsWith(".bin") || name.endsWith(".bin.tmp") || name.endsWith(".bin.heavy"))) return true;
        if (name.startsWith(monoPrefix) && (name.endsWith(".bin") || name.endsWith(".bin.tmp") || name.endsWith(".bin.heavy") || name.endsWith(".bin.busy") || name.endsWith(".bin.unfinished"))) return true;
        if (name.startsWith(svarPrefix) && (name.endsWith(".bin") || name.endsWith(".bin.tmp"))) return true;
        return name.startsWith("vk_pipelines") && (name.endsWith(".bin") || name.endsWith(".bin.lib") || name.endsWith(".keys") || name.endsWith(".bin.heavy"));
    }

    private static List<File> listFiles(File dir, String titleId, boolean includeTmp) {
        List<File> out = new ArrayList<>();
        File[] all = dir.listFiles();
        if (all == null) return out;
        for (File f : all) {
            if (!f.isFile()) continue;
            String nm = f.getName();
            if (!includeTmp && nm.endsWith(".tmp")) continue;
            if (isShaderCacheFile(nm, titleId)) out.add(f);
        }
        return out;
    }

    private static String summary(MainActivity a, String titleId) {
        List<File> files = listFiles(dir(a, titleId), titleId, false);
        if (files.isEmpty()) return Tx.s(R.string.game_shader_cache_none);
        long bytes = 0;
        int gplCount = 0;
        int completeCount = 0;
        int compiled = 0, total = 0;
        for (File f : files) {
            bytes += f.length();
            if (f.getName().startsWith("vk_gpl_cache_" + titleId + "_") && f.getName().endsWith(".bin")) {
                gplCount++;
                int[] hdr = readGplcHeader(f);
                if (hdr != null) {
                    if ((hdr[0] & GPLC_F_COMPLETE) != 0) completeCount++;
                    compiled = hdr[1];
                    total = hdr[2];
                }
            }
        }
        String mb = String.format(Locale.US, "%.1f MB", bytes / (1024.0 * 1024.0));
        if (gplCount > 0 && total > 0) {
            String st = completeCount > 0
                    ? Tx.s(R.string.game_shader_cache_complete)
                    : Tx.s(R.string.game_shader_cache_partial, compiled, total);
            return mb + " · " + st;
        }
        return mb + " (" + files.size() + ")";
    }

    /** Returns {flags, compiled_entries, total_entries} if f starts with a valid GPLC header, else null. */
    private static int[] readGplcHeader(File f) {
        if (f.length() < GPLC_HDR_SIZE) return null;
        byte[] buf = new byte[GPLC_HDR_SIZE];
        try (FileInputStream in = new FileInputStream(f)) {
            int n = 0;
            while (n < GPLC_HDR_SIZE) {
                int r = in.read(buf, n, GPLC_HDR_SIZE - n);
                if (r <= 0) return null;
                n += r;
            }
        } catch (IOException e) {
            return null;
        }
        ByteBuffer bb = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN);
        if (bb.getInt(0) != GPLC_MAGIC) return null;
        return new int[] {bb.getInt(20), bb.getInt(52), bb.getInt(48)};
    }

    static void section(MainActivity a, LinearLayout content, Game g, Runnable refresh) {
        Ui.add(content, Ui.section(a, Tx.s(R.string.game_compiled_shaders)), 16);
        LinearLayout card = Ui.add(content, Ui.listCard(a), 6);
        String sum = summary(a, g.titleId);
        card.addView(new Ui.Setting(a, "📤", Tx.s(R.string.game_export_compiled_shaders), v -> exportPick(a, g))
                .value(sum).chevron(a).view);
        card.addView(new Ui.Setting(a, "📥", Tx.s(R.string.game_import_compiled_shaders), v -> importPick(a, g))
                .value(Tx.s(R.string.game_import_compiled_shaders_desc)).chevron(a).view);
        card.addView(new Ui.Setting(a, "🗑️", Tx.s(R.string.game_delete_compiled_shaders), v -> confirmDelete(a, g, refresh))
                .value(Tx.s(R.string.game_delete_compiled_shaders_desc)).chevron(a).view);
    }

    private static void exportPick(MainActivity a, Game g) {
        List<File> files = listFiles(dir(a, g.titleId), g.titleId, false);
        if (files.isEmpty()) {
            Toast.makeText(a, Tx.s(R.string.game_shader_cache_none), Toast.LENGTH_SHORT).show();
            return;
        }
        pendingFor = g;
        String ts = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/zip")
                .putExtra(Intent.EXTRA_TITLE, "shader-cache-" + g.titleId + "-" + ts + ".zip");
        a.startActivityForResult(i, PICK_EXPORT);
    }

    private static void importPick(MainActivity a, Game g) {
        pendingFor = g;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        a.startActivityForResult(i, PICK_IMPORT);
    }

    private static void confirmDelete(MainActivity a, Game g, Runnable refresh) {
        List<File> files = listFiles(dir(a, g.titleId), g.titleId, true);
        if (files.isEmpty()) {
            Toast.makeText(a, Tx.s(R.string.game_shader_cache_none), Toast.LENGTH_SHORT).show();
            return;
        }
        new PadFocus.Builder(a).setTitle(Tx.s(R.string.game_delete_compiled_shaders))
                .setMessage(Tx.s(R.string.game_delete_compiled_shaders_confirm, g.name))
                .setPositiveButton(Tx.s(R.string.game_delete_compiled_shaders), (d, w) -> {
                    int n = 0;
                    for (File f : files) if (f.delete()) n++;
                    Toast.makeText(a, Tx.s(R.string.game_shader_cache_deleted, n), Toast.LENGTH_SHORT).show();
                    refresh.run();
                })
                .setNegativeButton(Tx.s(R.string.common_cancel), null).show();
    }

    static boolean documentResult(MainActivity a, int request, Uri doc) {
        if (request != PICK_EXPORT && request != PICK_IMPORT) return false;
        Game g = pendingFor;
        pendingFor = null;
        if (g == null || doc == null) return true;
        if (request == PICK_EXPORT) runExport(a, g, doc);
        else runImport(a, g, doc);
        return true;
    }

    private static OutputStream openOut(ContentResolver cr, Uri doc) throws IOException {
        try {
            OutputStream os = cr.openOutputStream(doc, "wt");
            if (os != null) return os;
        } catch (IllegalArgumentException | UnsupportedOperationException ignored) {}
        OutputStream os = cr.openOutputStream(doc);
        if (os == null) throw new IOException("cannot open destination");
        return os;
    }

    private static void runExport(MainActivity a, Game g, Uri doc) {
        ProgressDialog pd = new ProgressDialog(a);
        pd.setMessage(Tx.s(R.string.game_export_compiled_shaders));
        pd.setCancelable(false);
        pd.show();
        ContentResolver cr = a.getContentResolver();
        List<File> files = listFiles(dir(a, g.titleId), g.titleId, false);
        new Thread(() -> {
            String err = null;
            int count = 0;
            try (OutputStream os = openOut(cr, doc)) {
                ZipOutputStream zos = new ZipOutputStream(new BufferedOutputStream(os));
                zos.setLevel(Deflater.NO_COMPRESSION);
                byte[] buf = new byte[65536];
                for (File f : files) {
                    zos.putNextEntry(new ZipEntry(f.getName()));
                    try (InputStream fis = new BufferedInputStream(new FileInputStream(f))) {
                        for (int n; (n = fis.read(buf)) > 0; ) zos.write(buf, 0, n);
                    }
                    zos.closeEntry();
                    count++;
                }
                zos.finish();
                zos.flush();
            } catch (IOException | RuntimeException e) {
                err = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
            }
            String finalErr = err;
            int finalCount = count;
            a.runOnUiThread(() -> {
                pd.dismiss();
                if (finalErr != null) a.error(Tx.s(R.string.game_shader_cache_failed, finalErr));
                else Toast.makeText(a, Tx.s(R.string.game_shader_cache_exported, finalCount), Toast.LENGTH_LONG).show();
            });
        }, "nx-shader-export").start();
    }

    private static void runImport(MainActivity a, Game g, Uri doc) {
        ProgressDialog pd = new ProgressDialog(a);
        pd.setMessage(Tx.s(R.string.game_import_compiled_shaders));
        pd.setCancelable(false);
        pd.show();
        ContentResolver cr = a.getContentResolver();
        File targetDir = dir(a, g.titleId);
        String pickedName = docName(cr, doc);
        new Thread(() -> {
            String err = null;
            int count = 0;
            try {
                if (!targetDir.isDirectory() && !targetDir.mkdirs()) throw new IOException("cannot create cache directory");
                byte[] head = new byte[4];
                int hlen = 0;
                try (InputStream in = cr.openInputStream(doc)) {
                    if (in == null) throw new IOException("cannot open source");
                    while (hlen < 4) {
                        int r = in.read(head, hlen, 4 - hlen);
                        if (r <= 0) break;
                        hlen += r;
                    }
                }
                if (hlen == 4 && head[0] == 'P' && head[1] == 'K') {
                    try (ZipInputStream zis = new ZipInputStream(new BufferedInputStream(cr.openInputStream(doc)))) {
                        ZipEntry ze;
                        while ((ze = zis.getNextEntry()) != null) {
                            if (ze.isDirectory()) continue;
                            String name = new File(ze.getName()).getName();
                            if (!isShaderCacheFile(name, g.titleId) || name.endsWith(".tmp")) continue;
                            File tmp = new File(targetDir, name + ".tmp");
                            File dst = new File(targetDir, name);
                            writeStream(zis, tmp);
                            if (name.startsWith("vk_gpl_cache_") && readGplcHeader(tmp) == null) {
                                tmp.delete();
                                throw new IOException("invalid GPLC header in " + name);
                            }
                            if (!tmp.renameTo(dst)) { tmp.delete(); throw new IOException("cannot replace " + name); }
                            count++;
                        }
                    }
                    if (count == 0) throw new IOException("no matching shader cache files for " + g.titleId);
                } else if (hlen == 4 && ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN).getInt(0) == GPLC_MAGIC) {
                    if (pickedName == null || !pickedName.startsWith("vk_gpl_cache_" + g.titleId + "_") || !pickedName.endsWith(".bin")) {
                        throw new IOException("filename must match vk_gpl_cache_" + g.titleId + "_*.bin");
                    }
                    File tmp = new File(targetDir, pickedName + ".tmp");
                    File dst = new File(targetDir, pickedName);
                    try (InputStream in = cr.openInputStream(doc)) {
                        writeStream(in, tmp);
                    }
                    if (readGplcHeader(tmp) == null) { tmp.delete(); throw new IOException("invalid GPLC header"); }
                    if (!tmp.renameTo(dst)) { tmp.delete(); throw new IOException("cannot write " + pickedName); }
                    count = 1;
                } else {
                    throw new IOException("not a shader cache .zip or .bin file");
                }
            } catch (IOException | RuntimeException e) {
                err = e.getMessage();
            }
            String finalErr = err;
            int finalCount = count;
            a.runOnUiThread(() -> {
                pd.dismiss();
                if (finalErr != null) a.error(Tx.s(R.string.game_shader_cache_failed, finalErr));
                else {
                    Toast.makeText(a, Tx.s(R.string.game_shader_cache_imported, finalCount), Toast.LENGTH_LONG).show();
                    a.refresh();
                }
            });
        }, "nx-shader-import").start();
    }

    private static void writeStream(InputStream in, File out) throws IOException {
        byte[] buf = new byte[65536];
        try (OutputStream os = new BufferedOutputStream(new FileOutputStream(out))) {
            for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
        }
    }

    private static String docName(ContentResolver cr, Uri doc) {
        try (Cursor c = cr.query(doc, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (RuntimeException ignored) {}
        return null;
    }
}
