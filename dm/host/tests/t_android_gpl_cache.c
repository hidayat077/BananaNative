// Persistent Boot #1 GPL cache container, atomic checkpointing, header validation, stale pruning,
// worker throttling, and debounce policy tests (dm/gfx/vk_android/vka_gpl_cache.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../gfx/vk_android/vka_gpl_cache.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    char dir_tmpl[] = "/tmp/t_gpl_cache_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    CHECK(dir != NULL, "mkdtemp");
    if (!dir) return 1;

    const uint8_t uuid[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe};
    const char *title = "0100F2C0115B6000";
    const uint64_t lib_hash = 0x123456789abcdef0ull;
    const uint32_t vendor = 0x5143u, dev = 0x43050a00u, drv_ver = 0x06c00003u, total = 19u;

    char path[512];
    int fn_len = vka_gpl_cache_filename(path, sizeof path, dir, title, lib_hash, uuid);
    CHECK(fn_len > 0 && strstr(path, "vk_gpl_cache_0100F2C0115B6000_123456789abcdef0_0123456789abcdef1032547698badcfe.bin") != NULL,
          "filename format: %s", path);

    uint32_t bs_len = vka_gpl_cache_bitset_bytes(total);
    CHECK(bs_len == 3, "bitset bytes for 19 entries = 3, got %u", bs_len);
    uint8_t *bs = calloc(bs_len, 1);
    for (uint32_t i = 0; i < 10; i++) vka_gpl_cache_bit_set(bs, i);
    CHECK(vka_gpl_cache_bit_count(bs, total) == 10, "bit count 10");

    uint8_t blob[64];
    for (size_t i = 0; i < sizeof blob; i++) blob[i] = (uint8_t)(i * 7 + 3);

    vka_gpl_cache_hdr h = {
        .vendor_id = vendor,
        .device_id = dev,
        .driver_version = drv_ver,
        .flags = VKA_GPL_CACHE_F_GL_DEPTH,
        .dmsh_vk_lib_hash = lib_hash,
        .total_entries = total,
        .compiled_entries = 10,
    };
    memcpy(h.pipeline_cache_uuid, uuid, 16);
    CHECK(vka_gpl_cache_save_atomic(path, &h, bs, bs_len, blob, sizeof blob), "atomic partial save");

    vka_gpl_cache_hdr out_h; uint8_t *out_bs = NULL; void *out_blob = NULL; const char *why = NULL;
    CHECK(vka_gpl_cache_load(path, vendor, dev, drv_ver, uuid, lib_hash, total, 1, &out_h, &out_bs, &out_blob, &why),
          "load partial checkpoint: %s", why ? why : "ok");
    CHECK(out_h.compiled_entries == 10 && !(out_h.flags & VKA_GPL_CACHE_F_COMPLETE), "partial flags preserved");
    CHECK(out_bs && memcmp(out_bs, bs, bs_len) == 0, "bitset preserved");
    CHECK(out_blob && memcmp(out_blob, blob, sizeof blob) == 0, "vk blob preserved");
    free(out_bs); free(out_blob);

    // Mismatch checks: driver UUID, driverVersion, dmsh_vk.lib hash, entry count, GL depth mode.
    uint8_t bad_uuid[16]; memcpy(bad_uuid, uuid, 16); bad_uuid[15] ^= 1;
    CHECK(!vka_gpl_cache_load(path, vendor, dev, drv_ver, bad_uuid, lib_hash, total, 1, &out_h, &out_bs, &out_blob, &why) && why,
          "rejects pipelineCacheUUID mismatch");
    CHECK(!vka_gpl_cache_load(path, vendor, dev, drv_ver + 1, uuid, lib_hash, total, 1, &out_h, &out_bs, &out_blob, &why) && why,
          "rejects driverVersion mismatch");
    CHECK(!vka_gpl_cache_load(path, vendor, dev, drv_ver, uuid, lib_hash + 1, total, 1, &out_h, &out_bs, &out_blob, &why) && why,
          "rejects dmsh_vk.lib hash mismatch");
    CHECK(!vka_gpl_cache_load(path, vendor, dev, drv_ver, uuid, lib_hash, total + 1, 1, &out_h, &out_bs, &out_blob, &why) && why,
          "rejects entry count mismatch");
    CHECK(!vka_gpl_cache_load(path, vendor, dev, drv_ver, uuid, lib_hash, total, 0, &out_h, &out_bs, &out_blob, &why) && why,
          "rejects GL depth mismatch");

    // Complete checkpoint + stale pruning.
    for (uint32_t i = 10; i < total; i++) vka_gpl_cache_bit_set(bs, i);
    h.flags |= VKA_GPL_CACHE_F_COMPLETE;
    h.compiled_entries = total;
    CHECK(vka_gpl_cache_save_atomic(path, &h, bs, bs_len, blob, sizeof blob), "atomic complete save");
    CHECK(vka_gpl_cache_load(path, vendor, dev, drv_ver, uuid, lib_hash, total, 1, &out_h, &out_bs, &out_blob, &why),
          "load complete checkpoint");
    CHECK((out_h.flags & VKA_GPL_CACHE_F_COMPLETE) && vka_gpl_cache_bit_count(out_bs, total) == total, "complete flags & bitset");
    free(out_bs); free(out_blob);

    char stale1[512], stale2[512], other[512];
    snprintf(stale1, sizeof stale1, "%s/vk_gpl_cache_%s_0000000000000001_00000000000000000000000000000000.bin", dir, title);
    snprintf(stale2, sizeof stale2, "%s/vk_gpl_cache_%s_0000000000000002_00000000000000000000000000000000.bin.tmp", dir, title);
    snprintf(other, sizeof other, "%s/vk_gpl_cache_040074A01BF12000_0000000000000001_00000000000000000000000000000000.bin", dir);
    FILE *f1 = fopen(stale1, "wb"); if (f1) fclose(f1);
    FILE *f2 = fopen(stale2, "wb"); if (f2) fclose(f2);
    FILE *f3 = fopen(other, "wb"); if (f3) fclose(f3);
    const char *keep_bn = strrchr(path, '/') + 1;
    CHECK(vka_gpl_cache_prune_stale(dir, title, keep_bn) == 2, "pruned 2 stale files");
    CHECK(access(path, F_OK) == 0 && access(other, F_OK) == 0, "kept active and other-title caches");
    CHECK(access(stale1, F_OK) != 0 && access(stale2, F_OK) != 0, "removed stale .bin and .bin.tmp");

    // Corruption detection: flip a payload byte.
    FILE *cf = fopen(path, "r+b");
    CHECK(cf != NULL, "open for corruption test");
    if (cf) {
        fseek(cf, -1, SEEK_END);
        int ch = fgetc(cf);
        fseek(cf, -1, SEEK_END);
        fputc(ch ^ 0xff, cf);
        fclose(cf);
    }
    CHECK(!vka_gpl_cache_load(path, vendor, dev, drv_ver, uuid, lib_hash, total, 1, &out_h, &out_bs, &out_blob, &why) && why,
          "rejects corrupted payload");

    // Worker throttling & debounce policy checks.
    int ab = 0;
    CHECK(vka_precompile_target_workers(4, 10.0, 4096, 10000, 65530, &ab) == 4 && !ab, "full 4 workers");
    CHECK(vka_precompile_target_workers(4, 50.0, 4096, 10000, 65530, &ab) == 2 && !ab, "thermal step-down at 45s");
    CHECK(vka_precompile_target_workers(4, 10.0, 1000, 10000, 65530, &ab) == 1 && !ab, "low RAM step-down");
    CHECK(vka_precompile_target_workers(4, 10.0, 500, 10000, 65530, &ab) == 0 && ab, "critical RAM abort");
    CHECK(vka_precompile_target_workers(4, 10.0, 4096, 50000, 65530, &ab) == 0 && ab, "critical VMA abort");
    CHECK(!vka_precompile_debounce_due(49, 40000000000ull, 50, 30), "debounce needs >= 50 entries");
    CHECK(!vka_precompile_debounce_due(50, 29000000000ull, 50, 30), "debounce needs >= 30 s");
    CHECK(vka_precompile_debounce_due(50, 30000000000ull, 50, 30), "debounce triggers at >=50 and >=30s");

    free(bs);
    unlink(path);
    unlink(other);
    rmdir(dir);
    printf("t_android_gpl_cache: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
