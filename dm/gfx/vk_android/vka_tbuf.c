// Buffer textures (NVN_TEXTURE_TARGET_BUFFER) on the Vulkan replay. On NVN a buffer texture is a typed view of a pool's memory, read through a texture handle; the library's
// binding records say how a shader takes one:
//   - a texel buffer descriptor (UNIFORM_TEXEL_BUFFER, or STORAGE_TEXEL_BUFFER for an image slot): a VkBufferView over the placed guest memory, the segment's own buffer
//     (vka_mem.c), so it reads and writes the memory itself, in GPU order. Views are kept per (buffer, offset, range, format) and made again once a segment's buffer is
//     destroyed (vka_seg_gen); a view that goes is destroyed when the GPU is past the last fence reserved then (vka_after)
//   - a sampled image: the translator reads a 1D fetch it cannot tell from a buffer read as 2D at (x & 0x3FFF, x >> 14) (dm/third_party/ryujinx/README.md, the Mac host's
//     tbuf_as_2d), or as a 1D image. Vulkan cannot sample a buffer through an image descriptor, so the buffer is copied into an image of 16384-texel rows (or a 1D image) on
//     the GPU, from the same guest memory, outside the render pass and between full barriers: at its first use in a frame and again after a GPU write into its memory was
//     recorded since its last copy (vka_tbuf_wrote, from the texture code's write tracking, vka_tex.c gw_add), so it reads what the work before it wrote
// Unreal Engine 5 reads its per-object data in vertex shaders this way (Orbitals: the title screen's one plane read zeros and was never drawn, the background stayed black).
// Not done: a buffer texture bound as a storage image (its writes would have to go back to the memory); it reads and writes nothing (logged)
#include "vka.h"
#include "vka_tbuf_align.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

uint64_t vka_frame(void);
void vka_mem_describe(uint64_t addr, char *out, size_t n);   // (vka_mem.c, diagnostics)
enum { ROW = 16384, NVIEW = 4096, NIMG = 4096, TBUF_HASH_MAX = 65536 };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_twrote_gen;
static _Atomic uint32_t g_tv_retire_gen;
static _Atomic uint8_t g_fmt_ok[256];   // bit 0: cached, bit 1: uniform texel supported, bit 2: storage texel supported
enum { NTCOPY = 64 };
typedef struct { uint64_t addr, range, wgen; VkFormat fmt; VkBufferView cv; } tcopy_ent;
static tcopy_ent g_tcopy[NTCOPY];
static _Atomic int g_ntcopy;
static uint64_t g_tc_lo, g_tc_hi;
typedef struct { uint32_t id, ver, sgen, rgen; int mode; VkBufferView v; } tbm_ent;
static tbm_ent g_tbm[256];
typedef struct { int id, mode; uint8_t dim; uint32_t ver; uint64_t fr, wgen; } tpm_ent;
static tpm_ent g_tpm[256];
static int g_tbuf_mode = 1;

void vka_tbuf_sub_begin(void) {
    g_tbuf_mode = VKA_ENV("DM_VK_TBUF", 1);
    pthread_mutex_lock(&g_mu);
    atomic_store_explicit(&g_ntcopy, 0, memory_order_release);
    g_tc_lo = g_tc_hi = 0;
    pthread_mutex_unlock(&g_mu);
}
uint64_t vka_tbuf_wrote_gen(void) {
    return atomic_load_explicit(&g_twrote_gen, memory_order_acquire);
}
static int tbuf_fmt_ok(VkFormat fmt, int writes) {
    if ((uint32_t)fmt < 256u) {
        uint8_t c = atomic_load_explicit(&g_fmt_ok[(uint32_t)fmt], memory_order_acquire);
        if (c & 1u) return writes ? (c & 4u) != 0 : (c & 2u) != 0;
        VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, fmt, &fp);
        uint8_t nv = 1u
            | ((fp.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) ? 2u : 0u)
            | ((fp.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) ? 4u : 0u);
        atomic_store_explicit(&g_fmt_ok[(uint32_t)fmt], nv, memory_order_release);
        return writes ? (nv & 4u) != 0 : (nv & 2u) != 0;
    }
    VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, fmt, &fp);
    VkFormatFeatureFlags need = writes ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    return (fp.bufferFeatures & need) != 0;
}

typedef struct { int id, format; uint64_t addr, bytes; uint32_t elems; vka_fmt f; } tb_t;
static int tb_of(int id, tb_t *t) {
    if (!vka_tex_id_buffer(id, &t->addr, &t->elems, &t->format) || !t->elems) return 0;
    if (!vka_fmt_of(t->format, &t->f)) return 0;   // (logged by vka_fmt_of)
    if (t->f.bw != 1 || t->f.bh != 1 || t->f.kind >= 3 || !t->f.bpb) { VRED_ONCE("buffer texture of NVN format %#x: not a plain colour format, not bound", t->format); return 0; }
    t->id = id; t->bytes = (uint64_t)t->elems * t->f.bpb;
    return 1;
}
static int not_placed(const tb_t *t, const char *as) {
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) { char seg[400]; vka_mem_describe(t->addr, seg, sizeof seg);
        VRED("buffer texture ID %d (%u elements, format %#x) at %#llx: its %llu bytes are not in placed guest memory, it is not bound as a %s (%s)", t->id, t->elems, t->format,
             (unsigned long long)t->addr, (unsigned long long)t->bytes, as, seg); }
    return 0;
}

// ---------- Texel buffer views ----------
typedef struct { VkBuffer buf; VkDeviceSize off, range; VkFormat fmt; VkBufferView v; uint64_t last; } tview;
static tview g_v[NVIEW]; static int g_nv; static uint32_t g_vgen;
static int16_t g_vhead[NVIEW], g_vnext[NVIEW];   // 1-based slot indices (0 = empty)
static uint32_t view_h(VkBuffer buf, VkDeviceSize off, VkDeviceSize range, VkFormat fmt) {
    uint64_t h = (uint64_t)(uintptr_t)buf ^ (off * 0x9e3779b97f4a7c15ull) ^ (range << 16) ^ (uint64_t)(uint32_t)fmt;
    return (uint32_t)((h * 0x9e3779b97f4a7c15ull) >> 52) & (NVIEW - 1);
}
static void view_unlink(int slot) {
    uint32_t b = view_h(g_v[slot].buf, g_v[slot].off, g_v[slot].range, g_v[slot].fmt);
    for (int16_t *p = &g_vhead[b]; *p; p = &g_vnext[*p - 1]) if (*p == slot + 1) { *p = g_vnext[slot]; g_vnext[slot] = 0; return; }
}
static void view_link(int slot) {
    uint32_t b = view_h(g_v[slot].buf, g_v[slot].off, g_v[slot].range, g_v[slot].fmt);
    g_vnext[slot] = g_vhead[b]; g_vhead[b] = (int16_t)(slot + 1);
}
static void destroy_view(void *a) { vk.vkDestroyBufferView(V.dev, (VkBufferView)a, NULL); }
static void retire_view(VkBufferView v) { if (v) vka_after(vka_fence_last(), destroy_view, (void *)v); }
// DM_VK_TBUF (live; A/B): 1 (default) binds buffer textures as above, 2 leaves the storage texel buffers unbound (shaders read buffer textures but write none), 0 binds none (as before)
static int tbuf_mode(void) { return g_tbuf_mode; }
VkBufferView vka_tbuf_view(int id, int writes) {
    int mode = tbuf_mode(); if (!mode || (writes && mode == 2)) return VK_NULL_HANDLE;
    uint32_t ver = 0, sgen = 0, rgen = 0;
    tbm_ent *bm = NULL;
    if (!writes && mode == 1 && id > 0) {
        ver = vka_tex_id_ver(id);
        sgen = atomic_load_explicit(&vka_seg_gen, memory_order_acquire);
        rgen = atomic_load_explicit(&g_tv_retire_gen, memory_order_acquire);
        bm = &g_tbm[(uint32_t)id & 255u];
        if (bm->id == (uint32_t)id && bm->ver == ver && bm->sgen == sgen && bm->rgen == rgen && bm->mode == mode && bm->v) {
            return bm->v;
        }
    }
    tb_t t; if (!tb_of(id, &t)) return VK_NULL_HANDLE;
    uint64_t max = V.props.limits.maxTexelBufferElements, n = t.elems < max ? t.elems : max;
    if (n < t.elems) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 4) VRED("buffer texture ID %d: %u elements, the device's texel buffers hold %llu: the rest reads as zero", id, t.elems, (unsigned long long)max); }
    VkDeviceSize range = (VkDeviceSize)n * t.f.bpb, off = 0; VkBuffer buf = VK_NULL_HANDLE;
    if (vka_addr_run(t.addr, range, &buf, &off) < range) { not_placed(&t, "texel buffer"); return VK_NULL_HANDLE; }
    VkFormat fmt = t.f.vk;   // (an sRGB format's raw twin: a texel buffer has no sRGB decode)
    if (!tbuf_fmt_ok(fmt, writes)) {
        static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8)
            VRED("buffer texture ID %d: VkFormat %d is not a %s texel buffer format on this device: not bound", id, fmt, writes ? "storage" : "uniform");
        return VK_NULL_HANDLE;
    }
    VkDeviceSize al = V.props.limits.minTexelBufferOffsetAlignment;
    if (al > 1 && off % al) {
        // A view must start at a multiple of the device's alignment, which NVN does not require of a buffer texture's pool offset. A read-only one small enough to
        // snapshot is copied into aligned scratch (host visible and coherent, so no copy is recorded and this works with a render pass open); its bytes are those the
        // CPU wrote before this draw, as with the Metal tbuf_shadow. A write through a copied view could not go back to guest memory, so it stays unbound.
        if (vka_tbuf_copy(writes, range, off, (uint32_t)al)) {
            uint64_t wgen = atomic_load_explicit(&g_twrote_gen, memory_order_acquire);
            int nt = atomic_load_explicit(&g_ntcopy, memory_order_acquire);
            for (int i = 0; i < nt; i++) {
                if (g_tcopy[i].range == range && g_tcopy[i].addr == t.addr && g_tcopy[i].fmt == fmt && g_tcopy[i].wgen == wgen && g_tcopy[i].cv) {
                    return g_tcopy[i].cv;
                }
            }
            vka_span sp;
            if (vka_scratch(range, al > 16 ? al : 16, &sp) &&
                (vka_tex_gw_read(t.addr, range, sp.ptr) || vka_cpu_read(t.addr, sp.ptr, (size_t)range))) {
                VkBufferViewCreateInfo si = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = sp.buf, .format = fmt, .offset = sp.off, .range = range};
                VkBufferView cv = VK_NULL_HANDLE;
                if (!vk.vkCreateBufferView(V.dev, &si, NULL, &cv)) {
                    vka_after(vka_fence_last() + 1, destroy_view, (void *)(uintptr_t)cv);
                    pthread_mutex_lock(&g_mu);
                    int ntc = atomic_load_explicit(&g_ntcopy, memory_order_relaxed);
                    static int rr;
                    int slot = ntc < NTCOPY ? ntc : (rr++ & (NTCOPY - 1));
                    g_tcopy[slot] = (tcopy_ent){t.addr, (uint64_t)range, atomic_load_explicit(&g_twrote_gen, memory_order_acquire), fmt, cv};
                    if (ntc < NTCOPY) atomic_store_explicit(&g_ntcopy, ntc + 1, memory_order_release);
                    uint64_t hi = t.addr + (uint64_t)range;
                    if (!g_tc_hi || t.addr < g_tc_lo) g_tc_lo = t.addr;
                    if (hi > g_tc_hi) g_tc_hi = hi;
                    pthread_mutex_unlock(&g_mu);
                    return cv;
                }
            }
        }
        static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8)
            VRED("buffer texture ID %d at %#llx: offset %#llx in its segment is not a multiple of the device's texel buffer alignment %llu: not bound", id, (unsigned long long)t.addr, (unsigned long long)off, (unsigned long long)al);
        return VK_NULL_HANDLE;
    }
    if (writes) vka_tex_gw_opaque(t.addr, range);   // the shader may store into the memory: images over it are uploaded again (vka_tex.c)
    uint64_t now = vka_frame();
    pthread_mutex_lock(&g_mu);
    uint32_t gen = atomic_load(&vka_seg_gen);
    if (gen != g_vgen) {
        for (int i = 0; i < g_nv; i++) retire_view(g_v[i].v);
        g_nv = 0; memset(g_vhead, 0, sizeof g_vhead); g_vgen = gen;
        atomic_fetch_add_explicit(&g_tv_retire_gen, 1, memory_order_release);
    }
    for (int i = g_vhead[view_h(buf, off, range, fmt)]; i; i = g_vnext[i - 1]) {
        tview *tv = &g_v[i - 1];
        if (tv->buf == buf && tv->off == off && tv->range == range && tv->fmt == fmt) {
            tv->last = now; VkBufferView v = tv->v;
            uint32_t rg = atomic_load_explicit(&g_tv_retire_gen, memory_order_acquire);
            pthread_mutex_unlock(&g_mu);
            if (bm && v && gen == sgen) *bm = (tbm_ent){(uint32_t)id, ver, sgen, rg, mode, v};
            return v;
        }
    }
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buf, .format = fmt, .offset = off, .range = range};
    VkBufferView v = VK_NULL_HANDLE; VkResult r = vk.vkCreateBufferView(V.dev, &ci, NULL, &v);
    if (r) { pthread_mutex_unlock(&g_mu); static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8) VRED("buffer texture ID %d: vkCreateBufferView (VkFormat %d, %llu bytes) failed (%d): not bound", id, fmt, (unsigned long long)range, r); return VK_NULL_HANDLE; }
    int slot = g_nv < NVIEW ? g_nv++ : 0;
    if (slot == 0 && g_nv == NVIEW) {
        for (int i = 1; i < NVIEW; i++) if (g_v[i].last < g_v[slot].last) slot = i;
        view_unlink(slot); retire_view(g_v[slot].v);
        atomic_fetch_add_explicit(&g_tv_retire_gen, 1, memory_order_release);
    }
    g_v[slot] = (tview){buf, off, range, fmt, v, now};
    view_link(slot);
    static int noted; if (!noted++) VLOG("buffer textures: bound as texel buffers over guest memory (first: texture ID %d, %u elements, VkFormat %d)", id, t.elems, fmt);
    uint32_t rg = atomic_load_explicit(&g_tv_retire_gen, memory_order_acquire);
    pthread_mutex_unlock(&g_mu);
    if (bm && v && gen == sgen) *bm = (tbm_ent){(uint32_t)id, ver, sgen, rg, mode, v};
    return v;
}

// ---------- Images of buffer textures ----------
typedef struct { uint64_t addr, bytes, last, copied_frame, hash; uint32_t elems, w, h; int format, is2d, laid, dirty, hash_ok; VkImage img; VkDeviceMemory mem; VkImageView v; } timg;
static timg g_i[NIMG]; static _Atomic int g_ni;
static int16_t g_ihead[NIMG], g_inext[NIMG];   // 1-based slot indices (0 = empty)
static uint64_t g_i_lo, g_i_hi;                // address span of cached images, for fast rejection in vka_tbuf_wrote
static uint32_t img_h(uint64_t addr, uint32_t elems, int format, int is2d) {
    uint64_t h = addr ^ ((uint64_t)elems << 32) ^ ((uint64_t)(uint32_t)format << 1) ^ (uint64_t)(is2d != 0);
    return (uint32_t)((h * 0x9e3779b97f4a7c15ull) >> 52) & (NIMG - 1);
}
static void img_unlink(int slot) {
    uint32_t b = img_h(g_i[slot].addr, g_i[slot].elems, g_i[slot].format, g_i[slot].is2d);
    for (int16_t *p = &g_ihead[b]; *p; p = &g_inext[*p - 1]) if (*p == slot + 1) { *p = g_inext[slot]; g_inext[slot] = 0; return; }
}
static void img_link(int slot) {
    uint32_t b = img_h(g_i[slot].addr, g_i[slot].elems, g_i[slot].format, g_i[slot].is2d);
    g_inext[slot] = g_ihead[b]; g_ihead[b] = (int16_t)(slot + 1);
    uint64_t lo = g_i[slot].addr, hi = lo + g_i[slot].bytes;
    if (!g_i_hi || lo < g_i_lo) g_i_lo = lo;
    if (hi > g_i_hi) g_i_hi = hi;
}
// Hash of guest bytes [addr, addr + bytes) as the GPU sees them (vka_tex_gw_read). Must be called WITHOUT g_mu held (vka_tex_gw_read locks g_tmu, and gw_add calls vka_tbuf_wrote under g_tmu).
static int tbuf_hash(uint64_t addr, uint64_t bytes, uint64_t *out) {
    if (!bytes || bytes > TBUF_HASH_MAX) return 0;
    static _Thread_local uint8_t tmp[TBUF_HASH_MAX];
    if (!vka_tex_gw_read(addr, bytes, tmp)) return 0;
    uint64_t h = 0x9e3779b97f4a7c15ull ^ bytes, w;
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) { memcpy(&w, tmp + i, 8); h ^= w * 0x87c37b91114253d5ull; h = ((h << 31) | (h >> 33)) * 0x4cf5ad432745937full; }
    if (i < bytes) { w = 0; memcpy(&w, tmp + i, (size_t)(bytes - i)); h ^= w * 0x87c37b91114253d5ull; h = ((h << 31) | (h >> 33)) * 0x4cf5ad432745937full; }
    *out = h;
    return 1;
}
void vka_tbuf_wrote(uint64_t addr, uint64_t n) {
    int ni = atomic_load_explicit(&g_ni, memory_order_relaxed);
    int ntc = atomic_load_explicit(&g_ntcopy, memory_order_relaxed);
    if (!ni && !ntc) return;
    pthread_mutex_lock(&g_mu);
    int hit = 0;
    if (g_i_hi && addr < g_i_hi && g_i_lo < addr + n) {
        ni = atomic_load_explicit(&g_ni, memory_order_relaxed);
        for (int i = 0; i < ni; i++) if (addr < g_i[i].addr + g_i[i].bytes && g_i[i].addr < addr + n) { g_i[i].dirty = 1; g_i[i].hash_ok = 0; hit = 1; }
    }
    if (g_tc_hi && addr < g_tc_hi && g_tc_lo < addr + n) {
        ntc = atomic_load_explicit(&g_ntcopy, memory_order_relaxed);
        for (int i = 0; i < ntc; i++) if (g_tcopy[i].range && addr < g_tcopy[i].addr + g_tcopy[i].range && g_tcopy[i].addr < addr + n) { g_tcopy[i].range = 0; hit = 1; }
    }
    if (hit) atomic_fetch_add_explicit(&g_twrote_gen, 1, memory_order_release);
    pthread_mutex_unlock(&g_mu);
}
static void destroy_img(void *a) {
    timg *x = a;
    if (x->v) vk.vkDestroyImageView(V.dev, x->v, NULL);
    if (x->img) vk.vkDestroyImage(V.dev, x->img, NULL);
    if (x->mem) vk.vkFreeMemory(V.dev, x->mem, NULL);
    free(x);
}
static void retire_img(const timg *x) { timg *c = malloc(sizeof *c); if (!c) return; *c = *x; vka_after(vka_fence_last(), destroy_img, c); }
// The image shapes a buffer texture can be read as: 2D (rows of ROW texels) or 1D, no array, multisample or depth compare
static int shape(uint8_t dim, int *is2d) {
    if (dim & (DMSH_VK_DIM_ARRAY | DMSH_VK_DIM_MS | DMSH_VK_DIM_DEPTH)) return 0;
    int d = dim & DMSH_VK_DIM_MASK;
    if (d == 0 || d == 1) { *is2d = d == 1; return 1; }
    return 0;
}
static timg *img_find(const tb_t *t, int is2d) {   // (g_mu held)
    for (int i = g_ihead[img_h(t->addr, t->elems, t->format, is2d)]; i; i = g_inext[i - 1]) {
        timg *x = &g_i[i - 1];
        if (x->addr == t->addr && x->elems == t->elems && x->format == t->format && x->is2d == is2d) return x;
    }
    return NULL;
}
static timg *img_make(const tb_t *t, int is2d) {   // (g_mu held)
    uint32_t w, h;
    if (is2d) {
        w = t->elems < ROW ? t->elems : ROW; h = (t->elems + ROW - 1) / ROW;
        if (w > V.props.limits.maxImageDimension2D || h > V.props.limits.maxImageDimension2D) { VRED_ONCE("buffer texture of %u elements read as 2D rows of %d: larger than the device's 2D images, not bound", t->elems, ROW); return NULL; }
    } else {
        w = t->elems; h = 1;
        if (w > V.props.limits.maxImageDimension1D) { w = V.props.limits.maxImageDimension1D; VRED_ONCE("buffer texture of %u elements read as a 1D image: only its first %u (the device's 1D images)", t->elems, w); }
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = t->f.vk != t->f.view ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, .imageType = is2d ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_1D,
                            .format = t->f.vk, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    timg x = {t->addr, t->bytes, 0, 0, 0, t->elems, w, h, t->format, is2d, 0, 1, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &x.img);
    if (!r) {
        VkMemoryRequirements mr; vk.vkGetImageMemoryRequirements(V.dev, x.img, &mr);
        uint32_t ty = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); if (ty == UINT32_MAX) ty = vka_mem_type(mr.memoryTypeBits, 0, 0);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = ty};
        r = ty == UINT32_MAX ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vk.vkAllocateMemory(V.dev, &ai, NULL, &x.mem);
        if (!r) r = vk.vkBindImageMemory(V.dev, x.img, x.mem, 0);
    }
    if (!r) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = x.img, .viewType = is2d ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_1D, .format = t->f.view,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        r = vk.vkCreateImageView(V.dev, &vi, NULL, &x.v);
    }
    if (r) {
        static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8) VRED("buffer texture ID %d (%u elements, VkFormat %d): its %s image %ux%u could not be made (%d): not bound", t->id, t->elems, t->f.vk, is2d ? "2D" : "1D", w, h, r);
        x.laid = 0; timg *c = malloc(sizeof *c); if (c) { *c = x; destroy_img(c); }
        return NULL;
    }
    int ni = atomic_load_explicit(&g_ni, memory_order_relaxed);
    int slot = ni < NIMG ? ni : 0;
    if (ni < NIMG) atomic_store_explicit(&g_ni, ni + 1, memory_order_release);
    else { for (int i = 1; i < NIMG; i++) if (g_i[i].last < g_i[slot].last) slot = i; img_unlink(slot); retire_img(&g_i[slot]); }
    g_i[slot] = x;
    img_link(slot);
    return &g_i[slot];
}
int vka_tbuf_pending(int id, uint8_t dim) {
    if (!vka_tex_id_is_buffer(id)) return 0;
    int mode = tbuf_mode();
    if (!mode) return 0;
    uint32_t ver = vka_tex_id_ver(id);
    uint64_t fr = vka_frame(), wgen = atomic_load_explicit(&g_twrote_gen, memory_order_acquire);
    tpm_ent *pm = &g_tpm[((uint32_t)id * 7u + dim) & 255u];
    if (pm->id == id && pm->dim == dim && pm->mode == mode && pm->ver == ver && pm->fr == fr && pm->wgen == wgen) return 0;
    tb_t t; int is2d;
    if (!shape(dim, &is2d) || !tb_of(id, &t)) return 0;
    pthread_mutex_lock(&g_mu);
    timg *x = img_find(&t, is2d);
    if (!x) { pthread_mutex_unlock(&g_mu); return 1; }
    if (x->copied_frame == fr && !x->dirty) {
        pthread_mutex_unlock(&g_mu);
        *pm = (tpm_ent){id, mode, dim, ver, fr, wgen};
        return 0;
    }
    int can_hash = !x->dirty && x->laid && x->hash_ok && t.bytes <= TBUF_HASH_MAX && VKA_ENV("DM_VK_TBUF_HASH", 1);
    uint64_t old_hash = x->hash;
    pthread_mutex_unlock(&g_mu);
    if (can_hash) {   // skip the per-frame GPU copy and render-pass break when neither CPU nor GPU changed the buffer's bytes
        uint64_t h = 0;
        if (tbuf_hash(t.addr, t.bytes, &h) && h == old_hash) {
            pthread_mutex_lock(&g_mu);
            x = img_find(&t, is2d);
            if (x && !x->dirty && x->hash_ok && x->hash == old_hash) {
                x->copied_frame = fr; x->last = fr;
                pthread_mutex_unlock(&g_mu);
                *pm = (tpm_ent){id, mode, dim, ver, fr, wgen};
                return 0;
            }
            pthread_mutex_unlock(&g_mu);
        }
    }
    return 1;
}
void vka_tbuf_refresh(int id, uint8_t dim, VkCommandBuffer cb) {
    tb_t t; int is2d;
    if (!cb || !tb_of(id, &t)) return;
    if (!shape(dim, &is2d)) { VRED_ONCE("buffer texture ID %d read as an image of shape %#x (not 1D or 2D): not bound", id, dim); return; }
    uint64_t h = 0;
    int hok = t.bytes <= TBUF_HASH_MAX && VKA_ENV("DM_VK_TBUF_HASH", 1) && tbuf_hash(t.addr, t.bytes, &h);
    pthread_mutex_lock(&g_mu);
    timg *x = img_find(&t, is2d); if (!x) x = img_make(&t, is2d);
    uint64_t fr = vka_frame();
    if (!x || (x->copied_frame == fr && !x->dirty)) { pthread_mutex_unlock(&g_mu); return; }
    uint64_t n = is2d ? t.elems : x->w, bytes = n * t.f.bpb;
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0;
    if (vka_addr_run(t.addr, bytes, &buf, &off) < bytes) { pthread_mutex_unlock(&g_mu); not_placed(&t, "sampled image"); return; }
    if (off % t.f.bpb) {
        pthread_mutex_unlock(&g_mu); static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8)
            VRED("buffer texture ID %d at %#llx: offset %#llx in its segment is not a multiple of its texel size (%u): not copied into its image", id, (unsigned long long)t.addr, (unsigned long long)off, t.f.bpb);
        return;
    }
    vka_barrier(cb);   // the work recorded before (stores into the memory) before the copy reads it
    if (!x->laid) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = x->img,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        x->laid = 1;
    }
    VkBufferImageCopy rg[2]; uint32_t nr = 0;
    if (is2d) {
        uint32_t full = t.elems / ROW, rem = t.elems % ROW;
        if (full) rg[nr++] = (VkBufferImageCopy){off, ROW, full, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {ROW, full, 1}};
        if (rem) rg[nr++] = (VkBufferImageCopy){off + (VkDeviceSize)full * ROW * t.f.bpb, rem, 1, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, (int32_t)full, 0}, {rem, 1, 1}};
    } else rg[nr++] = (VkBufferImageCopy){off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {x->w, 1, 1}};
    vk.vkCmdCopyBufferToImage(cb, buf, x->img, VK_IMAGE_LAYOUT_GENERAL, nr, rg);
    vka_barrier(cb);   // the copy before the reads
    x->copied_frame = fr; x->dirty = 0; x->last = fr; x->hash = h; x->hash_ok = hok;
    static int noted; if (!noted++) VLOG("buffer textures: read as %s images, copied from guest memory on the GPU at their first use in a frame and after GPU writes (first: texture ID %d, %u elements, VkFormat %d)",
                                         is2d ? "2D (16384-texel rows)" : "1D", id, t.elems, t.f.vk);
    pthread_mutex_unlock(&g_mu);
}
VkImageView vka_tbuf_image_view(int id, uint8_t dim) {
    tb_t t; int is2d;
    if (!shape(dim, &is2d) || !tb_of(id, &t)) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    timg *x = img_find(&t, is2d); VkImageView v = VK_NULL_HANDLE;
    if (x && x->copied_frame) { x->last = vka_frame(); v = x->v; }
    pthread_mutex_unlock(&g_mu);
    return v;
}
