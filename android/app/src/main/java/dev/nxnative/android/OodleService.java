package dev.nxnative.android;

import android.app.Service;
import android.content.Intent;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.IOException;

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
        try {
            server = new LocalServerSocket(socketName);
            Native.load(titleId);
            LocalSocket client = server.accept();
            ParcelFileDescriptor dup = ParcelFileDescriptor.dup(client.getFileDescriptor());
            int fd = dup.detachFd();
            try { client.close(); } catch (IOException ignored) {}
            String[] args = {packPath};
            String[] env = {
                    "DM_OODLE_SERVE=1",
                    "DM_OODLE_SERVE_FD=" + fd,
                    "DM_ROMFS=" + packPath
            };
            int rc = Native.start(args, env, logPath, packPath);
            if (rc != 0) Log.e(TAG, "Native.start failed: " + rc);
        } catch (Throwable t) {
            Log.e(TAG, "OodleService failed", t);
        }
    }

    @Override
    public void onDestroy() {
        try { if (server != null) server.close(); } catch (IOException ignored) {}
        super.onDestroy();
        System.exit(0);
    }
}
