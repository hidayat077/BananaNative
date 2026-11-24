// CPU topology and auto-affinity mask computation (dm/gfx/vk_android/vka_aff.h):
// big.LITTLE / 1+3+4 SoCs (Snapdragon 870, etc.), homogeneous CPUs, and unknown/zero frequencies.
#include <stdio.h>
#include "../../gfx/vk_android/vka_aff.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    // 1+3+4 SoC (Snapdragon 870: 4x1804 + 3x2419 + 1x3187 MHz)
    long sd870[8] = {1804800, 1804800, 1804800, 1804800, 2419200, 2419200, 2419200, 3187200};
    vka_aff_masks m = vka_aff_compute(sd870, 8);
    CHECK(m.replay == 0xf0u, "SD870 replay mask: want 0xf0, got %#x", m.replay);
    CHECK(m.enc == 0x70u, "SD870 enc mask: want 0x70, got %#x", m.enc);

    // 4+4 big.LITTLE (only two tiers: 0 mid cores -> both get fast_mask)
    long bl44[8] = {1800000, 1800000, 1800000, 1800000, 2800000, 2800000, 2800000, 2800000};
    m = vka_aff_compute(bl44, 8);
    CHECK(m.replay == 0xf0u && m.enc == 0xf0u, "4+4 big.LITTLE: want 0xf0/0xf0, got %#x/%#x", m.replay, m.enc);

    // 4+1+1 (only 1 mid core -> enc falls back to fast_mask so it does not bottleneck on a single core)
    long t411[6] = {1500000, 1500000, 1500000, 1500000, 2400000, 3000000};
    m = vka_aff_compute(t411, 6);
    CHECK(m.replay == 0x30u && m.enc == 0x30u, "4+1+1: want 0x30/0x30, got %#x/%#x", m.replay, m.enc);

    // Homogeneous CPUs -> {0, 0} (untouched)
    long homo[4] = {2000000, 2000000, 2000000, 2000000};
    m = vka_aff_compute(homo, 4);
    CHECK(m.replay == 0 && m.enc == 0, "homogeneous: want 0/0, got %#x/%#x", m.replay, m.enc);

    // Unknown / zero frequencies -> {0, 0}
    long zeros[4] = {0, 0, 0, 0};
    m = vka_aff_compute(zeros, 4);
    CHECK(m.replay == 0 && m.enc == 0, "zeros: want 0/0, got %#x/%#x", m.replay, m.enc);
    m = vka_aff_compute(NULL, 4);
    CHECK(m.replay == 0 && m.enc == 0, "NULL: want 0/0, got %#x/%#x", m.replay, m.enc);

    printf("t_android_aff: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
