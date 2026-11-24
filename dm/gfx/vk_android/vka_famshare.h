// Resolution-family superseding eviction policy (vka_tex.c): the pure, Vulkan-free candidate predicate and eviction selection
// tested by dm/host/tests/t_android_famshare.c.
// On Switch engines with dynamic resolution (e.g. Luigi's Mansion 3), dozens of GPU-written NVNtexture objects at one (pool, pgen, mem, vk_fmt)
// stay alive at different resolutions, retaining independent Vulkan allocations (and on Adreno/Turnip, aliasing them across dimensions via
// VK_IMAGE_CREATE_ALIAS_BIT corrupts UBWC/LRZ metadata). Writing a GPU-only target overwrites older resolution variants at the same pool storage,
// so superseded siblings that are idle and past their GPU fence can be safely evicted while keeping their non-aliased memory independent.
#ifndef VKA_FAMSHARE_H
#define VKA_FAMSHARE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    VKF_POOL_CPU_NO_ACCESS = 0x1u,
    VKF_TEX_DISPLAY        = 0x1u,
    VKF_TEX_VIDEO_DECODE   = 0x2u,
    VKF_TEX_IMAGE          = 0x4u,
    VKF_TEX_COMPRESSIBLE   = 0x8u,
    VKF_TEX_LINEAR         = 0x10u,
    VKF_TEX_SPARSE         = 0x20u,
    VKF_TEX_LINEAR_RT      = 0x40u,
    VKF_TEX_MINIMAL_LAYOUT = 0x200u,
    VKF_TEX_EXCLUDED       = VKF_TEX_DISPLAY | VKF_TEX_VIDEO_DECODE | VKF_TEX_LINEAR | VKF_TEX_SPARSE | VKF_TEX_LINEAR_RT | VKF_TEX_MINIMAL_LAYOUT,
    VKF_TARGET_2D          = 1,
    VKF_KEEP_SIBLINGS      = 1,   // keep the 1 most recent sibling besides the written image (2 live variants per family)
    VKF_MIN_AGE            = 4    // minimum idle frames before an idle superseded sibling may be evicted
};

static inline int vkf_candidate(int has_pool, int has_mem, int has_layout, unsigned pool_flags, int pool_sparse,
                                int tex_flags, int target, uint32_t depth, uint32_t layers, uint32_t levels, uint32_t samples, uint32_t stride) {
    return has_pool && has_mem && !has_layout && (pool_flags & VKF_POOL_CPU_NO_ACCESS) && !pool_sparse
        && (tex_flags & VKF_TEX_COMPRESSIBLE) && !(tex_flags & VKF_TEX_EXCLUDED)
        && target == VKF_TARGET_2D && depth <= 1 && layers <= 1 && levels <= 1 && samples <= 1 && !stride;
}

typedef struct {
    uint64_t last_frame, last_fence;
    int holds, nkids, wb_pending;
} vkf_sib;

static inline int vkf_sib_evictable(const vkf_sib *s, uint64_t cur_frame, uint64_t done_fence) {
    if (!s || s->holds || s->nkids || s->wb_pending) return 0;
    if (s->last_frame >= cur_frame || cur_frame - s->last_frame < VKF_MIN_AGE) return 0;
    if (s->last_fence && done_fence <= s->last_fence) return 0;
    return 1;
}

// Marks evict_out[i] = 1 for superseded siblings outside the top `keep_siblings` most recently used (highest last_frame, tie-breaking on
// last_fence then lower index) that satisfy vkf_sib_evictable(). Returns the number of siblings marked for eviction.
static inline int vkf_select_evict(const vkf_sib *sibs, int n, uint64_t cur_frame, uint64_t done_fence, int keep_siblings, uint8_t *evict_out) {
    if (n <= 0 || !evict_out) return 0;
    memset(evict_out, 0, (size_t)n);
    if (keep_siblings < 0) keep_siblings = 0;
    if (n <= keep_siblings) return 0;
    int evicted = 0;
    for (int i = 0; i < n; i++) {
        int newer = 0;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            if (sibs[j].last_frame > sibs[i].last_frame
                || (sibs[j].last_frame == sibs[i].last_frame
                    && (sibs[j].last_fence > sibs[i].last_fence
                        || (sibs[j].last_fence == sibs[i].last_fence && j < i)))) {
                newer++;
            }
        }
        if (newer >= keep_siblings && vkf_sib_evictable(&sibs[i], cur_frame, done_fence)) {
            evict_out[i] = 1;
            evicted++;
        }
    }
    return evicted;
}
#endif