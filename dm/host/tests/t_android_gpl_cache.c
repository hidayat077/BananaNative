// Persistent Boot #1 GPL cache container, atomic checkpointing, header validation, stale pruning,
// worker throttling, and debounce policy tests (dm/gfx/vk_android/vka_gpl_cache.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../gfx/vk_android/vka_gpl_cache.h"
#include "../../gfx/vk_android/vka_shader_variants.h"
#include "../../gfx/vk_android/vka_pwlist.h"
#include "../../gfx/vk_android/vka_spv_retype_infer.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Tiny in-memory SPIR-V builder for the static retype-inference tests. IDs are fixed:
// %1 uint, %2 int, %3 float, %4 vec4<float>, %5 pointer, %6 variable, %7 function type,
// %8 function, %9 constant 0, body results from %10. Bound 64 covers every module here.
typedef struct { uint32_t w[256]; size_t n; } spv_test_b;
static void spv_test_init(spv_test_b *b) {
    memset(b, 0, sizeof *b);
    b->w[0] = 0x07230203u; b->w[1] = 0x00010000u; b->n = 5; b->w[3] = 64;
}
#define SPV(op, ...) do { \
    uint32_t a_[] = {__VA_ARGS__}; size_t l_ = sizeof a_ / sizeof a_[0] + 1; \
    if (b->n + l_ <= sizeof b->w / sizeof b->w[0]) { \
        b->w[b->n++] = (uint32_t)(l_ << 16) | (op); \
        memcpy(b->w + b->n, a_, sizeof a_); b->n += l_ - 1; \
    } \
} while (0)
#define SPV0(op) do { if (b->n < sizeof b->w / sizeof b->w[0]) b->w[b->n++] = (uint32_t)(1u << 16) | (op); } while (0)

// Input vec4<float> at loc; optional access chain; load; bitcast/convert to cast_ty (1 uint, 2 int);
// optional signed consumer of the cast result.
static size_t spv_test_vin(spv_test_b *b, uint32_t loc, uint32_t cast_ty, int use_chain,
                           uint32_t cast_op, int signed_consumer) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 6, 30, loc);   // annotations before types/variables
    SPV(21, 1, 32, 0); SPV(21, 2, 32, 1); SPV(22, 3, 32); SPV(23, 4, 3, 4);
    SPV(32, 5, 1, 4); SPV(59, 5, 6, 1);
    SPV(33, 7, 3); SPV(43, 1, 9, 0); SPV(54, 3, 8, 0, 7);
    uint32_t ptr = 6;
    if (use_chain) { SPV(65, 5, 10, 6, 9); ptr = 10; }
    SPV(61, 4, 11, ptr);
    SPV(cast_op, cast_ty, 12, 11);
    if (signed_consumer) SPV(177, 2, 13, 12, 9);   // OpSLessThan %2 %12 %9
    SPV0(56);
    return b->n;
}

// UniformConstant image at set/binding; load; unfiltered fetch or filtered sample;
// extract one component; bitcast to uint.
static size_t spv_test_tex(spv_test_b *b, uint32_t set, uint32_t binding, int sampled, uint32_t read_op) {
    SPV(17, 1); SPV(14, 0, 1);
    SPV(71, 7, 34, set); SPV(71, 7, 33, binding);   // annotations before types/variables
    SPV(21, 1, 32, 0); SPV(21, 2, 32, 1); SPV(22, 3, 32); SPV(23, 4, 3, 4);
    SPV(25, 5, 3, 2, 0, 0, 0, (uint32_t)sampled, 0);   // float sampled type
    SPV(32, 6, 0, 5); SPV(59, 6, 7, 0);
    SPV(33, 8, 4); SPV(43, 1, 9, 0); SPV(54, 4, 10, 0, 8);
    SPV(61, 5, 11, 7);
    SPV(read_op, 4, 12, 11, 9);
    SPV(81, 3, 13, 12, 0); SPV(124, 1, 14, 13);
    SPV0(56);
    return b->n;
}

// OpCompositeInsert: w[i+5..] are literal indices. %2 is an int->float bitcast (ff_int),
// stored nowhere; inserting %1 (a float constant) into %3 with literal indices 1 and 2
// must not inherit ff_int, so the store to Output @5 is not a fout hint.
static size_t spv_test_composite_insert(spv_test_b *b) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 14, 30, 5);
    SPV(21, 10, 32, 0); SPV(22, 11, 32); SPV(23, 12, 11, 4);
    SPV(32, 13, 3, 12); SPV(59, 13, 14, 3);
    SPV(33, 15, 11); SPV(54, 11, 16, 0, 15);
    SPV(43, 11, 1, 0); SPV(43, 10, 6, 0);
    SPV(124, 11, 2, 6);            // %2 = int->float bitcast, not stored
    SPV(80, 12, 3, 1, 1, 1, 1);    // %3 = vec4 of float constant, no ff_int
    SPV(82, 12, 4, 1, 3, 1, 2);    // %4 = CompositeInsert %1 into %3, literal indices 1, 2
    SPV(62, 14, 4);
    SPV0(56);
    return b->n;
}

// Control-flow instructions before the bitcast must not clobber rt[] for %1 (uint).
// An unguarded rt[w[i+2]] = w[i+1] would set rt[1] = %20 (the signed int type) and
// make the fout bitcast look signed.
static size_t spv_test_ctrl_flow_fout(spv_test_b *b) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 14, 30, 5);
    SPV(21, 10, 32, 0); SPV(21, 20, 32, 1); SPV(22, 11, 32); SPV(23, 12, 11, 4);
    SPV(32, 13, 3, 12); SPV(59, 13, 14, 3);
    SPV(33, 15, 11); SPV(54, 11, 16, 0, 15);
    SPV(43, 10, 1, 0);             // %1 = uint constant, rt[1] must stay %10
    SPV(248, 18);                  // OpLabel
    SPV(247, 20, 1);               // OpSelectionMerge (would write rt[1] = %20)
    SPV(250, 20, 1, 19);           // OpBranchConditional (would write rt[1] = %20)
    SPV(248, 19); SPV(124, 11, 2, 1);
    SPV(80, 12, 3, 2, 2, 2, 2); SPV(62, 14, 3);
    SPV0(56);
    return b->n;
}

// Input vec4<float> @4 -> load -> bitcast to vec4<uint> -> CompositeExtract -> SLessThan.
// The extracted scalar inherits c_vin, so the signed consumer promotes loc 4 to vin_s.
static size_t spv_test_vin_extract_signed(spv_test_b *b) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 6, 30, 4);
    SPV(21, 1, 32, 0); SPV(22, 3, 32); SPV(23, 4, 3, 4); SPV(23, 2, 1, 4);
    SPV(32, 5, 1, 4); SPV(59, 5, 6, 1);
    SPV(33, 7, 3); SPV(43, 1, 9, 0); SPV(54, 3, 8, 0, 7);
    SPV(61, 4, 10, 6); SPV(124, 2, 11, 10);
    SPV(81, 1, 12, 11, 0); SPV(177, 1, 13, 12, 9);
    SPV0(56);
    return b->n;
}

// Input vec4<float> @3 -> load -> store into an SC_FUNCTION local -> load -> bitcast to vec4<uint>.
static size_t spv_test_vin_local(spv_test_b *b) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 6, 30, 3);
    SPV(21, 1, 32, 0); SPV(23, 2, 1, 4); SPV(22, 3, 32); SPV(23, 4, 3, 4);
    SPV(32, 5, 1, 4); SPV(59, 5, 6, 1);
    SPV(32, 10, 7, 4); SPV(33, 7, 3); SPV(54, 3, 8, 0, 7);
    SPV(59, 10, 11, 7);   // Function-scope local declared inside the function
    SPV(61, 4, 12, 6); SPV(62, 11, 12); SPV(61, 4, 14, 11); SPV(124, 2, 15, 14);
    SPV0(56);
    return b->n;
}

// Sampled texture @68 -> OpImageSampleExplicitLod -> SC_FUNCTION local -> load -> extract -> bitcast.
static size_t spv_test_tex_local(spv_test_b *b) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 7, 34, 0); SPV(71, 7, 33, 68);
    SPV(21, 1, 32, 0); SPV(23, 2, 1, 4); SPV(22, 3, 32); SPV(23, 4, 3, 4);
    SPV(25, 5, 3, 2, 0, 0, 0, 1, 0); SPV(32, 6, 0, 5); SPV(59, 6, 7, 0);
    SPV(32, 10, 7, 4); SPV(33, 8, 4); SPV(43, 1, 12, 0); SPV(54, 4, 13, 0, 8);
    SPV(59, 10, 11, 7);   // Function-scope local declared inside the function
    SPV(61, 5, 14, 7); SPV(88, 4, 15, 14, 12);
    SPV(62, 11, 15); SPV(61, 4, 16, 11); SPV(81, 3, 17, 16, 0); SPV(124, 1, 18, 17);
    SPV0(56);
    return b->n;
}

// A value also consumed by real float math is not integer bits (vk_android_spv.py:308-345).
static size_t spv_test_vin_float_used(spv_test_b *b) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 6, 30, 2);
    SPV(21, 1, 32, 0); SPV(22, 3, 32); SPV(23, 4, 3, 4);
    SPV(32, 5, 1, 4); SPV(59, 5, 6, 1);
    SPV(33, 7, 3); SPV(54, 3, 8, 0, 7);
    SPV(61, 4, 10, 6); SPV(124, 1, 11, 10); SPV(133, 4, 12, 10, 10);   // OpFMul on %10
    SPV0(56);
    return b->n;
}
static size_t spv_test_tex_float_used(spv_test_b *b) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 7, 34, 0); SPV(71, 7, 33, 68);
    SPV(21, 1, 32, 0); SPV(22, 3, 32); SPV(23, 4, 3, 4);
    SPV(25, 5, 3, 2, 0, 0, 0, 1, 0); SPV(32, 6, 0, 5); SPV(59, 6, 7, 0);
    SPV(33, 8, 4); SPV(43, 1, 9, 0); SPV(54, 4, 10, 0, 8);
    SPV(61, 5, 11, 7); SPV(87, 4, 12, 11, 9); SPV(124, 1, 13, 12);
    SPV(133, 4, 14, 12, 12);   // OpFMul on the sampled value
    SPV0(56);
    return b->n;
}

// Output vec4<float> at loc 5; int constant; bitcast/convert to float; composite construct; store.
static size_t spv_test_fout(spv_test_b *b, uint32_t int_ty, uint32_t cast_op) {
    SPV(17, 1); SPV(14, 0, 1); SPV(71, 6, 30, 5);   // annotations before types/variables
    SPV(21, 1, 32, 0); SPV(21, 2, 32, 1); SPV(22, 3, 32); SPV(23, 4, 3, 4);
    SPV(32, 5, 3, 4); SPV(59, 5, 6, 3);
    SPV(33, 7, 3); SPV(43, int_ty, 9, 0); SPV(54, 3, 8, 0, 7);
    SPV(cast_op, 3, 10, 9);
    SPV(80, 4, 11, 10, 10, 10, 10);
    SPV(62, 6, 11);
    SPV0(56);
    return b->n;
}

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

    // Worker throttling & debounce policy checks (no fixed 45s step-down while RAM & maps are healthy).
    int ab = 0;
    CHECK(vka_precompile_target_workers(4, 10.0, 4096, 10000, 65530, &ab) == 4 && !ab, "full 4 workers at 10s");
    CHECK(vka_precompile_target_workers(4, 120.0, 2372, 3610, 65530, &ab) == 4 && !ab, "stays 4 workers past 45s when MemAvailable > 1500 & maps < 50%%");
    CHECK(vka_precompile_target_workers(4, 50.0, 1450, 10000, 65530, &ab) == 2 && !ab, "steps to 2 workers when MemAvailable <= 1500");
    CHECK(vka_precompile_target_workers(4, 50.0, 4096, 33000, 65530, &ab) == 2 && !ab, "steps to 2 workers when maps >= 50%%");
    CHECK(vka_precompile_target_workers(4, 10.0, 1000, 10000, 65530, &ab) == 1 && !ab, "low RAM step-down");
    CHECK(vka_precompile_target_workers(4, 10.0, 4096, 43000, 65530, &ab) == 1 && !ab, "high maps step-down");
    CHECK(vka_precompile_target_workers(4, 10.0, 500, 10000, 65530, &ab) == 0 && ab, "critical RAM abort");
    CHECK(vka_precompile_target_workers(4, 10.0, 4096, 50000, 65530, &ab) == 0 && ab, "critical VMA abort");
    CHECK(!vka_precompile_debounce_due(49, 40000000000ull, 50, 30), "debounce needs >= 50 entries");
    CHECK(!vka_precompile_debounce_due(50, 29000000000ull, 50, 30), "debounce needs >= 30 s");
    CHECK(vka_precompile_debounce_due(50, 30000000000ull, 50, 30), "debounce triggers at >=50 and >=30s");

    free(bs);
    unlink(path);
    unlink(other);

    // Sharded v2 base GPL cache container & resume tests (~24 shaders/shard).
    CHECK(vka_gpl_shard_count(440) == 32, "440 entries -> 32 shards, got %u", vka_gpl_shard_count(440));
    CHECK(vka_gpl_shard_count(10349) == 512, "10349 entries -> 512 shards, got %u", vka_gpl_shard_count(10349));
    CHECK(vka_gpl_shard_count(28603) == 2048, "28603 entries -> 2048 shards, got %u", vka_gpl_shard_count(28603));

    // Prewarm record validation regression test: gs_fx==2 (TF capture) must be accepted.
    vka_gkey tf_gk = {0};
    tf_gk.samples = 1;
    tf_gk.gs_fx = 2;
    pwrec tf_rec = {.kind = VKA_PW_GFX, .klen = (uint32_t)offsetof(vka_gkey, va), .key = {0x1234ull, 0, 0, 0, 0}};
    CHECK(vka_pw_rec_ok(&tf_rec, (const uint8_t *)&tf_gk) == 1, "gs_fx==2 (TF capture) accepted by vka_pw_rec_ok");
    tf_gk.gs_fx = 3;
    CHECK(vka_pw_rec_ok(&tf_rec, (const uint8_t *)&tf_gk) == 0, "gs_fx==3 rejected by vka_pw_rec_ok");
    uint32_t sc = 8;
    uint32_t s_entries = 1280;
    vka_gpl_shard_ent stab[8];
    char spath[512];
    vka_gpl_cache_filename(spath, sizeof spath, dir, title, lib_hash, uuid);
    vka_gpl_cache_hdr shdr = {
        .vendor_id = vendor, .device_id = dev, .driver_version = drv_ver,
        .flags = VKA_GPL_CACHE_F_GL_DEPTH, .dmsh_vk_lib_hash = lib_hash, .total_entries = s_entries,
    };
    memcpy(shdr.pipeline_cache_uuid, uuid, 16);
    uint64_t aoff = 0;
    int sfd = vka_gpl_sharded_create(spath, &shdr, sc, stab, &aoff);
    CHECK(sfd >= 0 && aoff == vka_gpl_shard_data_offset(sc), "create sharded v2 cache");
    uint8_t sb0[96], sb1[112];
    for (size_t i = 0; i < sizeof sb0; i++) sb0[i] = (uint8_t)(i + 17);
    for (size_t i = 0; i < sizeof sb1; i++) sb1[i] = (uint8_t)(i * 3 + 5);
    CHECK(vka_gpl_sharded_write_shard(sfd, &shdr, stab, sc, 0, 160, sb0, sizeof sb0, &aoff, 0), "write shard 0");
    CHECK(vka_gpl_sharded_write_shard(sfd, &shdr, stab, sc, 3, 160, sb1, sizeof sb1, &aoff, 1), "write shard 3");
    close(sfd);

    // Partial resume check (re-open rw & verify shard 0 and 3 survive while unfinished shards remain incomplete).
    vka_gpl_cache_hdr rshdr; vka_gpl_shard_ent rtab[8]; uint64_t r_aoff = 0;
    sfd = vka_gpl_sharded_open_rw(spath, vendor, dev, drv_ver, uuid, lib_hash, s_entries, sc, 1, &rshdr, rtab, &r_aoff, &why);
    CHECK(sfd >= 0 && !(rshdr.flags & VKA_GPL_CACHE_F_COMPLETE) && rshdr.compiled_entries == 320, "resume rw partial sharded cache");
    CHECK((rtab[0].entries & VKA_GPL_SHARD_DONE) && !(rtab[1].entries & VKA_GPL_SHARD_DONE) && (rtab[3].entries & VKA_GPL_SHARD_DONE), "shard done flags preserved");
    for (uint32_t s = 0; s < sc; s++) {
        if (rtab[s].entries & VKA_GPL_SHARD_DONE) continue;
        CHECK(vka_gpl_sharded_write_shard(sfd, &rshdr, rtab, sc, s, 160, sb0, sizeof sb0, &r_aoff, s == sc - 1), "finish remaining shard");
    }
    CHECK(vka_gpl_sharded_finalize(sfd, &rshdr, rtab, sc, 1), "finalize sharded v2 cache");
    close(sfd);

    const vka_gpl_shard_ent *ro_tab = NULL; void *ro_map = NULL; size_t ro_sz = 0;
    CHECK(vka_gpl_sharded_open_ro(spath, vendor, dev, drv_ver, uuid, lib_hash, s_entries, sc, 1, &rshdr, &ro_tab, &ro_map, &ro_sz, &why),
          "open finalized sharded v2 cache ro mmap: %s", why ? why : "ok");
    CHECK((rshdr.flags & VKA_GPL_CACHE_F_COMPLETE) && ro_tab && ro_map && ro_sz > 0, "sharded mmap complete");
    CHECK(ro_tab && ro_map && memcmp((const uint8_t *)ro_map + ro_tab[3].offset, sb1, sizeof sb1) == 0, "shard 3 bytes intact via mmap");
    if (ro_map) munmap(ro_map, ro_sz);
    unlink(spath);

    // Monolithic optimization cache container tests (vka_gpl_mono_cache_* with mmap load).
    char mnc_path[512];
    int mn_len = vka_gpl_mono_cache_filename(mnc_path, sizeof mnc_path, dir, title, lib_hash, uuid);
    CHECK(mn_len > 0 && strstr(mnc_path, "vk_monolithic_cache_") != NULL, "mono cache filename: %s", mnc_path);
    vka_gpl_cache_hdr mh = {
        .vendor_id = vendor, .device_id = dev, .driver_version = drv_ver,
        .flags = VKA_GPL_CACHE_F_GL_DEPTH, .dmsh_vk_lib_hash = lib_hash, .total_entries = 12, .compiled_entries = 12,
    };
    memcpy(mh.pipeline_cache_uuid, uuid, 16);
    CHECK(vka_gpl_mono_cache_save_atomic(mnc_path, &mh, blob, sizeof blob), "mono cache atomic save");
    void *mmap_base = NULL; size_t mmap_sz = 0; const void *mblob = NULL; vka_gpl_cache_hdr out_mh;
    CHECK(vka_gpl_mono_cache_load_mmap(mnc_path, vendor, dev, drv_ver, uuid, lib_hash, 1, &out_mh, &mmap_base, &mmap_sz, &mblob, &why),
          "mono cache mmap load: %s", why ? why : "ok");
    CHECK((out_mh.flags & VKA_GPL_CACHE_F_MONO) && out_mh.compiled_entries == 12 && memcmp(mblob, blob, sizeof blob) == 0, "mono mmap contents match");
    munmap(mmap_base, mmap_sz);
    unlink(mnc_path);

    // Delta retype VkPipelineCache container tests.
    char rpath[512], rstale[512], rother[512];
    int rfn = vka_gpl_retype_cache_filename(rpath, sizeof rpath, dir, title, lib_hash, uuid);
    CHECK(rfn > 0 && strstr(rpath, "vk_gpl_retype_cache_0100F2C0115B6000_123456789abcdef0_0123456789abcdef1032547698badcfe.bin") != NULL,
          "retype cache filename: %s", rpath);
    vka_gpl_cache_hdr rh = {
        .vendor_id = vendor,
        .device_id = dev,
        .driver_version = drv_ver,
        .flags = VKA_GPL_CACHE_F_GL_DEPTH,
        .dmsh_vk_lib_hash = lib_hash,
        .total_entries = 82,
        .compiled_entries = 82,
    };
    memcpy(rh.pipeline_cache_uuid, uuid, 16);
    CHECK(vka_gpl_retype_cache_save_atomic(rpath, &rh, blob, sizeof blob), "delta retype atomic save");
    vka_gpl_cache_hdr out_rh; void *out_rblob = NULL;
    CHECK(vka_gpl_retype_cache_load(rpath, vendor, dev, drv_ver, uuid, lib_hash, 1, &out_rh, &out_rblob, &why),
          "load delta retype cache: %s", why ? why : "ok");
    CHECK((out_rh.flags & VKA_GPL_CACHE_F_DELTA) && out_rh.compiled_entries == 82 && out_rh.bitset_bytes == 0,
          "delta retype header metadata preserved");
    CHECK(out_rblob && memcmp(out_rblob, blob, sizeof blob) == 0, "delta retype blob preserved");
    free(out_rblob);
    CHECK(!vka_gpl_retype_cache_load(rpath, vendor, dev, drv_ver, bad_uuid, lib_hash, 1, &out_rh, &out_rblob, &why) && why,
          "delta retype rejects UUID mismatch");
    CHECK(!vka_gpl_retype_cache_load(rpath, vendor, dev, drv_ver, uuid, lib_hash + 1, 1, &out_rh, &out_rblob, &why) && why,
          "delta retype rejects dmsh_vk.lib hash mismatch");
    CHECK(!vka_gpl_retype_cache_load(rpath, vendor, dev, drv_ver, uuid, lib_hash, 0, &out_rh, &out_rblob, &why) && why,
          "delta retype rejects GL depth mismatch");
    CHECK(!vka_gpl_retype_cache_load(path, vendor, dev, drv_ver, uuid, lib_hash, 1, &out_rh, &out_rblob, &why),
          "missing file rejected");
    snprintf(rstale, sizeof rstale, "%s/vk_gpl_retype_cache_%s_0000000000000001_00000000000000000000000000000000.bin", dir, title);
    snprintf(rother, sizeof rother, "%s/vk_gpl_retype_cache_040074A01BF12000_0000000000000001_00000000000000000000000000000000.bin", dir);
    FILE *rf1 = fopen(rstale, "wb"); if (rf1) fclose(rf1);
    FILE *rf2 = fopen(rother, "wb"); if (rf2) fclose(rf2);
    const char *rkeep = strrchr(rpath, '/') + 1;
    CHECK(vka_gpl_retype_cache_prune_stale(dir, title, rkeep) == 1, "pruned 1 stale delta retype cache");
    CHECK(access(rpath, F_OK) == 0 && access(rother, F_OK) == 0, "kept active and other-title delta retype caches");
    unlink(rpath);
    unlink(rother);

    // Driver-agnostic Shader Variant Manifest tests (vk_shader_variants.h).
    char mpath[512], mstale[512];
    int mfn = vka_svar_manifest_filename(mpath, sizeof mpath, dir, title, lib_hash);
    CHECK(mfn > 0 && strstr(mpath, "vk_shader_variants_0100F2C0115B6000_123456789abcdef0.bin") != NULL,
          "variant manifest filename: %s", mpath);
    vka_svar_set sv = {0};
    vka_svar_rec r_vs = {.shader_hash = 0x1111111111111111ull, .stage = 0 /* VS */, .vin_u = 0x5u, .vin_s = 0x2u, .flags = VKA_SVAR_F_GL_DEPTH};
    vka_svar_rec r_fs = {.shader_hash = 0x2222222222222222ull, .stage = 4 /* FS */, .fout_u = 0x1u, .tex_u = 0x4ull, .alpha_func = 3, .samples = 1, .flags = VKA_SVAR_F_PFB_LATE | VKA_SVAR_F_A2C};
    vka_svar_rec r_cs = {.shader_hash = 0x7f19e3052ceb6c32ull, .stage = 5 /* CS */, .tex_u = 0x14ull};
    CHECK(vka_svar_set_add(&sv, &r_vs) == 1, "add VS variant");
    CHECK(vka_svar_set_add(&sv, &r_vs) == 0, "dedup identical VS variant");
    CHECK(vka_svar_set_add(&sv, &r_fs) == 1, "add FS variant");
    CHECK(vka_svar_set_add(&sv, &r_cs) == 1, "add CS variant");
    CHECK(sv.n == 3, "manifest set has 3 unique variants");
    CHECK(vka_svar_manifest_save_atomic(mpath, lib_hash, sv.recs, sv.n), "save variant manifest atomically");
    vka_svar_set_free(&sv);

    vka_svar_set sv_loaded = {0};
    CHECK(vka_svar_manifest_load(mpath, lib_hash, &sv_loaded, &why) && sv_loaded.n == 3,
          "load variant manifest: %s", why ? why : "ok");
    CHECK(vka_svar_rec_eq(&sv_loaded.recs[0], &r_vs) && vka_svar_rec_eq(&sv_loaded.recs[1], &r_fs) && vka_svar_rec_eq(&sv_loaded.recs[2], &r_cs),
          "manifest records round-trip");
    CHECK(vka_svar_set_add(&sv_loaded, &r_cs) == 0, "loaded set deduplicates existing entry");
    vka_svar_set_free(&sv_loaded);

    CHECK(!vka_svar_manifest_load(mpath, lib_hash + 1, &sv_loaded, &why) && why, "manifest rejects dmsh_vk.lib hash mismatch");
    vka_svar_set_free(&sv_loaded);

    snprintf(mstale, sizeof mstale, "%s/vk_shader_variants_%s_0000000000000001.bin", dir, title);
    FILE *mf = fopen(mstale, "wb"); if (mf) fclose(mf);
    const char *mkeep = strrchr(mpath, '/') + 1;
    CHECK(vka_svar_manifest_prune_stale(dir, title, mkeep) == 1, "pruned 1 stale variant manifest");
    CHECK(access(mpath, F_OK) == 0 && access(mstale, F_OK) != 0, "kept active manifest and removed stale");

    // Regression test for DMSH_VK_STAGE_* indexing in tex_u/tex_s (FS=4 vs TCS=1).
    uint64_t tu_fs_only[5] = {0, 0, 0, 0, 0x2ull}, ts_zero[5] = {0};
    uint64_t tu_tcs_only[5] = {0, 0x8ull, 0, 0, 0};
    CHECK(vka_svar_is_fs_retyped(1, 0, 0, 0, tu_fs_only, ts_zero, 0) == 1, "tex-only FS (index 4) flagged as fs_retyped");
    CHECK(vka_svar_is_fs_retyped(1, 1 /* VKA_FS_RETYPE */, 0x1u, 0, tu_fs_only, ts_zero, 0) == 1, "tex + fout FS flagged as fs_retyped");
    CHECK(vka_svar_is_pre_retyped(0, 0, 0, tu_fs_only, ts_zero) == 0, "FS tex mask (index 4) must not mark pre_retyped");
    CHECK(vka_svar_is_fs_retyped(1, 0, 0, 0, tu_tcs_only, ts_zero, 0) == 0, "TCS tex mask (index 1) must not mark fs_retyped");
    CHECK(vka_svar_is_pre_retyped(0, 0, 0, tu_tcs_only, ts_zero) == 1, "TCS tex mask (index 1) marks pre_retyped");

    // Static SPIR-V retype inference (vka_spv_retype_infer.h).
    vka_spv_retype_hint hint;
    spv_test_b sb;

    // 1. vin positive, unsigned: Input vec4<float> @3 -> access chain -> load -> bitcast to uint.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_vin(&sb, 3, 1, 1, 124, 0), 0, 1, 1, 1, &hint) == 1,
          "vin unsigned: hint found");
    CHECK(hint.vin_u == (1u << 3) && hint.vin_s == 0, "vin unsigned: loc 3, got u=%#x s=%#x", hint.vin_u, hint.vin_s);
    CHECK(!hint.fout_u && !hint.fout_s && !hint.tex_u && !hint.tex_s, "vin unsigned: no other masks");

    // 2. vin positive, signed: int bitcast target, and a signed-only consumer of a uint bitcast.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_vin(&sb, 5, 2, 0, 124, 0), 0, 1, 0, 0, &hint) == 1,
          "vin signed type: hint found");
    CHECK(hint.vin_s == (1u << 5) && hint.vin_u == 0, "vin signed type: loc 5, got u=%#x s=%#x", hint.vin_u, hint.vin_s);
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_vin(&sb, 6, 1, 0, 124, 1), 0, 1, 0, 0, &hint) == 1,
          "vin signed consumer: hint found");
    CHECK(hint.vin_s == (1u << 6) && hint.vin_u == 0, "vin signed consumer: loc 6 promoted, got u=%#x s=%#x", hint.vin_u, hint.vin_s);

    // 3. vin negative: OpConvertFToU is not a bitcast.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_vin(&sb, 2, 1, 0, 109, 0), 0, 1, 1, 1, &hint) == 0,
          "vin negative: OpConvertFToU ignored");
    CHECK(!hint.vin_u && !hint.vin_s && !hint.fout_u && !hint.fout_s && !hint.tex_u && !hint.tex_s,
          "vin negative: all masks zero");

    // 4. tex positive: stage-relative binding 64+4 with tex_base 64, fetch -> extract -> bitcast.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_tex(&sb, 0, 64 + 4, 2, 95), 64, 0, 1, 0, &hint) == 1,
          "tex unsigned: hint found");
    CHECK(hint.tex_u == (1ull << 4) && hint.tex_s == 0, "tex unsigned: bit 4, got u=%#llx s=%#llx",
          (unsigned long long)hint.tex_u, (unsigned long long)hint.tex_s);
    CHECK(!hint.vin_u && !hint.vin_s && !hint.fout_u && !hint.fout_s, "tex unsigned: no other masks");

    // 5. tex negatives: filtered sample, out-of-range binding, non-zero descriptor set.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_tex(&sb, 0, 64 + 4, 1, 87), 64, 0, 1, 0, &hint) == 1 &&
          hint.tex_u == (1ull << 4), "OpImageSampleImplicitLod is a texel read like vka_spvpatch.c");
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_tex(&sb, 0, 64 + 64, 2, 95), 64, 0, 1, 0, &hint) == 0,
          "tex negative: binding outside stage range ignored");
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_tex(&sb, 1, 64 + 4, 2, 95), 64, 0, 1, 0, &hint) == 0,
          "tex negative: DescriptorSet != 0 ignored");
    CHECK(!hint.tex_u && !hint.tex_s, "tex negative: all masks zero");

    // 6. fout positive: int -> bitcast to float -> composite construct -> store to Output @5.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_fout(&sb, 1, 124), 0, 0, 0, 1, &hint) == 1,
          "fout unsigned: hint found");
    CHECK(hint.fout_u == (1u << 5) && hint.fout_s == 0, "fout unsigned: loc 5, got u=%#x s=%#x", hint.fout_u, hint.fout_s);
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_fout(&sb, 2, 124), 0, 0, 0, 1, &hint) == 1,
          "fout signed: hint found");
    CHECK(hint.fout_s == (1u << 5) && hint.fout_u == 0, "fout signed: loc 5, got u=%#x s=%#x", hint.fout_u, hint.fout_s);

    // 7. fout negative: OpConvertUToF is not a bitcast.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_fout(&sb, 1, 112), 0, 0, 0, 1, &hint) == 0,
          "fout negative: OpConvertUToF ignored");
    CHECK(!hint.fout_u && !hint.fout_s, "fout negative: masks zero");

    // 8. Malformed / truncated / bad magic / bad bound / bad instruction length: clean zeroed result.
    memset(&hint, 0xff, sizeof hint);
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, 4, 0, 1, 1, 1, &hint) == 0 && !hint.vin_u && !hint.vin_s &&
          !hint.fout_u && !hint.fout_s && !hint.tex_u && !hint.tex_s, "truncated header");
    spv_test_init(&sb); sb.w[0] = 0xdeadbeefu;
    CHECK(vka_spv_infer_retype(sb.w, sb.n, 0, 1, 1, 1, &hint) == 0 && !hint.vin_u, "bad magic");
    spv_test_init(&sb); sb.w[3] = 0;
    CHECK(vka_spv_infer_retype(sb.w, sb.n, 0, 1, 1, 1, &hint) == 0 && !hint.vin_u, "zero bound");
    spv_test_init(&sb); sb.w[3] = (1u << 20) + 1u;
    CHECK(vka_spv_infer_retype(sb.w, sb.n, 0, 1, 1, 1, &hint) == 0 && !hint.vin_u, "oversized bound");
    spv_test_init(&sb); sb.w[5] = 17u;   // zero-length instruction
    CHECK(vka_spv_infer_retype(sb.w, sb.n, 0, 1, 1, 1, &hint) == 0 && !hint.vin_u, "zero instruction length");
    spv_test_init(&sb); sb.w[5] = (200u << 16) | 17u;   // runs past the word array
    CHECK(vka_spv_infer_retype(sb.w, sb.n, 0, 1, 1, 1, &hint) == 0 && !hint.vin_u, "overlong instruction");

    // Review regressions: CompositeInsert literal indices, control-flow rt[] clobber,
    // and CompositeExtract of a bitcasted vin vector into a signed consumer.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_composite_insert(&sb), 0, 1, 1, 1, &hint) == 0 &&
          !hint.fout_u && !hint.fout_s, "CompositeInsert literal indices are not SSA provenance");
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_ctrl_flow_fout(&sb), 0, 1, 1, 1, &hint) == 1,
          "control flow before bitcast: hint found");
    CHECK(hint.fout_u == (1u << 5) && hint.fout_s == 0, "control flow did not clobber rt[], got u=%#x s=%#x",
          hint.fout_u, hint.fout_s);
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_vin_extract_signed(&sb), 0, 1, 0, 0, &hint) == 1,
          "CompositeExtract of bitcasted vin: hint found");
    CHECK(hint.vin_s == (1u << 4) && hint.vin_u == 0, "CompositeExtract promoted to signed, got u=%#x s=%#x",
          hint.vin_u, hint.vin_s);

    // Translator-pattern regressions: SC_FUNCTION locals, explicit LOD, and float-use rejection.
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_vin_local(&sb), 0, 1, 0, 0, &hint) == 1 &&
          hint.vin_u == (1u << 3) && hint.vin_s == 0, "vin via SC_FUNCTION local inferred");
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_tex_local(&sb), 64, 0, 1, 0, &hint) == 1 &&
          hint.tex_u == (1ull << 4) && hint.tex_s == 0, "tex via sample-explicit + SC_FUNCTION local inferred");
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_vin_float_used(&sb), 0, 1, 0, 0, &hint) == 0 &&
          !hint.vin_u && !hint.vin_s, "vin also used by OpFMul rejected");
    spv_test_init(&sb);
    CHECK(vka_spv_infer_retype(sb.w, spv_test_tex_float_used(&sb), 64, 0, 1, 0, &hint) == 0 &&
          !hint.tex_u && !hint.tex_s, "tex fetch also used by OpFMul rejected");

    // Corruption test on variant manifest payload.
    FILE *mcf = fopen(mpath, "r+b");
    if (mcf) { fseek(mcf, -1, SEEK_END); int ch = fgetc(mcf); fseek(mcf, -1, SEEK_END); fputc(ch ^ 0xff, mcf); fclose(mcf); }
    CHECK(!vka_svar_manifest_load(mpath, lib_hash, &sv_loaded, &why) && why, "manifest rejects corrupted payload");
    vka_svar_set_free(&sv_loaded);
    unlink(mpath);

    rmdir(dir);
    printf("t_android_gpl_cache: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
