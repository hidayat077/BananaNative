// Unit tests for Canonical vs Runtime pipeline key masking and dirty-state tracking (dm/gfx/vk_android/vka_eds.h).
#include <stdio.h>
#include <string.h>
#include "../../gfx/vk_android/vka_eds.h"
#include "../../gfx/vk_android/vka_pwlist.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static vka_gkey make_canon(uint8_t en, uint8_t src, uint8_t dst, uint8_t mask, uint8_t poly, uint8_t samples, uint8_t a2c, uint8_t dclamp, uint8_t logen, uint8_t logop) {
    vka_gkey k;
    memset(&k, 0, sizeof k);
    k.cfmt[0] = 37;
    k.dfmt = 126;
    k.ncolor = 1;
    k.samples = samples;
    k.topo = 3;
    k.poly_mode = poly;
    k.depth_clamp = dclamp;
    k.a2c = a2c;
    k.logic_en = logen;
    k.logic_op = logop;
    k.att[0].en = en;
    k.att[0].src = src;
    k.att[0].dst = dst;
    k.att[0].op = 0;
    k.att[0].srca = 1;
    k.att[0].dsta = 0;
    k.att[0].opa = 0;
    k.att[0].mask = mask;
    return k;
}

static void test_zero_mask_identity(void) {
    vka_gkey c = make_canon(1, 6, 7, 0xf, 1, 4, 1, 1, 1, 3);
    vka_gkey rt;
    vka_eds_runtime_key(&c, 0, 0, &rt);
    CHECK(memcmp(&c, &rt, sizeof c) == 0, "eds_mask=0 must preserve canonical key byte-for-byte");
}

static void test_full_eds_deduplication(void) {
    vka_gkey c1 = make_canon(0, 1, 0, 0xf, 0, 1, 0, 0, 0, 3);
    vka_gkey c2 = make_canon(1, 6, 7, 0x7, 1, 4, 1, 1, 1, 12);
    pwrec r1 = {.kind = VKA_PW_GFX, .klen = (uint32_t)offsetof(vka_gkey, va), .key = {0x11, 0x22}};
    pwrec r2 = r1;
    CHECK(vka_pw_hash(&r1, &c1) != vka_pw_hash(&r2, &c2), "canonical hashes must differ");

    uint32_t full = VKA_EDS2_MASK | VKA_EDS3_MASK;
    vka_gkey rt1, rt2;
    vka_eds_runtime_key(&c1, full, 0, &rt1);
    vka_eds_runtime_key(&c2, full, 0, &rt2);
    size_t len = offsetof(vka_gkey, va);
    CHECK(memcmp(&rt1, &rt2, len) == 0, "full EDS2+EDS3 mask must deduplicate dynamic state fields");
    CHECK(vka_pw_fnv(&rt1, len, VKA_PW_FNV0) == vka_pw_fnv(&rt2, len, VKA_PW_FNV0), "runtime hashes must match");
}

static void test_partial_mask_and_keep_a2c(void) {
    vka_gkey c = make_canon(1, 6, 7, 0x5, 2, 4, 1, 1, 1, 9);
    vka_gkey rt;
    vka_eds_runtime_key(&c, VKA_EDS_COLOR_WRITE_MASK | VKA_EDS_POLYGON_MODE | VKA_EDS_ALPHA_TO_COV, 1, &rt);
    CHECK(rt.att[0].mask == 0, "write mask zeroed");
    CHECK(rt.poly_mode == 0, "poly_mode zeroed");
    CHECK(rt.a2c == 1, "keep_a2c=1 must preserve a2c for pool-feedback early/late twin");
    CHECK(rt.att[0].en == 1 && rt.att[0].src == 6 && rt.att[0].dst == 7, "unmasked blend state preserved");
    CHECK(rt.samples == 4 && rt.depth_clamp == 1 && rt.logic_en == 1 && rt.logic_op == 9, "unmasked fields preserved");
}

static void test_vin_static_preserved(void) {
    vka_gkey c = make_canon(0, 1, 0, 0xf, 0, 1, 0, 0, 0, 3);
    c.vin_static = 1;
    c.va[0].used = 1; c.va[0].fmt = 100; c.va[0].offset = 12;
    c.vb[0].used = 1; c.vb[0].stride = 32;
    vka_gkey rt;
    vka_eds_runtime_key(&c, VKA_EDS2_MASK | VKA_EDS3_MASK, 0, &rt);
    CHECK(rt.vin_static == 1 && rt.va[0].fmt == 100 && rt.vb[0].stride == 32, "vin_static=1 preserves va/vb");
}

static void test_dirty_tracker(void) {
    uint64_t dirty = 0;
    vka_dyn_dirty_all(&dirty);
    CHECK(dirty == VKA_DYN_ALL, "dirty_all sets all bits");
    CHECK(vka_dyn_is_dirty(dirty, VKA_DYN_BLEND_EN), "blend_en dirty initially");
    CHECK(vka_dyn_is_dirty(dirty, VKA_DYN_WRITE_MASK), "write_mask dirty initially");
    vka_dyn_clear(&dirty, VKA_DYN_BLEND_EN);
    CHECK(!vka_dyn_is_dirty(dirty, VKA_DYN_BLEND_EN), "blend_en cleared");
    CHECK(vka_dyn_is_dirty(dirty, VKA_DYN_WRITE_MASK), "write_mask still dirty");
}

int main(void) {
    test_zero_mask_identity();
    test_full_eds_deduplication();
    test_partial_mask_and_keep_a2c();
    test_vin_static_preserved();
    test_dirty_tracker();
    printf("t_android_eds: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
