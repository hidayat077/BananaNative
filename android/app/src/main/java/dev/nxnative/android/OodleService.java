package dev.nxnative.android;

import android.app.Service;
import android.content.Intent;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;

/**
 * Runs the game's own Oodle/LZ5 decoder (dm/host/hle_oodle.c, DM_OODLE_SERVE=1) in a separate
 * {@code :oodle} process for on-device shader splitting ({@link PackService}).
 */
public final class OodleService extends Service {
    private static final String TAG = "nxoodle";
    static final String EXTRA_TITLE_ID = "title_id";
    static final String EXTRA_PACK_PATH = "pack_path";
    static final String EXTRA_SOCKET_NAME = "socket_name";
    static final String EXTRA_LOG_PATH = "log_path";

    private volatile boolean started;
    private LocalServerSocket server;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || started) return START_NOT_STICKY;
        started = true;
        final String titleId = intent.getStringExtra(EXTRA_TITLE_ID);
        final String packPath = intent.getStringExtra(EXTRA_PACK_PATH);
        final String socketName = intent.getStringExtra(EXTRA_SOCKET_NAME);
        final String logPath = intent.getStringExtra(EXTRA_LOG_PATH);
        Thread t = new Thread(() -> serve(titleId, packPath, socketName, logPath), "nxoodle-serve");
        t.start();
        return START_NOT_STICKY;
    }

    private void serve(String titleId, String packPath, String socketName, String logPath) {
        note(logPath, "starting the game's Oodle decoder for " + titleId);
        try {
            server = new LocalServerSocket(socketName);
            Native.load(titleId);
            LocalSocket client = server.accept();
            ParcelFileDescriptor dup = ParcelFileDescriptor.dup(client.getFileDescriptor());
            int fd = dup.detachFd();
            try { client.close(); } catch (IOException ignored) {}
            // dm/host/main.c host_preload: argv[0] is the program name, argv[1] the pack with the modules
            // (GameActivity passes the same two). With only packPath, argv[1] is null and no main is loaded.
            String[] args = {"nxhost", packPath};
            String[] env = {
                    "DM_OODLE_SERVE=1",
                    "DM_OODLE_SERVE_FD=" + fd,
                    "DM_ROMFS=" + packPath
            };
            int rc = Native.start(args, env, logPath, packPath);
            note(logPath, "the decoder ended with " + rc);
            if (rc != 0) Log.e(TAG, "Native.start failed: " + rc);
        } catch (Throwable t) {
            note(logPath, "failed: " + t);   // split.log cannot say why the socket never answered; this file can
            Log.e(TAG, "OodleService failed", t);
            try { if (server != null) server.close(); } catch (IOException ignored) {}
        }
    }

    /** What went wrong with the decoder goes beside the split's log, where the player can share it */
    private static void note(String path, String line) {
        if (path == null) return;
        try {
            Files.write(new File(path).toPath(), (line + "\n").getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException ignored) {}
    }

    @Override
    public void onDestroy() {
        try { if (server != null) server.close(); } catch (IOException ignored) {}
        super.onDestroy();
        System.exit(0);
    }
}
