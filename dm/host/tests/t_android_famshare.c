// Resolution-family physical image sharing (dm/gfx/vk_android/vka_famshare.h): candidate filtering, live descriptor tracking & family extent queries,
// and shared VkDeviceMemory allocation reference counting / smallest-fitting selection / stats.
#include <stdio.h>
#include "../../gfx/vk_android/vka_famshare.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MIB (1ull << 20)

int main(void) {
    // 1. Candidate filtering
    CHECK(vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_IMAGE, VKF_TARGET_2D, 1, 1, 1, 0),
          "CPU_NO_ACCESS compressible+image 2D 1-level 1-sample is eligible");
    CHECK(vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | 0x80u, VKF_TARGET_2D, 1, 1, 1, 0),
          "depth compressible target with zcull hint and without IMAGE flag is eligible");
    CHECK(!vkf_candidate(0, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 0), "no pool rejected");
    CHECK(!vkf_candidate(1, 0, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 0), "no mem rejected");
    CHECK(!vkf_candidate(1, 1, 1, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 0), "packaged layout rejected");
    CHECK(!vkf_candidate(1, 1, 0, 0, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_IMAGE, VKF_TARGET_2D, 1, 1, 1, 0), "CPU-visible pool rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 1, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 0), "sparse pool rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_IMAGE, VKF_TARGET_2D, 1, 1, 1, 0), "non-compressible rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_LINEAR, VKF_TARGET_2D, 1, 1, 1, 0), "linear rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_SPARSE, VKF_TARGET_2D, 1, 1, 1, 0), "sparse flag rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_DISPLAY, VKF_TARGET_2D, 1, 1, 1, 0), "display flag rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D + 1, 1, 1, 1, 0), "non-2D target rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 2, 1, 1, 0), "layered target rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 2, 1, 0), "mipped target rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 4, 0), "multisampled target rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 4096), "strided target rejected");

    // 2. Live descriptor table & LM3-style 101-variant family query
    vkf_desc_tab dt = {0};
    uint32_t mw = 0, mh = 0, mf = 0;
    CHECK(vkf_desc_put(&dt, 100, 0x1000, 1, 0x20000, 0x3f, VKF_TEX_COMPRESSIBLE, 1920, 1080), "single desc put");
    CHECK(!vkf_desc_query(&dt, 0x1000, 1, 0x20000, 0x3f, 1920, 1080, VKF_TEX_COMPRESSIBLE, &mw, &mh, &mf) && mw == 1920 && mh == 1080,
          "single description is not a family");

    // Insert 101 variants from 1769x995 to 1920x1080 at pool 0x2000
    for (uint32_t i = 0; i < 101; i++) {
        uint32_t w = 1769 + (i * (1920 - 1769)) / 100;
        uint32_t h = 995 + (i * (1080 - 995)) / 100;
        uint32_t fl = VKF_TEX_COMPRESSIBLE | (i == 50 ? VKF_TEX_IMAGE : 0);
        CHECK(vkf_desc_put(&dt, 1000 + i, 0x2000, 7, 0x400000, 0x29, fl, w, h), "family desc put %u", i);
    }
    CHECK(vkf_desc_query(&dt, 0x2000, 7, 0x400000, 0x29, 1769, 995, VKF_TEX_COMPRESSIBLE, &mw, &mh, &mf) == 1
          && mw == 1920 && mh == 1080 && (mf & VKF_TEX_IMAGE),
          "101-variant family reports largest extent 1920x1080 and ORed IMAGE flag (got %ux%u %#x)", mw, mh, mf);

    // Non-monotonic extents (1280x720 and 1200x760)
    CHECK(vkf_desc_put(&dt, 3000, 0x3000, 2, 0x1000, 0x25, VKF_TEX_COMPRESSIBLE, 1280, 720), "put 1280x720");
    CHECK(vkf_desc_put(&dt, 3001, 0x3000, 2, 0x1000, 0x25, VKF_TEX_COMPRESSIBLE, 1200, 760), "put 1200x760");
    CHECK(vkf_desc_query(&dt, 0x3000, 2, 0x1000, 0x25, 1280, 720, VKF_TEX_COMPRESSIBLE, &mw, &mh, &mf) == 1
          && mw == 1280 && mh == 760, "non-monotonic max takes max w and max h (%ux%u)", mw, mh);

    // Isolation by pool, pgen, offset, format
    CHECK(!vkf_desc_query(&dt, 0x2000, 8, 0x400000, 0x29, 1769, 995, VKF_TEX_COMPRESSIBLE, &mw, &mh, &mf), "pgen isolation");
    CHECK(!vkf_desc_query(&dt, 0x2000, 7, 0x401000, 0x29, 1769, 995, VKF_TEX_COMPRESSIBLE, &mw, &mh, &mf), "offset isolation");
    CHECK(!vkf_desc_query(&dt, 0x2000, 7, 0x400000, 0x3f, 1769, 995, VKF_TEX_COMPRESSIBLE, &mw, &mh, &mf), "format isolation");

    // Churn 600 serials (tombstones & rebuild)
    for (uint64_t s = 5000; s < 5600; s++) {
        CHECK(vkf_desc_put(&dt, s, 0x4000, 1, 0, 0x25, VKF_TEX_COMPRESSIBLE, 640, 360), "churn put");
        if (s & 1) CHECK(vkf_desc_del(&dt, s), "churn del");
    }
    vkf_desc_drop_pool(&dt, 0x2000);
    CHECK(!vkf_desc_query(&dt, 0x2000, 7, 0x400000, 0x29, 1769, 995, VKF_TEX_COMPRESSIBLE, &mw, &mh, &mf), "drop_pool cleared 0x2000");

    // 3. Family allocation table
    vkf_mem_tab mt = {0};
    vkf_key k1 = {0x2000, 7, 0x400000, 97, 1};
    vkf_key k2_pgen = {0x2000, 8, 0x400000, 97, 1};
    uint64_t h = 0, asz = 0;
    CHECK(vkf_mem_ref(&mt, &k1, 8 * MIB, &h, &asz) < 0, "empty table misses");
    CHECK(vkf_mem_add(&mt, &k1, 0xAAA1, 10 * MIB, 8 * MIB) == 0, "add first family alloc 10 MiB for 8 MiB small variant");
    CHECK(vkf_mem_ref_h(&mt, 0xAAA1, 10 * MIB) == 0, "temp probe ref increments");
    CHECK(vkf_mem_unref(&mt, 0xAAA1, 10 * MIB) == 0, "temp probe unref keeps alloc");
    for (int i = 0; i < 100; i++) {
        uint64_t sz = (uint64_t)(8 + (i % 3)) * MIB;
        CHECK(vkf_mem_ref(&mt, &k1, sz, &h, &asz) == 0 && h == 0xAAA1 && asz == 10 * MIB, "variant %d reuses 0xAAA1", i);
    }
    CHECK(vkf_mem_ref(&mt, &k2_pgen, 8 * MIB, &h, &asz) < 0, "different pgen misses");
    // Smallest-fitting selection when two allocs exist for same key
    CHECK(vkf_mem_add(&mt, &k1, 0xAAA2, 16 * MIB, 16 * MIB) == 1, "add larger 16 MiB alloc");
    CHECK(vkf_mem_ref(&mt, &k1, 9 * MIB, &h, &asz) == 0 && h == 0xAAA1, "9 MiB picks smaller 10 MiB alloc");
    CHECK(vkf_mem_ref(&mt, &k1, 12 * MIB, &h, &asz) == 1 && h == 0xAAA2, "12 MiB picks 16 MiB alloc");
    uint64_t im = 0, al = 0, by = 0, sv = 0;
    vkf_mem_stats(&mt, &im, &al, &by, &sv);
    CHECK(al == 2 && im == 104 && by == 26 * MIB && sv > 800 * MIB, "stats: %llu images, %llu allocs, %llu MiB, %llu MiB saved",
          (unsigned long long)im, (unsigned long long)al, (unsigned long long)(by / MIB), (unsigned long long)(sv / MIB));

    // Free in arbitrary order
    CHECK(vkf_mem_unref(&mt, 0xAAA2, 12 * MIB) == 0, "0xAAA2 first unref keeps");
    CHECK(vkf_mem_unref(&mt, 0xAAA2, 16 * MIB) == 1, "0xAAA2 second unref frees");
    CHECK(vkf_mem_unref(&mt, 0xAAA2, 16 * MIB) == -1, "double unref refused");
    CHECK(vkf_mem_unref(&mt, 0xAAA1, 8 * MIB) == 0, "0xAAA1 first unref keeps");

    free(dt.e);
    free(mt.e);
    printf("t_android_famshare: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
