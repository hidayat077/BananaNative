// The first-use storage rule (dm/gfx/vk_android/vka_texcontent.h, vka_tex.c storage_mode): a texture first used as a sample
// takes CPU content from its storage; a render target or shader image is GPU content and must never pick up stale storage.
// The header is free of Android and Vulkan headers so this check needs no device.
#include <stdio.h>
#include "../../gfx/vk_android/vka_texcontent.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    // An ordinary sampled colour texture: Switch 2 always, Switch 1 when the storage holds data
    CHECK(vka_tc_sampled_content(0, 0, 0, 0, 0, 0, 1, 1), "NVN1 colour texture with data");
    CHECK(!vka_tc_sampled_content(0, 0, 0, 0, 0, 0, 1, 0), "NVN1 colour texture with empty storage");
    CHECK(vka_tc_sampled_content(1, 0, 0, 0, 0, 0, 1, 0), "NVN2 colour texture with empty storage");
    CHECK(!vka_tc_sampled_content(0, 0, 0, 0, 0, 0, 0, 1), "not sampled first");
    // The regression: this exclusion was moved to the NVN2-only branch, so NVN1 render targets took stale storage
    CHECK(!vka_tc_sampled_content(0, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 0, 0, 0, 0, 1, 1), "NVN1 render target");
    CHECK(!vka_tc_sampled_content(1, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 0, 0, 0, 0, 1, 1), "NVN2 render target");
    const unsigned excl[] = {NVN_TEXTURE_FLAGS_IMAGE, NVN_TEXTURE_FLAGS_LINEAR, NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET, NVN_TEXTURE_FLAGS_SPARSE};
    for (unsigned i = 0; i < sizeof excl / sizeof *excl; i++) {
        CHECK(!vka_tc_sampled_content(0, excl[i], 0, 0, 0, 0, 1, 1), "NVN1 flag %#x", excl[i]);
        CHECK(!vka_tc_sampled_content(1, excl[i], 0, 0, 0, 0, 1, 1), "NVN2 flag %#x", excl[i]);
    }
    CHECK(!vka_tc_sampled_content(0, 0, NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS, 0, 0, 0, 1, 1), "CPU cannot write the pool");
    CHECK(!vka_tc_sampled_content(1, 0, NVN_MEMORY_POOL_FLAGS_VIRTUAL, 0, 0, 0, 1, 1), "virtual pool");
    CHECK(!vka_tc_sampled_content(1, 0, 0, 1, 0, 0, 1, 1), "packaged layout");
    CHECK(!vka_tc_sampled_content(1, 0, 0, 0, 1, 0, 1, 1), "compressed format");
    CHECK(!vka_tc_sampled_content(1, 0, 0, 0, 0, 3, 1, 1), "depth/stencil format");
    printf("%s: %d failures\n", __FILE__, fails);
    return fails != 0;
}
