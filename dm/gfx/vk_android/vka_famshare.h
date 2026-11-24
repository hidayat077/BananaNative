// Resolution-family physical image memory sharing (vka_imgmem.c, vka_tex.c): the descriptor table and memory aliasing bookkeeping,
// free of Vulkan so host unit tests (dm/host/tests/t_android_famshare.c) can check it.
// On Switch engines with dynamic resolution, many live NVNtexture objects at the same (pool, pgen, off, format) describe the same physical
// storage at different dimensions (e.g. Luigi's Mansion 3 keeps ~90-101 live GPU-written descriptors per family). Sharing one VkDeviceMemory
// sized to the family's largest declared extent across all resolution variants avoids N-way GPU memory multiplication.
#ifndef VKA_FAMSHARE_H
#define VKA_FAMSHARE_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
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
    VKF_TARGET_2D          = 1
};

#define VKF_TOMB (~0ull)

static inline int vkf_candidate(int has_pool, int has_mem, int has_layout, unsigned pool_flags, int pool_sparse,
                                int tex_flags, int target, uint32_t depth, uint32_t levels, uint32_t samples, uint32_t stride) {
    return has_pool && has_mem && !has_layout && (pool_flags & VKF_POOL_CPU_NO_ACCESS) && !pool_sparse
        && (tex_flags & VKF_TEX_COMPRESSIBLE) && !(tex_flags & VKF_TEX_EXCLUDED)
        && target == VKF_TARGET_2D && depth <= 1 && levels <= 1 && samples <= 1 && !stride;
}

typedef struct {
    uint64_t serial, pool, pgen;
    int64_t off;
    uint32_t format, flags, w, h;
} vkf_desc;

typedef struct {
    vkf_desc *e;
    size_t cap, live, tomb;
} vkf_desc_tab;

static inline size_t vkf_desc_hslot(uint64_t s, size_t cap) {
    return (size_t)((s * 0x9E3779B97F4A7C15ull) >> 20) & (cap - 1);
}

static inline vkf_desc *vkf_desc_get(const vkf_desc_tab *t, uint64_t serial) {
    if (!t->cap || !serial || serial == VKF_TOMB) return NULL;
    for (size_t i = vkf_desc_hslot(serial, t->cap);; i = (i + 1) & (t->cap - 1)) {
        if (t->e[i].serial == serial) return &t->e[i];
        if (!t->e[i].serial) return NULL;
    }
}

static inline int vkf_desc_rebuild(vkf_desc_tab *t, size_t cap) {
    vkf_desc *n = (vkf_desc *)calloc(cap, sizeof *n);
    if (!n) return 0;
    for (size_t i = 0; i < t->cap; i++) {
        if (t->e[i].serial && t->e[i].serial != VKF_TOMB) {
            size_t j = vkf_desc_hslot(t->e[i].serial, cap);
            while (n[j].serial) j = (j + 1) & (cap - 1);
            n[j] = t->e[i];
        }
    }
    free(t->e);
    t->e = n;
    t->cap = cap;
    t->tomb = 0;
    return 1;
}

static inline int vkf_desc_put(vkf_desc_tab *t, uint64_t serial, uint64_t pool, uint64_t pgen,
                               int64_t off, uint32_t format, uint32_t flags, uint32_t w, uint32_t h) {
    if (!serial || serial == VKF_TOMB) return 0;
    vkf_desc *e = vkf_desc_get(t, serial);
    if (e) {
        e->pool = pool; e->pgen = pgen; e->off = off; e->format = format; e->flags = flags; e->w = w; e->h = h;
        return 1;
    }
    if ((t->live + t->tomb + 1) * 2 > t->cap) {
        size_t c = t->cap ? t->cap : 1024;
        while ((t->live + 1) * 4 > c) c *= 2;
        if (!vkf_desc_rebuild(t, c) && t->live + t->tomb + 1 >= t->cap) return 0;
    }
    size_t i = vkf_desc_hslot(serial, t->cap);
    while (t->e[i].serial && t->e[i].serial != VKF_TOMB) i = (i + 1) & (t->cap - 1);
    if (t->e[i].serial == VKF_TOMB) t->tomb--;
    t->e[i] = (vkf_desc){serial, pool, pgen, off, format, flags, w, h};
    t->live++;
    return 1;
}

static inline int vkf_desc_del(vkf_desc_tab *t, uint64_t serial) {
    vkf_desc *e = vkf_desc_get(t, serial);
    if (!e) return 0;
    e->serial = VKF_TOMB;
    t->live--;
    t->tomb++;
    return 1;
}

static inline void vkf_desc_drop_pool(vkf_desc_tab *t, uint64_t pool) {
    if (!t->cap || !t->live || !pool) return;
    for (size_t i = 0; i < t->cap; i++) {
        if (t->e[i].serial && t->e[i].serial != VKF_TOMB && t->e[i].pool == pool) {
            t->e[i].serial = VKF_TOMB;
            t->live--;
            t->tomb++;
        }
    }
}

static inline int vkf_desc_query(const vkf_desc_tab *t, uint64_t pool, uint64_t pgen, int64_t off, uint32_t format,
                                 uint32_t cur_w, uint32_t cur_h, uint32_t cur_flags,
                                 uint32_t *max_w, uint32_t *max_h, uint32_t *or_flags) {
    uint32_t mw = cur_w, mh = cur_h, mf = cur_flags;
    int fam = 0;
    for (size_t i = 0; i < t->cap; i++) {
        const vkf_desc *e = &t->e[i];
        if (!e->serial || e->serial == VKF_TOMB) continue;
        if (e->pool != pool || e->pgen != pgen || e->off != off || e->format != format) continue;
        if (e->w != cur_w || e->h != cur_h) fam = 1;
        if (e->w > mw) mw = e->w;
        if (e->h > mh) mh = e->h;
        mf |= e->flags;
    }
    if (max_w) *max_w = mw;
    if (max_h) *max_h = mh;
    if (or_flags) *or_flags = mf;
    return fam;
}

typedef struct {
    uint64_t pool, pgen;
    int64_t off;
    uint32_t vk_fmt, mem_type;
} vkf_key;

typedef struct {
    vkf_key key;
    uint64_t h, size, bound_bytes;
    int refs;
} vkf_mem_ent;

typedef struct {
    vkf_mem_ent *e;
    int n, cap;
    uint64_t made, joined;
} vkf_mem_tab;

static inline int vkf_key_eq(const vkf_key *a, const vkf_key *b) {
    return a->pool == b->pool && a->pgen == b->pgen && a->off == b->off
        && a->vk_fmt == b->vk_fmt && a->mem_type == b->mem_type;
}

static inline int vkf_mem_ref(vkf_mem_tab *t, const vkf_key *k, uint64_t need_size,
                              uint64_t *h_out, uint64_t *alloc_size_out) {
    int best = -1;
    for (int i = 0; i < t->n; i++) {
        if (vkf_key_eq(&t->e[i].key, k) && t->e[i].size >= need_size) {
            if (best < 0 || t->e[i].size < t->e[best].size) best = i;
        }
    }
    if (best < 0) return -1;
    t->e[best].refs++;
    t->e[best].bound_bytes += need_size;
    t->joined++;
    if (h_out) *h_out = t->e[best].h;
    if (alloc_size_out) *alloc_size_out = t->e[best].size;
    return best;
}

static inline int vkf_mem_ref_h(vkf_mem_tab *t, uint64_t h, uint64_t img_size) {
    for (int i = 0; i < t->n; i++) {
        if (t->e[i].h == h) {
            t->e[i].refs++;
            t->e[i].bound_bytes += img_size;
            return i;
        }
    }
    return -1;
}

static inline int vkf_mem_add(vkf_mem_tab *t, const vkf_key *k, uint64_t h, uint64_t alloc_size, uint64_t img_size) {
    if (!h) return -1;
    if (t->n == t->cap) {
        int c = t->cap ? 2 * t->cap : 32;
        vkf_mem_ent *e = (vkf_mem_ent *)realloc(t->e, (size_t)c * sizeof *e);
        if (!e) return -1;
        t->e = e;
        t->cap = c;
    }
    vkf_mem_ent *e = &t->e[t->n];
    memset(e, 0, sizeof *e);
    e->key = *k; e->h = h; e->size = alloc_size; e->bound_bytes = img_size; e->refs = 1;
    t->made++;
    return t->n++;
}

static inline int vkf_mem_unref(vkf_mem_tab *t, uint64_t h, uint64_t img_size) {
    for (int i = 0; i < t->n; i++) {
        if (t->e[i].h != h) continue;
        t->e[i].bound_bytes = t->e[i].bound_bytes > img_size ? t->e[i].bound_bytes - img_size : 0;
        if (--t->e[i].refs > 0) return 0;
        t->e[i] = t->e[--t->n];
        return 1;
    }
    return -1;
}

static inline void vkf_mem_stats(const vkf_mem_tab *t, uint64_t *images, uint64_t *allocs, uint64_t *bytes, uint64_t *saved) {
    uint64_t im = 0, by = 0, sv = 0;
    for (int i = 0; i < t->n; i++) {
        im += (uint64_t)t->e[i].refs;
        by += t->e[i].size;
        if (t->e[i].bound_bytes > t->e[i].size) sv += t->e[i].bound_bytes - t->e[i].size;
    }
    if (images) *images = im;
    if (allocs) *allocs = (uint64_t)t->n;
    if (bytes) *bytes = by;
    if (saved) *saved = sv;
}
#endif
