// Independent-sets layout constants and GPL fast-link latency statistics (avg, p99, max in microseconds).
// Vulkan-free so host unit tests (dm/host/tests/t_android_eds.c) can test them directly.
#ifndef VKA_GPL_STATS_H
#define VKA_GPL_STATS_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    VKA_INDEP_SET_PRE  = 0,   // Pre-rasterization stages (VS/TCS/TES/GS/TS/MS)
    VKA_INDEP_SET_FS   = 1,   // Fragment stage
    VKA_INDEP_SET_POOL = 2,   // Bindless texture & sampler pools
    VKA_INDEP_SET_PFB  = 3,   // Pool-read feedback bitmap
};

// Fragment stage bindings sit in [DMSH_VK_STAGE_FS * 64, (DMSH_VK_STAGE_FS + 1) * 64) = [256, 320).
// Note: VKA_MEMU_ARENA_BINDING (382) is outside this range and stays in Set 0 (pre-raster).
static inline int vka_binding_is_fs(uint32_t binding) {
    return binding >= 256u && binding < 320u;
}

enum { VKA_GPL_LAT_CAP = 1024 };
typedef struct {
    uint64_t count, sum_us, max_us;
    uint32_t samp[VKA_GPL_LAT_CAP];
    uint32_t nsamp, pos;
} vka_gpl_lat;

static inline void vka_gpl_lat_add(vka_gpl_lat *s, uint64_t ns) {
    uint64_t us = ns / 1000ull;
    s->count++;
    s->sum_us += us;
    if (us > s->max_us) s->max_us = us;
    s->samp[s->pos] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    s->pos = (s->pos + 1u) & (VKA_GPL_LAT_CAP - 1u);
    if (s->nsamp < VKA_GPL_LAT_CAP) s->nsamp++;
}

static inline int vka_gpl_lat_cmp_(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static inline void vka_gpl_lat_summary(const vka_gpl_lat *s, double *avg_us, double *p99_us, double *max_us) {
    *avg_us = s->count ? (double)s->sum_us / (double)s->count : 0.0;
    *max_us = (double)s->max_us;
    if (!s->nsamp) { *p99_us = 0.0; return; }
    uint32_t tmp[VKA_GPL_LAT_CAP];
    for (uint32_t i = 0; i < s->nsamp; i++) tmp[i] = s->samp[i];
    qsort(tmp, s->nsamp, sizeof tmp[0], vka_gpl_lat_cmp_);
    *p99_us = (double)tmp[((size_t)(s->nsamp - 1u) * 99u) / 100u];
}

// Classifies a Vulkan shader-bearing pipeline creation result into RAM rehydrate (1 = hit VkPipelineCache ISA)
// vs true ir3 compiler miss (0 = compiled SPIR-V -> ISA). Uses VkPipelineCreationFeedback when valid, falling
// back to a conservative duration heuristic (< 5 ms with a pipeline cache) when the driver omits feedback flags.
static inline int vka_classify_compile_feedback(int valid, int cache_hit, int all_stages_valid, int all_stages_hit, int has_cache, uint64_t dur_ns) {
    if (all_stages_valid && !all_stages_hit) return 0;   // stage missed VkPipelineCache -> real ir3 compile
    if (dur_ns >= 20000000ull) return 0;                 // >= 20 ms is a real compile, not RAM rehydrate
    if (valid) return cache_hit != 0;
    if (all_stages_valid) return all_stages_hit != 0;
    return has_cache && dur_ns < 5000000ull;
}

typedef struct {
    uint64_t ir3_compiler_miss;
    uint64_t gpl_fast_link_count;
    double   gpl_fast_link_avg_us;
    double   gpl_fast_link_p99_us;
    uint64_t gpl_fast_link_max_us;
    uint64_t pipeline_ram_rehydrate_count;
    uint64_t frontend_shader_translate_count;
    uint64_t retype_fout;
    uint64_t retype_vin;
    uint64_t retype_tex;
    uint64_t retype_cs_img;
    uint64_t variant_manifest_loaded;
    uint64_t variant_manifest_precompiled;
    uint64_t variant_manifest_saved;
    uint64_t delta_retype_cache_bytes;
} vka_gpl_telemetry;

static inline int vka_gpl_telemetry_format(const vka_gpl_telemetry *t, char *out, size_t n) {
    if (!out || !n || !t) return 0;
    return snprintf(out, n,
        "ir3_compiler_miss=%llu gpl_fast_link_count=%llu gpl_fast_link_avg_us=%.0f gpl_fast_link_p99_us=%.0f gpl_fast_link_max_us=%llu "
        "pipeline_ram_rehydrate_count=%llu frontend_shader_translate_count=%llu (retype fout=%llu vin=%llu tex=%llu cs_img=%llu) "
        "variant_manifest_loaded=%llu variant_manifest_precompiled=%llu variant_manifest_saved=%llu delta_retype_cache_bytes=%llu",
        (unsigned long long)t->ir3_compiler_miss, (unsigned long long)t->gpl_fast_link_count,
        t->gpl_fast_link_avg_us, t->gpl_fast_link_p99_us, (unsigned long long)t->gpl_fast_link_max_us,
        (unsigned long long)t->pipeline_ram_rehydrate_count, (unsigned long long)t->frontend_shader_translate_count,
        (unsigned long long)t->retype_fout, (unsigned long long)t->retype_vin,
        (unsigned long long)t->retype_tex, (unsigned long long)t->retype_cs_img,
        (unsigned long long)t->variant_manifest_loaded, (unsigned long long)t->variant_manifest_precompiled,
        (unsigned long long)t->variant_manifest_saved, (unsigned long long)t->delta_retype_cache_bytes);
}

#endif
