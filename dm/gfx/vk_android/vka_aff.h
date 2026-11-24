// CPU topology and auto-affinity helpers for the Vulkan replay (vka_worker.c, vka_enc.c).
// Header-only, no Vulkan headers: dm/host/tests/t_android_aff.c tests vka_aff_compute.
#ifndef VKA_AFF_H
#define VKA_AFF_H
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef struct {
    uint32_t replay;   // big + prime cores (never little cores)
    uint32_t enc;      // big cores when >= 2 exist, else big + prime (keeps encoder off prime contention on 4+3+1 SoCs)
} vka_aff_masks;

// Computes CPU affinity masks from per-CPU max frequencies in kHz (entries <= 0 are ignored/offline).
// Returns {0, 0} when frequencies are unknown or homogeneous.
static inline vka_aff_masks vka_aff_compute(const long *khz, int n) {
    vka_aff_masks r = {0, 0};
    if (!khz || n <= 0) return r;
    if (n > 32) n = 32;
    long hi = 0, lo = 0;
    for (int c = 0; c < n; c++) {
        long f = khz[c];
        if (f <= 0) continue;
        if (f > hi) hi = f;
        if (!lo || f < lo) lo = f;
    }
    if (!hi || hi == lo) return r;
    uint32_t fast = 0, mid = 0;
    int nmid = 0;
    for (int c = 0; c < n; c++) {
        long f = khz[c];
        if (f > lo) {
            fast |= 1u << c;
            if (f < hi) { mid |= 1u << c; nmid++; }
        }
    }
    r.replay = fast;
    r.enc = nmid >= 2 ? mid : fast;
    return r;
}

// Reads /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq and returns computed affinity masks.
static inline vka_aff_masks vka_aff_topology(void) {
    long khz[32] = {0};
    long n = sysconf(_SC_NPROCESSORS_CONF);
    if (n <= 0) return (vka_aff_masks){0, 0};
    if (n > 32) n = 32;
    for (long c = 0; c < n; c++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", c);
        FILE *f = fopen(p, "r");
        if (f) {
            if (fscanf(f, "%ld", &khz[c]) != 1) khz[c] = 0;
            fclose(f);
        }
    }
    return vka_aff_compute(khz, (int)n);
}
#endif
