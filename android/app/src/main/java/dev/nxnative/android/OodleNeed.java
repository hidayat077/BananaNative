package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;

/**
 * Whether the phone's shader split needs the game's own Oodle / LZ5 decoder, so PackService starts
 * OodleService (DM_OODLE_ANDROID_SOCKET) for that step. The phone has no Mac host and no PC ooz worker:
 * shader_split.py asks for the decoder only when a file it scans is compressed, and the answer is run by
 * the game's own main in the :oodle process (dm/host/hle_oodle.c, DM_OODLE_SERVE).
 * <p>
 * The pack must hold the rewritten main: scripts/pack_android.py's rewrite writes main.patched.bin (there
 * is no main.android.bin — dm/host/loader.c loads &lt;module&gt;.patched.bin). The profile names the
 * containers the split decodes: Game Freak's Trinity packs (arc/data.trpfs), PlatinumGames' PKZ (.pkz),
 * GFPAK archives or Unreal paks (.pak). The pack's own copy of game.tsv is written by the pack step, after
 * the split, so until then the APK's profile in tools.zip is the one to read. Checked on the JVM by
 * dm/tools/tests/test_android_oodle.py with OodleNeedJvmTest.
 */
final class OodleNeed {
    private OodleNeed() {}

    static boolean needed(File pack, File gameDir) {
        if (!new File(pack, "main.patched.bin").isFile()) return false;
        for (File tsv : new File[] {new File(pack, "game.tsv"), new File(gameDir, "game.tsv")}) {
            if (!tsv.isFile()) continue;
            try {
                String text = new String(Files.readAllBytes(tsv.toPath()), StandardCharsets.UTF_8).toLowerCase(Locale.ROOT);
                if (text.contains("trpfs") || text.contains("pkz") || text.contains("gfpak") || text.contains(".pak") || text.contains(".forge")) return true;
            } catch (IOException ignored) {
                // A profile we cannot read is no reason to fail Prepare: the split says what it needs if so.
            }
        }
        return false;
    }
}
