// Resolution-family superseding eviction policy (dm/gfx/vk_android/vka_famshare.h): candidate filtering,
// sibling evictability bounds (holds/kids/wb/age/fence), and top-K most-recently-used protection.
#include <stdio.h>
#include "../../gfx/vk_android/vka_famshare.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    // 1. Candidate filtering
    CHECK(vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_IMAGE, VKF_TARGET_2D, 1, 1, 1, 1, 0),
          "CPU_NO_ACCESS compressible+image 2D 1-layer 1-level 1-sample is eligible");
    CHECK(vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | 0x80u, VKF_TARGET_2D, 1, 1, 1, 1, 0),
          "depth compressible target with zcull hint and without IMAGE flag is eligible");
    CHECK(!vkf_candidate(0, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 1, 0), "no pool rejected");
    CHECK(!vkf_candidate(1, 0, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 1, 0), "no mem rejected");
    CHECK(!vkf_candidate(1, 1, 1, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 1, 0), "packaged layout rejected");
    CHECK(!vkf_candidate(1, 1, 0, 0, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_IMAGE, VKF_TARGET_2D, 1, 1, 1, 1, 0), "CPU-visible pool rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 1, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 1, 0), "sparse pool rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_IMAGE, VKF_TARGET_2D, 1, 1, 1, 1, 0), "non-compressible rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_LINEAR, VKF_TARGET_2D, 1, 1, 1, 1, 0), "linear rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_SPARSE, VKF_TARGET_2D, 1, 1, 1, 1, 0), "sparse flag rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE | VKF_TEX_DISPLAY, VKF_TARGET_2D, 1, 1, 1, 1, 0), "display flag rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D + 1, 1, 1, 1, 1, 0), "non-2D target rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 2, 1, 1, 1, 0), "3D depth > 1 rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 2, 1, 1, 0), "layered target rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 2, 1, 0), "mipped target rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 4, 0), "multisampled target rejected");
    CHECK(!vkf_candidate(1, 1, 0, VKF_POOL_CPU_NO_ACCESS, 0, VKF_TEX_COMPRESSIBLE, VKF_TARGET_2D, 1, 1, 1, 1, 4096), "strided target rejected");

    // 2. Per-sibling evictability checks
    vkf_sib ok_s = {.last_frame = 96, .last_fence = 10};
    CHECK(vkf_sib_evictable(&ok_s, 100, 11), "idle >= VKF_MIN_AGE and done_fence > last_fence is evictable");
    CHECK(!vkf_sib_evictable(&ok_s, 100, 10), "done_fence == last_fence is still in flight: not evictable");
    CHECK(!vkf_sib_evictable(&ok_s, 99, 11), "age < VKF_MIN_AGE (3 < 4) is not evictable");
    CHECK(!vkf_sib_evictable(&ok_s, 96, 11), "same frame not evictable");
    vkf_sib no_fence = {.last_frame = 90, .last_fence = 0};
    CHECK(vkf_sib_evictable(&no_fence, 100, 0), "last_fence == 0 and aged is evictable");
    vkf_sib held = {.last_frame = 90, .last_fence = 5, .holds = 1};
    CHECK(!vkf_sib_evictable(&held, 100, 20), "held sibling not evictable");
    vkf_sib kid = {.last_frame = 90, .last_fence = 5, .nkids = 1};
    CHECK(!vkf_sib_evictable(&kid, 100, 20), "sibling with level children not evictable");
    vkf_sib wb = {.last_frame = 90, .last_fence = 5, .wb_pending = 1};
    CHECK(!vkf_sib_evictable(&wb, 100, 20), "sibling with pending writeback not evictable");

    // 3. Sibling selection cases
    uint8_t ev[128];

    // n <= VKF_KEEP_SIBLINGS -> keep 1 sibling
    vkf_sib one[1] = {{.last_frame = 10, .last_fence = 1}};
    CHECK(vkf_select_evict(one, 1, 100, 50, VKF_KEEP_SIBLINGS, ev) == 0 && !ev[0], "1 sibling stays kept");

    // 2 siblings with 1 stale -> keeps most recent, evicts older
    vkf_sib two[2] = {
        {.last_frame = 20, .last_fence = 2},
        {.last_frame = 80, .last_fence = 8},
    };
    CHECK(vkf_select_evict(two, 2, 100, 50, VKF_KEEP_SIBLINGS, ev) == 1 && ev[0] == 1 && ev[1] == 0,
          "2 siblings: keeps newer (80) and evicts older (20)");

    // 100 LM3-like dynamic-res siblings -> keeps top 1 (index 99) and evicts 99 stale ones
    vkf_sib lm3[100];
    for (int i = 0; i < 100; i++) lm3[i] = (vkf_sib){.last_frame = (uint64_t)(10 + i), .last_fence = (uint64_t)(10 + i)};
    int lm3_cnt = vkf_select_evict(lm3, 100, 200, 500, VKF_KEEP_SIBLINGS, ev);
    CHECK(lm3_cnt == 99 && ev[99] == 0, "100 siblings: evicts 99 stale and keeps newest (got %d)", lm3_cnt);
    for (int i = 0; i < 99; i++) CHECK(ev[i] == 1, "lm3[%d] marked for eviction", i);

    // 3 siblings all used within VKF_MIN_AGE -> evicts 0
    vkf_sib recent[3] = {
        {.last_frame = 97, .last_fence = 9},
        {.last_frame = 98, .last_fence = 10},
        {.last_frame = 99, .last_fence = 11},
    };
    CHECK(vkf_select_evict(recent, 3, 100, 50, VKF_KEEP_SIBLINGS, ev) == 0, "all siblings < VKF_MIN_AGE: evicts 0");

    // 1 held recent sibling + 2 stale siblings -> evicts both stale ones while keeping the held recent one
    vkf_sib held_recent[3] = {
        {.last_frame = 98, .last_fence = 10, .holds = 1},
        {.last_frame = 20, .last_fence = 2},
        {.last_frame = 30, .last_fence = 3},
    };
    CHECK(vkf_select_evict(held_recent, 3, 100, 50, VKF_KEEP_SIBLINGS, ev) == 2 && !ev[0] && ev[1] && ev[2],
          "held recent sibling satisfies keep quota so both stale siblings are evicted");

    printf("t_android_famshare: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}