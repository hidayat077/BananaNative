// Pipelines of the Vulkan replay. The device has no extended dynamic state 3 and no shader objects (android/spike results), so a graphics pipeline is keyed on the state those
// would have made dynamic: attachment formats and sample count, blending and write masks per attachment, logic operation, polygon mode, depth clamp, alpha to coverage, the
// topology class and the depth convention (SpecId 0 on the last pre-raster stage), with vertex input in the key only on a device without VK_EXT_vertex_input_dynamic_state.
// Everything else is dynamic (EDS1 / EDS2): viewport and scissor (one of each), cull mode, front face, topology, depth and stencil tests, depth bias, rasterizer discard,
// primitive restart, blend constants. Pipelines and set layouts live as long as the process; the VkPipelineCache goes to the app's files (DM_TEMP, the game's own temp folder)
// so the next start builds from it. Layouts follow dmsh_abi_vk.h: set 0 the per-draw records of every stage (push descriptors when they fit), set 1 the pools (vka_tex.c)
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_memu_abi.h"
#include "vka_pwlist.h"
#include "vka_psogate.h"
#include "vka_pwbusy.h"
#include "vka_gpl_stats.h"
#include "vka_gpl_cache.h"
#include "vka_shader_variants.h"
#include "vka_aff.h"
#include "vk_android.h"
#include "vk_prewarm_seed.h"   // the profile's seed list (dm/games/<id>/vk_prewarm.bin; dm/games/game.py seed): dm_vk_prewarm_seed, dm_vk_prewarm_seed_len (0: none)
#include "../nvn/nvn_mtl.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// Suspend (mtl_drain, the app going to the background): the prewarm and optimizer threads stop between pipelines until the game submits again (vka_pso_pause(0) at the next
// submission), so a backgrounded app does not keep compiling on the CPU for minutes
static _Atomic int g_pso_paused;
void vka_pso_pause(int on) { atomic_store(&g_pso_paused, on != 0); }
static void pause_point(void) { while (atomic_load_explicit(&g_pso_paused, memory_order_relaxed)) usleep(100000); }
// The keys' hash, 8 bytes a step: the draw path hashes a pipeline key (about 1 KB) per draw, and FNV a byte at a time took 1 us of it on the Adreno 840 phone's cores
static inline uint64_t fnv(const void *p, size_t n, uint64_t h) { return vka_pw_fnv(p, n, h); }
_Static_assert(VKA_PW_TOPO_MAX == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST && VKA_PW_POLY_MAX == VK_POLYGON_MODE_POINT && VKA_PW_LOGIC_MAX == VK_LOGIC_OP_SET
               && VKA_PW_BLENDF_MAX == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA && VKA_PW_BLENDOP_MAX == VK_BLEND_OP_MAX && VKA_PW_FS_VARIANT_MAX == VKA_FS_RETYPE, "vka_pwlist.h bounds");

// A small map from a byte key to a value (layouts, pipelines): open addressing on the key's hash, keys copied
typedef struct { uint64_t h; void *key; void *val; } ment;
typedef struct { ment *e; size_t cap, n, klen; } kmap;
static void *km_get(kmap *m, const void *key, uint64_t h) {
    if (!m->cap) return NULL;
    for (size_t i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) { if (!m->e[i].key) return NULL; if (m->e[i].h == h && !memcmp(m->e[i].key, key, m->klen)) return m->e[i].val; }
}
static void km_put(kmap *m, const void *key, uint64_t h, void *val) {
    if (2 * (m->n + 1) > m->cap) {
        size_t nc = m->cap ? 2 * m->cap : 256; ment *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < m->cap; i++) if (m->e[i].key) { size_t j = m->e[i].h & (nc - 1); while (n[j].key) j = (j + 1) & (nc - 1); n[j] = m->e[i]; }
        free(m->e); m->e = n; m->cap = nc;
    }
    size_t j = h & (m->cap - 1); while (m->e[j].key) j = (j + 1) & (m->cap - 1);
    void *k = malloc(m->klen); memcpy(k, key, m->klen);
    m->e[j] = (ment){h, k, val}; m->n++;
}
// The value slot of key, to change an entry in place (NULL: not in the map)
static void **km_slot(kmap *m, const void *key, uint64_t h) {
    if (!m->cap) return NULL;
    for (size_t i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) { if (!m->e[i].key) return NULL; if (m->e[i].h == h && !memcmp(m->e[i].key, key, m->klen)) return &m->e[i].val; }
}

// ---------- Layouts ----------
static VkShaderStageFlags stage_bit(int vks) {
    static const VkShaderStageFlags b[8] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                            VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT};
    return vks >= 0 && vks < 8 ? b[vks] : 0;
}
enum { MAXB = 6 * DMSH_VK_STAGE_BINDINGS };   // (task and mesh shaders take the tess control and vertex ranges: dmsh_abi_vk.h)
typedef struct { uint32_t n, pc; struct { uint16_t binding; uint8_t desc, pad; uint16_t count; uint16_t stages; } b[MAXB]; } lkey;   // pc: the push constant's stages (a task / mesh shader's compute form, vka_memu.c)
typedef struct { VkDescriptorSetLayout s0, s1; } pl_ikey;
static kmap g_layouts = {.klen = sizeof(lkey)};
static kmap g_dsl_pre = {.klen = sizeof(lkey)};
static kmap g_dsl_fs = {.klen = sizeof(lkey)};
static kmap g_pl_indep = {.klen = sizeof(pl_ikey)};
static VkDescriptorSetLayout g_empty_dsl;
static uint64_t g_nlayouts;

// The layout of a set of shaders, remembered by the shaders themselves: building the bindings key (3 KB) and hashing it was a third of the replay's CPU time at 5,500 draws a frame
typedef struct { vka_shader *sh[VKA_NSTAGE]; uint64_t compute; } skey;
static kmap g_lshort = {.klen = sizeof(skey)};
static kmap g_lshort_mono = {.klen = sizeof(skey)};
static int layout_slow(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out);
static int layout_compact(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out);
int vka_layout_of(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out) {
    skey sk; memset(&sk, 0, sizeof sk);
    for (int st = 0; st < VKA_NSTAGE; st++) if ((st == 5) == (compute != 0)) sk.sh[st] = sh[st];
    sk.compute = compute != 0;
    uint64_t h = fnv(&sk, sizeof sk, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    vka_layout *L = km_get(&g_lshort, &sk, h);
    if (L) { *out = *L; pthread_mutex_unlock(&g_mu); return out->layout != VK_NULL_HANDLE; }
    pthread_mutex_unlock(&g_mu);
    memset(out, 0, sizeof *out);
    int ok = layout_slow(sh, compute, out);
    L = malloc(sizeof *L); *L = *out;
    pthread_mutex_lock(&g_mu);
    if (!km_get(&g_lshort, &sk, h)) km_put(&g_lshort, &sk, h, L); else free(L);
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int vka_layout_mono_of(vka_shader *const sh[VKA_NSTAGE], vka_layout *out) {
    if (!V.gpl_indep) return vka_layout_of(sh, 0, out);
    skey sk; memset(&sk, 0, sizeof sk);
    for (int st = 0; st < VKA_NSTAGE; st++) if (st != 5) sk.sh[st] = sh[st];
    uint64_t h = fnv(&sk, sizeof sk, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    vka_layout *L = km_get(&g_lshort_mono, &sk, h);
    if (L) { *out = *L; pthread_mutex_unlock(&g_mu); return out->layout != VK_NULL_HANDLE; }
    pthread_mutex_unlock(&g_mu);
    memset(out, 0, sizeof *out);
    int ok = layout_compact(sh, 0, out);
    L = malloc(sizeof *L); *L = *out;
    pthread_mutex_lock(&g_mu);
    if (!km_get(&g_lshort_mono, &sk, h)) km_put(&g_lshort_mono, &sk, h, L); else free(L);
    pthread_mutex_unlock(&g_mu);
    return ok;
}
// mode: 0 = all stages (legacy/compute), 1 = pre-raster graphics stages only, 2 = fragment stage only
static int build_lkey(vka_shader *const sh[VKA_NSTAGE], int compute, int mode, lkey *k, uint32_t *out_ndesc) {
    memset(k, 0, sizeof *k);
    uint32_t ndesc = 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        const vka_shader *s = sh[st];
        if (!s || (st == 5) != (compute != 0)) continue;
        if (mode == 1 && st == 1) continue;
        if (mode == 2 && st != 1) continue;
        for (unsigned i = 0; i < s->e->bind_count; i++) {
            const dmsh_vk_bind *b = &s->b[i];
            if (b->set != DMSH_VK_SET_DRAW || b->kind == DMSH_VK_KIND_TEX_POOL || b->kind == DMSH_VK_KIND_SAMP_POOL) continue;
            if (k->n == MAXB) { VRED_ONCE("more than %d set-0 bindings in one pipeline", MAXB); return 0; }
            k->b[k->n].binding = b->binding; k->b[k->n].desc = b->desc; k->b[k->n].count = b->count ? b->count : 1; k->b[k->n].stages = (uint16_t)stage_bit(s->e->stage);
            ndesc += k->b[k->n].count; k->n++;
        }
        if (s->memu && k->n + 2 <= MAXB) {   // a task / mesh shader drawn through compute (vka_memu.c): the arena, and for its compute form the records and the push constant
            k->b[k->n].binding = VKA_MEMU_ARENA_BINDING; k->b[k->n].desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; k->b[k->n].count = 1; k->b[k->n].stages = (uint16_t)stage_bit(s->e->stage); ndesc++; k->n++;
            if (s->memu == 1) { k->b[k->n].binding = VKA_MEMU_REC_BINDING; k->b[k->n].desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; k->b[k->n].count = 1; k->b[k->n].stages = (uint16_t)stage_bit(s->e->stage); ndesc++; k->n++;
                                k->pc |= (uint32_t)stage_bit(s->e->stage); }
        }
        if (s->tf_fx && k->n + 5 <= MAXB) {   // transform feedback capture: the four capture buffers and the control block (vk_android_spv.py tf_capture)
            for (int b = 0; b < 5; b++) { k->b[k->n].binding = b < 4 ? VKA_TF_BUF_BINDING(b) : VKA_TF_CTL_BINDING; k->b[k->n].desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; k->b[k->n].count = 1; k->b[k->n].stages = (uint16_t)stage_bit(s->e->stage); ndesc++; k->n++; }
        }
        if (s->gs_fx > 0 && k->n < MAXB) {   // the claim words of a geometry shader that writes memory (vka_spv_gs_claim)
            k->b[k->n].binding = VKA_GS_CLAIM_BINDING; k->b[k->n].desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; k->b[k->n].count = 1; k->b[k->n].stages = (uint16_t)stage_bit(s->e->stage); ndesc++; k->n++;
        }
    }
    // Sorted by binding, so the same set of records gives the same key whatever the order of stages
    for (uint32_t i = 1; i < k->n; i++) for (uint32_t j = i; j > 0 && k->b[j - 1].binding > k->b[j].binding; j--) { typeof(k->b[0]) t = k->b[j]; k->b[j] = k->b[j - 1]; k->b[j - 1] = t; }
    *out_ndesc = ndesc;
    return 1;
}
typedef struct { VkDescriptorSetLayout dsl; int push; uint32_t ndesc; } dsl_ent;
static VkDescriptorSetLayout dsl_get(kmap *m, const lkey *k, uint32_t ndesc, int allow_push, int *is_push) {   // caller holds g_mu
    uint64_t h = fnv(k, sizeof *k, 1469598103934665603ull);
    dsl_ent *e = km_get(m, k, h);
    if (e) { if (is_push) *is_push = e->push; return e->dsl; }
    e = calloc(1, sizeof *e);
    e->ndesc = ndesc;
    e->push = allow_push && V.push_desc && ndesc <= V.max_push;
    VkDescriptorSetLayoutBinding *lb = calloc(k->n ? k->n : 1, sizeof *lb);
    for (uint32_t i = 0; i < k->n; i++) lb[i] = (VkDescriptorSetLayoutBinding){k->b[i].binding, (VkDescriptorType)k->b[i].desc, k->b[i].count, k->b[i].stages, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = e->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = k->n, .pBindings = lb};
    VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &e->dsl);
    free(lb);
    if (r) { e->dsl = VK_NULL_HANDLE; VRED_ONCE("descriptor set layout of %u bindings: %d", k->n, r); }
    km_put(m, k, h, e);
    if (is_push) *is_push = e->push;
    return e->dsl;
}
static VkDescriptorSetLayout empty_dsl_locked(void) {   // caller holds g_mu
    if (!g_empty_dsl) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_empty_dsl)) g_empty_dsl = VK_NULL_HANDLE;
    }
    return g_empty_dsl;
}
static VkPipelineLayout pl_indep_get(VkDescriptorSetLayout s0, VkDescriptorSetLayout s1) {   // caller holds g_mu
    pl_ikey ik = {s0, s1};
    uint64_t h = fnv(&ik, sizeof ik, 1469598103934665603ull);
    VkPipelineLayout *hit = km_get(&g_pl_indep, &ik, h);
    if (hit) return *hit;
    VkDescriptorSetLayout emp = empty_dsl_locked();
    VkDescriptorSetLayout sets[4] = {s0 ? s0 : emp, s1 ? s1 : emp, vka_bindless_layout(), vka_pfb_layout()};
    uint32_t nsets = sets[2] ? (sets[3] ? 4 : 3) : 2;
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = nsets, .pSetLayouts = sets};
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkResult r = vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &pl);
    if (r) { pl = VK_NULL_HANDLE; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("independent-sets pipeline layout: %d", r); }
    VkPipelineLayout *val = malloc(sizeof *val); *val = pl;
    km_put(&g_pl_indep, &ik, h, val); g_nlayouts++;
    return pl;
}
static int layout_indep(vka_shader *const sh[VKA_NSTAGE], vka_layout *out) {
    lkey kpre, kfs; uint32_t nd_pre = 0, nd_fs = 0;
    if (!build_lkey(sh, 0, 1, &kpre, &nd_pre) || !build_lkey(sh, 0, 2, &kfs, &nd_fs)) return 0;
    pthread_mutex_lock(&g_mu);
    int push = 0;
    VkDescriptorSetLayout s0 = dsl_get(&g_dsl_pre, &kpre, nd_pre, 1, &push);
    VkDescriptorSetLayout s1 = sh[1] ? dsl_get(&g_dsl_fs, &kfs, nd_fs, 0, NULL) : VK_NULL_HANDLE;
    out->set0 = s0;
    out->set1 = s1;
    out->push = push;
    out->indep = 1;
    out->ndesc = nd_pre;
    out->fs_ndesc = nd_fs;
    out->pre_layout = s0 ? pl_indep_get(s0, VK_NULL_HANDLE) : VK_NULL_HANDLE;
    out->fs_layout = s1 ? pl_indep_get(VK_NULL_HANDLE, s1) : VK_NULL_HANDLE;
    out->layout = (s0 && (!sh[1] || s1)) ? pl_indep_get(s0, s1) : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
    return out->layout != VK_NULL_HANDLE;
}
int vka_layout_pre_of(vka_shader *const sh[VKA_NSTAGE], vka_layout *out) {
    memset(out, 0, sizeof *out);
    if (!V.gpl_indep) return 0;
    lkey kpre; uint32_t nd = 0;
    if (!build_lkey(sh, 0, 1, &kpre, &nd)) return 0;
    pthread_mutex_lock(&g_mu);
    int push = 0;
    VkDescriptorSetLayout s0 = dsl_get(&g_dsl_pre, &kpre, nd, 1, &push);
    out->set0 = s0; out->push = push; out->indep = 1; out->ndesc = nd;
    out->pre_layout = out->layout = s0 ? pl_indep_get(s0, VK_NULL_HANDLE) : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
    return out->layout != VK_NULL_HANDLE;
}
int vka_layout_fs_of(vka_shader *fs, vka_layout *out) {
    memset(out, 0, sizeof *out);
    if (!fs || !V.gpl_indep) return 0;
    vka_shader *sh[VKA_NSTAGE] = {0}; sh[1] = fs;
    lkey kfs; uint32_t nd = 0;
    if (!build_lkey(sh, 0, 2, &kfs, &nd)) return 0;
    pthread_mutex_lock(&g_mu);
    VkDescriptorSetLayout s1 = dsl_get(&g_dsl_fs, &kfs, nd, 0, NULL);
    out->set1 = s1; out->indep = 1; out->fs_ndesc = nd;
    out->fs_layout = out->layout = s1 ? pl_indep_get(VK_NULL_HANDLE, s1) : VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
    return out->layout != VK_NULL_HANDLE;
}
static int layout_compact(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out) {
    lkey k; uint32_t ndesc = 0;
    if (!build_lkey(sh, compute, 0, &k, &ndesc)) return 0;
    uint64_t h = fnv(&k, sizeof k, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    vka_layout *L = km_get(&g_layouts, &k, h);
    if (L) { *out = *L; pthread_mutex_unlock(&g_mu); return out->layout != VK_NULL_HANDLE; }
    L = calloc(1, sizeof *L);
    L->ndesc = ndesc; L->push = V.push_desc && ndesc <= V.max_push;
    VkDescriptorSetLayoutBinding *lb = calloc(k.n ? k.n : 1, sizeof *lb);
    for (uint32_t i = 0; i < k.n; i++) lb[i] = (VkDescriptorSetLayoutBinding){k.b[i].binding, (VkDescriptorType)k.b[i].desc, k.b[i].count, k.b[i].stages, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = L->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = k.n, .pBindings = lb};
    VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &L->set0);
    free(lb);
    if (!r) {
        // set 1 the pool (bindless tables), set 2 the pool reads' bitmap when they are reported (vka_tex.c "Texture pool reads")
        VkDescriptorSetLayout sets[3] = {L->set0, vka_bindless_layout(), vka_pfb_layout()};
        VkPushConstantRange pr = {(VkShaderStageFlags)k.pc, 0, VKA_MEMU_PUSH_BYTES};
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = !sets[1] ? 1 : sets[2] ? 3 : 2, .pSetLayouts = sets, .pushConstantRangeCount = k.pc ? 1 : 0, .pPushConstantRanges = &pr};
        r = vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &L->layout);
    }
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("pipeline layout of %u bindings (%u descriptors): %d", k.n, ndesc, r); L->layout = VK_NULL_HANDLE; }
    L->pre_layout = L->fs_layout = L->layout;
    km_put(&g_layouts, &k, h, L); g_nlayouts++;
    *out = *L;
    pthread_mutex_unlock(&g_mu);
    return out->layout != VK_NULL_HANDLE;
}
static int layout_slow(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out) {
    if (!compute && V.gpl_indep) return layout_indep(sh, out);
    return layout_compact(sh, compute, out);
}


// ---------- The pipeline cache ----------
// DM_VK_PCACHE (restart): unset = $DM_TEMP/vk_pipelines[-<driver>]-fp0.bin (-fp0: the cache's generation, vka_fpenv.h VKA_PCACHE_GEN); a path = that file, as named; 0 = no
// file (the run starts empty and keeps nothing: a cold run). DM_VK_GUEST_FPCR=1 keeps no file either: its pipelines are compiled with the game's FPCR, and no later run
// may read them (vka_fpenv.h). The files of the generation before (vk_pipelines[-<driver>].bin, pipelines compiled with the game's flush-to-zero FPCR before 2026-11-08)
// are removed when the default file is opened, with their stamps. Before 2026-11-03 "0"
// was taken as a file named 0 in the working folder, so a second "cold" run read the first one's pipelines back (stutter-agent cold2gpl: 10 MB read, warm)
// The file is kept for one shader library at a time: its stamp <file>.lib (vka_pckeep.h; written with each save) names the library it was saved with (vka_shader_lib_id) and
// lists the shaders whose pipelines it holds with their entry hashes. A file of another library is read when its shaders are mostly byte for byte the same in this one
// (a restamped library, a delta update): the stale shaders it holds (changed or dropped since) may be at most DM_VK_PCACHE_KEEP percent (restart; default 50; 0 = a file of
// another library is never read) of the live ones, counted over every update the file was carried across; otherwise, or with no stamp, the run starts empty and its first
// save replaces the file. Before 2026-11-08 the file kept the shaders of every library ever played, and a driver that deserializes and uploads every shader of a cache when
// it is opened (Mesa's vk_pipeline_cache_load; Turnip gives each its code and private memory) paid for all of them: Ocarina of Time's 72 MB Turnip cache held 1,791 shaders,
// two of them from libraries before the HMMA spill fix with 80 and 54 KB of private memory a fiber, and opening it committed 5.2 GB of GPU memory, after which the game
// was killed by lmkd at its first 3D frames (docs/reference/memory.md "Pipeline cache of earlier shader libraries"). From then until the stamp listed its shaders, every
// library update started empty, and the first start after one waited minutes for its pipelines (docs/reference/memory.md "A shader library update that threw the
// pipeline cache away")
static uint64_t g_pc_lib;   // the library this run's cache file is stamped with (0: none known; no stamp written)
static vka_pck_stamp g_pck;   // the shaders the file was read with that this library has the same, and the stale estimate (the stamp's; written again with this run's)
static uint64_t pck_hash_of(void *ctx, uint64_t key) { (void)ctx; return vka_shader_content(key); }
static VkPipelineCache g_pc; static char g_pc_path[1024], g_pw_path[1100], g_pw_shared[1100]; static uint64_t g_made, g_saved_at, g_save_ns; static double g_create_ms;
static uint64_t g_ngfx, g_ncs, g_nfail, g_ncs_over, g_create_ns, g_frame_made;   // all pipelines made (frame path, background and prewarm); g_frame_made / g_create_ns: on the frame path only
static uint64_t g_probe_hit, g_probe_miss, g_probe_miss_ns;          // GPL: the cache-only try before a link (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)
static _Atomic uint64_t g_save_ns_total, g_hy_draws, g_pw_wait_ns;
static void pw_load(void); static void pw_start(void); static int pw_save(void); static int pw_dirty(void); static void unf_load(void); static void heavy_load(void);
static void *saver_main(void *a);
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;   // one save at a time (the saver thread, a forced save)
static pthread_mutex_t g_sv_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_sv_cv = PTHREAD_COND_INITIALIZER; static int g_sv_want, g_sv_up;

// ---------- Boot #1 GPL + Compute pre-compile & sharded binary cache (vka_gpl_cache.h) ----------
static int g_gpl_cache_on;
static char g_gpl_cache_path[1100];
static vka_gpl_cache_hdr g_gpl_hdr;
static pthread_mutex_t g_merge_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_pre_active, g_pre_compiled, g_pre_total, g_pre_skip, g_pre_stop, g_pre_checkpoint_want, g_pre_active_workers;
static _Atomic uint64_t g_pre_t0_ns;

enum { SHARD_LRU_CAP = 64 };
typedef struct {
    int shard_id;
    VkPipelineCache pc;
    uint32_t size;
    int ref;
    int loading;
    uint64_t tick;
} shard_lru_slot;
static struct {
    void *map_base;
    size_t map_size;
    const vka_gpl_shard_ent *table;
    uint32_t shard_count;
} g_shard_store;
static shard_lru_slot g_shard_lru[SHARD_LRU_CAP];
static size_t g_shard_open_bytes;
static uint64_t g_shard_tick;
static pthread_mutex_t g_shard_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_shard_cv = PTHREAD_COND_INITIALIZER;

static VkPipelineCache g_pc_retype;
static char g_retype_cache_path[1100];
static vka_gpl_cache_hdr g_retype_hdr;
static int g_retype_cache_loaded;
static _Atomic int g_retype_dirty;
static _Atomic uint64_t g_retype_cache_bytes;

static uint64_t g_nlib[5], g_nlinked, g_nopt; static double g_lib_ms, g_link_ms;
static VkPipelineCache g_pc_mono;
static char g_mono_cache_path[1100];
static vka_gpl_cache_hdr g_mono_hdr;
static int g_mono_cache_loaded;
static _Atomic int g_mono_dirty;
static _Atomic uint64_t g_mono_cache_bytes, g_mono_made;

static char g_svar_path[1100];
static vka_svar_set g_svar;
static pthread_mutex_t g_svar_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_svar_dirty;
static _Atomic uint64_t g_svar_loaded_n, g_svar_precompiled_n, g_svar_saved_n;
static uint8_t *g_pw_load; static size_t g_pw_load_len; static uint32_t g_pw_load_n;   // the loaded records, read by Boot #1 / prewarm threads (not changed after pw_load)
enum { L_VI, L_PRE, L_FS, L_FO, L_PREFS, NLIB };
static void vka_boot_precompile_run(const vka_shlib *L, int rw_fd, vka_gpl_shard_ent *table, uint32_t shard_count, uint64_t append_off);
static void vka_retype_variants_startup(const vka_shlib *L, const char *dir, uint64_t lib_hash, int gl_depth);
static void mono_cache_startup(const char *dir, uint64_t lib_hash, int gl_depth);
static void svar_record(const vka_svar_rec *rec);
static int vkf_int_class(uint32_t f);
static void precomp_fill_dyn(VkDynamicState *out_dyn, VkPipelineDynamicStateCreateInfo *out_dy, int part);
static VkPipelineCache shard_pc_acquire(uint64_t shader_key, int *out_slot) {
    if (out_slot) *out_slot = -1;
    if (!g_gpl_cache_on || !g_shard_store.map_base || !g_shard_store.table || !g_shard_store.shard_count || !shader_key)
        return VK_NULL_HANDLE;
    uint32_t sid = vka_gpl_shard_of(shader_key, g_shard_store.shard_count);
    vka_gpl_shard_ent ent = g_shard_store.table[sid];
    if (!(ent.entries & VKA_GPL_SHARD_DONE) || !ent.size) return VK_NULL_HANDLE;

    int mb = VKA_ENV("DM_VK_GPL_SHARD_BUDGET_MB", VKA_ENV("DM_VK_RAM_SAVER", 0) >= 2 ? 16 : 32);
    if (mb < 8) mb = 8;
    size_t budget = (size_t)mb << 20;
    VkPipelineCache dead[SHARD_LRU_CAP];
    int ndead = 0;

    pthread_mutex_lock(&g_shard_mu);
    for (;;) {
        int found = -1, free_slot = -1;
        for (int i = 0; i < SHARD_LRU_CAP; i++) {
            if (g_shard_lru[i].shard_id == (int)sid) { found = i; break; }
            if (g_shard_lru[i].shard_id < 0 && free_slot < 0) free_slot = i;
        }
        if (found >= 0) {
            if (g_shard_lru[found].loading) {
                pthread_cond_wait(&g_shard_cv, &g_shard_mu);
                continue;
            }
            if (g_shard_lru[found].pc != VK_NULL_HANDLE) {
                g_shard_lru[found].ref++;
                g_shard_lru[found].tick = ++g_shard_tick;
                VkPipelineCache ret = g_shard_lru[found].pc;
                pthread_mutex_unlock(&g_shard_mu);
                if (out_slot) *out_slot = found;
                return ret;
            }
            free_slot = found;
        }
        // Evict unreferenced LRU shards while over budget or when all LRU slots are occupied.
        while (free_slot < 0 || (g_shard_open_bytes + ent.size > budget)) {
            int victim = -1;
            uint64_t best_tick = UINT64_MAX;
            for (int i = 0; i < SHARD_LRU_CAP; i++) {
                if (g_shard_lru[i].shard_id >= 0 && !g_shard_lru[i].loading && g_shard_lru[i].ref == 0 && g_shard_lru[i].tick < best_tick) {
                    best_tick = g_shard_lru[i].tick;
                    victim = i;
                }
            }
            if (victim < 0) break;
            if (g_shard_lru[victim].pc != VK_NULL_HANDLE && ndead < SHARD_LRU_CAP)
                dead[ndead++] = g_shard_lru[victim].pc;
            if (g_shard_open_bytes >= g_shard_lru[victim].size) g_shard_open_bytes -= g_shard_lru[victim].size;
            else g_shard_open_bytes = 0;
            g_shard_lru[victim].shard_id = -1;
            g_shard_lru[victim].pc = VK_NULL_HANDLE;
            g_shard_lru[victim].size = 0;
            if (free_slot < 0) free_slot = victim;
        }
        if (free_slot < 0) {
            pthread_cond_wait(&g_shard_cv, &g_shard_mu);
            continue;
        }
        g_shard_lru[free_slot].shard_id = (int)sid;
        g_shard_lru[free_slot].loading = 1;
        g_shard_lru[free_slot].ref = 1;
        g_shard_lru[free_slot].size = ent.size;
        g_shard_lru[free_slot].tick = ++g_shard_tick;
        g_shard_lru[free_slot].pc = VK_NULL_HANDLE;
        pthread_mutex_unlock(&g_shard_mu);

        for (int i = 0; i < ndead; i++) vk.vkDestroyPipelineCache(V.dev, dead[i], NULL);
        const uint8_t *blob = (const uint8_t *)g_shard_store.map_base + ent.offset;
        VkPipelineCache pc = VK_NULL_HANDLE;
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = ent.size, .pInitialData = blob};
        if (vk.vkCreatePipelineCache(V.dev, &ci, NULL, &pc) != VK_SUCCESS) pc = VK_NULL_HANDLE;
        // Release page tables for the read-only mmap region immediately after driver ingest.
        uintptr_t p0 = ((uintptr_t)blob + 4095u) & ~(uintptr_t)4095u;
        uintptr_t p1 = ((uintptr_t)blob + ent.size) & ~(uintptr_t)4095u;
        if (p1 > p0) madvise((void *)p0, (size_t)(p1 - p0), MADV_DONTNEED);

        pthread_mutex_lock(&g_shard_mu);
        g_shard_lru[free_slot].loading = 0;
        if (pc) {
            g_shard_lru[free_slot].pc = pc;
            g_shard_open_bytes += ent.size;
        } else {
            g_shard_lru[free_slot].shard_id = -1;
            g_shard_lru[free_slot].ref = 0;
            g_shard_lru[free_slot].size = 0;
        }
        pthread_cond_broadcast(&g_shard_cv);
        pthread_mutex_unlock(&g_shard_mu);
        if (pc && out_slot) *out_slot = free_slot;
        return pc;
    }
}
static void shard_pc_release(int slot) {
    if (slot < 0 || slot >= SHARD_LRU_CAP) return;
    pthread_mutex_lock(&g_shard_mu);
    if (g_shard_lru[slot].ref > 0) g_shard_lru[slot].ref--;
    pthread_cond_broadcast(&g_shard_cv);
    pthread_mutex_unlock(&g_shard_mu);
}
static void precomp_pw_variants(const vka_shlib *L) {
    if (!g_pw_load || !g_pw_load_len) return;
    uint32_t added = 0;
    for (size_t o = 0; o + sizeof(pwrec) <= g_pw_load_len;) {
        pwrec r; memcpy(&r, g_pw_load + o, sizeof r);
        const uint8_t *kb = g_pw_load + o + sizeof r;
        o += vka_pw_size(&r);
        if (r.kind == VKA_PW_CS && r.klen == VKA_PW_CS_MASKS && r.key[0]) {
            uint64_t m[2]; memcpy(m, kb, sizeof m);
            if (!(m[0] | m[1])) continue;
            const dmsh_vk_entry *e = vka_shlib_find(L, r.key[0]);
            if (!e) continue;
            vka_svar_rec vr = {0};
            vr.shader_hash = e->key;
            vr.stage = DMSH_VK_STAGE_CS;
            vr.tex_u = m[0]; vr.tex_s = m[1];
            svar_record(&vr); added++;
        } else if (r.kind == VKA_PW_GFX) {
            vka_gkey k; memset(&k, 0, sizeof k); memcpy(&k, kb, r.klen < sizeof k ? r.klen : sizeof k);
            if (k.vin_static != (uint32_t)!V.vin_dyn) continue;
            if (r.key[1]) {
                const dmsh_vk_entry *e = vka_shlib_find(L, r.key[1]);
                if (e && e->stage == DMSH_VK_STAGE_FS) {
                    uint32_t fu = 0, fsm = 0;
                    if (k.fs_variant == VKA_FS_RETYPE) {
                        for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
                            int c = vkf_int_class(k.cfmt[i]);
                            if (c && (e->fout_mask >> i & 1) && (e->fout_types >> (2 * i) & 3) == DMSH_VK_TYPE_FLOAT) {
                                if (c == 1) fu |= 1u << i; else fsm |= 1u << i;
                            }
                        }
                    }
                    uint8_t afunc = (k.alpha_func != 0 && (e->flags & DMSH_VK_F_ALPHA_TEST)) ? k.alpha_func : 0;
                    uint64_t tu = k.tex_u[DMSH_VK_STAGE_FS], ts = k.tex_s[DMSH_VK_STAGE_FS];
                    if (fu || fsm || afunc || tu || ts) {
                        uint32_t eds_fs = V.eds_mask;
                        if (vka_classic_rp()) eds_fs &= ~(uint32_t)(VKA_EDS_RAST_SAMPLES | VKA_EDS_SAMPLE_MASK);
                        vka_svar_rec vr = {0};
                        vr.shader_hash = e->key;
                        vr.stage = DMSH_VK_STAGE_FS;
                        vr.lib_variant = (uint8_t)(k.fs_variant == VKA_FS_RETYPE ? DMSH_VK_MOD_BASE : k.fs_variant);
                        vr.fout_u = fu; vr.fout_s = fsm;
                        vr.tex_u = tu; vr.tex_s = ts;
                        vr.alpha_func = afunc;
                        vr.samples = (eds_fs & VKA_EDS_RAST_SAMPLES) ? 1u : (k.samples ? k.samples : 1u);
                        if (k.a2c) vr.flags |= VKA_SVAR_F_A2C;
                        svar_record(&vr); added++;
                    }
                }
            }
            if ((k.vin_u | k.vin_s) && r.key[0]) {
                const dmsh_vk_entry *e = vka_shlib_find(L, r.key[0]);
                if (e && e->stage == DMSH_VK_STAGE_VS) {
                    vka_svar_rec vr = {0};
                    vr.shader_hash = e->key;
                    vr.stage = DMSH_VK_STAGE_VS;
                    vr.lib_variant = k.gs_fx == 2 && (e->flags & DMSH_VK_F_TF) ? DMSH_VK_MOD_TF : DMSH_VK_MOD_BASE;
                    vr.vin_u = k.vin_u; vr.vin_s = k.vin_s;
                    vr.tex_u = k.tex_u[DMSH_VK_STAGE_VS]; vr.tex_s = k.tex_s[DMSH_VK_STAGE_VS];
                    if (!vka_depth_zero() && (e->flags & DMSH_VK_F_GL_DEPTH)) vr.flags |= VKA_SVAR_F_GL_DEPTH;
                    svar_record(&vr); added++;
                }
            }
        }
    }
    if (added > 0) VLOG("boot precompile: queued %u seeded variant record(s) into variant manifest", added);
}

__attribute__((visibility("default"))) int nxhost_precompile_status(int *compiled, int *total, uint64_t *elapsed_ms) {
    int act = atomic_load_explicit(&g_pre_active, memory_order_acquire);
    if (compiled) *compiled = atomic_load_explicit(&g_pre_compiled, memory_order_relaxed);
    if (total) *total = atomic_load_explicit(&g_pre_total, memory_order_relaxed);
    if (elapsed_ms) {
        uint64_t t0 = atomic_load_explicit(&g_pre_t0_ns, memory_order_relaxed);
        *elapsed_ms = (act && t0) ? (now_ns() - t0) / 1000000ull : 0;
    }
    return act;
}

__attribute__((visibility("default"))) void nxhost_precompile_skip(void) {
    atomic_store_explicit(&g_pre_skip, 1, memory_order_release);
}

static void init_pw_paths(const char *e, const char *t, const char *home) {
    const char *pf = getenv("DM_VK_PREWARM_FILE");
    g_pw_shared[0] = 0;
    if (pf && *pf) snprintf(g_pw_path, sizeof g_pw_path, "%s", pf);
    else if (e && *e && strcmp(e, "0")) snprintf(g_pw_path, sizeof g_pw_path, "%s.keys", e);
    else if ((t && *t) || (home && *home)) {
        const char *dir = t && *t ? t : home;
        vka_pwlist_name(g_pw_path, sizeof g_pw_path, dir, vka_driver_tag());
        snprintf(g_pw_shared, sizeof g_pw_shared, "%s/" VKA_PWLIST_SHARED, dir);
    }
}

static void pc_open_once(void) {
    const char *e = getenv("DM_VK_PCACHE"), *t = getenv("DM_TEMP"), *home = getenv("HOME");
    init_pw_paths(e, t, home);
    for (int i = 0; i < SHARD_LRU_CAP; i++) g_shard_lru[i].shard_id = -1;
    if (V.boot_precompile && !vka_classic_rp() && !vka_fp_guest() && !(e && !strcmp(e, "0"))) {
        vka_bindless_init();
        const vka_shlib *L = vka_shader_lib();
        const char *dir = (t && *t) ? t : (home && *home) ? home : NULL;
        if (L && dir && V.vin_dyn) {
            uint64_t lib_hash = vka_shader_lib_id();
            int gl_depth = !vka_depth_zero();
            uint32_t total = L->h->entry_count;
            uint32_t shard_count = vka_gpl_shard_count(total);
            if (vka_gpl_cache_filename(g_gpl_cache_path, sizeof g_gpl_cache_path, dir, DM_GAME_ID, lib_hash, V.props.pipelineCacheUUID) < (int)sizeof g_gpl_cache_path) {
                const char *bn = strrchr(g_gpl_cache_path, '/');
                int pruned = vka_gpl_cache_prune_stale(dir, DM_GAME_ID, bn ? bn + 1 : g_gpl_cache_path);
                if (pruned > 0) VLOG("gpl cache: pruned %d stale cache file(s) for %s", pruned, DM_GAME_ID);
                vka_gpl_cache_hdr hdr;
                const vka_gpl_shard_ent *ro_tab = NULL; void *ro_map = NULL; size_t ro_sz = 0; const char *why = NULL;
                int loaded_ro = vka_gpl_sharded_open_ro(g_gpl_cache_path, V.props.vendorID, V.props.deviceID, V.props.driverVersion,
                                                        V.props.pipelineCacheUUID, lib_hash, total, shard_count, gl_depth,
                                                        &hdr, &ro_tab, &ro_map, &ro_sz, &why);
                if (loaded_ro && (hdr.flags & VKA_GPL_CACHE_F_COMPLETE)) {
                    g_gpl_cache_on = 1;
                    g_gpl_hdr = hdr;
                    g_shard_store.map_base = ro_map;
                    g_shard_store.map_size = ro_sz;
                    g_shard_store.table = ro_tab;
                    g_shard_store.shard_count = shard_count;
                    VLOG("gpl cache %s: loaded complete Boot #2+ sharded cache (%u / %u shaders across %u shards, %.1f MB mmap)",
                         g_gpl_cache_path, hdr.compiled_entries, hdr.total_entries, shard_count, ro_sz / (1024.0 * 1024.0));
                } else {
                    if (loaded_ro) munmap(ro_map, ro_sz);
                    vka_gpl_shard_ent *tab = calloc(shard_count, sizeof *tab);
                    uint64_t append_off = 0;
                    int rw_fd = -1;
                    if (tab) {
                        rw_fd = vka_gpl_sharded_open_rw(g_gpl_cache_path, V.props.vendorID, V.props.deviceID, V.props.driverVersion,
                                                        V.props.pipelineCacheUUID, lib_hash, total, shard_count, gl_depth,
                                                        &hdr, tab, &append_off, &why);
                        if (rw_fd >= 0) {
                            VLOG("gpl cache %s: resuming partial Boot #1 sharded checkpoint (%u / %u shaders across %u shards)",
                                 g_gpl_cache_path, hdr.compiled_entries, hdr.total_entries, shard_count);
                        } else {
                            if (why && strcmp(why, "missing") != 0) {
                                unlink(g_gpl_cache_path);
                                VLOG("gpl cache %s: discarded (%s), starting fresh Boot #1 sharded compile", g_gpl_cache_path, why);
                            }
                            memset(&hdr, 0, sizeof hdr);
                            hdr.vendor_id = V.props.vendorID;
                            hdr.device_id = V.props.deviceID;
                            hdr.driver_version = V.props.driverVersion;
                            hdr.flags = gl_depth ? VKA_GPL_CACHE_F_GL_DEPTH : 0;
                            memcpy(hdr.pipeline_cache_uuid, V.props.pipelineCacheUUID, VK_UUID_SIZE);
                            hdr.dmsh_vk_lib_hash = lib_hash;
                            hdr.total_entries = total;
                            rw_fd = vka_gpl_sharded_create(g_gpl_cache_path, &hdr, shard_count, tab, &append_off);
                        }
                    }
                    if (rw_fd >= 0 && tab) {
                        g_gpl_cache_on = 1;
                        g_gpl_hdr = hdr;
                        pw_load();
                        vka_boot_precompile_run(L, rw_fd, tab, shard_count, append_off);
                        loaded_ro = vka_gpl_sharded_open_ro(g_gpl_cache_path, V.props.vendorID, V.props.deviceID, V.props.driverVersion,
                                                            V.props.pipelineCacheUUID, lib_hash, total, shard_count, gl_depth,
                                                            &hdr, &ro_tab, &ro_map, &ro_sz, &why);
                        if (loaded_ro) {
                            g_gpl_hdr = hdr;
                            g_shard_store.map_base = ro_map;
                            g_shard_store.map_size = ro_sz;
                            g_shard_store.table = ro_tab;
                            g_shard_store.shard_count = shard_count;
                        }
                    } else {
                        g_gpl_cache_path[0] = 0;
                    }
                    free(tab);
                }
                if (g_gpl_cache_on) {
                    vka_retype_variants_startup(L, dir, lib_hash, gl_depth);
                    mono_cache_startup(dir, lib_hash, gl_depth);
                    unf_load();
                    heavy_load();
                    pw_load();
                    pthread_t th;
                    if (!pthread_create(&th, NULL, saver_main, NULL)) { pthread_detach(th); g_sv_up = 1; }
                    pw_start();
                    return;
                }
            }
        }
    }
    if ((e && !strcmp(e, "0")) || vka_fp_guest()) g_pc_path[0] = 0;
    else if (e && *e) snprintf(g_pc_path, sizeof g_pc_path, "%s", e);
    else if ((t && *t) || (home && *home)) {   // a custom driver (DM_VK_DRIVER) keeps its own file, so switching drivers leaves the system driver's cache alone
        const char *dt = vka_driver_tag(), *dir = t && *t ? t : home;
        vka_pcache_name(g_pc_path, sizeof g_pc_path, dir, dt, VKA_PCACHE_GEN);
        char old[1024], olds[1100]; vka_pcache_name(old, sizeof old, dir, dt, "");   // the generation before: never read again
        snprintf(olds, sizeof olds, "%s.lib", old);
        if (!unlink(old)) VLOG("pipeline cache %s of the generation before " VKA_PCACHE_GEN " (compiled with the game's FPCR) removed", old);
        unlink(olds);
    }
    void *data = NULL; size_t n = 0;
    FILE *f = g_pc_path[0] ? fopen(g_pc_path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 32 && (data = malloc((size_t)sz)) && fread(data, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz;
        fclose(f);
        // The header names the device: a cache of another driver is dropped here (the driver would ignore it too)
        uint32_t hl, ver, vendor, devid; uint8_t uuid[VK_UUID_SIZE];
        if (n) { memcpy(&hl, data, 4); memcpy(&ver, (uint8_t *)data + 4, 4); memcpy(&vendor, (uint8_t *)data + 8, 4); memcpy(&devid, (uint8_t *)data + 12, 4); memcpy(uuid, (uint8_t *)data + 16, VK_UUID_SIZE);
                 if (hl < 32 || ver != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vendor != V.props.vendorID || devid != V.props.deviceID || memcmp(uuid, V.props.pipelineCacheUUID, VK_UUID_SIZE)) { VLOG("pipeline cache %s is another driver's: started empty", g_pc_path); n = 0; } }
    }
    g_pc_lib = g_pc_path[0] ? vka_shader_lib_id() : 0;
    memset(&g_pck, 0, sizeof g_pck);
    if (n && g_pc_lib) {   // the stamp: the library the file was saved with and the shaders it holds
        char sp[1100]; snprintf(sp, sizeof sp, "%s.lib", g_pc_path);
        char *sb = NULL; long sl = 0; FILE *sf = fopen(sp, "rb");
        if (sf) { fseek(sf, 0, SEEK_END); sl = ftell(sf); fseek(sf, 0, SEEK_SET); if (sl > 0 && sl < (64l << 20) && (sb = malloc((size_t)sl)) && fread(sb, 1, (size_t)sl, sf) != (size_t)sl) sl = 0; fclose(sf); }
        vka_pck_stamp old; vka_pck_parse(sb ? sb : "", sb ? (size_t)sl : 0, &old); free(sb);
        int keep = VKA_ENV("DM_VK_PCACHE_KEEP", 50); size_t live, changed; uint64_t dead; uint64_t t0 = now_ns();
        int d = vka_pck_decide(&old, g_pc_lib, keep, pck_hash_of, NULL, &g_pck, &live, &changed, &dead);
        double ms = (now_ns() - t0) / 1e6;
        switch (d) {
        case VKA_PCK_CARRIED:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: kept, %zu of the %zu shaders it lists are byte for byte the same in this one (%zu changed or dropped: "
                 "their pipelines are made again), %llu stale shaders in the file in all, at most %d%% of the kept ones allowed (DM_VK_PCACHE_KEEP; %.0f ms)", g_pc_path, n, live, old.n, changed,
                 (unsigned long long)dead, keep, ms); break;
        case VKA_PCK_UNVERIFIED:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: kept, its stamp (an older host's) names one library but lists no shaders; they are counted from this "
                 "run on", g_pc_path, n); break;
        case VKA_PCK_STALE:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: started empty, %zu of the %zu shaders it lists are the same in this one, %llu stale (changed or "
                 "dropped by this and earlier updates), over DM_VK_PCACHE_KEEP=%d%% of them; the next save replaces it (%.0f ms)", g_pc_path, n, live, old.n,
                 (unsigned long long)(old.dead + changed), keep, ms); n = 0; break;
        case VKA_PCK_OFF:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: started empty (DM_VK_PCACHE_KEEP=0; the next save replaces it)", g_pc_path, n); n = 0; break;
        case VKA_PCK_NO_STAMP:
            VLOG("pipeline cache %s (%zu bytes) is not stamped with its shader library: started empty (it may hold every library's shaders, which would all be loaded; the next "
                 "save replaces it)", g_pc_path, n); n = 0; break;
        default: break;
        }
        if (!vka_pck_reads(d)) vka_pck_free(&g_pck);
        vka_pck_free(&old);
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = n, .pInitialData = n ? data : NULL};
    VkResult r = vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc);
    if (r && n) { ci.initialDataSize = 0; ci.pInitialData = NULL; r = vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc); vka_pck_free(&g_pck); n = 0; }   // (started empty after all)
    if (r) { g_pc = VK_NULL_HANDLE; VRED("vkCreatePipelineCache %d: pipelines are built without a cache", r); }
    else VLOG("pipeline cache %s: %zu bytes read (its stamp lists %zu shaders, %llu stale)", g_pc_path[0] ? g_pc_path : e && !strcmp(e, "0") ? "(none: DM_VK_PCACHE=0)" : vka_fp_guest() ? "(none: DM_VK_GUEST_FPCR=1)" : "(not kept: no DM_TEMP or HOME)", n,
              g_pck.n, (unsigned long long)g_pck.dead);
    free(data);
    unf_load();
    pw_load();
    pthread_t th;
    if (!pthread_create(&th, NULL, saver_main, NULL)) { pthread_detach(th); g_sv_up = 1; }
    pw_start();
}
static void pc_open(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, pc_open_once); }   // (the prewarm threads it starts call it too: they wait for it)
void vka_pso_prepare(void) { vka_bindless_init(); pc_open(); }
// len bytes of data to path through tmp, whole or not at all: 1 when path holds them. The stream is closed exactly once (fclose ends it even when it fails, so a second
// fclose on the failure path was undefined), and tmp is removed on any failure; the file path held before stays
static int pc_write_atomic(const char *path, const char *tmp, const void *data, size_t len) {
    FILE *f = data ? fopen(tmp, "wb") : NULL;
    if (f) {
        int ok = fwrite(data, 1, len, f) == len;
        if (ok && fflush(f)) ok = 0;
        if (ok && fsync(fileno(f))) ok = 0;
        if (fclose(f)) ok = 0;
        if (ok && !rename(tmp, path)) return 1;
    }
    unlink(tmp);
    return 0;
}
static size_t svar_and_retype_save_locked(void) {
    int want_svar = atomic_exchange_explicit(&g_svar_dirty, 0, memory_order_acq_rel);
    int want_retype = atomic_exchange_explicit(&g_retype_dirty, 0, memory_order_acq_rel);
    if (!want_svar && !want_retype) return 0;

    vka_svar_rec *snap = NULL;
    uint32_t count = 0;
    pthread_mutex_lock(&g_svar_mu);
    count = g_svar.n;
    if (count > 0 && g_svar.recs) {
        snap = malloc((size_t)count * sizeof *snap);
        if (snap) memcpy(snap, g_svar.recs, (size_t)count * sizeof *snap);
    }
    pthread_mutex_unlock(&g_svar_mu);

    // Always flush the delta retype VkPipelineCache BEFORE committing the manifest so any
    // manifest entry on disk already has its compiled binary in vk_gpl_retype_cache_*.bin.
    size_t ret_bytes = 0;
    int retype_ok = 1;
    if (want_retype && g_pc_retype && g_retype_cache_path[0]) {
        retype_ok = 0;
        size_t n = 0; void *d = NULL;
        VkResult gr = VK_INCOMPLETE;
        for (int tr = 0; tr < 3 && gr == VK_INCOMPLETE; tr++) {
            size_t q = 0; if (vk.vkGetPipelineCacheData(V.dev, g_pc_retype, &q, NULL) || !q) { gr = VK_ERROR_UNKNOWN; break; }
            n = q + q / 4; free(d); d = malloc(n); if (!d) { gr = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
            gr = vk.vkGetPipelineCacheData(V.dev, g_pc_retype, &n, d);
        }
        if (gr == VK_SUCCESS && d && n) {
            vka_gpl_cache_hdr h = g_retype_hdr;
            h.total_entries = count;
            h.compiled_entries = count;
            if (vka_gpl_retype_cache_save_atomic(g_retype_cache_path, &h, d, (uint32_t)n)) {
                g_retype_hdr = h;
                atomic_store_explicit(&g_retype_cache_bytes, n, memory_order_relaxed);
                ret_bytes = n;
                retype_ok = 1;
            } else {
                atomic_store_explicit(&g_retype_dirty, 1, memory_order_relaxed);
            }
        } else if (gr == VK_INCOMPLETE || gr == VK_ERROR_OUT_OF_HOST_MEMORY) {
            atomic_store_explicit(&g_retype_dirty, 1, memory_order_relaxed);
        } else {
            retype_ok = 1;
        }
        free(d);
    }

    if (want_svar && retype_ok && g_svar_path[0] && g_gpl_hdr.dmsh_vk_lib_hash && (!count || snap)) {
        if (vka_svar_manifest_save_atomic(g_svar_path, g_gpl_hdr.dmsh_vk_lib_hash, snap, count)) {
            atomic_store_explicit(&g_svar_saved_n, count, memory_order_relaxed);
        } else {
            atomic_store_explicit(&g_svar_dirty, 1, memory_order_relaxed);
        }
    } else if (want_svar) {
        atomic_store_explicit(&g_svar_dirty, 1, memory_order_relaxed);
    }
    free(snap);

    return ret_bytes;
}
static size_t mono_cache_save_locked(void) {
    if (!g_gpl_cache_on || !g_pc_mono || !g_mono_cache_path[0]) return 0;
    if (!atomic_exchange_explicit(&g_mono_dirty, 0, memory_order_acq_rel)) return 0;
    size_t n = 0; void *d = NULL;
    VkResult gr = VK_INCOMPLETE;
    for (int tr = 0; tr < 3 && gr == VK_INCOMPLETE; tr++) {
        size_t q = 0; if (vk.vkGetPipelineCacheData(V.dev, g_pc_mono, &q, NULL) || !q) { gr = VK_ERROR_UNKNOWN; break; }
        n = q + q / 4; free(d); d = malloc(n); if (!d) { gr = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
        gr = vk.vkGetPipelineCacheData(V.dev, g_pc_mono, &n, d);
    }
    size_t saved = 0;
    if (gr == VK_SUCCESS && d && n) {
        vka_gpl_cache_hdr h = g_mono_hdr;
        uint32_t cnt = (uint32_t)atomic_load_explicit(&g_mono_made, memory_order_relaxed);
        uint32_t tot = g_mono_hdr.compiled_entries + cnt;
        h.total_entries = tot;
        h.compiled_entries = tot;
        if (vka_gpl_mono_cache_save_atomic(g_mono_cache_path, &h, d, (uint32_t)n)) {
            g_mono_hdr = h;
            atomic_store_explicit(&g_mono_cache_bytes, n, memory_order_relaxed);
            saved = n;
        } else {
            static _Atomic int k;
            if (atomic_fetch_add(&k, 1) < 4) VRED("monolithic cache not written to %s", g_mono_cache_path);
            atomic_store_explicit(&g_mono_dirty, 1, memory_order_relaxed);
        }
    } else if (gr == VK_INCOMPLETE) {
        atomic_store_explicit(&g_mono_dirty, 1, memory_order_relaxed);
    } else {
        static _Atomic int k;
        if (atomic_fetch_add(&k, 1) < 4) VRED("monolithic cache: vkGetPipelineCacheData failed (%d, n=%zu)", (int)gr, n);
    }
    free(d);
    return saved;
}
// The cache and the prewarm list to their files. vkGetPipelineCacheData runs without g_mu (the cache is internally synchronized): holding it there kept the replay thread's
// pipeline lookups waiting for a 17 MB copy once a minute
static void save_now(void) {
    uint64_t t = now_ns();
    uint64_t made = __atomic_load_n(&g_made, __ATOMIC_RELAXED);
    size_t n = 0; void *d = NULL;
    if (g_gpl_cache_on) {
        // Keep Boot #1 sharded vk_gpl_cache_*.bin strictly read-only in-game; persist only the lightweight
        // variant manifest + delta retype VkPipelineCache + monolithic optimization cache independently.
        size_t ret_n = svar_and_retype_save_locked();
        size_t mono_n = mono_cache_save_locked();
        int keys = pw_save();
        uint64_t dt = now_ns() - t; atomic_fetch_add(&g_save_ns_total, dt);
        if (ret_n) VLOG("delta retype cache: %zu bytes written to %s (%u variants), %.0f ms (on the saver thread)",
                        ret_n, g_retype_cache_path, (unsigned)atomic_load_explicit(&g_svar_saved_n, memory_order_relaxed), dt / 1e6);
        if (mono_n || keys > 0) VLOG("monolithic cache: %zu bytes written to %s (%llu pipelines made this run), prewarm list %d keys, %.0f ms (on the saver thread)",
                                     mono_n, g_mono_cache_path[0] ? g_mono_cache_path : "(none)",
                                     (unsigned long long)atomic_load_explicit(&g_mono_made, memory_order_relaxed), keys, dt / 1e6);
        return;
    } else if (g_pc && g_pc_path[0] && made != g_saved_at) {
        // The cache grows between the size query and the copy while prewarm and optimizer threads add pipelines: the copy then answers VK_INCOMPLETE with a partial
        // blob. Tried up to 3 times with a quarter more room; only a complete copy (VK_SUCCESS) is written
        VkResult gr = VK_INCOMPLETE;
        for (int tr = 0; tr < 3 && gr == VK_INCOMPLETE; tr++) {
            size_t q = 0; if (vk.vkGetPipelineCacheData(V.dev, g_pc, &q, NULL) || !q) { gr = VK_ERROR_UNKNOWN; break; }
            n = q + q / 4; free(d); d = malloc(n); if (!d) { gr = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
            gr = vk.vkGetPipelineCacheData(V.dev, g_pc, &n, d);
        }
        if (gr != VK_SUCCESS) { free(d); d = NULL; n = 0; static _Atomic int k; if (atomic_fetch_add(&k, 1) < 4) VRED("pipeline cache not saved: vkGetPipelineCacheData %d after 3 tries (pipelines being added meanwhile); the next save tries again", (int)gr); }
        if (d) {
            char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pc_path);
            if (pc_write_atomic(g_pc_path, tmp, d, n)) {
                g_saved_at = made;
                // Its stamp after it: the shaders it was read with that this library keeps, and those this run made. Written whole or not at all (a stamp that
                // cannot be written is removed, and the next run starts empty)
                char sp[1100], st[1200]; snprintf(sp, sizeof sp, "%s.lib", g_pc_path); snprintf(st, sizeof st, "%s.lib.tmp", g_pc_path);
                size_t nr = 0, tl = 0; vka_pck_ent *run = g_pc_lib ? vka_shader_made(&nr) : NULL;
                char *txt = g_pc_lib ? vka_pck_format(g_pc_lib, g_pck.dead, g_pck.ent, g_pck.n, run, nr, &tl) : NULL; free(run);
                if (!pc_write_atomic(sp, st, txt, tl)) unlink(sp);
                free(txt);
            }
            else { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VRED("pipeline cache not written to %s", g_pc_path); free(d); d = NULL; n = 0; }   // (the copy is freed: it was dropped unfreed)
        }
    }
    int keys = pw_save();
    uint64_t dt = now_ns() - t; atomic_fetch_add(&g_save_ns_total, dt);
    if (d || keys > 0 || (g_gpl_cache_on && n)) VLOG("pipeline cache: %zu bytes written to %s (%llu pipelines made this run), prewarm list %d keys, %.0f ms (on the saver thread)", n,
                                                    g_gpl_cache_on ? (g_mono_cache_path[0] ? g_mono_cache_path : g_retype_cache_path) : g_pc_path[0] ? g_pc_path : "(none)",
                                                    (unsigned long long)made, keys, dt / 1e6);
    free(d);
}
static void svar_record(const vka_svar_rec *rec) {
    if (!rec || !rec->shader_hash) return;
    pthread_mutex_lock(&g_svar_mu);
    int added = vka_svar_set_add(&g_svar, rec);
    pthread_mutex_unlock(&g_svar_mu);
    if (added) {
        atomic_store_explicit(&g_svar_dirty, 1, memory_order_relaxed);
        atomic_store_explicit(&g_retype_dirty, 1, memory_order_relaxed);
    }
}
static void *saver_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-pcache"); vka_fp_host("vk-pcache");
    for (;;) {
        pthread_mutex_lock(&g_sv_mu); while (!g_sv_want) pthread_cond_wait(&g_sv_cv, &g_sv_mu); g_sv_want = 0; pthread_mutex_unlock(&g_sv_mu);
        pthread_mutex_lock(&g_save_mu); save_now(); pthread_mutex_unlock(&g_save_mu);
    }
    return NULL;
}
// From the replay thread at each submission, and after a pipeline that took over 1 s on the frame path (a cold first 3D frame is one submission of a minute or more of
// pipelines, and a run stopped in it saved none of them, so every run started cold again: Ocarina of Time, 2026-11-08): a save is due DM_VK_PCACHE_SAVE_S seconds (default
// 20) after the last one when pipelines were made since; it runs on the saver thread. force 1 (the end of the run, the shader library check): now, on the caller's thread.
// force 2 (a pipeline that took over 1 s): due DM_VK_PCACHE_SAVE_SLOW_S (default 3) after the last save, on the saver thread. With the 20 s rule a slow one made in the last
// 20 s of a run was lost when the process was stopped or killed, and the next run made it again on the frame path: Ocarina of Time's compute pipeline 3d0d6d61bb9076e7,
// 3.1 s in run p-sys1 five seconds after its last save, 2.3 s again in the warm run after it (2026-11-08)
void vka_pso_save(int force) {
    if (!g_pc && !g_gpl_cache_on) return;
    if (atomic_load_explicit(&g_pre_active, memory_order_relaxed)) {
        if (force == 1) atomic_store_explicit(&g_pre_checkpoint_want, 1, memory_order_release);
        return;
    }
    uint64_t t = now_ns();
    if (g_gpl_cache_on) {
        int dirty = atomic_load_explicit(&g_svar_dirty, memory_order_relaxed) ||
                    atomic_load_explicit(&g_retype_dirty, memory_order_relaxed) ||
                    atomic_load_explicit(&g_mono_dirty, memory_order_relaxed) ||
                    pw_dirty();
        if (!dirty && force != 1) return;
        if (force != 1) {
            int s = force == 2 ? VKA_ENV("DM_VK_PCACHE_SAVE_SLOW_S", 3) : VKA_ENV("DM_VK_PCACHE_SAVE_S", 20);
            if (s < 1) s = 1;
            if (t - g_save_ns < (uint64_t)s * 1000000000ull) return;
            g_save_ns = t;
            if (g_sv_up) { pthread_mutex_lock(&g_sv_mu); g_sv_want = 1; pthread_cond_signal(&g_sv_cv); pthread_mutex_unlock(&g_sv_mu); return; }
        }
    } else if (force != 1) {
        uint64_t made = __atomic_load_n(&g_made, __ATOMIC_RELAXED);
        if (made == g_saved_at) return;
        int s = force == 2 ? VKA_ENV("DM_VK_PCACHE_SAVE_SLOW_S", 3) : VKA_ENV("DM_VK_PCACHE_SAVE_S", 20); if (s < 1) s = 1;
        if (t - g_save_ns < (uint64_t)s * 1000000000ull) return;
        g_save_ns = t;
        if (g_sv_up) { pthread_mutex_lock(&g_sv_mu); g_sv_want = 1; pthread_cond_signal(&g_sv_cv); pthread_mutex_unlock(&g_sv_mu); return; }
    }
    g_save_ns = t;
    pthread_mutex_lock(&g_save_mu); save_now(); pthread_mutex_unlock(&g_save_mu);
}
void vka_gpl_line(char *out, size_t n);
void vka_pso_async_line(char *out, size_t n);
void mtl_pso_line(char *out, size_t n) {
    if (!n) return;
    int k = snprintf(out, n, "%llu graphics + %llu compute pipelines (%llu failed, %llu compute (program, layout) pairs not made for shared memory, %llu on the frame path), %llu set layouts, %.0f ms creating", (unsigned long long)g_ngfx, (unsigned long long)g_ncs,
                     (unsigned long long)g_nfail, (unsigned long long)g_ncs_over, (unsigned long long)g_frame_made, (unsigned long long)g_nlayouts, g_create_ms);
    if (V.gpl && k > 0 && (size_t)k + 3 < n) { memcpy(out + k, "; ", 3); vka_gpl_line(out + k + 2, n - (size_t)k - 2); }
    size_t o = strlen(out); char a[200]; vka_pso_async_line(a, sizeof a);
    if (a[0] && o + 2 < n) snprintf(out + o, n - o, "; %s", a);
}

// The long-frame log's counters (hle_nvn.c): pipelines made on the frame path and the time it took, cumulative (it takes each frame's difference). Background and prewarm
// pipelines are not the frame's. Shader modules are made with the shader (vka_shader_make), not counted here
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { pthread_mutex_lock(&g_mu); if (lib) *lib = 0; if (pso) *pso = g_frame_made; pthread_mutex_unlock(&g_mu); }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { pthread_mutex_lock(&g_mu); if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = g_create_ns; pthread_mutex_unlock(&g_mu); }
// "draws with a hybrid pipeline" of the [longframe] line: on Android, draws recorded with a fast-linked GPL pipeline (before its optimized one took its place); made = pipelines
// linked. (The Mac's hybrid pipelines are its pulling stand-ins; this stub returned 0 on Android until 2026-11-03, whatever GPL did)
static uint64_t g_nlinked_pub;
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { if (draws) *draws = atomic_load(&g_hy_draws); if (made) *made = __atomic_load_n(&g_nlinked_pub, __ATOMIC_RELAXED); if (none) *none = 0; }
// Pipeline-side attribution for the [longframe] line (vka_extra.c): the saver's time, the frame path's waits for a prewarm thread making the same pipeline
void vka_pso_waits(uint64_t *save_ns, uint64_t *pw_wait_ns) { if (save_ns) *save_ns = atomic_load(&g_save_ns_total); if (pw_wait_ns) *pw_wait_ns = atomic_load(&g_pw_wait_ns); }

// ---------- Graphics pipelines ----------
// The map's value: the monolithic pipeline and/or the fast-linked hybrid stand-in, and whether a thread is still making it (busy: the frame path
// waits for that thread instead of making it a second time). p_mono is published atomically once the background optimizer builds the 1-set monolithic twin
typedef struct gval { VkPipeline p_mono, p_hybrid; uint8_t busy, pw; pid_t tid; } gval;   // pw, tid: a prewarm thread is making it (busy), that thread (busy_wait)
typedef struct { vka_shader *sh[VKA_NSTAGE]; VkPipelineLayout layout; vka_gkey k; } gkey_full;   // (sh[5], compute, stays NULL)
static kmap g_gfx = {.klen = sizeof(gkey_full)};
static pthread_cond_t g_busy_cv = PTHREAD_COND_INITIALIZER;
// The create info of a graphics pipeline, built in place (its members point at one another): the whole pipeline and each stage library take their parts from it
typedef struct {
    VkPipelineShaderStageCreateInfo ss[5]; uint32_t ns; VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg[5];
    VkBool32 gl_true; uint32_t afunc; VkSpecializationMapEntry me, ae; VkSpecializationInfo spec, aspec;
    VkVertexInputBindingDescription vb[33]; VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDivisorDescription vd[33];
    VkPipelineVertexInputDivisorStateCreateInfo vds; VkPipelineVertexInputStateCreateInfo vi; VkPipelineInputAssemblyStateCreateInfo ia; VkPipelineTessellationStateCreateInfo ts;
    VkPipelineViewportStateCreateInfo vp; VkPipelineRasterizationStateCreateInfo rs; VkPipelineMultisampleStateCreateInfo ms; VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cb[8]; VkPipelineColorBlendStateCreateInfo bs; VkDynamicState dyn[40]; VkPipelineDynamicStateCreateInfo dy;
    VkFormat cf[8]; VkPipelineRenderingCreateInfo ri; VkGraphicsPipelineCreateInfo ci; int tess, mesh;   // mesh: a task / mesh pipeline (pstate_fill)
    uint32_t fu, fsm; uint8_t pfb_late;
    VkDescriptorSetLayout set0;   // the layout's set 0 (the library probe's independent-sets layout)
    VkPipelineLayout pre_layout, fs_layout; int indep;
    vka_shader *sh[VKA_NSTAGE];
} pstate;

// A VkFormat's class for colour outputs: 1 UINT, 2 SINT, 0 the rest (float, normalized, scaled)
static int vkf_int_class(uint32_t f) {
    static const uint16_t u[] = {13, 20, 27, 34, 41, 48, 55, 62, 68, 74, 81, 88, 95, 98, 101, 104, 107, 110, 113, 116, 119};
    for (size_t i = 0; i < sizeof u / sizeof *u; i++) { if (f == u[i]) return 1; if (f == u[i] + 1u) return 2; }
    return 0;
}
// VKA_FS_RETYPE: the pixel shader's float outputs (fout_types) that it writes (fout_mask) into UINT / SINT attachments of the key: retyped in its module (vka_shader_fout_module)
static void fout_masks(const vka_shader *fs, const vka_gkey *k, uint32_t *um, uint32_t *sm) {
    *um = *sm = 0;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        int c = vkf_int_class(k->cfmt[i]);
        if (c && (fs->e->fout_mask >> i & 1) && (fs->e->fout_types >> (2 * i) & 3) == DMSH_VK_TYPE_FLOAT) { if (c == 1) *um |= 1u << i; else *sm |= 1u << i; }
    }
}
// Optional EDS2/EDS3 dynamic states active for a pipeline (classic render passes bake sample count)
static uint32_t active_eds_mask(int mesh) {
    uint32_t m = V.eds_mask;
    if (vka_classic_rp()) m &= ~(uint32_t)(VKA_EDS_RAST_SAMPLES | VKA_EDS_SAMPLE_MASK);
    if (mesh) m &= ~(uint32_t)VKA_EDS_PATCH_CP;
    return m;
}
static void runtime_gkey(vka_shader *const sh[VKA_NSTAGE], const vka_gkey *k_canon, vka_gkey *k_rt) {
    int keep_a2c = sh[1] && vka_tex_pfb_flags() && vka_shader_pfb_has_twin(sh[1]->mods[DMSH_VK_MOD_BASE]);
    vka_eds_runtime_key(k_canon, active_eds_mask(sh[7] != NULL), keep_a2c, k_rt);
}
// 0 = a stage's module is missing (logged by vka_shader_module)
static int pstate_fill_ex(pstate *P, vka_shader *const sh_in[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int mono) {
    memset(P, 0, sizeof *P);
    vka_shader *sh[VKA_NSTAGE];
    if (mono && V.gpl_indep) {
        for (int i = 0; i < VKA_NSTAGE; i++) sh[i] = (i != 5 && sh_in[i]) ? vka_shader_mono_peer(sh_in[i]) : sh_in[i];
        vka_shader_mono_scope(1);
    } else {
        memcpy(sh, sh_in, sizeof sh);
    }
    memcpy(P->sh, sh, sizeof P->sh);
    // Stages: vertex, tess control, tess evaluation, geometry, fragment; the GL depth constant on the last of the first four. A mesh pipeline (NVN2 task and mesh programs,
    // nvnCommandBufferDrawMeshTasks): task (optional), mesh, fragment, the GL depth constant on the mesh stage; it has no vertex input or input assembly state
    static const int ovtx[5] = {0, 3, 4, 2, 1}, omesh[5] = {6, 7, 1, -1, -1};
    const int mesh = sh[7] != NULL, *order = mesh ? omesh : ovtx, nord = mesh ? 3 : 5;
    int last_pre = -1;
    for (int i = 0; i < nord - 1; i++) if (sh[order[i]]) last_pre = order[i];
    P->gl_true = VK_TRUE; P->me = (VkSpecializationMapEntry){DMSH_VK_SPEC_GL_DEPTH, 0, sizeof(VkBool32)}; P->spec = (VkSpecializationInfo){1, &P->me, sizeof P->gl_true, &P->gl_true};
    P->afunc = k->alpha_func; P->ae = (VkSpecializationMapEntry){DMSH_VK_SPEC_ALPHA_FUNC, 0, sizeof(uint32_t)};   // the alpha test's function on an F_ALPHA_TEST pixel shader
    P->aspec = (VkSpecializationInfo){1, &P->ae, sizeof P->afunc, &P->afunc};
    for (int i = 0; i < nord; i++) {
        vka_shader *s = sh[order[i]]; if (!s) continue;
        int fs = order[i] == 1, gl = k->gl_depth && order[i] == last_pre && (s->e->flags & DMSH_VK_F_GL_DEPTH);
        uint32_t fu = 0, fsm = 0; if (fs && k->fs_variant == VKA_FS_RETYPE) fout_masks(s, k, &fu, &fsm);
        VkShaderModule mod = order[i] == 0 && k->gs_fx == 2 && s->tf_fx ? vka_shader_module(s, DMSH_VK_MOD_TF)   // transform feedback capture (vka_draw.c g_tf_cap)
                           : order[i] == 0 && (k->vin_u | k->vin_s) ? vka_shader_vin_module(s, k->vin_u, k->vin_s) : order[i] == 2 && k->gs_fx == 1 ? vka_shader_gs_module(s)
                           : fs && k->fs_variant == VKA_FS_RETYPE ? (fu | fsm ? vka_shader_fout_module(s, fu, fsm) : vka_shader_module(s, DMSH_VK_MOD_BASE))
                           : vka_shader_module(s, fs ? k->fs_variant : DMSH_VK_MOD_BASE);
        if (!mod && (order[i] == 2 || order[i] == 0) && k->gs_fx) mod = vka_shader_module(s, DMSH_VK_MOD_BASE);   // no module with the claim (logged): the shader as it is
        if (mod && order[i] == 2 && sh[1]) {   // the pixel shader's inputs the geometry shader does not write, added (vka_shader_gs_link_module); none: the module above
            VkShaderModule lm = vka_shader_gs_link_module(s, sh[0], sh[1], k->gs_fx == 1 && mod == vka_shader_gs_module(s));
            if (lm) mod = lm;
        }
        if (!mod) { if (mono && V.gpl_indep) vka_shader_mono_scope(0); return 0; }
        // A task or mesh module that reads its subgroup lanes, with its specialization constants folded in instead of VkSpecializationInfo (vka_shader_spec_module)
        VkShaderModule frz = order[i] >= 6 ? vka_shader_spec_module(s, gl) : VK_NULL_HANDLE;
        if (frz) mod = frz;
        // Float textures and images bound to integer texture views (vka_draw.c tex_masks): the module above with them retyped (vka_shader_tex_module); not with the geometry claim
        int rg = vka_shader_tex_range(s);
        if (rg >= 0 && rg < 5 && (k->tex_u[rg] | k->tex_s[rg])) {
            vka_texmod_key tk = {fs ? (k->fs_variant == VKA_FS_RETYPE ? DMSH_VK_MOD_BASE : k->fs_variant) : DMSH_VK_MOD_BASE, order[i] == 0 ? k->vin_u : 0, order[i] == 0 ? k->vin_s : 0,
                                 fu, fsm, frz ? gl : -1, k->tex_u[rg], k->tex_s[rg]};
            VkShaderModule t = order[i] == 2 && k->gs_fx ? VK_NULL_HANDLE : vka_shader_tex_module(s, &tk);
            if (t) mod = t;
            else VRED_ONCE("%016llx: a stage module with its textures retyped was not made: its integer texture views are read as floats", (unsigned long long)s->key);
        }
        // A pixel module that reports its pool reads was given early depth tests where they give the late ones' result; alpha to coverage, or its alpha test on, takes the
        // twin without them (vka_shader.c "Texture pool reads")
        if (fs) {
            P->fu = fu; P->fsm = fsm;
            VkShaderModule bm = mod;
            mod = vka_shader_pfb_pick(mod, k->a2c, k->alpha_func);
            P->pfb_late = (uint8_t)(mod != bm);
        }
        uint32_t n = P->ns++;
        P->ss[n] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = (VkShaderStageFlagBits)stage_bit(s->e->stage), .module = mod, .pName = "main",
                                                    .pSpecializationInfo = frz ? NULL : gl ? &P->spec : fs && P->afunc && (s->e->flags & DMSH_VK_F_ALPHA_TEST) ? &P->aspec : NULL};
        uint32_t pin = (s->e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(s->e->stage) : 0;   // subgroup shaders of a stage whose size the device varies: pinned to the library's (vka_shader_sg_pin)
        if (pin) { P->rsg[n] = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
                   P->ss[n].pNext = &P->rsg[n]; }
    }
    uint32_t nvb = 0, nva = 0, nvd = 0;
    if (k->vin_static) {
        for (uint32_t i = 0; i < 32; i++) if (k->va[i].used) P->va[nva++] = (VkVertexInputAttributeDescription){i, k->va[i].binding, (VkFormat)k->va[i].fmt, k->va[i].offset};
        for (uint32_t i = 0; i < 33; i++) if (k->vb[i].used) {
            P->vb[nvb++] = (VkVertexInputBindingDescription){i, k->vb[i].stride, k->vb[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
            if (k->vb[i].divisor > 1) P->vd[nvd++] = (VkVertexInputBindingDivisorDescription){i, k->vb[i].divisor};
        }
    }
    P->vds = (VkPipelineVertexInputDivisorStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO, .vertexBindingDivisorCount = nvd, .pVertexBindingDivisors = P->vd};
    P->vi = (VkPipelineVertexInputStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nvd ? &P->vds : NULL, .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = P->vb,
                                                   .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = P->va};
    P->ia = (VkPipelineInputAssemblyStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = (VkPrimitiveTopology)k->topo};
    P->ts = (VkPipelineTessellationStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, .patchControlPoints = 3};
    P->vp = (VkPipelineViewportStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};   // one viewport, as nvn_mtl.m draws
    P->rs = (VkPipelineRasterizationStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = k->depth_clamp && V.depth_clamp,
        .polygonMode = (VkPolygonMode)k->poly_mode, .lineWidth = 1.0f};
    P->ms = (VkPipelineMultisampleStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1), .alphaToCoverageEnable = k->a2c};
    P->ds = (VkPipelineDepthStencilStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .maxDepthBounds = 1.0f};
    for (int i = 0; i < k->ncolor; i++)
        P->cb[i] = (VkPipelineColorBlendAttachmentState){k->att[i].en, (VkBlendFactor)k->att[i].src, (VkBlendFactor)k->att[i].dst, (VkBlendOp)k->att[i].op,
                                                          (VkBlendFactor)k->att[i].srca, (VkBlendFactor)k->att[i].dsta, (VkBlendOp)k->att[i].opa, k->att[i].mask};
    P->bs = (VkPipelineColorBlendStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .logicOpEnable = k->logic_en, .logicOp = (VkLogicOp)k->logic_op, .attachmentCount = k->ncolor, .pAttachments = P->cb};
    static const VkDynamicState base[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_LINE_WIDTH};
    uint32_t nd = 0;
    for (size_t i = 0; i < sizeof base / sizeof *base; i++)   // (a mesh pipeline may not name the primitive topology or restart: it has no input assembly)
        if (!mesh || (base[i] != VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY && base[i] != VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)) P->dyn[nd++] = base[i];
    if (!k->vin_static && !mesh) P->dyn[nd++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    if (V.fb_loop) P->dyn[nd++] = VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT;
    uint32_t eds = (mono && V.gpl_indep) ? 0u : active_eds_mask(mesh);
    if (eds & VKA_EDS_DEPTH_CLAMP) P->dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    if (eds & VKA_EDS_POLYGON_MODE) P->dyn[nd++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    if (!mesh && (sh[3] || sh[4]) && (eds & VKA_EDS_PATCH_CP)) P->dyn[nd++] = VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT;
    if (eds & VKA_EDS_RAST_SAMPLES) P->dyn[nd++] = VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT;
    if (eds & VKA_EDS_SAMPLE_MASK) P->dyn[nd++] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
    if (eds & VKA_EDS_ALPHA_TO_COV) P->dyn[nd++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
    if (eds & VKA_EDS_LOGIC_OP_ENABLE) P->dyn[nd++] = VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT;
    if (eds & VKA_EDS_LOGIC_OP) P->dyn[nd++] = VK_DYNAMIC_STATE_LOGIC_OP_EXT;
    if (eds & VKA_EDS_COLOR_BLEND_ENABLE) P->dyn[nd++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
    if (eds & VKA_EDS_COLOR_BLEND_EQ) P->dyn[nd++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
    if (eds & VKA_EDS_COLOR_WRITE_MASK) P->dyn[nd++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
    P->dy = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = nd, .pDynamicStates = P->dyn};
    for (int i = 0; i < k->ncolor; i++) P->cf[i] = (VkFormat)k->cfmt[i];
    P->ri = (VkPipelineRenderingCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = P->cf,
                                            .depthAttachmentFormat = (VkFormat)k->dfmt, .stencilAttachmentFormat = (VkFormat)k->sfmt};
    P->tess = !mesh && (sh[3] || sh[4]); P->mesh = mesh;
    P->ia.topology = P->tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : (k->topo ? (VkPrimitiveTopology)k->topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    P->ci = (VkGraphicsPipelineCreateInfo){VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = vka_classic_rp() ? P->ri.pNext : &P->ri, .stageCount = P->ns, .pStages = P->ss, .pVertexInputState = mesh ? NULL : &P->vi,
        .pInputAssemblyState = mesh ? NULL : &P->ia,
        .pTessellationState = P->tess ? &P->ts : NULL, .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pMultisampleState = &P->ms, .pDepthStencilState = &P->ds, .pColorBlendState = &P->bs,
        .pDynamicState = &P->dy, .layout = lay->layout};
    if (vka_classic_rp()) { P->ci.renderPass = vka_rp_compat(P->cf, k->ncolor, (VkFormat)k->dfmt, (VkFormat)k->sfmt, k->samples); P->ci.subpass = 0; }   // DM_VK_CLASSIC_RP
    P->set0 = lay->set0; P->pre_layout = lay->pre_layout; P->fs_layout = lay->fs_layout; P->indep = lay->indep;
    if (mono && V.gpl_indep) vka_shader_mono_scope(0);
    return 1;
}
static int pstate_fill(pstate *P, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    return pstate_fill_ex(P, sh, lay, k, !lay->indep);
}
static const char *stage_name(VkShaderStageFlagBits b) { return b == VK_SHADER_STAGE_VERTEX_BIT ? "vertex" : b == VK_SHADER_STAGE_FRAGMENT_BIT ? "fragment" : b == VK_SHADER_STAGE_GEOMETRY_BIT ? "geometry"
                                                              : b == VK_SHADER_STAGE_TASK_BIT_EXT ? "task" : b == VK_SHADER_STAGE_MESH_BIT_EXT ? "mesh" : "tessellation"; }
// Pipelines being made now (whole pipelines, any thread): the watchdog's dump names them (vka_pso_wait_dump), so a stop inside the driver's compiler says which shaders
// (a frame-pointer backtrace ends in the stripped driver). Slots are claimed with a compare-and-swap; a build that finds none full is simply not listed
enum { NBUILD = 16 };
static struct { _Atomic pid_t tid; _Atomic uint64_t t0, vs, fs; const char *_Atomic what; } g_build[NBUILD];
static void vka_pso_wait_dump(void) {
    uint64_t t = now_ns(); int n = 0;
    for (int i = 0; i < NBUILD; i++) { pid_t id = atomic_load(&g_build[i].tid); if (!id) continue; n++;
        fprintf(stderr, "[dm] [vk] %s being made by thread %d for %.1f s: shaders %016llx %016llx\n", atomic_load(&g_build[i].what), (int)id, (t - atomic_load(&g_build[i].t0)) / 1e9,
                (unsigned long long)atomic_load(&g_build[i].vs), (unsigned long long)atomic_load(&g_build[i].fs)); }
    if (!n) fprintf(stderr, "[dm] [vk] no pipeline being made\n");
}
void crash_set_wait_dump(void (*fn)(void));
static void build_reg(void) { crash_set_wait_dump(vka_pso_wait_dump); }
static int build_begin(const char *what, uint64_t vs, uint64_t fs) {
    static pthread_once_t o = PTHREAD_ONCE_INIT; pthread_once(&o, build_reg);
    for (int i = 0; i < NBUILD; i++) { pid_t z = 0; if (atomic_compare_exchange_strong(&g_build[i].tid, &z, -1)) {
        atomic_store(&g_build[i].what, what); atomic_store(&g_build[i].vs, vs); atomic_store(&g_build[i].fs, fs); atomic_store(&g_build[i].t0, now_ns()); atomic_store(&g_build[i].tid, gettid()); return i; } }
    return -1;
}
// Shaders (pre-rasterization or compute keys) whose build took DM_VK_PREWARM_HEAVY_MS or more on this driver, kept beside its pipeline cache (<cache file>.heavy, a hex key
// a line, appended as they are found). Turnip compiles some of them again every run, cache hit or not (Ocarina of Time: vertex b1f06157308e3b22 8.6-18 s in each of four
// runs), and prewarm threads that start them in the same second all pass the age check of pw_heavy_hold: three at once took 58,619 maps of 65,530. A known one is held
// from its start
enum { NHEAVY = 1024 };
static uint64_t g_heavy[NHEAVY]; static _Atomic int g_heavy_n; static char g_heavy_path[1100]; static pthread_mutex_t g_heavy_mu = PTHREAD_MUTEX_INITIALIZER;
static int heavy_has(uint64_t k) { int n = atomic_load(&g_heavy_n); for (int i = 0; i < n; i++) if (g_heavy[i] == k) return 1; return 0; }
static void heavy_add(uint64_t k, int save) {
    pthread_mutex_lock(&g_heavy_mu);
    int n = atomic_load(&g_heavy_n);
    if (k && n < NHEAVY && !heavy_has(k)) {
        g_heavy[n] = k; atomic_store(&g_heavy_n, n + 1);
        const char *base = g_pc_path[0] ? g_pc_path : g_mono_cache_path[0] ? g_mono_cache_path : g_retype_cache_path;
        if (!g_heavy_path[0] && base[0]) snprintf(g_heavy_path, sizeof g_heavy_path, "%s.heavy", base);
        FILE *f = save && g_heavy_path[0] ? fopen(g_heavy_path, "a") : NULL;
        if (f) { fprintf(f, "%016llx\n", (unsigned long long)k); fclose(f); }
    }
    pthread_mutex_unlock(&g_heavy_mu);
}
static void heavy_load(void) {
    static int done;
    if (done) return;
    const char *base = g_pc_path[0] ? g_pc_path : g_mono_cache_path[0] ? g_mono_cache_path : g_retype_cache_path;
    if (!base[0]) return;
    done = 1;
    pthread_mutex_lock(&g_heavy_mu); snprintf(g_heavy_path, sizeof g_heavy_path, "%s.heavy", base); pthread_mutex_unlock(&g_heavy_mu);
    FILE *f = fopen(g_heavy_path, "r"); if (!f) return;
    char ln[64]; while (fgets(ln, sizeof ln, f)) { uint64_t k = strtoull(ln, NULL, 16); if (k) heavy_add(k, 0); }
    fclose(f);
    VLOG("long compiles: %d shaders of earlier runs on this driver (%s): prewarm makes them one at a time (DM_VK_PREWARM_HEAVY_MS)", atomic_load(&g_heavy_n), g_heavy_path);
}
// Speculative compiles that did not finish (vka_pwbusy.h): while a prewarm key or an optimizer's twin builds, its shader key is in <cache file>.busy; a key still there at
// the next start was building when the process ended, and joins <cache file>.unfinished, which prewarm and the optimizer leave out on this driver (a workaround for a
// driver compiler that runs away on one shader: Pokopia's af8f64d9f5542ef2, 60-77 s and the process's memory maps on Qualcomm's). A build of it that finishes on any path
// takes it off the list (build_end)
static vkpb_set g_busy, g_unf; static char g_busy_path[1100], g_unf_path[1100]; static pthread_mutex_t g_unf_mu = PTHREAD_MUTEX_INITIALIZER; static _Atomic int g_unf_n;
static _Atomic uint32_t g_pw_unf;   // prewarm keys left out for it this run
static void unf_write(const char *path, const vkpb_set *s) {   // caller holds g_unf_mu; a temporary file renamed over it, so a crash mid-write leaves the old one
    if (!path[0]) return;
    char b[VKPB_MAX * 17 + 1], tmp[1200]; size_t n = vkpb_format(s, b, sizeof b);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    if (fwrite(b, 1, n, f) != n) { fclose(f); unlink(tmp); return; }
    if (fclose(f) || rename(tmp, path)) unlink(tmp);
}
static int unf_read(const char *path, vkpb_set *s) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char b[VKPB_MAX * 17 + 64]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
    return vkpb_parse(s, b, n);
}
static int unf_has(uint64_t k) { if (!k || !atomic_load(&g_unf_n)) return 0; pthread_mutex_lock(&g_unf_mu); int r = vkpb_has(&g_unf, k); pthread_mutex_unlock(&g_unf_mu); return r; }
static void unf_load(void) {   // once, at the pipeline cache's opening (pc_open_once), before the prewarm threads start
    static int done;
    const char *base = g_pc_path[0] ? g_pc_path : g_mono_cache_path[0] ? g_mono_cache_path : NULL;
    if (done || !base || !*base) return;
    done = 1;
    pthread_mutex_lock(&g_unf_mu);
    snprintf(g_busy_path, sizeof g_busy_path, "%s.busy", base); snprintf(g_unf_path, sizeof g_unf_path, "%s.unfinished", base);
    unf_read(g_unf_path, &g_unf);
    vkpb_set busy; memset(&busy, 0, sizeof busy); unf_read(g_busy_path, &busy);
    int added = vkpb_carry(&g_unf, &busy);
    for (int i = 0; i < busy.n; i++)
        VRED("pipeline compile of shader %016llx was still running when the process ended (an earlier run on this driver): prewarm and the optimizer leave it out from now "
             "on (%s); a draw that needs it still has it made", (unsigned long long)busy.k[i], g_unf_path);
    if (added) unf_write(g_unf_path, &g_unf);
    if (busy.n) unlink(g_busy_path);
    atomic_store(&g_unf_n, g_unf.n);
    if (g_unf.n && !added) VLOG("speculative compiles left out: %d shaders whose compile did not finish in an earlier run on this driver (%s)", g_unf.n, g_unf_path);
    pthread_mutex_unlock(&g_unf_mu);
}
static void spec_begin(uint64_t k) {   // a speculative build of shader k starts
    if (!k || !g_busy_path[0]) return;
    pthread_mutex_lock(&g_unf_mu); if (vkpb_add(&g_busy, k)) unf_write(g_busy_path, &g_busy); pthread_mutex_unlock(&g_unf_mu);
}
static void spec_end(uint64_t k) {
    if (!k || !g_busy_path[0]) return;
    pthread_mutex_lock(&g_unf_mu);
    if (vkpb_del(&g_busy, k)) { if (g_busy.n) unf_write(g_busy_path, &g_busy); else unlink(g_busy_path); }
    pthread_mutex_unlock(&g_unf_mu);
}
static void unf_done(uint64_t k) {   // a build of shader k returned (build_end): off the unfinished list
    if (!k || !atomic_load(&g_unf_n)) return;
    pthread_mutex_lock(&g_unf_mu);
    if (vkpb_del(&g_unf, k)) {
        atomic_store(&g_unf_n, g_unf.n); unf_write(g_unf_path, &g_unf);
        VLOG("pipeline compile of shader %016llx finished this time: no longer left out of prewarm (%s)", (unsigned long long)k, g_unf_path);
    }
    pthread_mutex_unlock(&g_unf_mu);
}
static void build_end(int i) {
    if (i < 0) return;
    uint64_t vs = atomic_load(&g_build[i].vs), fs = atomic_load(&g_build[i].fs), age = now_ns() - atomic_load(&g_build[i].t0);
    const char *what = atomic_load(&g_build[i].what);
    atomic_store(&g_build[i].tid, 0);
    int ms = VKA_ENV("DM_VK_PREWARM_HEAVY_MS", 1000);
    uint64_t k = vs ? vs : fs;
    unf_done(k);
    if (k && ms > 0 && age >= (uint64_t)ms * 1000000ull && !heavy_has(k)) {
        heavy_add(k, 1);
        VLOG("long compile: %s of shader %016llx took %.1f s: remembered (%s), compiles hold for such shaders", what, (unsigned long long)k, age / 1e9, g_heavy_path[0] ? g_heavy_path : "this run only");
    }
}
// The same list in the log, with each thread's name (the memory maps thread's histogram, vka_mem.c maps_hist_log, DM_VK_MAPS_HIST): which compiles run while the maps
// outside the guest block climb
void vka_pso_builds_log(void) {
    uint64_t t = now_ns(); int n = 0;
    for (int i = 0; i < NBUILD; i++) {
        pid_t id = atomic_load(&g_build[i].tid); if (id <= 0) continue;
        char path[64], comm[32] = "?"; snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)id);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) { ssize_t k = read(fd, comm, sizeof comm - 1); close(fd); if (k > 0) { comm[k] = 0; if (comm[k - 1] == '\n') comm[k - 1] = 0; } }
        VLOG("  being made now: %s by thread %d '%s' for %.1f s (shaders %016llx %016llx)", atomic_load(&g_build[i].what), (int)id, comm,
             (t - atomic_load(&g_build[i].t0)) / 1e9, (unsigned long long)atomic_load(&g_build[i].vs), (unsigned long long)atomic_load(&g_build[i].fs));
        n++;
    }
    if (!n) VLOG("  being made now: no pipeline");
}
static int record_compile_outcome(const VkPipelineCreationFeedback *fb_all, const VkPipelineCreationFeedback *fb_st, uint32_t nst,
                                  int has_cache, uint64_t dur_ns, const char *what, uint64_t vs, uint64_t fs);
// The whole pipeline (optimized), with creation feedback (core in 1.3) for the line of a slow one: whether the pipeline cache had it and each stage's share. how: NULL = on the
// frame path (counted as the frame's), else where it was made (" in the background", " (prewarm)")
static VkPipeline full_pipeline(pstate *P, vka_shader *const sh[VKA_NSTAGE], const char *how) {
    VkPipelineCreationFeedback fb_all = {0}, fb_st[5]; memset(fb_st, 0, sizeof fb_st);
    VkPipelineCreationFeedbackCreateInfo fbi = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, .pNext = P->ci.pNext, .pPipelineCreationFeedback = &fb_all,
                                                .pipelineStageCreationFeedbackCount = P->ns, .pPipelineStageCreationFeedbacks = fb_st};
    P->ci.pNext = &fbi;
    uint64_t t0 = now_ns();
    VkPipeline p = VK_NULL_HANDLE;
    const vka_shader *pre0 = P->mesh ? sh[7] : sh[0];
    int bslot = build_begin("whole graphics pipeline", pre0 ? pre0->key : 0, sh[1] ? sh[1]->key : 0);
    VkPipelineCache pc_use = g_gpl_cache_on ? g_pc_mono : g_pc;
    VkResult r = vk.vkCreateGraphicsPipelines(V.dev, pc_use, 1, &P->ci, NULL, &p);
    build_end(bslot);
    P->ci.pNext = fbi.pNext;
    uint64_t dns = now_ns() - t0; double ms_ = dns / 1e6;
    if (!how && !r && p) record_compile_outcome(&fb_all, fb_st, P->ns, pc_use != VK_NULL_HANDLE, dns, "whole graphics pipeline", pre0 ? pre0->key : 0, sh[1] ? sh[1]->key : 0);
    if (g_gpl_cache_on && g_pc_mono && !r && p) {
        int valid = (fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0;
        int hit = (fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
        int all_st_hit = P->ns > 0;
        for (uint32_t i = 0; i < P->ns; i++) {
            if (!(fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ||
                !(fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)) {
                all_st_hit = 0;
                break;
            }
        }
        // Turnip frequently reports HIT_BIT on fb_all even for 200+ ms cold monolithic compiles; treat any
        // first-run build, uncached stage, missing hit bit, or >5 ms compile as populating g_pc_mono.
        if (!g_mono_cache_loaded || !valid || !hit || !all_st_hit || dns > 5000000ull) {
            atomic_fetch_add_explicit(&g_mono_made, 1, memory_order_relaxed);
            atomic_store_explicit(&g_mono_dirty, 1, memory_order_relaxed);
        }
    }
    pthread_mutex_lock(&g_mu);
    if (r) { p = VK_NULL_HANDLE; g_nfail++; } else { g_ngfx++; g_made++; if (!how) g_frame_made++; }
    g_create_ms += ms_; if (!how) g_create_ns += dns;   // made in the background or by prewarm: not the frame's time
    pthread_mutex_unlock(&g_mu);
    if (!how && !r && ms_ > 1000) vka_pso_save(2);   // a slow one on the frame path: saved within seconds, not at the next submission (see vka_pso_save)
    const vka_shader *pre = P->mesh ? sh[7] : sh[0];   // the vertex shader, or a mesh pipeline's mesh shader
    if (r) {   // every stage named: a geometry or tessellation shader in the pipeline is as likely to be the one the driver's compiler refused (DKB's terrain pipeline, 2026-11-07)
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 16) {
            char mid[96]; size_t o = 0; mid[0] = 0;
            if (!P->mesh && sh[3]) o += (size_t)snprintf(mid + o, sizeof mid - o, " tess control %016llx", (unsigned long long)sh[3]->key);
            if (!P->mesh && sh[4] && o < sizeof mid) o += (size_t)snprintf(mid + o, sizeof mid - o, " tess evaluation %016llx", (unsigned long long)sh[4]->key);
            if (!P->mesh && sh[2] && o < sizeof mid) o += (size_t)snprintf(mid + o, sizeof mid - o, " geometry %016llx", (unsigned long long)sh[2]->key);
            VRED("graphics pipeline (%s %016llx%s fragment %016llx, %u colour targets, topology %u): %d; its draws are skipped", P->mesh ? "mesh" : "vertex",
                 pre ? (unsigned long long)pre->key : 0ull, mid, sh[1] ? (unsigned long long)sh[1]->key : 0ull, P->ri.colorAttachmentCount, P->mesh ? 0u : P->ia.topology, r);
        }
    }
    else if (P->mesh) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32) VLOG("mesh pipeline: task %016llx mesh %016llx fragment %016llx, %.1f ms%s", sh[6] ? (unsigned long long)sh[6]->key : 0ull,
                                                                                      (unsigned long long)sh[7]->key, sh[1] ? (unsigned long long)sh[1]->key : 0ull, ms_, how ? how : ""); }
    else if (ms_ > 50) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 200) {
            char st[160]; size_t o = 0; st[0] = 0;
            for (uint32_t i = 0; i < P->ns && o < sizeof st; i++)
                o += (size_t)snprintf(st + o, sizeof st - o, "%s%s %.0f ms%s", i ? ", " : "", stage_name(P->ss[i].stage), (fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? fb_st[i].duration / 1e6 : -1.0,
                                      (fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? " (cached)" : "");
            VLOG("graphics pipeline (%s %016llx fragment %016llx) took %.0f ms%s (pipeline cache %s; %s)", P->mesh ? "mesh" : "vertex", pre ? (unsigned long long)pre->key : 0ull, sh[1] ? (unsigned long long)sh[1]->key : 0ull, ms_, how ? how : "",
                 !(fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? "?" : (fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? "hit" : "miss", st[0] ? st : "no stage times");
        }
    }
    return p;
}

// ---------- Graphics pipeline libraries (VK_EXT_graphics_pipeline_library, V.gpl) ----------
// Making a pipeline the cache lacks took 60-160 ms on the Adreno 840's own driver (500 ms at the phone's 1 GHz thermal cap), on the replay thread in the middle of a frame:
// the frame's GPU work waited for the rest of its recording, and the game's frame-rate controller, which times frame start to GPU end, dropped to 20 fps for a moment.
// With libraries the pipeline is linked without optimization from four parts, each kept by its own key: vertex input, the pre-rasterization shaders, the fragment shader
// and the fragment output. Parts recur far more than whole pipelines (a fragment shader with many vertex shaders, the same targets and blending), so a miss usually compiles
// one stage, or none. The optimized whole pipeline is then made on a background thread (into the pipeline cache) and takes the linked one's place in the map; draws recorded
// before that keep the linked one, which stays alive with the process like every pipeline here.
// Found 2026-11-03 (stutter-agent cold2gpl / cold3gpl / gpl-2335 logs): Qualcomm's Adreno 840 driver (build 718a07726a) refused EVERY pre-rasterization library with
// VK_ERROR_UNKNOWN (-13), 0 made in any run: no pipeline was ever linked, each miss paid a vertex-input library, a refused library and then the whole pipeline on the frame
// path (cold3gpl 70 ms a pipeline against 59 without GPL). The first refusal now runs a probe (pre_probe): the same library made with one thing changed at a time, each
// result logged; a change that makes it is adopted for the run (g_pre_adapt), and with none, GPL turns itself off after DM_VK_GPL_GIVEUP refusals (default 3) with no
// pre-rasterization library made, so a miss costs the whole pipeline only
typedef struct { uint8_t topo; uint32_t vin_static; uint8_t va[sizeof(((vka_gkey *)0)->va)], vb[sizeof(((vka_gkey *)0)->vb)]; } vikey;
// rp: with classic render passes (DM_VK_CLASSIC_RP) the shader parts are made against the pipeline's render pass (vka_rp_compat), which must be the same for every part
// of one link; the fragment output's key holds the formats and samples that pick it
typedef struct { VkShaderModule mod[4]; VkShaderStageFlagBits stage[4]; uint32_t pin[4]; uint8_t gl[4]; VkPipelineLayout layout; VkRenderPass rp; uint8_t depth_clamp, poly_mode, tess, vin_static; } prekey;
typedef struct { VkShaderModule mod; uint32_t pin, afunc; VkPipelineLayout layout; VkRenderPass rp; uint8_t samples, a2c, vin_static; } fskey;
typedef struct { uint32_t cfmt[8], dfmt, sfmt; uint8_t ncolor, samples, a2c, logic_en, logic_op, vin_static, att[8][8]; } fokey;
typedef struct { prekey pre; fskey fs; } prefskey;
static kmap g_lib[NLIB] = {{.klen = sizeof(vikey)}, {.klen = sizeof(prekey)}, {.klen = sizeof(fskey)}, {.klen = sizeof(fokey)}, {.klen = sizeof(prefskey)}};
static vka_gpl_lat g_link_lat;
static _Atomic uint64_t g_ir3_miss, g_ram_rehydrate;
static _Atomic uint64_t g_retype_shard_hit, g_retype_shard_miss;   // Boot #1 mini-shard hits/misses for retyped stages
static _Atomic int g_retype_miss_logged;                           // bounded diagnostic lines (max 12)

static int record_compile_outcome(const VkPipelineCreationFeedback *fb_all, const VkPipelineCreationFeedback *fb_st, uint32_t nst,
                                  int has_cache, uint64_t dur_ns, const char *what, uint64_t vs, uint64_t fs) {
    int valid = (fb_all->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0;
    int hit = (fb_all->flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    int all_st_valid = nst > 0, all_st_hit = nst > 0;
    for (uint32_t i = 0; i < nst; i++) {
        if (!(fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) all_st_valid = 0;
        if (!(fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)) all_st_hit = 0;
    }
    if (vka_classify_compile_feedback(valid, hit, all_st_valid, all_st_hit, has_cache, dur_ns)) {
        atomic_fetch_add_explicit(&g_ram_rehydrate, 1, memory_order_relaxed);
        return 1;
    } else {
        uint64_t m = atomic_fetch_add_explicit(&g_ir3_miss, 1, memory_order_relaxed) + 1;
        if (m <= 32) VLOG("ir3_compiler_miss #%llu (%s, vs %016llx fs %016llx, %.1f ms)",
                          (unsigned long long)m, what, (unsigned long long)vs, (unsigned long long)fs, dur_ns / 1e6);
        return 0;
    }
}
static const char *const g_lib_name[NLIB] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output", "pre-rasterization + fragment shader"};
enum { A_NOCACHE = 1, A_RETAIN = 2, A_NORI = 4, A_COMBINED = 8 };
static unsigned g_pre_adapt; static int g_pre_probed, g_pre_refused; static _Atomic int g_gpl_off;
// Cache-only probes for a retyped stage (V.cache_ctl = pipelineCreationCacheControl): the delta
// retype cache first, then the shader's read-only Boot #1 mini-shard. The flag is put back before
// the caller's normal live compile, so no driver-visible state is left changed.
static VkResult retype_probe_gfx(VkGraphicsPipelineCreateInfo *ci, VkPipelineCache pc, VkPipeline *p) {
    VkPipelineCreateFlags saved = ci->flags;
    ci->flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    VkResult r = vk.vkCreateGraphicsPipelines(V.dev, pc, 1, ci, NULL, p);
    ci->flags = saved;
    return r;
}
static VkResult retype_probe_cs(VkComputePipelineCreateInfo *ci, VkPipelineCache pc, VkPipeline *p) {
    VkPipelineCreateFlags saved = ci->flags;
    ci->flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    VkResult r = vk.vkCreateComputePipelines(V.dev, pc, 1, ci, NULL, p);
    ci->flags = saved;
    return r;
}
// With classic render passes (DM_VK_CLASSIC_RP) too: until 2026-11-11 they turned the libraries off, so every pipeline the cache lacked was made whole and optimized, and
// the asynchronous threads (DM_VK_PSO_ASYNC), meant to link, compiled whole pipelines instead (Turnip on the Adreno 840: 0.2-3.8 s each in Ocarina of Time's market,
// a backlog of 431 that drained for minutes). The libraries take the pipeline's render pass instead (gpl_link)
static int gpl_on(void) { return V.gpl && !atomic_load(&g_gpl_off); }
// A library's map value: the library, LIB_REFUSED (the driver refused it: the whole pipeline is made instead) or LIB_BUSY (a thread is making it). Several threads link
// (the replay thread, the DM_VK_PSO_ASYNC threads) and often miss the same part at once (one fragment shader under many vertex shaders): the first claims the key with
// LIB_BUSY and makes it, the others wait for it on g_lib_cv instead of compiling it again (each copy a slow compile, a second map entry and a leaked library). A thread
// holds at most one claim and gives it up before it asks for its next part, so the waits cannot form a cycle
#define LIB_REFUSED ((void *)(uintptr_t)1)
#define LIB_BUSY ((void *)(uintptr_t)2)
static pthread_cond_t g_lib_cv = PTHREAD_COND_INITIALIZER;
static VkPipeline lib_make(int kind, const void *key, size_t klen, VkGraphicsPipelineCreateInfo *ci, VkGraphicsPipelineLibraryFlagsEXT part, int *refused, uint64_t vs, uint64_t fs, int cache_only, VkPipelineCache pc_override, int *from_shard, int *shard_missed) {
    if (from_shard) *from_shard = 0;
    if (shard_missed) *shard_missed = 0;
    uint64_t h = fnv(key, klen, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    void *hit;
    if (cache_only) {
        hit = km_get(&g_lib[kind], key, h);
        pthread_mutex_unlock(&g_mu);
        if (hit) return (hit == LIB_REFUSED || hit == LIB_BUSY) ? VK_NULL_HANDLE : (VkPipeline)hit;
    } else {
        while ((hit = km_get(&g_lib[kind], key, h)) == LIB_BUSY) pthread_cond_wait(&g_lib_cv, &g_mu);   // (looked up again: an adaptation may have emptied the map meanwhile)
        if (!hit) km_put(&g_lib[kind], key, h, LIB_BUSY);
        pthread_mutex_unlock(&g_mu);
        if (hit) return hit == LIB_REFUSED ? VK_NULL_HANDLE : (VkPipeline)hit;
    }
    int shp = kind == L_PRE || kind == L_PREFS;
    VkPipelineCreationFeedback fb_all = {0}, fb_st[5]; memset(fb_st, 0, sizeof fb_st);
    uint32_t nst = (kind == L_PRE || kind == L_FS || kind == L_PREFS) && ci->stageCount <= 5 ? ci->stageCount : 0;
    VkPipelineCreationFeedbackCreateInfo fbi = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
        .pNext = shp && (g_pre_adapt & A_NORI) ? NULL : ci->pNext, .pPipelineCreationFeedback = &fb_all,
        .pipelineStageCreationFeedbackCount = nst, .pPipelineStageCreationFeedbacks = fb_st};
    const void *orig_next = ci->pNext;
    VkPipelineCreateFlags orig_flags = ci->flags;
    VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = nst ? &fbi : (shp && (g_pre_adapt & A_NORI) ? NULL : ci->pNext), .flags = part};
    ci->pNext = &li;
    ci->flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (((g_pre_adapt & A_RETAIN) || V.gpl_lto) ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0);
    if (cache_only && (kind == L_PRE || kind == L_FS || kind == L_PREFS)) ci->flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    uint64_t t0 = now_ns();
    int bs = build_begin(g_lib_name[kind], vs, fs);
    int use_cache = !(shp && (g_pre_adapt & A_NOCACHE));
    int shard_slot = -1;
    uint64_t shard_key = (kind == L_FS) ? fs : (vs ? vs : fs);
    VkPipeline p = VK_NULL_HANDLE; VkResult r = VK_ERROR_UNKNOWN;
    int from_shard_local = 0, has_cache = 0, settled = 0;
    // A retyped L_PRE / L_FS stage probes g_pc_retype, then the shader's Boot #1 mini-shard,
    // both cache-only. Non-retyped stages keep the single-cache path below.
    if (use_cache && pc_override && pc_override == g_pc_retype && g_gpl_cache_on && V.cache_ctl &&
        (kind == L_PRE || kind == L_FS)) {
        r = retype_probe_gfx(ci, g_pc_retype, &p);              // 1: delta retype cache
        if (r == VK_SUCCESS && p) { settled = 1; has_cache = 1; }
        else {
            p = VK_NULL_HANDLE;
            VkPipelineCache shard_pc = shard_pc_acquire(shard_key, &shard_slot);
            if (shard_pc != VK_NULL_HANDLE) {
                r = retype_probe_gfx(ci, shard_pc, &p);        // 2: Boot #1 mini-shard
                shard_pc_release(shard_slot); shard_slot = -1;
                if (r == VK_SUCCESS && p) {
                    atomic_fetch_add_explicit(&g_retype_shard_hit, 1, memory_order_relaxed);
                    from_shard_local = 1; settled = 1; has_cache = 1;
                } else {
                    atomic_fetch_add_explicit(&g_retype_shard_miss, 1, memory_order_relaxed);
                    if (shard_missed) *shard_missed = 1;
                    p = VK_NULL_HANDLE;
                }
            } else {
                atomic_fetch_add_explicit(&g_retype_shard_miss, 1, memory_order_relaxed);
                if (shard_missed) *shard_missed = 1;
            }
        }
    }
    if (!settled && !cache_only) {
        VkPipelineCache pc_use = !use_cache ? VK_NULL_HANDLE
                               : pc_override ? pc_override
                               : (g_gpl_cache_on && (kind == L_PRE || kind == L_FS || kind == L_PREFS)) ? shard_pc_acquire(shard_key, &shard_slot)
                               : g_pc;
        has_cache = pc_use != VK_NULL_HANDLE;
        r = vk.vkCreateGraphicsPipelines(V.dev, pc_use, 1, ci, NULL, &p);
    } else if (!settled) {
        // DM_VK_PSO_ASYNC cache-only try: both caches missed, report a miss without compiling.
        r = VK_PIPELINE_COMPILE_REQUIRED; p = VK_NULL_HANDLE;
    }
    shard_pc_release(shard_slot);   // always, including every early return below; -1 is a no-op
    build_end(bs);
    ci->pNext = orig_next;
    ci->flags = orig_flags;
    uint64_t dns = now_ns() - t0;
    double ms_ = dns / 1e6;
    if (from_shard) *from_shard = from_shard_local;
    if (cache_only) {
        if (r != VK_SUCCESS || !p) return VK_NULL_HANDLE;
        if (nst) atomic_fetch_add_explicit(&g_ram_rehydrate, 1, memory_order_relaxed);
        pthread_mutex_lock(&g_mu);
        void *cur = km_get(&g_lib[kind], key, h);
        if (!cur) { km_put(&g_lib[kind], key, h, (void *)p); g_nlib[kind]++; g_lib_ms += ms_; }
        else if (cur != LIB_BUSY && cur != LIB_REFUSED) { vk.vkDestroyPipeline(V.dev, p, NULL); p = (VkPipeline)cur; }
        else { vk.vkDestroyPipeline(V.dev, p, NULL); p = VK_NULL_HANDLE; }
        pthread_mutex_unlock(&g_mu);
        return p;
    }
    if (!r && p && nst) {
        // A Boot #1 mini-shard hit is a cache hit: never an ir3 miss, and never a reason to
        // grow the delta retype cache or the variant manifest.
        int rehy = from_shard_local ? 1 : record_compile_outcome(&fb_all, fb_st, nst, has_cache, dns, g_lib_name[kind], vs, fs);
        if (!rehy && !from_shard_local && pc_override && pc_override == g_pc_retype)
            atomic_store_explicit(&g_retype_dirty, 1, memory_order_relaxed);
    }
    pthread_mutex_lock(&g_mu);
    if (r) p = VK_NULL_HANDLE; else g_nlib[kind]++;
    g_lib_ms += ms_;
    void **slot = km_slot(&g_lib[kind], key, h);   // (gone if an adaptation emptied the map while it was made: then not kept, as it was made without the adaptation)
    if (slot) *slot = p ? (void *)p : LIB_REFUSED;
    pthread_cond_broadcast(&g_lib_cv);
    pthread_mutex_unlock(&g_mu);
    if (r) { if (refused) *refused = r; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("graphics pipeline library (%s): %d; the whole pipeline is made instead", g_lib_name[kind], r); }
    else if (ms_ > 50) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 100) VLOG("graphics pipeline library (%s) took %.0f ms", g_lib_name[kind], ms_); }
    return p;
}
// Each library names only the dynamic states of its own part (the Adreno 840's driver refused a pre-rasterization library given the whole list, VK_ERROR_UNKNOWN);
// multisample dynamic states belong to both fragment shader and fragment output libraries; the link takes them from the libraries
static uint32_t dyn_part_mask(VkDynamicState d) {
    switch (d) {
    case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT: case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
        return 1u << L_VI;
    case VK_DYNAMIC_STATE_VIEWPORT: case VK_DYNAMIC_STATE_SCISSOR: case VK_DYNAMIC_STATE_LINE_WIDTH: case VK_DYNAMIC_STATE_DEPTH_BIAS:
    case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: case VK_DYNAMIC_STATE_CULL_MODE: case VK_DYNAMIC_STATE_FRONT_FACE:
    case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: case VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT:
    case VK_DYNAMIC_STATE_POLYGON_MODE_EXT: case VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT:
        return 1u << L_PRE;
    case VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT: case VK_DYNAMIC_STATE_SAMPLE_MASK_EXT: case VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT:
        return (1u << L_FS) | (1u << L_FO);
    case VK_DYNAMIC_STATE_BLEND_CONSTANTS: case VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT:
    case VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT: case VK_DYNAMIC_STATE_LOGIC_OP_EXT:
    case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT: case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT: case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:
        return 1u << L_FO;
    default: return 1u << L_FS;   // depth and stencil tests, depth bounds
    }
}
static void dyn_of(const pstate *P, int part, VkDynamicState *out, VkPipelineDynamicStateCreateInfo *dy) {
    uint32_t n = 0, want = part == L_PREFS ? ((1u << L_PRE) | (1u << L_FS)) : (1u << part);
    for (uint32_t i = 0; i < P->dy.dynamicStateCount; i++) if (dyn_part_mask(P->dyn[i]) & want) out[n++] = P->dyn[i];
    *dy = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = n, .pDynamicStates = out};
}

// ---------- Boot #1 GPL + Compute pre-compile worker pool ----------
static uint64_t mem_avail_mb(void);
int plat_maps_count(int *max);

static void precomp_fill_dyn(VkDynamicState *out_dyn, VkPipelineDynamicStateCreateInfo *out_dy, int part) {
    static const VkDynamicState base[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_LINE_WIDTH};
    uint32_t eds = active_eds_mask(0), n = 0, want = 1u << part;
    for (size_t i = 0; i < sizeof base / sizeof *base; i++)
        if (dyn_part_mask(base[i]) & want) out_dyn[n++] = base[i];
    if (V.vin_dyn && (dyn_part_mask(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    if (V.fb_loop && (dyn_part_mask(VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT;
    if ((eds & VKA_EDS_DEPTH_CLAMP) && (dyn_part_mask(VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    if ((eds & VKA_EDS_POLYGON_MODE) && (dyn_part_mask(VK_DYNAMIC_STATE_POLYGON_MODE_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    if ((eds & VKA_EDS_RAST_SAMPLES) && (dyn_part_mask(VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT;
    if ((eds & VKA_EDS_SAMPLE_MASK) && (dyn_part_mask(VK_DYNAMIC_STATE_SAMPLE_MASK_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
    if ((eds & VKA_EDS_ALPHA_TO_COV) && (dyn_part_mask(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
    if ((eds & VKA_EDS_LOGIC_OP_ENABLE) && (dyn_part_mask(VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT;
    if ((eds & VKA_EDS_LOGIC_OP) && (dyn_part_mask(VK_DYNAMIC_STATE_LOGIC_OP_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_LOGIC_OP_EXT;
    if ((eds & VKA_EDS_COLOR_BLEND_ENABLE) && (dyn_part_mask(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
    if ((eds & VKA_EDS_COLOR_BLEND_EQ) && (dyn_part_mask(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
    if ((eds & VKA_EDS_COLOR_WRITE_MASK) && (dyn_part_mask(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT) & want)) out_dyn[n++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
    *out_dy = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = n, .pDynamicStates = out_dyn};
}

static void precomp_pipe_keep(VkPipeline p, VkPipeline *pipes, int *npipes, int max_pipes) {
    if (!p) return;
    if (*npipes < max_pipes) pipes[(*npipes)++] = p;
    else vk.vkDestroyPipeline(V.dev, p, NULL);   // still compiled into local_pc
}

static void precomp_compile_entry(const vka_shlib *L, uint32_t idx, VkPipelineCache local_pc, int gl_depth,
                                  VkPipeline *pipes, int *npipes, int max_pipes,
                                  uint32_t *c_vs, uint32_t *c_fs, uint32_t *c_cs, uint32_t *c_skip, uint32_t *c_fail,
                                  _Atomic uint32_t *r_vin, _Atomic uint32_t *r_tex, _Atomic uint32_t *r_fout,
                                  _Atomic uint32_t *r_ok, _Atomic uint32_t *r_fail) {
    vka_precomp_sh ps;
    if (!vka_shader_precomp_open(L, idx, &ps)) { (*c_skip)++; return; }
    int hv = (ps.retype_hint.vin_u | ps.retype_hint.vin_s) != 0;
    int ht = (ps.retype_hint.tex_u | ps.retype_hint.tex_s) != 0;
    int hf = (ps.retype_hint.fout_u | ps.retype_hint.fout_s) != 0;
    if (hv) atomic_fetch_add_explicit(r_vin, 1, memory_order_relaxed);
    if (ht) atomic_fetch_add_explicit(r_tex, 1, memory_order_relaxed);
    if (hf) atomic_fetch_add_explicit(r_fout, 1, memory_order_relaxed);
    if ((hv || ht || hf) && ps.retype_mod == VK_NULL_HANDLE) atomic_fetch_add_explicit(r_fail, 1, memory_order_relaxed);
    vka_shader tmp = {0};
    tmp.key = ps.e->key; tmp.e = ps.e; tmp.b = ps.b; tmp.lib = L; tmp.tf_fx = ps.tf_fx; tmp.gs_fx = ps.gs_fx;
    VkPipelineCreateFlags lto_flag = V.gpl_lto ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0;
    VkPipelineRenderingCreateInfo ri0 = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};

    if (ps.e->stage == DMSH_VK_STAGE_VS) {
        vka_shader *sh[VKA_NSTAGE] = {0}; sh[0] = &tmp;
        vka_layout lay;
        if (!vka_layout_pre_of(sh, &lay)) { (*c_fail)++; goto done; }
        VkDynamicState dl[40]; VkPipelineDynamicStateCreateInfo dy;
        precomp_fill_dyn(dl, &dy, L_PRE);
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1.0f};
        VkBool32 gl_true = VK_TRUE;
        VkSpecializationMapEntry me = {DMSH_VK_SPEC_GL_DEPTH, 0, sizeof(VkBool32)};
        VkSpecializationInfo spec = {1, &me, sizeof gl_true, &gl_true};
        int use_gl = gl_depth && (ps.e->flags & DMSH_VK_F_GL_DEPTH);
        uint32_t pin = (ps.e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(DMSH_VK_STAGE_VS) : 0;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
        VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = &ri0, .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT};
        static const int vs_vars[2] = {DMSH_VK_MOD_BASE, DMSH_VK_MOD_TF};
        for (int vi = 0; vi < 2; vi++) {
            VkShaderModule mod = ps.mods[vs_vars[vi]];
            if (!mod) continue;
            VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL,
                .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = mod, .pName = "main", .pSpecializationInfo = use_gl ? &spec : NULL};
            VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | lto_flag, .stageCount = 1, .pStages = &st,
                .pViewportState = &vp, .pRasterizationState = &rs, .pDynamicState = &dy, .layout = lay.pre_layout};
            VkPipeline p = VK_NULL_HANDLE;
            if (vk.vkCreateGraphicsPipelines(V.dev, local_pc, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
                (*c_vs)++;
                if (*npipes < max_pipes) pipes[(*npipes)++] = p; else vk.vkDestroyPipeline(V.dev, p, NULL);
            } else (*c_fail)++;
        }
        if (ps.retype_mod != VK_NULL_HANDLE) {   // inferred vin / tex pre-rasterization library, same shape as the base one
            VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL,
                .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = ps.retype_mod, .pName = "main", .pSpecializationInfo = use_gl ? &spec : NULL};
            VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | lto_flag, .stageCount = 1, .pStages = &st,
                .pViewportState = &vp, .pRasterizationState = &rs, .pDynamicState = &dy, .layout = lay.pre_layout};
            VkPipeline p = VK_NULL_HANDLE;
            if (vk.vkCreateGraphicsPipelines(V.dev, local_pc, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
                precomp_pipe_keep(p, pipes, npipes, max_pipes);
                atomic_fetch_add_explicit(r_ok, 1, memory_order_relaxed);
            } else atomic_fetch_add_explicit(r_fail, 1, memory_order_relaxed);
        }
    } else if (ps.e->stage == DMSH_VK_STAGE_FS) {
        vka_layout lay;
        if (!vka_layout_fs_of(&tmp, &lay)) { (*c_fail)++; goto done; }
        VkDynamicState dl[40]; VkPipelineDynamicStateCreateInfo dy;
        precomp_fill_dyn(dl, &dy, L_FS);
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .maxDepthBounds = 1.0f};
        uint32_t pin = (ps.e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(DMSH_VK_STAGE_FS) : 0;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
        VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = &ri0, .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT};
        VkShaderModule fs_mods[5] = {ps.mods[DMSH_VK_MOD_BASE], ps.mods[DMSH_VK_MOD_UINT], ps.mods[DMSH_VK_MOD_UINT_MIXED], ps.mods[DMSH_VK_MOD_DUAL], ps.pfb_late};
        for (int fi = 0; fi < 5; fi++) {
            VkShaderModule mod = fs_mods[fi];
            if (!mod) continue;
            VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT, .alphaToCoverageEnable = (fi == 4) ? VK_TRUE : VK_FALSE};
            VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = mod, .pName = "main"};
            VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | lto_flag, .stageCount = 1, .pStages = &st,
                .pMultisampleState = &ms, .pDepthStencilState = &ds, .pDynamicState = &dy, .layout = lay.fs_layout};
            VkPipeline p = VK_NULL_HANDLE;
            if (vk.vkCreateGraphicsPipelines(V.dev, local_pc, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
                (*c_fs)++;
                if (*npipes < max_pipes) pipes[(*npipes)++] = p; else vk.vkDestroyPipeline(V.dev, p, NULL);
            } else (*c_fail)++;
        }
        if (ps.retype_mod != VK_NULL_HANDLE) {   // inferred fout / tex fragment library, as in svar_compile_one
            VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = ps.retype_mod, .pName = "main"};
            VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
            VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | lto_flag, .stageCount = 1, .pStages = &st,
                .pMultisampleState = &ms, .pDepthStencilState = &ds, .pDynamicState = &dy, .layout = lay.fs_layout};
            VkPipeline p = VK_NULL_HANDLE;
            if (vk.vkCreateGraphicsPipelines(V.dev, local_pc, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
                precomp_pipe_keep(p, pipes, npipes, max_pipes);
                atomic_fetch_add_explicit(r_ok, 1, memory_order_relaxed);
            } else atomic_fetch_add_explicit(r_fail, 1, memory_order_relaxed);
            if (ps.retype_pfb_late != VK_NULL_HANDLE) {   // same shape, twin without early fragment tests
                st.module = ps.retype_pfb_late;
                VkPipeline p2 = VK_NULL_HANDLE;
                if (vk.vkCreateGraphicsPipelines(V.dev, local_pc, 1, &ci, NULL, &p2) == VK_SUCCESS && p2) {
                    precomp_pipe_keep(p2, pipes, npipes, max_pipes);
                    atomic_fetch_add_explicit(r_ok, 1, memory_order_relaxed);
                } else atomic_fetch_add_explicit(r_fail, 1, memory_order_relaxed);
            }
        }
    } else if (ps.e->stage == DMSH_VK_STAGE_CS) {
        vka_shader *sh[VKA_NSTAGE] = {0}; sh[5] = &tmp;
        vka_layout lay;
        if (!layout_slow(sh, 1, &lay)) { (*c_fail)++; goto done; }
        VkShaderModule csm = ps.mods[DMSH_VK_MOD_SHUFFLE_LDS] ? ps.mods[DMSH_VK_MOD_SHUFFLE_LDS] : ps.mods[DMSH_VK_MOD_BASE];
        if (!csm) { (*c_fail)++; goto done; }
        uint32_t pin = (ps.e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(DMSH_VK_STAGE_CS) : 0;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
        int spill = 0;
        for (unsigned i = 0; i < ps.e->bind_count; i++) if (ps.b[i].kind == DMSH_VK_KIND_SHARED_SPILL) spill = 1;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .flags = spill ? VK_PIPELINE_CREATE_DISPATCH_BASE_BIT : 0, .layout = lay.layout,
            .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = csm, .pName = "main"}};
        VkPipeline p = VK_NULL_HANDLE;
        if (vk.vkCreateComputePipelines(V.dev, local_pc, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
            (*c_cs)++;
            if (*npipes < max_pipes) pipes[(*npipes)++] = p; else vk.vkDestroyPipeline(V.dev, p, NULL);
        } else (*c_fail)++;
        if (ps.retype_mod != VK_NULL_HANDLE) {   // inferred tex compute pipeline, same args as the base one
            VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .flags = spill ? VK_PIPELINE_CREATE_DISPATCH_BASE_BIT : 0, .layout = lay.layout,
                .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL,
                          .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = ps.retype_mod, .pName = "main"}};
            VkPipeline p = VK_NULL_HANDLE;
            if (vk.vkCreateComputePipelines(V.dev, local_pc, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
                precomp_pipe_keep(p, pipes, npipes, max_pipes);
                atomic_fetch_add_explicit(r_ok, 1, memory_order_relaxed);
            } else atomic_fetch_add_explicit(r_fail, 1, memory_order_relaxed);
        }
    }
done:
    vka_shader_precomp_destroy(&ps);
}

typedef struct {
    uint32_t start;
    uint32_t count;
} precomp_shard_slice;

typedef struct {
    const vka_shlib *L;
    const precomp_shard_slice *slices;
    const uint32_t *shard_indices;
    const uint32_t *shard_order;
    uint32_t rem_shards;
    uint32_t shard_count;
    vka_gpl_shard_ent *table;
    int rw_fd;
    uint64_t append_off;
    uint32_t shards_saved;
    int max_workers;
    int gl_depth;
    _Atomic uint32_t next_shard;
    _Atomic uint64_t last_check_ns;
    _Atomic uint32_t vs_n, fs_n, cs_n, skip_n, fail_n;
    _Atomic uint32_t rv_n, rt_n, rf_n, rok_n, rfail_n;   // inferred vin / tex / fout, compiled, failed
} precomp_ctx;

typedef struct {
    precomp_ctx *ctx;
    int wid;
} precomp_arg;

static void *precomp_worker(void *arg_) {
    precomp_arg *pa = (precomp_arg *)arg_;
    precomp_ctx *ctx = pa->ctx;
    int wid = pa->wid;
    char tname[16]; snprintf(tname, sizeof tname, "vk-pre-%d", wid);
    pthread_setname_np(pthread_self(), tname);
    vka_fp_host(tname);
    setpriority(PRIO_PROCESS, 0, 5);

    enum { SHARD_PIPES_CAP = 512 };
    VkPipeline pipes[SHARD_PIPES_CAP];
    uint32_t c_vs = 0, c_fs = 0, c_cs = 0, c_skip = 0, c_fail = 0;

    for (;;) {
        if (atomic_load_explicit(&g_pre_skip, memory_order_acquire) || atomic_load_explicit(&g_pre_stop, memory_order_acquire)) break;
        while (atomic_load_explicit(&g_pso_paused, memory_order_relaxed) &&
               !atomic_load_explicit(&g_pre_skip, memory_order_acquire) &&
               !atomic_load_explicit(&g_pre_stop, memory_order_acquire) &&
               atomic_load_explicit(&ctx->next_shard, memory_order_acquire) < ctx->rem_shards) {
            usleep(100000);
        }
        uint64_t now = now_ns();
        uint64_t prev = atomic_load_explicit(&ctx->last_check_ns, memory_order_relaxed);
        if (now - prev >= 500000000ull && atomic_compare_exchange_strong(&ctx->last_check_ns, &prev, now)) {
            int mx = 0, maps = plat_maps_count(&mx), abort_early = 0;
            uint64_t av = mem_avail_mb();
            double elapsed_s = (now - atomic_load_explicit(&g_pre_t0_ns, memory_order_relaxed)) / 1e9;
            int tgt = vka_precompile_target_workers(ctx->max_workers, elapsed_s, av, maps, mx, &abort_early);
            if (abort_early) {
                VLOG("boot precompile: stopping early for memory/maps safety (MemAvailable %llu MiB, maps %d / %d)", (unsigned long long)av, maps, mx);
                atomic_store_explicit(&g_pre_stop, 1, memory_order_release);
            } else {
                int was = atomic_exchange_explicit(&g_pre_active_workers, tgt, memory_order_acq_rel);
                if (tgt != was) VLOG("boot precompile: workers %d -> %d (%.1fs, MemAvailable %llu MiB, maps %d / %d)", was, tgt, elapsed_s, (unsigned long long)av, maps, mx);
            }
        }
        while (wid >= atomic_load_explicit(&g_pre_active_workers, memory_order_acquire) &&
               !atomic_load_explicit(&g_pre_skip, memory_order_acquire) &&
               !atomic_load_explicit(&g_pre_stop, memory_order_acquire) &&
               atomic_load_explicit(&ctx->next_shard, memory_order_acquire) < ctx->rem_shards) {
            usleep(100000);
        }
        if (atomic_load_explicit(&g_pre_skip, memory_order_acquire) || atomic_load_explicit(&g_pre_stop, memory_order_acquire) ||
            atomic_load_explicit(&ctx->next_shard, memory_order_acquire) >= ctx->rem_shards) break;

        uint32_t pos = atomic_fetch_add_explicit(&ctx->next_shard, 1, memory_order_relaxed);
        if (pos >= ctx->rem_shards) break;
        uint32_t sid = ctx->shard_order[pos];
        precomp_shard_slice sl = ctx->slices[sid];

        VkPipelineCache local_pc = VK_NULL_HANDLE;
        if (sl.count > 0) {
            VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            if (vk.vkCreatePipelineCache(V.dev, &pci, NULL, &local_pc)) local_pc = VK_NULL_HANDLE;
        }
        int npipes = 0;
        int aborted_mid_shard = 0;
        for (uint32_t j = 0; j < sl.count; j++) {
            if (atomic_load_explicit(&g_pre_skip, memory_order_acquire) || atomic_load_explicit(&g_pre_stop, memory_order_acquire)) {
                aborted_mid_shard = 1;
                break;
            }
            while (atomic_load_explicit(&g_pso_paused, memory_order_relaxed) &&
                   !atomic_load_explicit(&g_pre_skip, memory_order_acquire) &&
                   !atomic_load_explicit(&g_pre_stop, memory_order_acquire)) {
                usleep(100000);
            }
            uint32_t idx = ctx->shard_indices[sl.start + j];
            if (local_pc) {
                precomp_compile_entry(ctx->L, idx, local_pc, ctx->gl_depth, pipes, &npipes, SHARD_PIPES_CAP, &c_vs, &c_fs, &c_cs, &c_skip, &c_fail,
                                      &ctx->rv_n, &ctx->rt_n, &ctx->rf_n, &ctx->rok_n, &ctx->rfail_n);
            } else {
                c_fail++;
            }
            atomic_fetch_add_explicit(&g_pre_compiled, 1, memory_order_relaxed);
        }

        for (int k = 0; k < npipes; k++) if (pipes[k]) vk.vkDestroyPipeline(V.dev, pipes[k], NULL);

        if (!aborted_mid_shard) {
            size_t n = 0; void *d = NULL;
            if (local_pc && sl.count > 0) {
                VkResult gr = VK_INCOMPLETE;
                for (int tr = 0; tr < 3 && gr == VK_INCOMPLETE; tr++) {
                    size_t q = 0; if (vk.vkGetPipelineCacheData(V.dev, local_pc, &q, NULL) || !q) { gr = VK_ERROR_UNKNOWN; break; }
                    n = q + q / 4; free(d); d = malloc(n); if (!d) { gr = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
                    gr = vk.vkGetPipelineCacheData(V.dev, local_pc, &n, d);
                }
                if (gr != VK_SUCCESS) { free(d); d = NULL; n = 0; }
            }
            if (local_pc && vk.vkDestroyPipelineCache) { vk.vkDestroyPipelineCache(V.dev, local_pc, NULL); local_pc = VK_NULL_HANDLE; }
            if (d || sl.count == 0) {
                pthread_mutex_lock(&g_save_mu);
                int want_sync = ((++ctx->shards_saved & 15u) == 0u) || atomic_exchange_explicit(&g_pre_checkpoint_want, 0, memory_order_acq_rel);
                if (!vka_gpl_sharded_write_shard(ctx->rw_fd, &g_gpl_hdr, ctx->table, ctx->shard_count, sid, sl.count, d, (uint32_t)n, &ctx->append_off, want_sync)) {
                    static _Atomic int k;
                    if (atomic_fetch_add(&k, 1) < 4) VRED("gpl cache: failed writing shard %u to %s", sid, g_gpl_cache_path);
                }
                pthread_mutex_unlock(&g_save_mu);
                crash_watch_progress(g_gpl_hdr.compiled_entries, "boot shader precompile");
            }
            free(d);
        } else {
            // Rewind pre_compiled count for unfinished shard so UI doesn't double-count on resume.
            if (local_pc && vk.vkDestroyPipelineCache) { vk.vkDestroyPipelineCache(V.dev, local_pc, NULL); local_pc = VK_NULL_HANDLE; }
        }
    }
    atomic_fetch_add_explicit(&ctx->vs_n, c_vs, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->fs_n, c_fs, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->cs_n, c_cs, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->skip_n, c_skip, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->fail_n, c_fail, memory_order_relaxed);
    return NULL;
}

static void vka_boot_precompile_run(const vka_shlib *L, int rw_fd, vka_gpl_shard_ent *table, uint32_t shard_count, uint64_t append_off) {
    uint32_t total = L->h->entry_count;
    if (!total || rw_fd < 0 || !table || !shard_count) {
        if (rw_fd >= 0) close(rw_fd);
        return;
    }

    precomp_shard_slice *slices = calloc(shard_count, sizeof *slices);
    uint32_t *shard_indices = malloc((size_t)total * sizeof *shard_indices);
    uint32_t *shard_order = malloc((size_t)shard_count * sizeof *shard_order);
    uint8_t *shard_prio = calloc(shard_count, 1);
    if (!slices || !shard_indices || !shard_order || !shard_prio) {
        free(slices); free(shard_indices); free(shard_order); free(shard_prio);
        close(rw_fd);
        return;
    }

    for (uint32_t i = 0; i < total; i++) {
        uint32_t sid = vka_gpl_shard_of(L->ent[i].key, shard_count);
        slices[sid].count++;
    }
    uint32_t off = 0;
    for (uint32_t s = 0; s < shard_count; s++) {
        slices[s].start = off;
        off += slices[s].count;
        slices[s].count = 0;
    }
    for (uint32_t i = 0; i < total; i++) {
        uint32_t sid = vka_gpl_shard_of(L->ent[i].key, shard_count);
        shard_indices[slices[sid].start + slices[sid].count++] = i;
    }

    if (g_pw_load && g_pw_load_len) {
        for (size_t o = 0; o + sizeof(pwrec) <= g_pw_load_len;) {
            pwrec r; memcpy(&r, g_pw_load + o, sizeof r);
            o += vka_pw_size(&r);
            int nkeys = r.kind == VKA_PW_CS ? 1 : r.kind == VKA_PW_MESH ? 3 : 5;
            for (int s = 0; s < nkeys; s++) {
                if (!r.key[s]) continue;
                shard_prio[vka_gpl_shard_of(r.key[s], shard_count)] = 1;
            }
        }
    }

    uint32_t rem_shards = 0, already_shaders = 0;
    for (uint32_t s = 0; s < shard_count; s++) {
        if (table[s].entries & VKA_GPL_SHARD_DONE) {
            already_shaders += (table[s].entries & ~VKA_GPL_SHARD_DONE);
        } else if (shard_prio[s]) {
            shard_order[rem_shards++] = s;
        }
    }
    for (uint32_t s = 0; s < shard_count; s++) {
        if (!(table[s].entries & VKA_GPL_SHARD_DONE) && !shard_prio[s]) shard_order[rem_shards++] = s;
    }

    if (!rem_shards) {
        pthread_mutex_lock(&g_save_mu);
        vka_gpl_sharded_finalize(rw_fd, &g_gpl_hdr, table, shard_count, 1);
        pthread_mutex_unlock(&g_save_mu);
        VLOG("boot precompile: complete (%u / %u shaders finalized across %u shards; cache %.1f MB)",
             already_shaders, total, shard_count, append_off / (1024.0 * 1024.0));
        VLOG("boot precompile retype: no remaining shards, inferred vin 0, tex 0, fout 0; compiled 0, failed 0 (DM_VK_BOOT_PRECOMPILE_RETYPE)");
        close(rw_fd);
        free(slices); free(shard_indices); free(shard_order); free(shard_prio);
        return;
    }

    vka_aff_masks aff = vka_aff_topology();
    int bm = aff.replay ? __builtin_popcount(aff.replay) : (int)sysconf(_SC_NPROCESSORS_ONLN) / 2;
    int max_w = bm < 1 ? 1 : bm > 4 ? 4 : bm;
    if (VKA_ENV("DM_VK_RAM_SAVER", 0) >= 2 && max_w > 2) max_w = 2;
    if ((uint32_t)max_w > rem_shards) max_w = (int)rem_shards;

    uint64_t t0 = now_ns();
    atomic_store_explicit(&g_pre_t0_ns, t0, memory_order_relaxed);
    atomic_store_explicit(&g_pre_total, (int)total, memory_order_relaxed);
    atomic_store_explicit(&g_pre_compiled, (int)already_shaders, memory_order_relaxed);
    atomic_store_explicit(&g_pre_skip, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pre_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pre_checkpoint_want, 0, memory_order_relaxed);
    atomic_store_explicit(&g_pre_active_workers, max_w, memory_order_relaxed);
    atomic_store_explicit(&g_pre_active, 1, memory_order_release);

    VLOG("boot precompile: starting %u / %u shaders across %u remaining shards (%u total shards) on %d workers (gl_depth=%d, %s)",
         total - already_shaders, total, rem_shards, shard_count, max_w, !vka_depth_zero(), g_gpl_cache_path);

    precomp_ctx ctx = {
        .L = L, .slices = slices, .shard_indices = shard_indices, .shard_order = shard_order,
        .rem_shards = rem_shards, .shard_count = shard_count, .table = table,
        .rw_fd = rw_fd, .append_off = append_off, .max_workers = max_w, .gl_depth = !vka_depth_zero(),
    };
    atomic_init(&ctx.next_shard, 0);
    atomic_init(&ctx.last_check_ns, t0);
    atomic_init(&ctx.vs_n, 0); atomic_init(&ctx.fs_n, 0); atomic_init(&ctx.cs_n, 0);
    atomic_init(&ctx.skip_n, 0); atomic_init(&ctx.fail_n, 0);
    atomic_init(&ctx.rv_n, 0); atomic_init(&ctx.rt_n, 0); atomic_init(&ctx.rf_n, 0);
    atomic_init(&ctx.rok_n, 0); atomic_init(&ctx.rfail_n, 0);

    pthread_t th[4];
    precomp_arg args[4];
    int started = 0;
    for (int i = 0; i < max_w; i++) {
        args[i] = (precomp_arg){&ctx, i};
        if (!vka_compiler_thread(&th[started], precomp_worker, &args[i])) started++;
    }
    if (!started) {
        args[0] = (precomp_arg){&ctx, 0};
        precomp_worker(&args[0]);
    } else {
        for (int i = 0; i < started; i++) dm_host_thread_join(th[i], NULL);
    }

    int skipped_by_user = atomic_load_explicit(&g_pre_skip, memory_order_relaxed);
    int stopped_early = atomic_load_explicit(&g_pre_stop, memory_order_relaxed);
    uint32_t done_shards = 0, done_now = 0;
    for (uint32_t s = 0; s < shard_count; s++) {
        if (table[s].entries & VKA_GPL_SHARD_DONE) {
            done_shards++;
            done_now += (table[s].entries & ~VKA_GPL_SHARD_DONE);
        }
    }
    int complete = (done_shards == shard_count);

    pthread_mutex_lock(&g_save_mu);
    vka_gpl_sharded_finalize(rw_fd, &g_gpl_hdr, table, shard_count, complete);
    pthread_mutex_unlock(&g_save_mu);
    close(rw_fd);

    atomic_store_explicit(&g_pre_active, 0, memory_order_release);
    free(slices); free(shard_indices); free(shard_order); free(shard_prio);

    VLOG("boot precompile: %s (%u / %u shaders across %u / %u shards in %.1f s on %d workers; VS %u, FS %u, CS %u, skipped %u, failed %u; cache %.1f MB)",
         complete ? "complete" : skipped_by_user ? "skipped by user" : stopped_early ? "stopped early (memory/maps guard)" : "partial",
         done_now, total, done_shards, shard_count, (now_ns() - t0) / 1e9, started ? started : 1,
         atomic_load(&ctx.vs_n), atomic_load(&ctx.fs_n), atomic_load(&ctx.cs_n),
         atomic_load(&ctx.skip_n), atomic_load(&ctx.fail_n), ctx.append_off / (1024.0 * 1024.0));
    VLOG("boot precompile retype: inferred vin %u, tex %u, fout %u; compiled %u, failed %u (DM_VK_BOOT_PRECOMPILE_RETYPE)",
         atomic_load(&ctx.rv_n), atomic_load(&ctx.rt_n), atomic_load(&ctx.rf_n),
         atomic_load(&ctx.rok_n), atomic_load(&ctx.rfail_n));
}

typedef struct {
    const vka_shlib *L;
    const vka_svar_rec *recs;
    uint32_t count;
    int max_workers;
    int can_probe;
    _Atomic uint32_t next;
    _Atomic uint64_t t0_ns;
    _Atomic uint64_t last_check_ns;
    _Atomic int active_workers;
    _Atomic int stop;
    _Atomic uint32_t cached_n, compiled_n, fail_n;
} svar_pre_ctx;

typedef struct {
    svar_pre_ctx *ctx;
    int wid;
} svar_pre_arg;

static void svar_flush_batch(VkPipelineCache *local_pc, VkPipeline *pipes, int *npipes) {
    if (!*npipes) return;
    pthread_mutex_lock(&g_merge_mu);
    if (*local_pc && g_pc_retype && vk.vkMergePipelineCaches) {
        VkResult mr = vk.vkMergePipelineCaches(V.dev, g_pc_retype, 1, local_pc);
        if (mr) { static _Atomic int w; if (atomic_fetch_add(&w, 1) < 4) VRED("variant manifest: vkMergePipelineCaches failed: %d", (int)mr); }
    }
    for (int k = 0; k < *npipes; k++) if (pipes[k]) vk.vkDestroyPipeline(V.dev, pipes[k], NULL);
    *npipes = 0;
    if (*local_pc && vk.vkDestroyPipelineCache) {
        vk.vkDestroyPipelineCache(V.dev, *local_pc, NULL);
        *local_pc = VK_NULL_HANDLE;
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (vk.vkCreatePipelineCache(V.dev, &ci, NULL, local_pc)) *local_pc = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&g_merge_mu);
}

static void svar_compile_one(const vka_shlib *L, const vka_svar_rec *rec, VkPipelineCache local_pc, int can_probe,
                             VkPipeline *pipes, int *npipes, int max_pipes, uint32_t *cached_n, uint32_t *compiled_n, uint32_t *fail_n) {
    const dmsh_vk_entry *e = vka_shlib_find(L, rec->shader_hash);
    if (!e || e->stage != rec->stage) { (*fail_n)++; return; }
    VkShaderModule base_mod = VK_NULL_HANDLE;
    VkShaderModule mod = vka_shader_precomp_variant(L, e, rec, &base_mod);
    if (!mod) { (*fail_n)++; return; }

    vka_shader tmp = {0};
    tmp.key = e->key; tmp.e = e; tmp.b = vka_shader_precomp_binds(L, e); tmp.lib = L;
    if (rec->stage == DMSH_VK_STAGE_VS && (e->flags & DMSH_VK_F_TF) && e->shared && vka_shlib_module(L, e, DMSH_VK_MOD_TF) && VKA_ENV("DM_VK_TF", 1)) tmp.tf_fx = 1;
    if (rec->stage == DMSH_VK_STAGE_GS && (rec->flags & VKA_SVAR_F_GS_CLAIM)) tmp.gs_fx = 1;

    VkPipelineCreateFlags lto_flag = V.gpl_lto ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0;
    VkPipelineRenderingCreateInfo ri0 = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    uint32_t pin = (e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(rec->stage) : 0;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};

    if (rec->stage == DMSH_VK_STAGE_CS) {
        uint32_t smax = V.props.limits.maxComputeSharedMemorySize;
        if (smax && e->shared > smax && !VKA_ENV("DM_VK_CS_SHARED_OVER", 0)) { (*fail_n)++; goto done; }
        vka_shader *sh[VKA_NSTAGE] = {0}; sh[5] = &tmp;
        vka_layout lay;
        if (!layout_slow(sh, 1, &lay)) { (*fail_n)++; goto done; }
        int spill = 0;
        for (unsigned i = 0; i < e->bind_count; i++) if (tmp.b && tmp.b[i].kind == DMSH_VK_KIND_SHARED_SPILL) spill = 1;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .flags = spill ? VK_PIPELINE_CREATE_DISPATCH_BASE_BIT : 0, .layout = lay.layout,
            .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}};
        if (can_probe) {
            ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            VkPipeline p = VK_NULL_HANDLE;
            if (vk.vkCreateComputePipelines(V.dev, g_pc_retype, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
                vk.vkDestroyPipeline(V.dev, p, NULL);
                (*cached_n)++;
                goto done;
            }
            ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        }
        VkPipeline p = VK_NULL_HANDLE;
        if (vk.vkCreateComputePipelines(V.dev, local_pc ? local_pc : g_pc_retype, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
            (*compiled_n)++;
            if (*npipes < max_pipes) pipes[(*npipes)++] = p; else vk.vkDestroyPipeline(V.dev, p, NULL);
        } else (*fail_n)++;
    } else if (rec->stage == DMSH_VK_STAGE_FS) {
        vka_layout lay;
        if (!vka_layout_fs_of(&tmp, &lay)) { (*fail_n)++; goto done; }
        VkDynamicState dl[40]; VkPipelineDynamicStateCreateInfo dy;
        precomp_fill_dyn(dl, &dy, L_FS);
        uint32_t afunc = rec->alpha_func;
        VkSpecializationMapEntry ae = {DMSH_VK_SPEC_ALPHA_FUNC, 0, sizeof afunc};
        VkSpecializationInfo aspec = {1, &ae, sizeof afunc, &afunc};
        int use_afunc = afunc != 0 && (e->flags & DMSH_VK_F_ALPHA_TEST);
        VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = &ri0, .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT};
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = (VkSampleCountFlagBits)(rec->samples ? rec->samples : VK_SAMPLE_COUNT_1_BIT),
            .alphaToCoverageEnable = (rec->flags & VKA_SVAR_F_A2C) ? VK_TRUE : VK_FALSE};
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .maxDepthBounds = 1.0f};
        VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = mod, .pName = "main", .pSpecializationInfo = use_afunc ? &aspec : NULL};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
            .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | lto_flag, .stageCount = 1, .pStages = &st,
            .pMultisampleState = &ms, .pDepthStencilState = &ds, .pDynamicState = &dy, .layout = lay.fs_layout};
        if (can_probe) {
            ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            VkPipeline p = VK_NULL_HANDLE;
            if (vk.vkCreateGraphicsPipelines(V.dev, g_pc_retype, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
                vk.vkDestroyPipeline(V.dev, p, NULL);
                (*cached_n)++;
                goto done;
            }
            ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        }
        VkPipeline p = VK_NULL_HANDLE;
        if (vk.vkCreateGraphicsPipelines(V.dev, local_pc ? local_pc : g_pc_retype, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
            (*compiled_n)++;
            if (*npipes < max_pipes) pipes[(*npipes)++] = p; else vk.vkDestroyPipeline(V.dev, p, NULL);
        } else (*fail_n)++;
    } else {
        int st_idx = vka_nvn_stage(rec->stage);
        if (st_idx < 0 || st_idx == 1 || st_idx >= 5) { (*fail_n)++; goto done; }
        vka_shader *sh[VKA_NSTAGE] = {0}; sh[st_idx] = &tmp;
        vka_layout lay;
        if (!vka_layout_pre_of(sh, &lay)) { (*fail_n)++; goto done; }
        VkDynamicState dl[40]; VkPipelineDynamicStateCreateInfo dy;
        precomp_fill_dyn(dl, &dy, L_PRE);
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1.0f};
        VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, .patchControlPoints = 3};
        int tess = (st_idx == 3 || st_idx == 4);
        VkBool32 gl_true = VK_TRUE;
        VkSpecializationMapEntry me = {DMSH_VK_SPEC_GL_DEPTH, 0, sizeof gl_true};
        VkSpecializationInfo spec = {1, &me, sizeof gl_true, &gl_true};
        int use_gl = (rec->flags & VKA_SVAR_F_GL_DEPTH) && (e->flags & DMSH_VK_F_GL_DEPTH);
        VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = &ri0, .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT};
        VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL,
            .stage = (VkShaderStageFlagBits)stage_bit(rec->stage), .module = mod, .pName = "main", .pSpecializationInfo = use_gl ? &spec : NULL};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
            .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | lto_flag, .stageCount = 1, .pStages = &st,
            .pTessellationState = tess ? &ts : NULL, .pViewportState = &vp, .pRasterizationState = &rs, .pDynamicState = &dy, .layout = lay.pre_layout};
        if (can_probe) {
            ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            VkPipeline p = VK_NULL_HANDLE;
            if (vk.vkCreateGraphicsPipelines(V.dev, g_pc_retype, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
                vk.vkDestroyPipeline(V.dev, p, NULL);
                (*cached_n)++;
                goto done;
            }
            ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        }
        VkPipeline p = VK_NULL_HANDLE;
        if (vk.vkCreateGraphicsPipelines(V.dev, local_pc ? local_pc : g_pc_retype, 1, &ci, NULL, &p) == VK_SUCCESS && p) {
            (*compiled_n)++;
            if (*npipes < max_pipes) pipes[(*npipes)++] = p; else vk.vkDestroyPipeline(V.dev, p, NULL);
        } else (*fail_n)++;
    }
done:
    if (base_mod) vk.vkDestroyShaderModule(V.dev, base_mod, NULL);
}

static void *svar_pre_worker(void *arg_) {
    svar_pre_arg *pa = (svar_pre_arg *)arg_;
    svar_pre_ctx *ctx = pa->ctx;
    int wid = pa->wid;
    char tname[16]; snprintf(tname, sizeof tname, "vk-svar-%d", wid);
    pthread_setname_np(pthread_self(), tname);
    vka_fp_host(tname);
    setpriority(PRIO_PROCESS, 0, 5);

    VkPipelineCache local_pc = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (vk.vkCreatePipelineCache(V.dev, &pci, NULL, &local_pc)) local_pc = VK_NULL_HANDLE;

    enum { BATCH_PIPES = 100 };
    VkPipeline pipes[BATCH_PIPES];
    int npipes = 0;
    uint32_t c_cached = 0, c_compiled = 0, c_fail = 0;

    for (;;) {
        if (atomic_load_explicit(&ctx->stop, memory_order_acquire) ||
            atomic_load_explicit(&ctx->next, memory_order_acquire) >= ctx->count) break;
        uint64_t now = now_ns();
        uint64_t prev = atomic_load_explicit(&ctx->last_check_ns, memory_order_relaxed);
        if (now - prev >= 500000000ull && atomic_compare_exchange_strong(&ctx->last_check_ns, &prev, now)) {
            int mx = 0, maps = plat_maps_count(&mx), abort_early = 0;
            uint64_t av = mem_avail_mb();
            double elapsed_s = (now - atomic_load_explicit(&ctx->t0_ns, memory_order_relaxed)) / 1e9;
            int tgt = vka_precompile_target_workers(ctx->max_workers, elapsed_s, av, maps, mx, &abort_early);
            if (abort_early) atomic_store_explicit(&ctx->stop, 1, memory_order_release);
            else atomic_store_explicit(&ctx->active_workers, tgt, memory_order_release);
        }
        while (wid >= atomic_load_explicit(&ctx->active_workers, memory_order_acquire) &&
               !atomic_load_explicit(&ctx->stop, memory_order_acquire) &&
               atomic_load_explicit(&ctx->next, memory_order_acquire) < ctx->count) {
            usleep(50000);
        }
        if (atomic_load_explicit(&ctx->stop, memory_order_acquire)) break;
        uint32_t pos = atomic_fetch_add_explicit(&ctx->next, 1, memory_order_relaxed);
        if (pos >= ctx->count) break;
        svar_compile_one(ctx->L, &ctx->recs[pos], local_pc, ctx->can_probe, pipes, &npipes, BATCH_PIPES, &c_cached, &c_compiled, &c_fail);
        if (npipes >= BATCH_PIPES) svar_flush_batch(&local_pc, pipes, &npipes);
    }
    svar_flush_batch(&local_pc, pipes, &npipes);
    if (local_pc && vk.vkDestroyPipelineCache) vk.vkDestroyPipelineCache(V.dev, local_pc, NULL);
    atomic_fetch_add_explicit(&ctx->cached_n, c_cached, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->compiled_n, c_compiled, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctx->fail_n, c_fail, memory_order_relaxed);
    return NULL;
}

static void vka_retype_variants_startup(const vka_shlib *L, const char *dir, uint64_t lib_hash, int gl_depth) {
    if (!L || !dir || !*dir) return;
    if (vka_svar_manifest_filename(g_svar_path, sizeof g_svar_path, dir, DM_GAME_ID, lib_hash) < (int)sizeof g_svar_path) {
        const char *bn = strrchr(g_svar_path, '/');
        int pruned = vka_svar_manifest_prune_stale(dir, DM_GAME_ID, bn ? bn + 1 : g_svar_path);
        if (pruned > 0) VLOG("variant manifest: pruned %d stale manifest(s) for %s", pruned, DM_GAME_ID);
        const char *why_sv = NULL;
        if (!vka_svar_manifest_load(g_svar_path, lib_hash, &g_svar, &why_sv) && why_sv && strcmp(why_sv, "missing") != 0) {
            unlink(g_svar_path);
            VLOG("variant manifest %s: discarded (%s)", g_svar_path, why_sv);
        }
        atomic_store_explicit(&g_svar_loaded_n, g_svar.n, memory_order_relaxed);
        atomic_store_explicit(&g_svar_saved_n, g_svar.n, memory_order_relaxed);
    }
    precomp_pw_variants(L);
    if (vka_gpl_retype_cache_filename(g_retype_cache_path, sizeof g_retype_cache_path, dir, DM_GAME_ID, lib_hash, V.props.pipelineCacheUUID) < (int)sizeof g_retype_cache_path) {
        const char *bn = strrchr(g_retype_cache_path, '/');
        int pruned = vka_gpl_retype_cache_prune_stale(dir, DM_GAME_ID, bn ? bn + 1 : g_retype_cache_path);
        if (pruned > 0) VLOG("delta retype cache: pruned %d stale cache(s) for %s", pruned, DM_GAME_ID);
        heavy_load();
        memset(&g_retype_hdr, 0, sizeof g_retype_hdr);
        g_retype_hdr.vendor_id = V.props.vendorID;
        g_retype_hdr.device_id = V.props.deviceID;
        g_retype_hdr.driver_version = V.props.driverVersion;
        g_retype_hdr.flags = VKA_GPL_CACHE_F_DELTA | (gl_depth ? VKA_GPL_CACHE_F_GL_DEPTH : 0);
        memcpy(g_retype_hdr.pipeline_cache_uuid, V.props.pipelineCacheUUID, VK_UUID_SIZE);
        g_retype_hdr.dmsh_vk_lib_hash = lib_hash;

        vka_gpl_cache_hdr rhdr; void *rmap = NULL; size_t rmap_sz = 0; const void *rblob = NULL; const char *why_r = NULL;
        if (vka_gpl_aux_cache_load_mmap(g_retype_cache_path, V.props.vendorID, V.props.deviceID, V.props.driverVersion,
                                        V.props.pipelineCacheUUID, lib_hash, gl_depth, VKA_GPL_CACHE_F_DELTA,
                                        &rhdr, &rmap, &rmap_sz, &rblob, &why_r)) {
            VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = rhdr.vk_cache_bytes, .pInitialData = rblob};
            VkResult r = vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc_retype);
            munmap(rmap, rmap_sz);
            if (r == VK_SUCCESS && g_pc_retype) {
                g_retype_hdr = rhdr;
                g_retype_cache_loaded = 1;
                atomic_store_explicit(&g_retype_cache_bytes, rhdr.vk_cache_bytes, memory_order_relaxed);
                VLOG("delta retype cache %s: loaded (%u variants, %u bytes)", g_retype_cache_path, rhdr.compiled_entries, rhdr.vk_cache_bytes);
            } else {
                g_pc_retype = VK_NULL_HANDLE;
                VLOG("delta retype cache %s: vkCreatePipelineCache failed (%d), starting fresh delta cache", g_retype_cache_path, (int)r);
            }
        } else if (why_r && strcmp(why_r, "missing") != 0) {
            unlink(g_retype_cache_path);
            VLOG("delta retype cache %s: discarded (%s), starting fresh delta cache", g_retype_cache_path, why_r);
        }
        if (!g_pc_retype) {
            VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            if (vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc_retype) != VK_SUCCESS) g_pc_retype = VK_NULL_HANDLE;
        }
    }
    if (!g_pc_retype || !g_svar.n) return;

    uint64_t t0 = now_ns();
    vka_aff_masks aff = vka_aff_topology();
    int bm = aff.replay ? __builtin_popcount(aff.replay) : (int)sysconf(_SC_NPROCESSORS_ONLN) / 2;
    int max_w = bm < 1 ? 1 : bm > 4 ? 4 : bm;
    if (VKA_ENV("DM_VK_RAM_SAVER", 0) >= 2 && max_w > 2) max_w = 2;
    if ((uint32_t)max_w > g_svar.n) max_w = (int)g_svar.n;

    svar_pre_ctx ctx = {.L = L, .recs = g_svar.recs, .count = g_svar.n, .max_workers = max_w, .can_probe = g_retype_cache_loaded && V.cache_ctl};
    atomic_init(&ctx.next, 0);
    atomic_init(&ctx.t0_ns, t0);
    atomic_init(&ctx.last_check_ns, t0);
    atomic_init(&ctx.active_workers, max_w);
    atomic_init(&ctx.stop, 0);
    atomic_init(&ctx.cached_n, 0);
    atomic_init(&ctx.compiled_n, 0);
    atomic_init(&ctx.fail_n, 0);

    pthread_t th[4];
    svar_pre_arg args[4];
    int started = 0;
    for (int i = 0; i < max_w; i++) {
        args[i] = (svar_pre_arg){&ctx, i};
        if (!vka_compiler_thread(&th[started], svar_pre_worker, &args[i])) started++;
    }
    if (!started) {
        args[0] = (svar_pre_arg){&ctx, 0};
        svar_pre_worker(&args[0]);
    } else {
        for (int i = 0; i < started; i++) dm_host_thread_join(th[i], NULL);
    }

    uint32_t cached = atomic_load(&ctx.cached_n), compiled = atomic_load(&ctx.compiled_n), failed = atomic_load(&ctx.fail_n);
    atomic_store_explicit(&g_svar_precompiled_n, (uint64_t)(cached + compiled), memory_order_relaxed);
    if (compiled > 0 || atomic_load_explicit(&g_svar_dirty, memory_order_relaxed)) {
        if (compiled > 0) atomic_store_explicit(&g_retype_dirty, 1, memory_order_relaxed);
        pthread_mutex_lock(&g_save_mu);
        svar_and_retype_save_locked();
        pthread_mutex_unlock(&g_save_mu);
    }
    VLOG("variant manifest: loaded %u, precompiled %u (%u cached, %u newly compiled, %u failed) in %.1f ms; delta retype cache %llu bytes",
         g_svar.n, cached + compiled, cached, compiled, failed, (now_ns() - t0) / 1e6,
         (unsigned long long)atomic_load_explicit(&g_retype_cache_bytes, memory_order_relaxed));
}
static void mono_cache_startup(const char *dir, uint64_t lib_hash, int gl_depth) {
    if (!dir || !*dir) return;
    if (vka_gpl_mono_cache_filename(g_mono_cache_path, sizeof g_mono_cache_path, dir, DM_GAME_ID, lib_hash, V.props.pipelineCacheUUID) >= (int)sizeof g_mono_cache_path) {
        g_mono_cache_path[0] = 0;
        return;
    }
    const char *bn = strrchr(g_mono_cache_path, '/');
    int pruned = vka_gpl_mono_cache_prune_stale(dir, DM_GAME_ID, bn ? bn + 1 : g_mono_cache_path);
    if (pruned > 0) VLOG("monolithic cache: pruned %d stale cache(s) for %s", pruned, DM_GAME_ID);
    memset(&g_mono_hdr, 0, sizeof g_mono_hdr);
    g_mono_hdr.vendor_id = V.props.vendorID;
    g_mono_hdr.device_id = V.props.deviceID;
    g_mono_hdr.driver_version = V.props.driverVersion;
    g_mono_hdr.flags = VKA_GPL_CACHE_F_MONO | (gl_depth ? VKA_GPL_CACHE_F_GL_DEPTH : 0);
    memcpy(g_mono_hdr.pipeline_cache_uuid, V.props.pipelineCacheUUID, VK_UUID_SIZE);
    g_mono_hdr.dmsh_vk_lib_hash = lib_hash;

    vka_gpl_cache_hdr mhdr; void *mmap_base = NULL; size_t mmap_sz = 0; const void *mblob = NULL; const char *why_m = NULL;
    if (vka_gpl_mono_cache_load_mmap(g_mono_cache_path, V.props.vendorID, V.props.deviceID, V.props.driverVersion,
                                     V.props.pipelineCacheUUID, lib_hash, gl_depth, &mhdr, &mmap_base, &mmap_sz, &mblob, &why_m)) {
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = mhdr.vk_cache_bytes, .pInitialData = mblob};
        VkResult r = vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc_mono);
        munmap(mmap_base, mmap_sz);
        if (r == VK_SUCCESS && g_pc_mono) {
            g_mono_hdr = mhdr;
            g_mono_cache_loaded = 1;
            atomic_store_explicit(&g_mono_cache_bytes, mhdr.vk_cache_bytes, memory_order_relaxed);
            VLOG("monolithic cache %s: loaded (%u pipelines, %u bytes)", g_mono_cache_path, mhdr.compiled_entries, mhdr.vk_cache_bytes);
        } else {
            g_pc_mono = VK_NULL_HANDLE;
            VLOG("monolithic cache %s: vkCreatePipelineCache failed (%d), starting fresh", g_mono_cache_path, (int)r);
        }
    } else if (why_m && strcmp(why_m, "missing") != 0) {
        unlink(g_mono_cache_path);
        VLOG("monolithic cache %s: discarded (%s), starting fresh", g_mono_cache_path, why_m);
    } else {
        VLOG("monolithic cache %s: none yet (starting fresh)", g_mono_cache_path);
    }
    if (!g_pc_mono) {
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc_mono) != VK_SUCCESS) g_pc_mono = VK_NULL_HANDLE;
    }
}
// One try of the probe: the library made (and destroyed), its result
static VkResult try_lib(VkGraphicsPipelineCreateInfo ci, VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineCache pc, VkPipelineCreateFlags extra) {
    VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = ci.pNext, .flags = part};
    ci.pNext = &li; ci.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | extra;
    int bs = build_begin("linked graphics pipeline", 0, 0);
    VkPipeline p = VK_NULL_HANDLE; VkResult r = vk.vkCreateGraphicsPipelines(V.dev, pc, 1, &ci, NULL, &p);
    build_end(bs);
    if (!r && p) vk.vkDestroyPipeline(V.dev, p, NULL);
    return r;
}
// The probe at the first refused pre-rasterization library (once a run): the same library with one thing changed at a time, then all the simplifications together. Logged as one
// line; returns the adaptation to use from now on (0 = none of the adoptable ones made it)
static unsigned pre_probe(const VkGraphicsPipelineCreateInfo *ci0, const pstate *P, const VkPipelineShaderStageCreateInfo *pre, uint32_t npre, const VkPipelineShaderStageCreateInfo *fs, VkResult first) {
    char line[900]; size_t o = 0; line[0] = 0;
#define NOTE(name, r) do { if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, "%s%s %s", o ? ", " : "", name, (r) ? "refused" : "made"); \
                           if ((r) && o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, " (%d)", (int)(r)); } while (0)
    VkPipelineCreateFlags RET = VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    VkResult r_same = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("again as made", r_same);
    VkResult r_nc = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, VK_NULL_HANDLE, 0); NOTE("no pipeline cache", r_nc);
    VkResult r_ret = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, RET); NOTE("RETAIN_LINK_TIME_OPTIMIZATION_INFO", r_ret);
    VkGraphicsPipelineCreateInfo c = *ci0; c.pNext = NULL;
    VkResult r_nori = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no VkPipelineRenderingCreateInfo", r_nori);
    VkViewport vp0 = {0, 0, 64, 64, 0, 1}; VkRect2D sc0 = {{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo vps = P->vp; vps.pViewports = &vp0; vps.pScissors = &sc0;
    c = *ci0; c.pViewportState = &vps; c.pDynamicState = NULL;
    VkResult r_nodyn = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no dynamic state", r_nodyn);
    VkPipelineShaderStageCreateInfo st[4]; int pinned = 0;
    for (uint32_t i = 0; i < npre; i++) { st[i] = pre[i]; st[i].pSpecializationInfo = NULL; }
    c = *ci0; c.pStages = st;
    VkResult r_nospec = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no specialization constants", r_nospec);
    for (uint32_t i = 0; i < npre; i++) { st[i] = pre[i]; if (st[i].pNext) pinned = 1; st[i].pNext = NULL; }
    if (pinned) { VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no subgroup size pins", r); }
    if (npre > 1) { c = *ci0; c.stageCount = 1; c.pStages = pre; c.pTessellationState = NULL; VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("first stage only", r); }
    {   // The pipeline layout with independent sets (diagnostic: a pipeline of such a layout binds only with it, not adopted)
        VkDescriptorSetLayout sets[3] = {P->set0, vka_bindless_layout(), vka_pfb_layout()}; VkPipelineLayout il = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = !sets[1] ? 1 : sets[2] ? 3 : 2, .pSetLayouts = sets};
        if (P->set0 && !vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &il)) { c = *ci0; c.layout = il; VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("independent-sets layout", r); vk.vkDestroyPipelineLayout(V.dev, il, NULL); }
    }
    VkResult r_comb = VK_ERROR_UNKNOWN;
    {   // Pre-rasterization and the fragment shader in one library
        VkPipelineShaderStageCreateInfo both[5]; uint32_t nb = 0; for (uint32_t i = 0; i < npre; i++) both[nb++] = pre[i]; if (fs) both[nb++] = *fs;
        VkDynamicState dl[40]; VkPipelineDynamicStateCreateInfo dy; dyn_of(P, L_PREFS, dl, &dy);
        c = *ci0; c.stageCount = nb; c.pStages = both; c.pMultisampleState = &P->ms; c.pDepthStencilState = &P->ds; c.pDynamicState = &dy;
        r_comb = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, g_pc, 0); NOTE("with the fragment shader", r_comb);
        c.pDynamicState = NULL; c.pViewportState = &vps;
        VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, g_pc, 0); NOTE("with the fragment shader, no dynamic state", r);
    }
    {   // Everything at once: no cache, no rendering info, no dynamic state, no specialization constants, no pins, the first stage only
        VkPipelineShaderStageCreateInfo s1 = pre[0]; s1.pSpecializationInfo = NULL; s1.pNext = NULL;
        c = *ci0; c.pNext = NULL; c.pViewportState = &vps; c.pDynamicState = NULL; c.stageCount = 1; c.pStages = &s1; c.pTessellationState = NULL;
        VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, VK_NULL_HANDLE, RET); NOTE("all simplified", r);
    }
#undef NOTE
    unsigned a = !r_same ? 0 : !r_nc ? A_NOCACHE : !r_ret ? A_RETAIN : !r_nori ? A_NORI : !r_comb ? A_COMBINED : 0;
    VLOG("graphics pipeline library probe (pre-rasterization refused with %d; vertex %s, %u stage%s): %s; %s", first, npre ? stage_name(pre[0].stage) : "-", npre, npre == 1 ? "" : "s", line,
         !r_same ? "the second try made it (a transient refusal)" : a == A_NOCACHE ? "adopted: libraries without the pipeline cache" : a == A_RETAIN ? "adopted: RETAIN_LINK_TIME_OPTIMIZATION_INFO"
         : a == A_NORI ? "adopted: no VkPipelineRenderingCreateInfo" : a == A_COMBINED ? "adopted: one library for pre-rasterization and the fragment shader" : "nothing adoptable");
    return a;
}
// The parts of P, each made once; then the link. VK_NULL_HANDLE = a part failed (or missed the cache when cache_only != 0)
static VkPipeline gpl_link(pstate *P, const vka_gkey *k, int cache_only) {
    const vka_shader *pre0 = P->mesh ? P->sh[7] : P->sh[0];
    uint64_t vs_key = pre0 ? pre0->key : 0;
    uint64_t fs_key = P->sh[1] ? P->sh[1]->key : 0;
    VkDynamicState dl[NLIB][40]; VkPipelineDynamicStateCreateInfo dy[NLIB];
    for (int i = 0; i < NLIB; i++) dyn_of(P, i, dl[i], &dy[i]);
    VkPipeline part[4]; uint32_t nparts = 0;
    {   vikey key; memset(&key, 0, sizeof key); key.topo = ((active_eds_mask(P->mesh) & VKA_EDS_TOPO_UNRESTRICTED) && !P->tess) ? 0 : k->topo; key.vin_static = k->vin_static;
        if (k->vin_static) { memcpy(key.va, k->va, sizeof key.va); memcpy(key.vb, k->vb, sizeof key.vb); }
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pVertexInputState = &P->vi, .pInputAssemblyState = &P->ia, .pDynamicState = &dy[L_VI]};
        if (!(part[nparts++] = lib_make(L_VI, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, NULL, 0, 0, cache_only, VK_NULL_HANDLE, NULL, NULL))) return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo pre[4], *fs = NULL; uint32_t npre = 0;
    for (uint32_t i = 0; i < P->ns; i++) { if (P->ss[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = &P->ss[i]; else if (npre < 4) pre[npre++] = P->ss[i]; }
    VkPipelineRenderingCreateInfo ri0 = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};   // the shader parts take only the view mask (0); the formats are the output's
    prekey pk; memset(&pk, 0, sizeof pk);
    for (uint32_t i = 0; i < npre; i++) { pk.mod[i] = pre[i].module; pk.stage[i] = pre[i].stage; pk.gl[i] = pre[i].pSpecializationInfo != NULL;
                                          pk.pin[i] = pre[i].pNext ? ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)pre[i].pNext)->requiredSubgroupSize : 0; }
    VkRenderPass rp = P->ci.renderPass;   // (VK_NULL_HANDLE with dynamic rendering; pstate_fill sets it for classic render passes)
    VkPipelineLayout pre_lay = P->indep ? P->pre_layout : P->ci.layout;
    VkPipelineLayout fs_lay = P->indep ? P->fs_layout : P->ci.layout;
    pk.layout = pre_lay; pk.rp = rp; pk.depth_clamp = (uint8_t)P->rs.depthClampEnable; pk.poly_mode = (uint8_t)P->rs.polygonMode; pk.tess = (uint8_t)P->tess; pk.vin_static = (uint8_t)k->vin_static;
    fskey fk; memset(&fk, 0, sizeof fk);
    if (fs) { fk.mod = fs->module; fk.afunc = fs->pSpecializationInfo ? P->afunc : 0; fk.pin = fs->pNext ? ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)fs->pNext)->requiredSubgroupSize : 0; }
    fk.layout = fs_lay; fk.rp = rp; fk.samples = (uint8_t)P->ms.rasterizationSamples; fk.a2c = (uint8_t)P->ms.alphaToCoverageEnable; fk.vin_static = (uint8_t)k->vin_static;
    int pre_retyped = vka_svar_is_pre_retyped(P->mesh, k->vin_u, k->vin_s, k->tex_u, k->tex_s);
    int fs_retyped = vka_svar_is_fs_retyped(P->sh[1] != NULL, k->fs_variant == VKA_FS_RETYPE, P->fu, P->fsm, k->tex_u, k->tex_s, fk.afunc);
    VkPipelineCache pre_pc = (g_pc_retype && pre_retyped) ? g_pc_retype : VK_NULL_HANDLE;
    VkPipelineCache fs_pc = (g_pc_retype && fs_retyped) ? g_pc_retype : VK_NULL_HANDLE;
    int pre_from_shard = 0, fs_from_shard = 0;   // a Boot #1 mini-shard hit needs no svar entry
    int pre_missed = 0, fs_missed = 0;           // bounded miss diagnostics below
    if (g_pre_adapt & A_COMBINED) {
        prefskey key; memset(&key, 0, sizeof key); key.pre = pk; key.fs = fk;
        VkPipelineShaderStageCreateInfo both[5]; uint32_t nb = 0; for (uint32_t i = 0; i < npre; i++) both[nb++] = pre[i]; if (fs) both[nb++] = *fs;
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = nb, .pStages = both, .pTessellationState = P->tess ? &P->ts : NULL,
                                           .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pMultisampleState = &P->ms, .pDepthStencilState = &P->ds, .pDynamicState = &dy[L_PREFS], .layout = P->ci.layout,
                                           .renderPass = rp};
        VkPipelineCache comb_pc = (g_pc_retype && (pre_retyped || fs_retyped)) ? g_pc_retype : VK_NULL_HANDLE;
        if (!(part[nparts++] = lib_make(L_PREFS, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, NULL, vs_key, fs_key, cache_only, comb_pc, NULL, NULL))) return VK_NULL_HANDLE;
        if (!cache_only && !pre_from_shard && pre_retyped && npre == 1 && P->sh[0] && ((k->vin_u | k->vin_s) != 0 || (k->tex_u[DMSH_VK_STAGE_VS] | k->tex_s[DMSH_VK_STAGE_VS]) != 0)) {
            vka_svar_rec vr = {0};
            vr.shader_hash = P->sh[0]->key;
            vr.stage = DMSH_VK_STAGE_VS;
            vr.lib_variant = (k->gs_fx == 2 && P->sh[0]->tf_fx) ? DMSH_VK_MOD_TF : DMSH_VK_MOD_BASE;
            vr.vin_u = k->vin_u; vr.vin_s = k->vin_s;
            vr.tex_u = k->tex_u[DMSH_VK_STAGE_VS]; vr.tex_s = k->tex_s[DMSH_VK_STAGE_VS];
            vr.flags = pk.gl[0] ? VKA_SVAR_F_GL_DEPTH : 0;
            svar_record(&vr);
        }
        if (!cache_only && !fs_from_shard && fs_retyped) {
            vka_svar_rec vr = {0};
            vr.shader_hash = P->sh[1]->key;
            vr.stage = DMSH_VK_STAGE_FS;
            vr.lib_variant = (uint8_t)(k->fs_variant == VKA_FS_RETYPE ? DMSH_VK_MOD_BASE : k->fs_variant);
            vr.fout_u = P->fu; vr.fout_s = P->fsm;
            vr.tex_u = k->tex_u[DMSH_VK_STAGE_FS]; vr.tex_s = k->tex_s[DMSH_VK_STAGE_FS];
            vr.alpha_func = (uint8_t)fk.afunc;
            vr.samples = fk.samples;
            vr.flags = (P->pfb_late ? VKA_SVAR_F_PFB_LATE : 0) | (fk.a2c ? VKA_SVAR_F_A2C : 0);
            svar_record(&vr);
        }
    } else {
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = npre, .pStages = pre, .pTessellationState = P->tess ? &P->ts : NULL,
                                           .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pDynamicState = &dy[L_PRE], .layout = pre_lay, .renderPass = rp};
        int refused = 0;
        VkGraphicsPipelineCreateInfo ci_probe = ci;   // (lib_make chains its own structure onto ci and restores it)
        if (!(part[nparts++] = lib_make(L_PRE, &pk, sizeof pk, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, &refused, vs_key, 0, cache_only, pre_pc, &pre_from_shard, &pre_missed))) {
            if (!cache_only && refused && !__atomic_load_n(&g_nlib[L_PRE], __ATOMIC_RELAXED)) {
                int first = 0;
                pthread_mutex_lock(&g_mu); int nref = ++g_pre_refused; if (!g_pre_probed) { g_pre_probed = 1; first = 1; } pthread_mutex_unlock(&g_mu);
                // DM_VK_GPL_PROBE=1 (default 0 since 2026-11-03): the probe makes ~11-13 libraries synchronously on the replay thread, a 1-6 s hitch in the middle of a frame at
                // every run's first refusal; it is a one-off diagnostic run (android/STUTTER-TEST.md step 1), not something every run should pay for
                if (first && VKA_ENV("DM_VK_GPL_PROBE", 0)) {
                    unsigned a = pre_probe(&ci_probe, P, pre, npre, fs, (VkResult)refused);
                    if (a) { pthread_mutex_lock(&g_mu); g_pre_adapt = a; for (int i = L_PRE; i < NLIB; i += L_PREFS - L_PRE) { free(g_lib[i].e); g_lib[i].e = NULL; g_lib[i].cap = g_lib[i].n = 0; } pthread_mutex_unlock(&g_mu); }   // (the refused pre-rasterization entries: tried again with the adaptation)
                }
                int giveup = VKA_ENV("DM_VK_GPL_GIVEUP", 3);
                if (!g_pre_adapt && giveup > 0 && nref >= giveup && !atomic_exchange(&g_gpl_off, 1))
                    VLOG("graphics pipeline libraries: off for this run: %d pre-rasterization libraries refused and none made (the probe found nothing to adopt); a pipeline the cache lacks is made whole (DM_VK_GPL_GIVEUP=0 keeps trying)", nref);
            }
            return VK_NULL_HANDLE;
        }
        if (!cache_only && pre_missed && P->sh[0] &&
            atomic_fetch_add_explicit(&g_retype_miss_logged, 1, memory_order_relaxed) < 12) {
            vka_spv_retype_hint inf; int got = vka_shader_infer_hint(P->sh[0], &inf);
            VLOG("retype shard miss pre %016llx: runtime vin %#x/%#x tex %#llx/%#llx fout %#x/%#x; inferred %d vin %#x/%#x tex %#llx/%#llx; samples %u a2c %u afunc %u pfb_late %u fs_variant %u gl %u lto %d adapt %#x",
                 (unsigned long long)P->sh[0]->key, k->vin_u, k->vin_s,
                 (unsigned long long)k->tex_u[DMSH_VK_STAGE_VS], (unsigned long long)k->tex_s[DMSH_VK_STAGE_VS], P->fu, P->fsm,
                 got, inf.vin_u, inf.vin_s, (unsigned long long)inf.tex_u, (unsigned long long)inf.tex_s,
                 (unsigned)P->ms.rasterizationSamples, (unsigned)P->ms.alphaToCoverageEnable, (unsigned)P->afunc,
                 (unsigned)P->pfb_late, (unsigned)k->fs_variant, (unsigned)k->gl_depth, V.gpl_lto, g_pre_adapt);
        }
        if (!cache_only && !pre_from_shard && pre_retyped && npre == 1 && P->sh[0] && ((k->vin_u | k->vin_s) != 0 || (k->tex_u[DMSH_VK_STAGE_VS] | k->tex_s[DMSH_VK_STAGE_VS]) != 0)) {
            vka_svar_rec vr = {0};
            vr.shader_hash = P->sh[0]->key;
            vr.stage = DMSH_VK_STAGE_VS;
            vr.lib_variant = (k->gs_fx == 2 && P->sh[0]->tf_fx) ? DMSH_VK_MOD_TF : DMSH_VK_MOD_BASE;
            vr.vin_u = k->vin_u; vr.vin_s = k->vin_s;
            vr.tex_u = k->tex_u[DMSH_VK_STAGE_VS]; vr.tex_s = k->tex_s[DMSH_VK_STAGE_VS];
            vr.flags = pk.gl[0] ? VKA_SVAR_F_GL_DEPTH : 0;
            svar_record(&vr);
        }
        VkGraphicsPipelineCreateInfo cf = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = fs ? 1 : 0, .pStages = fs, .pMultisampleState = &P->ms,
                                           .pDepthStencilState = &P->ds, .pDynamicState = &dy[L_FS], .layout = fs_lay, .renderPass = rp};
        if (!(part[nparts++] = lib_make(L_FS, &fk, sizeof fk, &cf, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, NULL, 0, fs_key, cache_only, fs_pc, &fs_from_shard, &fs_missed))) return VK_NULL_HANDLE;
        if (!cache_only && fs_missed && P->sh[1] &&
            atomic_fetch_add_explicit(&g_retype_miss_logged, 1, memory_order_relaxed) < 12) {
            vka_spv_retype_hint inf; int got = vka_shader_infer_hint(P->sh[1], &inf);
            VLOG("retype shard miss fs %016llx: runtime vin %#x/%#x tex %#llx/%#llx fout %#x/%#x; inferred %d vin %#x/%#x tex %#llx/%#llx; samples %u a2c %u afunc %u pfb_late %u fs_variant %u gl %u lto %d adapt %#x",
                 (unsigned long long)P->sh[1]->key, k->vin_u, k->vin_s,
                 (unsigned long long)k->tex_u[DMSH_VK_STAGE_FS], (unsigned long long)k->tex_s[DMSH_VK_STAGE_FS], P->fu, P->fsm,
                 got, inf.vin_u, inf.vin_s, (unsigned long long)inf.tex_u, (unsigned long long)inf.tex_s,
                 (unsigned)P->ms.rasterizationSamples, (unsigned)P->ms.alphaToCoverageEnable, (unsigned)P->afunc,
                 (unsigned)P->pfb_late, (unsigned)k->fs_variant, (unsigned)k->gl_depth, V.gpl_lto, g_pre_adapt);
        }
        if (!cache_only && !fs_from_shard && fs_retyped) {
            vka_svar_rec vr = {0};
            vr.shader_hash = P->sh[1]->key;
            vr.stage = DMSH_VK_STAGE_FS;
            vr.lib_variant = (uint8_t)(k->fs_variant == VKA_FS_RETYPE ? DMSH_VK_MOD_BASE : k->fs_variant);
            vr.fout_u = P->fu; vr.fout_s = P->fsm;
            vr.tex_u = k->tex_u[DMSH_VK_STAGE_FS]; vr.tex_s = k->tex_s[DMSH_VK_STAGE_FS];
            vr.alpha_func = (uint8_t)fk.afunc;
            vr.samples = fk.samples;
            vr.flags = (P->pfb_late ? VKA_SVAR_F_PFB_LATE : 0) | (fk.a2c ? VKA_SVAR_F_A2C : 0);
            svar_record(&vr);
        }
    }
    {   fokey key; memset(&key, 0, sizeof key);
        memcpy(key.cfmt, k->cfmt, sizeof key.cfmt); key.dfmt = k->dfmt; key.sfmt = k->sfmt; key.ncolor = k->ncolor; key.samples = (uint8_t)P->ms.rasterizationSamples; key.a2c = k->a2c;
        key.logic_en = k->logic_en; key.logic_op = k->logic_op; key.vin_static = (uint8_t)k->vin_static; memcpy(key.att, k->att, sizeof key.att);
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &P->ri, .pMultisampleState = &P->ms, .pColorBlendState = &P->bs, .pDynamicState = &dy[L_FO],
                                           .renderPass = rp};   // (with a render pass the formats come from it; P->ri is then ignored)
        if (!(part[nparts++] = lib_make(L_FO, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, NULL, 0, 0, cache_only, VK_NULL_HANDLE, NULL, NULL))) return VK_NULL_HANDLE; }
    VkPipelineLibraryCreateInfoKHR libs = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = nparts, .pLibraries = part};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &libs, .layout = P->ci.layout, .renderPass = rp};   // no LINK_TIME_OPTIMIZATION: the fast link
    uint64_t t0 = now_ns();
    int bs = build_begin("graphics pipeline (no cache)", vs_key, fs_key);
    VkPipeline p = VK_NULL_HANDLE; VkResult r = vk.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    build_end(bs);
    uint64_t dns = now_ns() - t0;
    pthread_mutex_lock(&g_mu); g_link_ms += dns / 1e6; if (!r) { vka_gpl_lat_add(&g_link_lat, dns); g_nlinked++; __atomic_store_n(&g_nlinked_pub, g_nlinked, __ATOMIC_RELAXED); } pthread_mutex_unlock(&g_mu);
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("graphics pipeline link: %d; the whole pipeline is made instead", r); return VK_NULL_HANDLE; }
    return p;
}

// DM_VK_PREWARM_OVERLAP=1: every background compile on its own, ungated (the default on drivers other than Turnip before 2026-11-11); default 0: the gate below
static int pso_overlap(void) { return VKA_ENV("DM_VK_PREWARM_OVERLAP", 0) != 0; }
static int pso_heavy_ms(void) { return VKA_ENV("DM_VK_PREWARM_HEAVY_MS", 1000); }
static uint64_t pso_heavy_ns(void) { int ms = pso_heavy_ms(); return ms > 0 ? (uint64_t)ms * 1000000ull : 0; }

// Whether a pipeline thread holds its next compile: while a build anywhere in the process (frame path, asynchronous, prewarm) has run for heavy_ns or more, or while
// another build of the same pre-rasterization or compute shader (key) runs.
static int pw_heavy_hold(uint64_t key, uint64_t heavy_ns) {
    uint64_t t = now_ns(); int mine = key && heavy_has(key);
    for (int i = 0; i < NBUILD; i++) {
        if (atomic_load(&g_build[i].tid) <= 0) continue;
        uint64_t vs = atomic_load(&g_build[i].vs);
        uint64_t fs = atomic_load(&g_build[i].fs);
        if (t - atomic_load(&g_build[i].t0) >= heavy_ns || (key && (vs == key || fs == key)) || (mine && ((vs && heavy_has(vs)) || (fs && heavy_has(fs))))) return 1;
    }
    return 0;
}
// The background compiles' gate (vka_psogate.h): the asynchronous threads' compiles (draws wait for them) first, the optimizer's and prewarm's after them, as many at once
// as MemAvailable and the memory maps' headroom allow (DM_VK_PSO_SLOTS at most, default 4; 1 on Turnip, gate_cap), a shader known to compile long (.heavy) alone. It replaced one mutex
// (g_pw_one) that on Turnip ran every background compile one at a time in whatever order the threads reached it: with Ocarina of Time's market, 431 pipelines that
// draws waited for queued behind 751 prewarm keys, and after them still drained at one pipeline at a time with 1.7 GB of memory and half the maps budget free.
// The memory and maps it reads are refreshed at most every 500 ms, by the thread that finds them stale, outside the gate's lock (/proc/self/maps takes milliseconds)
static uint64_t mem_avail_mb(void);
int plat_maps_count(int *max);
static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static vkpg_state g_gate; static _Atomic int g_gate_slots = 1, g_gate_slots_logged, g_gate_maps = -1, g_gate_mx; static _Atomic uint64_t g_gate_spec_held; static _Atomic uint64_t g_gate_t, g_gate_frame_wait_ns, g_gate_spec_wait_ns;
// DM_VK_PSO_SLOTS's default: 1 on Turnip (driverID), 4 elsewhere (vka_psogate.h vkpg_cap_default)
// RAM saver Strong (DM_VK_RAM_SAVER=2) keeps it at 1 everywhere: each compile in the driver holds tens to hundreds of MiB while it runs
static int gate_cap(void) { return VKA_ENV("DM_VK_PSO_SLOTS", VKA_ENV("DM_VK_RAM_SAVER", 0) >= 2 ? 1 : vkpg_cap_default(V.driver_id == VK_DRIVER_ID_MESA_TURNIP)); }
static int gate_slots(void) {
    uint64_t t = now_ns(), last = atomic_load(&g_gate_t);
    if ((!last || t - last >= 500000000ull) && atomic_compare_exchange_strong(&g_gate_t, &last, t)) {
        int mx = 0, maps = plat_maps_count(&mx);
        uint64_t av = mem_avail_mb();
        int cap = gate_cap(), n = vkpg_slots(av, maps, mx, cap), was = atomic_exchange(&g_gate_slots, n);
        atomic_store(&g_gate_maps, maps); atomic_store(&g_gate_mx, mx);
        if ((n != was || !last) && atomic_fetch_add(&g_gate_slots_logged, 1) < 32)
            VLOG("background pipeline compiles: %d at once (was %d): MemAvailable %llu MiB, memory maps %d of %d (DM_VK_PSO_SLOTS at most %d%s)", n, was, (unsigned long long)av, maps, mx,
                 cap, getenv("DM_VK_PSO_SLOTS") ? "" : VKA_ENV("DM_VK_RAM_SAVER", 0) >= 2 ? ", 1 by default with RAM saver Strong" : V.driver_id == VK_DRIVER_ID_MESA_TURNIP ? ", 1 on Turnip by default" : "");
    }
    return atomic_load(&g_gate_slots);
}
// frame: a compile a draw waits for (an asynchronous thread's); heavy: its shader is in the .heavy list. Returns the token gate_leave takes (-1: not gated)
static int gate_enter(int frame, int heavy) {
    if (pso_overlap()) return -1;
    uint64_t t0 = now_ns(); int waited = 0;
    for (;;) {
        int slots = gate_slots();   // (outside the lock: a refresh reads /proc/self/maps)
        pthread_mutex_lock(&g_gate_mu);
        vkpg_state o = g_gate;   // the others: this thread's own wait left out
        if (waited) { if (frame) { if (heavy) o.frame_heavy_wait--; else o.frame_wait--; } else { if (heavy) o.spec_heavy_wait--; else o.spec_wait--; } }
        // a speculative compile waits while the maps are over vkpg_spec_ok's share (the count is the slots' own, read every 0.5 s); the frame's never does
        int held = !frame && !vkpg_spec_ok(atomic_load(&g_gate_maps), atomic_load(&g_gate_mx), VKA_ENV("DM_VK_SPEC_MAPS_PCT", VKPG_SPEC_PCT));
        if (held && !atomic_exchange(&g_gate_spec_held, 1))
            VLOG("speculative pipeline compiles (prewarm, optimizer) wait: memory maps %d of %d (from %d %% of vm.max_map_count; the driver's compiles leave maps behind), the frame's compiles go on",
                 atomic_load(&g_gate_maps), atomic_load(&g_gate_mx), VKA_ENV("DM_VK_SPEC_MAPS_PCT", VKPG_SPEC_PCT));
        if (!held && vkpg_may_enter(&o, slots, frame, heavy)) break;   // (with the lock held)
        if (!waited) { waited = 1; if (frame) { if (heavy) g_gate.frame_heavy_wait++; else g_gate.frame_wait++; } else { if (heavy) g_gate.spec_heavy_wait++; else g_gate.spec_wait++; } }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 250000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_gate_cv, &g_gate_mu, &ts);   // (timed: memory freed elsewhere raises the slots without a gate_leave)
        pthread_mutex_unlock(&g_gate_mu);
        pause_point();
    }
    if (waited) { if (frame) { if (heavy) g_gate.frame_heavy_wait--; else g_gate.frame_wait--; } else { if (heavy) g_gate.spec_heavy_wait--; else g_gate.spec_wait--; } }
    g_gate.busy++; if (heavy) g_gate.heavy_busy++;
    pthread_mutex_unlock(&g_gate_mu);
    if (waited) atomic_fetch_add(frame ? &g_gate_frame_wait_ns : &g_gate_spec_wait_ns, now_ns() - t0);
    return heavy ? 1 : 0;
}
static void gate_leave(int tok) {
    if (tok < 0) return;
    pthread_mutex_lock(&g_gate_mu);
    g_gate.busy--; if (tok) g_gate.heavy_busy--;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}
// A job's shaders against the .heavy list: its pre-rasterization (or compute) shader or its fragment shader
static int gate_heavy(const vka_shader *pre, const vka_shader *fs) { return (pre && heavy_has(pre->key)) || (fs && heavy_has(fs->key)); }
// The hold of pw_heavy_hold inside the gate: another build of the same shader (the driver would compile it twice), or a known long one outside the gate (the frame
// path's own compute and mesh pipelines). Its age clause (any build past DM_VK_PREWARM_HEAVY_MS) is the ungated threads' only: in the gate the slots already follow
// memory and the maps, and the age clause made every slot wait for any 1 s compile (most of the market's whole pipelines on Turnip took 1-3 s). The time held, ns
static uint64_t gate_hold(int tok, uint64_t key) {
    uint64_t h = pso_heavy_ns(), t0 = 0;
    if (!h) return 0;
    if (tok >= 0) h = UINT64_MAX;
    while (pw_heavy_hold(key, h)) { if (!t0) t0 = now_ns(); usleep(50000); pause_point(); }
    return t0 ? now_ns() - t0 : 0;
}

// The background thread that makes the optimized pipelines, one after another, oldest first; each takes its linked pipeline's place in its map entry
typedef struct opt_job { struct opt_job *next; gval *g; vka_shader *sh[VKA_NSTAGE]; vka_layout lay; vka_gkey k; } opt_job;
static opt_job *g_opt_head, **g_opt_tail = &g_opt_head; static pthread_mutex_t g_opt_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_opt_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_opt_waiting;
static void *opt_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-psopt"); vka_fp_host("vk-psopt");
    // Under the game's threads, as the prewarm and asynchronous threads: nothing waits for an optimized pipeline (its linked one draws meanwhile). Before 2026-11-10 it kept
    // the priority of the thread that queued its first job: nice 10 when that was an asynchronous thread, but the replay thread's -8 without DM_VK_PSO_ASYNC (the default),
    // so every optimized compile (60-130 ms each on Turnip) competed with the recording and the game's threads for a core
    setpriority(PRIO_PROCESS, 0, 10);
    for (;;) {
        pause_point();
        pthread_mutex_lock(&g_opt_mu);
        while (!g_opt_head) pthread_cond_wait(&g_opt_cv, &g_opt_mu);
        opt_job *j = g_opt_head; g_opt_head = j->next; if (!g_opt_head) g_opt_tail = &g_opt_head;
        pthread_mutex_unlock(&g_opt_mu);
        const vka_shader *pre = j->sh[7] ? j->sh[7] : j->sh[0];
        uint64_t key = pre ? pre->key : (j->sh[1] ? j->sh[1]->key : 0);
        int left = unf_has(key);   // its compile did not finish in an earlier run (unf_load): the linked pipeline stays
        int tok = left ? -1 : gate_enter(0, gate_heavy(pre, j->sh[1]));   // speculative: its linked pipeline draws meanwhile
        if (!left) gate_hold(tok, key);
        pstate *P = left ? NULL : malloc(sizeof *P);
        if (P) spec_begin(key);
        VkPipeline p = P && pstate_fill_ex(P, j->sh, &j->lay, &j->k, 1) ? full_pipeline(P, j->sh, " in the background") : VK_NULL_HANDLE;
        if (P) spec_end(key);
        if (!left) gate_leave(tok);
        pthread_mutex_lock(&g_mu);
        if (p) { __atomic_store_n(&j->g->p_mono, p, __ATOMIC_RELEASE); g_nopt++; }   // fallback keeps p_hybrid intact
        g_opt_waiting--;
        pthread_mutex_unlock(&g_mu);
        free(P); free(j);
    }
    return NULL;
}
static void opt_queue(gval *g, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    static int ok;
    opt_job *j = malloc(sizeof *j); if (!j) return;
    j->next = NULL; j->g = g; memcpy(j->sh, sh, sizeof j->sh); j->lay = *lay; j->k = *k;
    if (!ok) { pthread_t th; static _Atomic int tried; if (!atomic_exchange(&tried, 1)) { ok = !vka_compiler_thread(&th, opt_main, NULL); if (ok) pthread_detach(th); else VRED("pipeline optimizer thread not started: linked pipelines stay"); } }
    if (!ok) { free(j); return; }
    pthread_mutex_lock(&g_mu); g_opt_waiting++; pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_opt_mu); *g_opt_tail = j; g_opt_tail = &j->next; pthread_cond_signal(&g_opt_cv); pthread_mutex_unlock(&g_opt_mu);
}
void vka_gpl_line(char *out, size_t n) {
    pthread_mutex_lock(&g_mu);
    double avg_us = 0, p99_us = 0, max_us = 0;
    vka_gpl_lat_summary(&g_link_lat, &avg_us, &p99_us, &max_us);
    if (n) snprintf(out, n, "libraries %llu vertex input / %llu pre-rasterization / %llu fragment shader / %llu fragment output / %llu combined, %.0f ms; %llu linked (%.1f ms, avg %.0f us, p99 %.0f us, max %.0f us), %llu optimized in the background, %llu waiting; "
                    "ir3_miss %llu, rehydrate %llu; cache-only tries %llu hit / %llu missed (%.0f ms in the misses)%s",
                    (unsigned long long)g_nlib[L_VI], (unsigned long long)g_nlib[L_PRE], (unsigned long long)g_nlib[L_FS], (unsigned long long)g_nlib[L_FO], (unsigned long long)g_nlib[L_PREFS], g_lib_ms,
                    (unsigned long long)g_nlinked, g_link_ms, avg_us, p99_us, max_us, (unsigned long long)g_nopt, (unsigned long long)g_opt_waiting,
                    (unsigned long long)atomic_load_explicit(&g_ir3_miss, memory_order_relaxed),
                    (unsigned long long)atomic_load_explicit(&g_ram_rehydrate, memory_order_relaxed),
                    (unsigned long long)g_probe_hit, (unsigned long long)g_probe_miss,
                    g_probe_miss_ns / 1e6, atomic_load(&g_gpl_off) ? "; OFF for this run (pre-rasterization refused)" : "");
    pthread_mutex_unlock(&g_mu);
}
void vka_pso_telemetry_line(char *out, size_t n) {
    if (!out || !n) return;
    vka_gpl_telemetry t = {0};
    pthread_mutex_lock(&g_mu);
    double avg_us = 0, p99_us = 0, max_us = 0;
    vka_gpl_lat_summary(&g_link_lat, &avg_us, &p99_us, &max_us);
    t.gpl_fast_link_count = g_link_lat.count;
    t.gpl_fast_link_avg_us = avg_us;
    t.gpl_fast_link_p99_us = p99_us;
    t.gpl_fast_link_max_us = (uint64_t)max_us;
    pthread_mutex_unlock(&g_mu);
    t.ir3_compiler_miss = atomic_load_explicit(&g_ir3_miss, memory_order_relaxed);
    t.pipeline_ram_rehydrate_count = atomic_load_explicit(&g_ram_rehydrate, memory_order_relaxed);
    t.frontend_shader_translate_count = vka_shader_frontend_xlate_count();
    vka_shader_retype_counts(&t.retype_fout, &t.retype_vin, &t.retype_tex, &t.retype_cs_img);
    t.variant_manifest_loaded = atomic_load_explicit(&g_svar_loaded_n, memory_order_relaxed);
    t.variant_manifest_precompiled = atomic_load_explicit(&g_svar_precompiled_n, memory_order_relaxed);
    t.variant_manifest_saved = atomic_load_explicit(&g_svar_saved_n, memory_order_relaxed);
    t.delta_retype_cache_bytes = atomic_load_explicit(&g_retype_cache_bytes, memory_order_relaxed);
    vka_gpl_telemetry_format(&t, out, n);
    // Appended here rather than in vka_gpl_stats.h: these counters belong to the Boot #1
    // mini-shard rehydration probe, not to the delta retype cache's own telemetry struct.
    size_t used = strlen(out);
    if (used + 1 < n)
        snprintf(out + used, n - used, " retype_shard_hit=%llu retype_shard_miss=%llu",
                 (unsigned long long)atomic_load_explicit(&g_retype_shard_hit, memory_order_relaxed),
                 (unsigned long long)atomic_load_explicit(&g_retype_shard_miss, memory_order_relaxed));
}

// ---------- The prewarm list (2026-11-03) ----------
// The pipeline keys a run made on its frame path, kept beside the pipeline cache, one list per driver (vk_pipelines-<driver>.keys, vka_fpenv.h vka_pwlist_name) and made again by DM_VK_PREWARM background threads (default 2, nice 10)
// as soon as the next run makes its first pipeline: by the time the game reaches a scene, the pipelines it needs are in the map, and the frame path neither compiles nor
// even reads the cache for them. With the driver's pipeline cache warm each costs 1-4 ms on a background core; with it cold (a new driver) each is a compile, still off the
// frame path. A key names shaders by their library key and the state bytes (vka_gkey), not driver code, so one list serves every driver and survives a shader library
// change (a shader that is no longer in the library drops its records). Records whose shaders are missing are dropped at the next save; the list keeps at most
// DM_VK_PREWARM_KEEP records (default 16384), and a run makes at most DM_VK_PREWARM_MAX of them (default 8192): about 1 KB of host memory each, plus the driver's pipeline
// inst[0] / inst[1]: an instanced run's pipeline (vka_draw.c): its vertex / pixel shader is the variant of key[0] / key[1] for the uniform block in slot inst - 1 of the shader's
// own binding range (binding stage * DMSH_VK_STAGE_BINDINGS + slot; 0: the shader itself).
// Older lists have zeros there; an older build reading a newer list makes the plain pipeline of such a record, which is harmless
// The records' format, their check and the layout fingerprint: vka_pwlist.h
static pthread_mutex_t g_pw_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_pw_buf; static size_t g_pw_len, g_pw_cap; static uint32_t g_pw_n; static int g_pw_dirty;   // every record (loaded, then new), under g_pw_mu
static int pw_dirty(void) { return __atomic_load_n(&g_pw_dirty, __ATOMIC_RELAXED) != 0; }
static uint64_t *g_pw_set; static size_t g_pw_setcap, g_pw_setn;   // their hashes
static _Atomic uint32_t g_pw_next, g_pw_next_mesh, g_pw_tix, g_pw_done, g_pw_made, g_pw_had, g_pw_bad; static int g_pw_threads; static uint64_t g_pw_t0; static uint32_t g_pw_seed_n;
static size_t pw_size(const pwrec *r) { return vka_pw_size(r); }
static int pw_set_add(uint64_t h) {   // 1 = new
    if (!h) h = 1;
    if (2 * (g_pw_setn + 1) > g_pw_setcap) {
        size_t nc = g_pw_setcap ? 2 * g_pw_setcap : 1024; uint64_t *n = calloc(nc, sizeof *n); if (!n) return 0;
        for (size_t i = 0; i < g_pw_setcap; i++) if (g_pw_set[i]) { size_t j = g_pw_set[i] & (nc - 1); while (n[j]) j = (j + 1) & (nc - 1); n[j] = g_pw_set[i]; }
        free(g_pw_set); g_pw_set = n; g_pw_setcap = nc;
    }
    size_t j = h & (g_pw_setcap - 1);
    for (; g_pw_set[j]; j = (j + 1) & (g_pw_setcap - 1)) if (g_pw_set[j] == h) return 0;
    g_pw_set[j] = h; g_pw_setn++; return 1;
}
// Every record is kept in memory for the run (the loaded ones first, then the new ones); DM_VK_PREWARM_KEEP applies when the list is written (pw_save keeps the newest).
// The cap was checked here against the count including the loaded records, so a list that had reached it took no new key ever again: it froze
static void pw_append(const pwrec *r, const void *kb) {   // caller holds g_pw_mu
    size_t sz = pw_size(r);
    if (g_pw_len + sz > g_pw_cap) { size_t nc = g_pw_cap ? 2 * g_pw_cap : 65536; while (nc < g_pw_len + sz) nc *= 2; uint8_t *n = realloc(g_pw_buf, nc); if (!n) return; g_pw_buf = n; g_pw_cap = nc; }
    memcpy(g_pw_buf + g_pw_len, r, sizeof *r); memset(g_pw_buf + g_pw_len + sizeof *r, 0, sz - sizeof *r); memcpy(g_pw_buf + g_pw_len + sizeof *r, kb, r->klen);
    g_pw_len += sz; g_pw_n++;
}
// A graphics pipeline's stages as a record's five slots (vka_pwlist.h): a vertex pipeline's are its first five, a mesh pipeline's mesh, fragment and task
static void pw_slots(vka_shader *const sh[VKA_NSTAGE], vka_shader *s5[5], int *kind) {
    if (sh[7]) { *kind = VKA_PW_MESH; s5[0] = sh[7]; s5[1] = sh[1]; s5[2] = sh[6]; s5[3] = s5[4] = NULL; }
    else { *kind = VKA_PW_GFX; for (int i = 0; i < 5; i++) s5[i] = sh[i]; }
}
// A pipeline the frame path made: into the list unless it is there. sh: the record's five slots (vertex: sh[0..4]; mesh: mesh, fragment, task; compute: the shader)
static void pw_note(int kind, vka_shader *const sh[5], const void *kb, uint32_t klen) {
    if (!g_pw_path[0]) return;
    pwrec r; memset(&r, 0, sizeof r); r.kind = (uint8_t)kind; r.klen = klen;
    for (int i = 0; i < 5; i++) {   // an instanced variant: its base key and its binding, so the next run makes it ahead too (its pipeline was a frame-path hitch every run)
        if (!vka_shader_is_variant(sh[i])) { r.key[i] = sh[i] ? sh[i]->key : 0; continue; }
        uint32_t lo = sh[i]->e->stage * DMSH_VK_STAGE_BINDINGS, b = sh[i]->inst_bind - 1;
        if (kind != VKA_PW_GFX || i > 1 || !sh[i]->inst_bind || b < lo || b - lo >= DMSH_VK_STAGE_BINDINGS) return;
        r.key[i] = sh[i]->e->key; r.inst[i] = (uint8_t)(b - lo + 1);
    }
    uint64_t h = vka_pw_hash(&r, kb);
    pthread_mutex_lock(&g_pw_mu);
    if (pw_set_add(h)) { pw_append(&r, kb); g_pw_dirty = 1; }
    pthread_mutex_unlock(&g_pw_mu);
}
// A compute pipeline the frame path made: its record, with the retype masks as key bytes when its images are retyped (vka_pwlist.h VKA_PW_CS_MASKS), so the next run's
// prewarm makes the module its dispatches use, not the base one they do not
static void pw_note_cs(vka_shader *cs, uint64_t umask, uint64_t smask) {
    vka_shader *s5[5] = {cs, NULL, NULL, NULL, NULL}; uint64_t m[2] = {umask, smask};
    if (umask | smask) pw_note(VKA_PW_CS, s5, m, VKA_PW_CS_MASKS); else pw_note(VKA_PW_CS, s5, "", 0);
}
// The records of one list into the run's (caller holds g_pw_mu), those already there skipped: the count taken, *skipped those with impossible values. -1 = not read (*why)
static int pw_take(const uint8_t *d, size_t len, uint32_t *skipped, const char **why) {
    size_t o; uint32_t want, n = 0, took = 0; int e = vka_pw_header(d, len, &o, &want);
    if (e) { *why = e == VKA_PW_LAYOUT ? "another key layout (an older or newer host)" : e == VKA_PW_FORMAT ? "another format" : "too short"; return -1; }
    pwrec r; const uint8_t *kb; int ok;
    while (n < want && vka_pw_next(d, len, &o, &r, &kb, &ok)) {
        n++;
        if (!ok) { (*skipped)++; continue; }
        r.bad = 0;
        if (pw_set_add(vka_pw_hash(&r, kb))) { pw_append(&r, kb); took++; }
    }
    return (int)took;
}
// The run's list: this device's own (<cache>.keys, the keys of its earlier runs, in the order they were first made), then the profile's seed list for those it lacks
// (vka_pwlist.h; DM_VK_PREWARM_SEED=0 leaves it out). On a first start the seed is the whole list: the prewarm threads make the pipelines the game's first scenes need
// while the game plays its logos and menus, where before every one was made on the frame path at the first 3D frame (Ocarina of Time: 128 pipelines, 79 s)
static void pw_load(void) {
    static int done;
    if (done || !g_pw_path[0]) return;
    done = 1;
    uint32_t skipped = 0; const char *why = NULL; int own = -1, seed = -1;
    FILE *f = fopen(g_pw_path, "rb");
    // No list of this driver's yet: the one every driver shared before 2026-11-18 (vka_fpenv.h vka_pwlist_name), once, so an update does not start cold. It may hold
    // other drivers' keys, which this driver's list (written from it at the next save) keeps; a vertex key of the other vertex input path is not made (pw_main). The
    // shared file stays for the other drivers
    if (!f && g_pw_shared[0] && (f = fopen(g_pw_shared, "rb"))) { VLOG("prewarm list %s: none yet; the list every driver shared before, %s, taken as its start", g_pw_path, g_pw_shared); g_pw_dirty = 1; }
    uint8_t *d = NULL; long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); d = sz > 0 ? malloc((size_t)sz) : NULL; if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; } fclose(f); }
    pthread_mutex_lock(&g_pw_mu);
    if (!f) VLOG("prewarm list %s: none yet (written as this run makes pipelines)", g_pw_path);
    else if ((own = d ? pw_take(d, (size_t)sz, &skipped, &why) : -1) < 0) { VLOG("prewarm list %s: %s, not used (rewritten as this run makes pipelines)", g_pw_path, d ? why : "unreadable"); g_pw_dirty = 1; }
    if (skipped) { VLOG("prewarm list %s: %u records with impossible values skipped (dropped at the next save)", g_pw_path, skipped); g_pw_dirty = 1; }
    free(d);
    if (dm_vk_prewarm_seed_len && VKA_ENV("DM_VK_PREWARM_SEED", 1)) {
        uint32_t sk = 0; const char *sw = NULL;
        seed = pw_take(dm_vk_prewarm_seed, dm_vk_prewarm_seed_len, &sk, &sw);
        if (seed < 0) VRED("prewarm seed of the profile (vk_prewarm.bin, %u bytes): %s, not used: dm/tools/vk_prewarm.py makes it again from a run of this host", (unsigned)dm_vk_prewarm_seed_len, sw);
        else { g_pw_seed_n = (uint32_t)seed; if (seed) g_pw_dirty = 1; }   // (kept in this device's list from now on: the next save writes them)
    }
    g_pw_load = malloc(g_pw_len ? g_pw_len : 1); if (g_pw_load) { memcpy(g_pw_load, g_pw_buf, g_pw_len); g_pw_load_len = g_pw_len; g_pw_load_n = g_pw_n; }
    pthread_mutex_unlock(&g_pw_mu);
    VLOG("prewarm list %s: %u pipeline keys (%d of earlier runs on this device, %d more from the profile's seed list%s)", g_pw_path, g_pw_load_n, own > 0 ? own : 0, seed > 0 ? seed : 0,
         !dm_vk_prewarm_seed_len ? ": the profile has none" : seed < 0 ? ": not used" : "");
}
static int pw_save(void) {   // the saver thread or a forced save: the list when it changed; -1 = not written
    if (!g_pw_path[0]) return 0;
    pthread_mutex_lock(&g_pw_mu);
    if (!g_pw_dirty) { pthread_mutex_unlock(&g_pw_mu); return 0; }
    // Records the prewarm threads found broken (a shader not in the library any more) are left out
    size_t len = 0; uint32_t n = 0; uint8_t *out = malloc(sizeof(pwhdr2) + g_pw_len);
    if (!out) { pthread_mutex_unlock(&g_pw_mu); return -1; }
    // At most DM_VK_PREWARM_KEEP records, the newest: the oldest loaded ones go first (a key no run has needed for a while)
    uint32_t good = 0; for (size_t o = 0; o < g_pw_len;) { pwrec r; memcpy(&r, g_pw_buf + o, sizeof r); if (!r.bad) good++; o += pw_size(&r); }
    int keep = VKA_ENV("DM_VK_PREWARM_KEEP", 16384); uint32_t drop = keep > 0 && good > (uint32_t)keep ? good - (uint32_t)keep : 0;
    for (size_t o = 0; o < g_pw_len;) { pwrec r; memcpy(&r, g_pw_buf + o, sizeof r); size_t sz = pw_size(&r);
        if (!r.bad) { if (drop) drop--; else { memcpy(out + sizeof(pwhdr2) + len, g_pw_buf + o, sz); len += sz; n++; } } o += sz; }
    g_pw_dirty = 0;
    pthread_mutex_unlock(&g_pw_mu);
    pwhdr2 h; memset(&h, 0, sizeof h); memcpy(h.magic, VKA_PW_MAGIC2, 8); h.gkey_size = sizeof(vka_gkey); h.n = n; h.layout = vka_pw_layout(); memcpy(out, &h, sizeof h);
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pw_path);
    int ok = pc_write_atomic(g_pw_path, tmp, out, sizeof h + len);
    if (!ok) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VRED("prewarm list not written to %s", g_pw_path); pthread_mutex_lock(&g_pw_mu); g_pw_dirty = 1; pthread_mutex_unlock(&g_pw_mu); }
    free(out);
    return ok ? (int)n : -1;
}
static void pw_mark_bad(size_t off) {   // the record at off of the loaded list: dropped at the next save (the loaded records are the first ones of g_pw_buf, in the same order)
    pthread_mutex_lock(&g_pw_mu);
    if (off + sizeof(pwrec) <= g_pw_len) { g_pw_buf[off + offsetof(pwrec, bad)] = 1; g_pw_dirty = 1; }
    pthread_mutex_unlock(&g_pw_mu);
}
// Why a pipeline the frame path made was not made ahead (the first DM_VK_PREWARM_WHY of them, 64; 0 = none): the loaded list's records with the same shaders, and the
// fields in which the nearest of them differs from the key asked for. "no record with these shaders" = new to the list (an area or effect no earlier run reached: prewarm
// cannot help, only GPL or a faster compile); fields named = the list held a pipeline of the same shaders in another state (a state the key keeps that earlier runs did
// not meet, or a field the key should not keep because the pipeline does not depend on it)
// where: " on the frame path", or " in the background" for an asynchronous one (DM_VK_PSO_ASYNC; ms then counts from the miss)
static void pw_explain(int kind, vka_shader *const sh[5], const vka_gkey *k, uint32_t klen, double ms, int linked, const char *where) {
    static _Atomic int nlog; int lim = VKA_ENV("DM_VK_PREWARM_WHY", 64);
    if (!g_pw_load || atomic_load(&nlog) >= lim || atomic_fetch_add(&nlog, 1) >= lim) return;
    static const struct { const char *name; size_t lo, hi; } F[] = {
        {"colour formats", offsetof(vka_gkey, cfmt), offsetof(vka_gkey, dfmt)}, {"depth format", offsetof(vka_gkey, dfmt), offsetof(vka_gkey, sfmt)},
        {"stencil format", offsetof(vka_gkey, sfmt), offsetof(vka_gkey, ncolor)}, {"colour count", offsetof(vka_gkey, ncolor), offsetof(vka_gkey, samples)},
        {"samples", offsetof(vka_gkey, samples), offsetof(vka_gkey, topo)}, {"topology", offsetof(vka_gkey, topo), offsetof(vka_gkey, poly_mode)},
        {"polygon mode", offsetof(vka_gkey, poly_mode), offsetof(vka_gkey, depth_clamp)}, {"depth clamp", offsetof(vka_gkey, depth_clamp), offsetof(vka_gkey, a2c)},
        {"alpha to coverage", offsetof(vka_gkey, a2c), offsetof(vka_gkey, logic_en)}, {"logic op", offsetof(vka_gkey, logic_en), offsetof(vka_gkey, gl_depth)},
        {"GL depth", offsetof(vka_gkey, gl_depth), offsetof(vka_gkey, fs_variant)}, {"fragment variant", offsetof(vka_gkey, fs_variant), offsetof(vka_gkey, alpha_func)},
        {"alpha func", offsetof(vka_gkey, alpha_func), offsetof(vka_gkey, gs_fx)}, {"geometry claim", offsetof(vka_gkey, gs_fx), offsetof(vka_gkey, vin_u)},
        {"vertex input retype", offsetof(vka_gkey, vin_u), offsetof(vka_gkey, tex_u)}, {"texture retype", offsetof(vka_gkey, tex_u), offsetof(vka_gkey, att)}, {"blend", offsetof(vka_gkey, att), offsetof(vka_gkey, vin_static)},
        {"static vertex input", offsetof(vka_gkey, vin_static), sizeof(vka_gkey)}};
    enum { NF = sizeof F / sizeof F[0] };
    uint64_t want[5]; for (int i = 0; i < 5; i++) want[i] = sh[i] ? sh[i]->key : 0;
    int same_sh = 0, best = -1; unsigned bestmask = 0;
    for (size_t o = 0; o + sizeof(pwrec) <= g_pw_load_len;) {
        pwrec r; memcpy(&r, g_pw_load + o, sizeof r); const uint8_t *kb = g_pw_load + o + sizeof r; o += pw_size(&r);
        if (r.kind != kind || memcmp(r.key, want, sizeof want)) continue;
        same_sh++;
        vka_gkey rk; memset(&rk, 0, sizeof rk); memcpy(&rk, kb, r.klen < sizeof rk ? r.klen : sizeof rk);
        const uint8_t *a = (const uint8_t *)&rk, *b = (const uint8_t *)k; int nd = 0; unsigned mask = 0;
        for (int f = 0; f < NF; f++) { size_t hi = F[f].hi < klen ? F[f].hi : klen; if (F[f].lo < hi && memcmp(a + F[f].lo, b + F[f].lo, hi - F[f].lo)) { nd++; mask |= 1u << f; } }
        if (best < 0 || nd < best) { best = nd; bestmask = mask; }
    }
    char d[256] = ""; size_t q = 0;
    for (int f = 0; f < NF && q < sizeof d; f++) if (bestmask >> f & 1) q += (size_t)snprintf(d + q, sizeof d - q, "%s%s", q ? ", " : "", F[f].name);
    if (!same_sh) VLOG("pipeline made%s (%.1f ms%s), not in the prewarm list: no record with these shaders (%s %016llx fs %016llx; first met in this run)", where,
                       ms, linked ? ", linked" : "", kind == VKA_PW_MESH ? "mesh" : "vs", (unsigned long long)want[0], (unsigned long long)want[1]);
    else VLOG("pipeline made%s (%.1f ms%s), not in the prewarm list: %d records with these shaders, the nearest differs in %s", where,
              ms, linked ? ", linked" : "", same_sh, d[0] ? d : "nothing: the same key (the prewarm threads had not reached it yet)");
}
static gval *gfx_obtain(const gkey_full *fk, uint64_t h, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay_mono, const vka_gkey *k_canon, int prewarm, int *made);
static VkPipeline cs_obtain(vka_shader *cs, const vka_layout *lay, int prewarm, int *made, uint64_t umask, uint64_t smask, int frame);
// MemAvailable in MiB (/proc/meminfo; 0 = not known)
static uint64_t mem_avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char line[128]; uint64_t kb = 0;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) { kb = strtoull(line + 13, NULL, 10); break; }
    if (f) fclose(f);
    return kb >> 10;
}
static _Atomic uint64_t g_pw_memwait_ns, g_pw_heavywait_ns;
// A record of a run that drew mesh draws through compute (vka_memu.c): a compute form or a vertex shader of a task / mesh shader, whose keys are no library keys
static int pw_memu_rec(const pwrec *r) {
    if (r->kind == VKA_PW_MESH) return 0;
    for (int s = 0; s < (r->kind == VKA_PW_CS ? 1 : 5); s++) if (r->key[s] && vka_shader_memu_key(r->key[s])) return 1;
    return 0;
}
static void *pw_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-prewarm"); vka_fp_host("vk-prewarm");
    setpriority(PRIO_PROCESS, 0, 10);   // this thread (Linux: per-thread nice), under the game's threads
    uint32_t max = (uint32_t)VKA_ENV("DM_VK_PREWARM_MAX", 8192);
    size_t *offs = NULL;   // walked once by each thread to find the record of index i: built here (the list is small)
    // The order: mesh pipelines, then compute, then vertex pipelines, each kind in the list's order (the order earlier runs first needed them). A cold mesh pipeline took
    // 3-17 s on Qualcomm's compiler and a compute one 1-3 s, a vertex one about 0.1 s: Ocarina of Time's first 3D frame needs all of them at once, and the frame path makes
    // the cheap ones itself in less time than one slow one takes, so the slow kinds go first (seed v1 in list order left 33 mesh pipelines to the frame path: 102 s).
    // Mesh pipelines are made by one prewarm thread at a time (the first; it joins the others when they are done): four at once took the compiler's memory with them
    // (std::bad_alloc thrown in the driver, the game process aborted 30 s after launch, MemAvailable 6.0 → 2.4 GB in 10 s; run t-sys1c, 2026-11-08)
    uint32_t n = g_pw_load_n, nm = 0, me = atomic_fetch_add(&g_pw_tix, 1); offs = malloc((n ? n : 1) * sizeof *offs);
    if (offs) { static const uint8_t rank[3] = {VKA_PW_MESH, VKA_PW_CS, VKA_PW_GFX}; uint32_t i = 0;
        for (int k = 0; k < 3; k++) for (size_t o = 0, j = 0; j < n; j++) { pwrec r; memcpy(&r, g_pw_load + o, sizeof r); if (r.kind == rank[k] || (k == 2 && r.kind > VKA_PW_MESH)) offs[i++] = o; o += pw_size(&r); }
        for (uint32_t j = 0; j < n; j++) { pwrec r; memcpy(&r, g_pw_load + offs[j], sizeof r); if (r.kind != VKA_PW_MESH) break; nm++; } }
    // Every thread but the first waits while the phone has less than DM_VK_PREWARM_MEM_MB (default 2048) available: compiles take the driver hundreds of MB each, and
    // with the game's first 3D scene loaded the phone had 1.1-1.9 GB left; four threads then ran it out (Turnip run t-tur1c: MemAvailable 0.38 GB, mprotect failing with
    // ENOMEM, a crash at 83 s). The first thread goes on as the frame path would make the same pipelines one at a time anyway
    uint32_t mem_mb = (uint32_t)VKA_ENV("DM_VK_PREWARM_MEM_MB", 2048);
    // Each build goes through the gate (gate_enter: the frame's compiles first, as many at once as memory allows, a known long one alone) and holds while another build
    // of the same shader runs (gate_hold); with DM_VK_PREWARM_OVERLAP=1 each thread builds on its own and holds while any long compile runs (DM_VK_PREWARM_HEAVY_MS)
    for (;;) {
        pause_point();
        if (me > 0 && mem_mb) { uint64_t t0 = 0, av; while ((av = mem_avail_mb()) && av < mem_mb && atomic_load(&g_pw_next) < n) { if (!t0) t0 = now_ns(); usleep(250000); pause_point(); }
                                if (t0) atomic_fetch_add(&g_pw_memwait_ns, now_ns() - t0); }
        uint32_t i = me == 0 ? atomic_fetch_add(&g_pw_next_mesh, 1) : nm;   // the mesh thread: the next mesh record while there is one
        if (i >= nm) { uint32_t q = atomic_fetch_add(&g_pw_next, 1); i = nm + q; }
        if (!offs || i >= n || i >= max) break;
        pwrec r; memcpy(&r, g_pw_load + offs[i], sizeof r);
        const uint8_t *kb = g_pw_load + offs[i] + sizeof r;
        vka_shader *sh[VKA_NSTAGE]; memset(sh, 0, sizeof sh); int bad = !vka_pw_rec_ok(&r, kb);
        if (bad) {}
        else if (!V.memu && pw_memu_rec(&r)) continue;   // (mesh draws native here: a run through compute's compute forms and vertex pipelines are not made, the record kept)
        else if (r.kind == VKA_PW_CS) { if (!(sh[5] = vka_shader_by_key(r.key[0]))) bad = 1; }
        else if (r.kind == VKA_PW_MESH && V.memu) continue;   // (mesh draws through compute: no mesh pipeline, the record kept; their compute forms and vertex pipelines have records of their own)
        else if (r.kind == VKA_PW_MESH) {   // the frame path's stages of a mesh draw (vka_draw.c): task 6, mesh 7, fragment 1
            if (!(sh[7] = vka_shader_by_key(r.key[0])) || !(sh[1] = vka_shader_by_key(r.key[1])) || (r.key[2] && !(sh[6] = vka_shader_by_key(r.key[2])))) bad = 1;
        }
        else {
            for (int s = 0; s < 5; s++) if (r.key[s] && vka_shader_variant_key(r.key[s])) bad = 2;   // an older list's instanced variant: dropped quietly
            for (int s = 0; s < 5 && !bad; s++) if (r.key[s] && !(sh[s] = vka_shader_by_key(r.key[s]))) bad = 1;
            // A mesh shader's generated vertex shader with a pixel shader whose inputs it does not write: no draw pairs them, so a record of an older host (before
            // 73ed0b5c a draw met twice in a look-ahead took its first meeting's vertex shader: docs/reference/rendering.md "Mesh draws through compute matched to the
            // wrong meeting of a record"), which Qualcomm's driver refuses with -13 at every start. Dropped
            if (!bad && sh[0] && sh[0]->memu == 2 && !vka_memu_vs_feeds(sh[0], sh[1])) {
                bad = 3; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VLOG("prewarm: record of vertex %016llx and pixel %016llx dropped: the mesh shader's vertex shader does not write the pixel shader's inputs (a record of an older host)",
                                                                                (unsigned long long)sh[0]->key, (unsigned long long)sh[1]->key);
            }
        }
        vka_layout lay;
        if (!bad && !(r.kind != VKA_PW_CS && V.gpl_indep ? vka_layout_mono_of(sh, &lay) : vka_layout_of(sh, r.kind == VKA_PW_CS, &lay))) bad = 1;
        int skip = 0;   // an instanced run's pipeline with instancing off: not made, kept in the list
        for (int s = 0; s < 2 && !bad && !skip && r.kind == VKA_PW_GFX; s++) if (r.inst[s] && !(sh[s] = vka_inst_variant(sh[s], sh[s]->e->stage * DMSH_VK_STAGE_BINDINGS + r.inst[s] - 1u))) skip = 1;
        if (skip) continue;
        int tok = -1; uint64_t key = 0;
        if (!bad) {   // through the gate after the frame's compiles (gate_enter), then held while another build of this shader runs (gate_hold)
            const vka_shader *pre = r.kind == VKA_PW_CS ? sh[5] : r.kind == VKA_PW_MESH ? sh[7] : sh[0];
            key = pre ? pre->key : 0;
            if (unf_has(key)) {   // its compile did not finish in an earlier run on this driver (unf_load): left out, the record kept
                if (atomic_fetch_add(&g_pw_unf, 1) < 4) VLOG("prewarm leaves out %s pipeline of shader %016llx: its compile did not finish in an earlier run on this driver",
                                                              r.kind == VKA_PW_CS ? "a compute" : r.kind == VKA_PW_MESH ? "a mesh" : "a graphics", (unsigned long long)key);
                continue;
            }
            tok = gate_enter(0, gate_heavy(pre, r.kind == VKA_PW_CS ? NULL : sh[1]));
            atomic_fetch_add(&g_pw_heavywait_ns, gate_hold(tok, key));
            spec_begin(key);
        }
        int made = 0;
        if (bad) { atomic_fetch_add(&g_pw_bad, 1); pw_mark_bad(offs[i]); }
        else if (r.kind == VKA_PW_CS) {   // (with its retype masks: the module its dispatches use)
            uint64_t m[2] = {0, 0}; if (r.klen == VKA_PW_CS_MASKS) memcpy(m, kb, sizeof m);
            cs_obtain(sh[5], &lay, 1, &made, m[0], m[1], 0);
        }
        else {
            vka_gkey k; memset(&k, 0, sizeof k); memcpy(&k, kb, r.klen < sizeof k ? r.klen : sizeof k);
            if (k.vin_static != (uint32_t)!V.vin_dyn) { atomic_fetch_add(&g_pw_bad, 1); }   // a list from a device with the other vertex input (not this one's keys)
            else {
                gkey_full fk; memset(&fk, 0, sizeof fk); memcpy(fk.sh, sh, sizeof fk.sh); fk.layout = lay.layout; fk.k = k;
                size_t len = k.vin_static ? sizeof fk : offsetof(gkey_full, k.va);
                gfx_obtain(&fk, fnv(&fk, len, 1469598103934665603ull), len, sh, &lay, &k, 1, &made);
            }
        }
        if (!bad) spec_end(key);
        gate_leave(tok);
        if (made) atomic_fetch_add(&g_pw_made, 1); else if (!bad) atomic_fetch_add(&g_pw_had, 1);
        setpriority(PRIO_PROCESS, 0, 10);   // back under the game's threads if a waiting frame path raised it (busy_wait)
    }
    free(offs);
    if (atomic_fetch_add(&g_pw_done, 1) + 1 == (uint32_t)g_pw_threads)
        VLOG("prewarm: %u of %u pipeline keys in %.1f s on %d threads: %u made, %u already there, %u dropped (shaders no longer in the library, another device's keys); the frame path waited %.0f ms for them; "
             "threads waited %.1f s for memory (DM_VK_PREWARM_MEM_MB), %.1f s for a long compile (DM_VK_PREWARM_HEAVY_MS); %u left out (a compile that did not finish before)",
             (unsigned)(atomic_load(&g_pw_made) + atomic_load(&g_pw_had) + atomic_load(&g_pw_bad)), n, (now_ns() - g_pw_t0) / 1e9, g_pw_threads, (unsigned)atomic_load(&g_pw_made),
             (unsigned)atomic_load(&g_pw_had), (unsigned)atomic_load(&g_pw_bad), atomic_load(&g_pw_wait_ns) / 1e6, atomic_load(&g_pw_memwait_ns) / 1e9,
             atomic_load(&g_pw_heavywait_ns) / 1e9, (unsigned)atomic_load(&g_pw_unf));
    return NULL;
}
// DM_VK_PREWARM threads (default 4, at most 4; 2 before 2026-11-08). With the pipeline cache warm a key costs a few milliseconds (297 in 0.1 s on 2 threads); with it
// cold each is a compile of 60-100 ms, some vertex and mesh ones 2-9 s on Qualcomm's compiler, and Ocarina of Time's title scene alone needs about 130, so the list is
// done before the game's first 3D frame only on more threads. They run at nice 10, under the game's threads, during logos and menus that leave most cores idle.
// Default off on Qualcomm's driver before 512.800 (V.qcom_old): 512.676 appears to keep about 6 MB of memory outside the process per pipeline (MemAvailable against the pipeline count), and the list's ~650 pipelines
// took MemAvailable from 3.3 GB to 1.1-1.6 GB in Fire Emblem Fortune's Weave's opening (321 made without it); the low-memory killer then ended the game in the class showcase. DM_VK_PREWARM=<n> wins
static void pw_start(void) {
    int nt = VKA_ENV("DM_VK_PREWARM", V.qcom_old ? 0 : 4);
    if (nt <= 0 || !g_pw_load_n) { if (g_pw_load_n) VLOG("prewarm: off (%s): %u keys not made ahead", getenv("DM_VK_PREWARM") ? "DM_VK_PREWARM=0" : "default on Qualcomm's driver before 512.800; DM_VK_PREWARM=4 on", g_pw_load_n); return; }
    if (nt > 4) nt = 4;
    heavy_load();   // the shaders earlier runs found long on this driver (build_end)
    g_pw_t0 = now_ns(); g_pw_threads = 0;
    for (int i = 0; i < nt; i++) { pthread_t th; if (!vka_compiler_thread(&th, pw_main, NULL)) { pthread_detach(th); g_pw_threads++; } }
    VLOG("prewarm: %u pipeline keys of earlier runs made ahead on %d background threads (DM_VK_PREWARM=0 off)", g_pw_load_n, g_pw_threads);
}

// ---------- Graphics pipelines: the lookup ----------
// The entry of a key, made by this thread when no one has it (made = 1). A key another thread is making (busy): the frame path waits for it (the time goes to the [longframe]
// line's prewarm wait), a prewarm thread goes on to its next key
// The frame path waits for another thread making the same key (caller holds g_mu). A prewarm thread runs at nice 10, under the game's threads, so the frame waited for a
// low-priority build (a priority inversion, worse than making it itself): that thread is raised to the waiter's priority for the rest of its build (pw_main puts itself back
// at nice 10 before its next key)
static void busy_wait(gval *g) {
    uint64_t t0 = now_ns();
    if (g->pw && g->tid > 0) { errno = 0; int me = getpriority(PRIO_PROCESS, 0); if (!(me == -1 && errno)) setpriority(PRIO_PROCESS, (id_t)g->tid, me); }
    while (g->busy) pthread_cond_wait(&g_busy_cv, &g_mu);
    atomic_fetch_add(&g_pw_wait_ns, now_ns() - t0);
}
// DM_VK_PSO_ASYNC=N (restart; default 0 = off): a stopgap for frame-path pipelines on slow compilers (Turnip on the Adreno 740 made Donkey Kong Bananza's fragment shader
// libraries in 77-213 ms, 96 pipelines and 13.7 s of frame waits in 40 s of wall punching). The frame path still makes the cache-only try (a fraction of a millisecond when
// the pipeline cache has it); on a miss the entry is made on one of N background threads (at most 4) and its draws are skipped until it is there, as Eden's asynchronous
// shaders do. A skipped draw is missing from that frame only, except where the game keeps what it drew (a texture drawn once and reused): that stays wrong until redrawn.
// Mesh pipelines are still made on the frame path; compute pipelines go through the same queue (DM_VK_CS_ASYNC, cs_obtain). Open: the cause, slow library compiles on
// Turnip, is not addressed. cs: a compute job (sh[5] its shader, g an entry of g_cs; len, k unused)
typedef struct as_job { struct as_job *next; gval *g; size_t len; vka_shader *sh[VKA_NSTAGE]; vka_layout lay; vka_gkey k, k_canon; uint64_t t0, cost; int cs; uint64_t umask, smask; } as_job;   // umask, smask: a compute job's retyped images (cs_obtain)
static as_job *g_as_head, **g_as_tail = &g_as_head; static pthread_mutex_t g_as_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_as_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_as_queued, g_as_made, g_as_skips, g_as_ns, g_as_lat_max_ns; static _Atomic int g_as_depth, g_as_depth_max;
// The queue's order (vka_psogate.h vkpg_take_oldest): the job of least predicted cost first, the oldest in its turn. DM_VK_PSO_ORDER=0 (restart): arrival order, as
// before 2026-11-11. DM_VK_PSO_FAIR_MS (restart, 2000; 0 never): how long the oldest job waits before it takes turns with the cheapest. Under g_as_mu: g_as_credit (ns)
static int g_as_order = -1; static uint64_t g_as_fair_ns; static int64_t g_as_credit; static uint64_t g_as_cheap_n, g_as_old_n, g_as_lat_cheap_ns, g_as_lat_old_ns;
// A job's predicted cost: the SPIR-V bytes of its stages (a library part's compile on Turnip grew with them: ~25-170 ms under 70 KB, 1.6-3.8 s for the market's
// shaders of 88-130 KB), a shader in the .heavy list (it took DM_VK_PREWARM_HEAVY_MS or more before) counted as 64 MiB more, so after every unknown one
static uint64_t as_cost(vka_shader *const sh[VKA_NSTAGE]) {
    uint64_t c = 0;
    for (int i = 0; i < VKA_NSTAGE; i++) {   // every stage: a mesh pipeline's task and mesh shaders (6, 7) too
        const vka_shader *s = sh[i]; if (!s) continue;
        const dmsh_vk_module *m = s->lib && s->e ? vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE) : NULL;
        c += m ? m->size : 65536;
        if (heavy_has(s->key)) c += 64ull << 20;
    }
    return c;
}
static void gfx_build(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay_mono, const vka_gkey *k_canon, int prewarm, int async, uint64_t t_make);
static void cs_build(gval *g, vka_shader *cs, const vka_layout *lay, VkShaderModule csm, uint64_t umask, uint64_t smask, int prewarm, int async);
static void *as_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-psoasync"); vka_fp_host("vk-psoasync");
    setpriority(PRIO_PROCESS, 0, 10);   // under the game's threads, as the prewarm threads: no frame waits for these builds (their draws are skipped instead)
    for (;;) {
        pause_point();
        pthread_mutex_lock(&g_as_mu);
        while (!g_as_head) pthread_cond_wait(&g_as_cv, &g_as_mu);
        // The oldest (the head) in its turn, else the cheapest (the first of least cost: arrival order among equals)
        as_job **pp = &g_as_head; int oldest = 1;
        if (g_as_order && !vkpg_take_oldest(now_ns() - g_as_head->t0, g_as_fair_ns, g_as_credit)) {
            for (as_job **q = &g_as_head; *q; q = &(*q)->next) if ((*q)->cost < (*pp)->cost) pp = q;
            oldest = pp == &g_as_head;   // (the cheapest is the oldest: counted as an oldest pick)
        }
        as_job *j = *pp; *pp = j->next;
        if (g_as_tail == &j->next) g_as_tail = pp;
        pthread_mutex_unlock(&g_as_mu);
        const vka_shader *pre = j->cs ? j->sh[5] : j->sh[7] ? j->sh[7] : j->sh[0];
        uint64_t key = pre ? pre->key : (j->sh[1] ? j->sh[1]->key : 0);
        int tok = gate_enter(1, gate_heavy(pre, j->cs ? NULL : j->sh[1]));   // the frame's: its draws (dispatches) are skipped until it is made, so it goes before the speculative ones
        gate_hold(tok, key);
        uint64_t t0 = now_ns();
        if (j->cs) cs_build(j->g, j->sh[5], &j->lay, j->umask | j->smask ? vka_shader_img_module(j->sh[5], j->umask, j->smask) : vka_shader_cs_module(j->sh[5]), j->umask, j->smask, 0, 1);
        else gfx_build(j->g, j->len, j->sh, &j->lay, &j->k_canon, 0, 1, j->t0);
        gate_leave(tok);
        uint64_t t1 = now_ns(), lat = t1 - j->t0, cur = atomic_load(&g_as_lat_max_ns);
        while (lat > cur && !atomic_compare_exchange_weak(&g_as_lat_max_ns, &cur, lat)) {}
        pthread_mutex_lock(&g_as_mu);
        if (oldest) { g_as_credit -= (int64_t)(t1 - t0); g_as_old_n++; g_as_lat_old_ns += lat; } else { g_as_credit += (int64_t)(t1 - t0); g_as_cheap_n++; g_as_lat_cheap_ns += lat; }
        if (g_as_credit > (int64_t)g_as_fair_ns) g_as_credit = (int64_t)g_as_fair_ns;   // (cheap time saved up while nothing was old does not buy a run of oldest picks later)
        pthread_mutex_unlock(&g_as_mu);
        atomic_fetch_add(&g_as_ns, t1 - t0); atomic_fetch_add(&g_as_made, 1); atomic_fetch_sub(&g_as_depth, 1);
        free(j);
    }
    return NULL;
}
static int g_as_n;
static void as_start(void) {
    int v = VKA_ENV("DM_VK_PSO_ASYNC", 0); v = v < 0 ? 0 : v > 4 ? 4 : v;
    g_as_order = VKA_ENV("DM_VK_PSO_ORDER", 1) != 0; { int f = VKA_ENV("DM_VK_PSO_FAIR_MS", 2000); g_as_fair_ns = f > 0 ? (uint64_t)f * 1000000ull : (uint64_t)INT64_MAX; }   // (0: the oldest never takes a turn)
    for (int i = 0; i < v; i++) { pthread_t th; if (!vka_compiler_thread(&th, as_main, NULL)) { pthread_detach(th); g_as_n++; } }
    if (v) VLOG("asynchronous pipelines: %d of %d background threads (DM_VK_PSO_ASYNC): a pipeline the cache lacks is made off the frame path and its draws are skipped until then", g_as_n, v);
}
static int as_threads(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, as_start); return g_as_n; }
static void as_queue(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay_mono, const vka_gkey *k_canon, uint64_t t0) {
    as_job *j = malloc(sizeof *j);
    if (!j) { gfx_build(g, len, sh, lay_mono, k_canon, 0, 0, t0); return; }
    j->next = NULL; j->g = g; j->len = len; memcpy(j->sh, sh, sizeof j->sh); j->lay = *lay_mono; j->k = *k_canon; j->k_canon = *k_canon; j->t0 = t0; j->cost = as_cost(sh); j->cs = 0;
    int d = atomic_fetch_add(&g_as_depth, 1) + 1, m = atomic_load(&g_as_depth_max);
    while (d > m && !atomic_compare_exchange_weak(&g_as_depth_max, &m, d)) {}
    atomic_fetch_add(&g_as_queued, 1);
    pthread_mutex_lock(&g_as_mu); *g_as_tail = j; g_as_tail = &j->next; pthread_cond_signal(&g_as_cv); pthread_mutex_unlock(&g_as_mu);
}
void vka_pso_async_line(char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (as_threads() <= 0) return;
    pthread_mutex_lock(&g_as_mu);
    uint64_t cn = g_as_cheap_n, on = g_as_old_n; double cl = cn ? g_as_lat_cheap_ns / 1e6 / cn : 0.0, ol = on ? g_as_lat_old_ns / 1e6 / on : 0.0;
    pthread_mutex_unlock(&g_as_mu);
    snprintf(out, n, "asynchronous: %llu queued, %llu made (%.0f ms in all, longest wait from the miss %.0f ms), deepest queue %d, draws skipped waiting %llu; "
             "background compiles %d at once (%s), their gate held the frame's %.0f ms and the speculative ones %.0f ms; "
             "taken %s: %llu cheapest (mean wait %.0f ms), %llu oldest (mean wait %.0f ms)",
             (unsigned long long)atomic_load(&g_as_queued), (unsigned long long)atomic_load(&g_as_made), atomic_load(&g_as_ns) / 1e6, atomic_load(&g_as_lat_max_ns) / 1e6,
             atomic_load(&g_as_depth_max), (unsigned long long)atomic_load(&g_as_skips), atomic_load(&g_gate_slots), pso_overlap() ? "ungated: DM_VK_PREWARM_OVERLAP=1" : "by memory, DM_VK_PSO_SLOTS",
             atomic_load(&g_gate_frame_wait_ns) / 1e6, atomic_load(&g_gate_spec_wait_ns) / 1e6,
             g_as_order ? "least cost first, the oldest in turn (DM_VK_PSO_ORDER)" : "in arrival order (DM_VK_PSO_ORDER=0)",
             (unsigned long long)cn, cl, (unsigned long long)on, ol);
}
static gval *gfx_obtain(const gkey_full *fk, uint64_t h, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay_mono, const vka_gkey *k_canon, int prewarm, int *made) {
    *made = 0;
    int async = !prewarm && !sh[7] && as_threads() > 0;
    pthread_mutex_lock(&g_mu);
    gval *g = km_get(&g_gfx, fk, h);
    if (g) {
        if (g->busy && (prewarm || async)) { pthread_mutex_unlock(&g_mu); return prewarm ? NULL : g; }
        if (g->busy) busy_wait(g);
        pthread_mutex_unlock(&g_mu);
        return g;
    }
    g = calloc(1, sizeof *g); g->busy = 1; g->pw = (uint8_t)(prewarm != 0); g->tid = prewarm ? gettid() : 0; km_put(&g_gfx, fk, h, g);
    pthread_mutex_unlock(&g_mu);
    uint64_t t_make = now_ns();
    if (async) {
        pc_open();
        VkPipeline p = VK_NULL_HANDLE;
        int linked = 0;
        pstate *P = malloc(sizeof *P);
        if (P && gpl_on() && !sh[7]) {   // (the cache-only try needs pipelineCreationCacheControl, which GPL is on with)
            if (g_gpl_cache_on && g_pc_mono && g_mono_cache_loaded && V.cache_ctl && pstate_fill_ex(P, sh, lay_mono, k_canon, 1)) {
                P->ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                uint64_t tm0 = now_ns();
                VkResult mr = vk.vkCreateGraphicsPipelines(V.dev, g_pc_mono, 1, &P->ci, NULL, &p);
                uint64_t mdns = now_ns() - tm0;
                P->ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                if (mr == VK_SUCCESS && p) {
                    pthread_mutex_lock(&g_mu);
                    g_ngfx++; g_made++; g_frame_made++; g_probe_hit++; g_create_ms += mdns / 1e6; g_create_ns += mdns;
                    pthread_mutex_unlock(&g_mu);
                    atomic_fetch_add_explicit(&g_ram_rehydrate, 1, memory_order_relaxed);
                } else {
                    p = VK_NULL_HANDLE;
                }
            }
            if (!p) {
                vka_gkey k_rt;
                runtime_gkey(sh, k_canon, &k_rt);
                if (V.gpl_indep && !V.gpl_lto && V.cache_ctl) {
                    vka_layout lay_indep;
                    if (vka_layout_of(sh, 0, &lay_indep) && pstate_fill_ex(P, sh, &lay_indep, &k_rt, 0)) {
                        uint64_t t0 = now_ns();
                        p = gpl_link(P, &k_rt, 1);
                        uint64_t dns = now_ns() - t0;
                        if (p) {
                            linked = 1;
                            pthread_mutex_lock(&g_mu);
                            g_ngfx++; g_frame_made++; g_create_ms += dns / 1e6; g_create_ns += dns;
                            pthread_mutex_unlock(&g_mu);
                        }
                    }
                } else if (!g_gpl_cache_on && pstate_fill_ex(P, sh, lay_mono, &k_rt, 0)) {
                    P->ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                    uint64_t t0 = now_ns();
                    VkResult r = vk.vkCreateGraphicsPipelines(V.dev, g_pc, 1, &P->ci, NULL, &p);
                    uint64_t dns = now_ns() - t0;
                    if (r != VK_SUCCESS) p = VK_NULL_HANDLE;
                    pthread_mutex_lock(&g_mu);
                    if (p) { g_ngfx++; g_made++; g_frame_made++; g_probe_hit++; g_create_ms += dns / 1e6; atomic_fetch_add_explicit(&g_ram_rehydrate, 1, memory_order_relaxed); } else { g_probe_miss++; g_probe_miss_ns += dns; }
                    g_create_ns += dns;
                    pthread_mutex_unlock(&g_mu);
                }
            }
        }
        free(P);
        if (p) {
            pw_note(VKA_PW_GFX, sh, k_canon, (uint32_t)(len - offsetof(gkey_full, k))); pw_explain(VKA_PW_GFX, sh, k_canon, (uint32_t)(len - offsetof(gkey_full, k)), (now_ns() - t_make) / 1e6, linked, " on the frame path");
            pthread_mutex_lock(&g_mu);
            if (linked) __atomic_store_n(&g->p_hybrid, p, __ATOMIC_RELEASE);
            else __atomic_store_n(&g->p_mono, p, __ATOMIC_RELEASE);
            __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE);
            pthread_cond_broadcast(&g_busy_cv);
            pthread_mutex_unlock(&g_mu);
            if (linked && (!V.gpl_indep || V.gpl_lto || (g_gpl_cache_on && VKA_ENV("DM_VK_GPL_OPT", 1)))) opt_queue(g, sh, lay_mono, k_canon);
            *made = 1;
            return g;
        }
        as_queue(g, len, sh, lay_mono, k_canon, t_make);
        *made = 1;
        return g;
    }
    gfx_build(g, len, sh, lay_mono, k_canon, prewarm, 0, t_make);
    *made = 1;
    return g;
}
// A new entry g (busy) made and published. async: a frame-path miss made on a background thread (as_main): linked from parts like the frame path, its time not the frame's
static void gfx_build(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay_mono, const vka_gkey *k_canon, int prewarm, int async, uint64_t t_make) {
    pc_open();
    VkPipeline p = VK_NULL_HANDLE; int linked = 0;
    pstate *P = malloc(sizeof *P);
    if (P) {
        if (!prewarm && gpl_on() && !sh[7]) {
            if (g_gpl_cache_on && g_pc_mono && g_mono_cache_loaded && V.cache_ctl && pstate_fill_ex(P, sh, lay_mono, k_canon, 1)) {
                P->ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                uint64_t tm0 = now_ns();
                VkResult mr = vk.vkCreateGraphicsPipelines(V.dev, g_pc_mono, 1, &P->ci, NULL, &p);
                uint64_t mdns = now_ns() - tm0;
                P->ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                if (mr == VK_SUCCESS && p) {
                    pthread_mutex_lock(&g_mu);
                    g_ngfx++; g_made++; if (!async) g_frame_made++; g_probe_hit++; g_create_ms += mdns / 1e6; if (!async) g_create_ns += mdns;
                    pthread_mutex_unlock(&g_mu);
                    atomic_fetch_add_explicit(&g_ram_rehydrate, 1, memory_order_relaxed);
                } else {
                    p = VK_NULL_HANDLE;
                }
            }
            if (!p) {
                vka_gkey k_rt;
                runtime_gkey(sh, k_canon, &k_rt);
                vka_layout lay_gpl = *lay_mono;
                int have_gpl_lay = V.gpl_indep ? vka_layout_of(sh, 0, &lay_gpl) : 1;
                if (have_gpl_lay && pstate_fill_ex(P, sh, &lay_gpl, &k_rt, 0)) {
                    if (async || (V.gpl_indep && !V.gpl_lto)) {
                        uint64_t t1 = now_ns();
                        p = gpl_link(P, &k_rt, 0);
                        uint64_t d2 = now_ns() - t1;
                        if (p) {
                            linked = 1;
                            pthread_mutex_lock(&g_mu);
                            g_ngfx++; if (!async) g_frame_made++; g_create_ms += d2 / 1e6; if (!async) g_create_ns += d2;
                            pthread_mutex_unlock(&g_mu);
                        } else if (!async) {
                            pthread_mutex_lock(&g_mu); g_create_ns += d2; pthread_mutex_unlock(&g_mu);
                        }
                    } else {
                        // The pipeline cache may already hold the optimized one (made in an earlier run): then it is made at once (a fraction of a millisecond), else linked from parts
                        P->ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                        uint64_t t0 = now_ns();
                        int bs = build_begin("graphics pipeline (cache probe)", 0, 0);
                        VkPipelineCache pc_probe = g_gpl_cache_on ? g_pc_mono : g_pc;
                        VkResult r = vk.vkCreateGraphicsPipelines(V.dev, pc_probe, 1, &P->ci, NULL, &p);
                        build_end(bs);
                        P->ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                        uint64_t dns = now_ns() - t0;
                        if (r == VK_SUCCESS && p) { pthread_mutex_lock(&g_mu); g_ngfx++; g_made++; g_frame_made++; g_probe_hit++; g_create_ms += dns / 1e6; g_create_ns += dns; pthread_mutex_unlock(&g_mu); atomic_fetch_add_explicit(&g_ram_rehydrate, 1, memory_order_relaxed); }
                        else {
                            pthread_mutex_lock(&g_mu); g_probe_miss++; g_probe_miss_ns += dns; g_create_ns += dns; pthread_mutex_unlock(&g_mu);
                            p = VK_NULL_HANDLE;
                            uint64_t t1 = now_ns();
                            p = gpl_link(P, &k_rt, 0);
                            if (p) { uint64_t d2 = now_ns() - t1; linked = 1; pthread_mutex_lock(&g_mu); g_ngfx++; g_frame_made++; g_create_ms += d2 / 1e6; g_create_ns += d2; pthread_mutex_unlock(&g_mu); }
                            else { uint64_t d2 = now_ns() - t1; pthread_mutex_lock(&g_mu); g_create_ns += d2; pthread_mutex_unlock(&g_mu); }
                        }
                    }
                }
            }
        }
        if (!p && !linked) {
            if (pstate_fill_ex(P, sh, lay_mono, k_canon, 1))
                p = full_pipeline(P, sh, prewarm ? " (prewarm)" : async ? " in the background (asynchronous)" : NULL);
        }
    }
    free(P);
    if (!prewarm) { int kind; vka_shader *s5[5]; pw_slots(sh, s5, &kind); pw_note(kind, s5, k_canon, (uint32_t)(len - offsetof(gkey_full, k)));
                    pw_explain(kind, s5, k_canon, (uint32_t)(len - offsetof(gkey_full, k)), (now_ns() - t_make) / 1e6, linked, async ? " in the background" : " on the frame path"); }
    pthread_mutex_lock(&g_mu);
    if (linked) __atomic_store_n(&g->p_hybrid, p, __ATOMIC_RELEASE);
    else __atomic_store_n(&g->p_mono, p, __ATOMIC_RELEASE);
    __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_busy_cv);
    pthread_mutex_unlock(&g_mu);
    if (linked && (!V.gpl_indep || V.gpl_lto || (g_gpl_cache_on && VKA_ENV("DM_VK_GPL_OPT", 1)))) opt_queue(g, sh, lay_mono, k_canon);
}
_Thread_local int vka_pso_wait;   // set by the pipeline calls of a draw or dispatch: the NULL they return is a pipeline still being made on the asynchronous threads (not a refusal), vka.h
static inline VkPipeline gval_use_ex(gval *g, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay_mono, int *out_hybrid, vka_layout *out_lay) {
    vka_pso_wait = 0;
    if (out_hybrid) *out_hybrid = 0;
    if (out_lay && lay_mono) *out_lay = *lay_mono;
    if (!g) return VK_NULL_HANDLE;
    VkPipeline pm = __atomic_load_n(&g->p_mono, __ATOMIC_ACQUIRE);
    if (pm) return pm;
    VkPipeline ph = __atomic_load_n(&g->p_hybrid, __ATOMIC_ACQUIRE);
    if (ph) {
        atomic_fetch_add_explicit(&g_hy_draws, 1, memory_order_relaxed);
        if (out_hybrid) *out_hybrid = 1;
        if (out_lay && V.gpl_indep) vka_layout_of(sh, 0, out_lay);
        return ph;
    }
    if (__atomic_load_n(&g->busy, __ATOMIC_ACQUIRE)) { atomic_fetch_add_explicit(&g_as_skips, 1, memory_order_relaxed); vka_pso_wait = 1; }
    return VK_NULL_HANDLE;
}
VkPipeline vka_gfx_pipeline_ex(vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay_mono, const vka_gkey *k, int *out_hybrid, vka_layout *out_lay) {
    static gkey_full fk, last; static gval *last_g;
    // Key on canonical state + compact monolithic layout so static non-EDS3 PSOs keep distinct states
    // while sharing entries cleanly with Boot #2+ prewarm and vk-psopt.
    size_t len = k->vin_static ? sizeof fk : offsetof(gkey_full, k.va);
    memset(&fk, 0, len);
    memcpy(fk.sh, sh, sizeof fk.sh); fk.layout = lay_mono->layout; memcpy(&fk.k, k, len - offsetof(gkey_full, k));
    if (last_g && !memcmp(&fk, &last, len)) return gval_use_ex(last_g, sh, lay_mono, out_hybrid, out_lay);
    int made;
    gval *g = gfx_obtain(&fk, fnv(&fk, len, 1469598103934665603ull), len, sh, lay_mono, k, 0, &made);
    memcpy(&last, &fk, len); last_g = g;
    return gval_use_ex(g, sh, lay_mono, out_hybrid, out_lay);
}
VkPipeline vka_gfx_pipeline(vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    int hy = 0; vka_layout out_lay, mlay;
    if (V.gpl_indep && lay && lay->indep && vka_layout_mono_of(sh, &mlay)) lay = &mlay;
    return vka_gfx_pipeline_ex(sh, lay, k, &hy, &out_lay);
}

// ---------- Compute pipelines ----------
typedef struct { vka_shader *cs; VkPipelineLayout layout; uint64_t umask, smask; } ckey;   // masks: storage images retyped for integer textures (vka_shader_img_module)
static kmap g_cs = {.klen = sizeof(ckey)};
// DM_VK_CS_ASYNC (live; default 1, effective only with DM_VK_PSO_ASYNC threads; 0 = the frame waits): a compute pipeline a dispatch needs and the map lacks is made on the
// asynchronous threads (as_main, the graphics misses' queue and its order: least predicted cost first, a .heavy shader last, DM_VK_PSO_ORDER), its dispatches skipped until
// it is there, as the graphics misses' draws are. With pipelineCreationCacheControl the frame path first asks the pipeline cache alone (FAIL_ON_PIPELINE_COMPILE_REQUIRED):
// a pipeline an earlier run made costs no skip. Until 2026-11-15 compute pipelines were always made on the frame path: Ocarina of Time's effect shaders (3,587 compute
// programs in cs_effect.bin, too many to prewarm them all) took 3.5-4.4 s each on Qualcomm's driver the first time an effect appeared (9e52219432f52a98, the Deku Seed
// shot: a 3.7 s frame mid-play). A skipped dispatch leaves its outputs as they were: a particle effect is missing or frozen for those frames; work the game keeps (a
// buffer written once, indirect arguments) stays stale until dispatched again. Pipelines with retyped images (umask / smask, vka_draw.c img_retype) go the same way: their
// dispatches are skipped until they are made, never run with the base pipeline (which would read the integer texels as floats). Until 2026-11-18 they were made on the
// frame path, and the base pipeline that prewarm or this queue had made was never dispatched: Pokémon Pokopia's particle and light compute shaders (6bef3d23a8ddeafd,
// af8f64d9f5542ef2 and others, 1-2.5 s each on Turnip) froze the game for about 5 s twice in its first minutes, every run (their records now carry the masks: pw_note_cs)
static int cs_async_on(void) { return VKA_ENV("DM_VK_CS_ASYNC", 1) && as_threads() > 0; }
static void cs_queue(gval *g, vka_shader *cs, const vka_layout *lay, VkShaderModule csm, uint64_t umask, uint64_t smask) {
    as_job *j = calloc(1, sizeof *j);
    if (!j) { cs_build(g, cs, lay, csm, umask, smask, 0, 0); return; }
    j->g = g; j->sh[5] = cs; j->lay = *lay; j->t0 = now_ns(); j->cost = as_cost(j->sh); j->cs = 1; j->umask = umask; j->smask = smask;
    int d = atomic_fetch_add(&g_as_depth, 1) + 1, m = atomic_load(&g_as_depth_max);
    while (d > m && !atomic_compare_exchange_weak(&g_as_depth_max, &m, d)) {}
    atomic_fetch_add(&g_as_queued, 1);
    pthread_mutex_lock(&g_as_mu); *g_as_tail = j; g_as_tail = &j->next; pthread_cond_signal(&g_as_cv); pthread_mutex_unlock(&g_as_mu);
}
static VkPipeline cs_obtain(vka_shader *cs, const vka_layout *lay, int prewarm, int *made, uint64_t umask, uint64_t smask, int frame) {
    *made = 0; vka_pso_wait = 0;
    if (VKA_ENV("DM_VK_SKIP_NDVE", 1) && vka_is_ndve_key(cs->key)) return VK_NULL_HANDLE;
    VkShaderModule csm = umask | smask ? vka_shader_img_module(cs, umask, smask) : vka_shader_cs_module(cs);
    if (!csm) return VK_NULL_HANDLE;   // (a retyped module that cannot be made: the caller takes the base pipeline)
    ckey k; memset(&k, 0, sizeof k); k.cs = cs; k.layout = lay->layout; k.umask = umask; k.smask = smask;
    uint64_t h = fnv(&k, sizeof k, 1469598103934665603ull);
    int async = frame && !prewarm && cs_async_on();
    pthread_mutex_lock(&g_mu);
    gval *g = km_get(&g_cs, &k, h);
    if (g) {
        if (g->busy && (prewarm || async)) {   // (a frame dispatch: skipped while another thread makes it)
            pthread_mutex_unlock(&g_mu);
            if (async) { atomic_fetch_add_explicit(&g_as_skips, 1, memory_order_relaxed); vka_pso_wait = 1; }
            return VK_NULL_HANDLE;
        }
        if (g->busy) busy_wait(g);
        VkPipeline p = g->p_mono; pthread_mutex_unlock(&g_mu); return p;
    }
    g = calloc(1, sizeof *g); g->busy = 1; g->pw = (uint8_t)(prewarm != 0); g->tid = prewarm ? gettid() : 0; km_put(&g_cs, &k, h, g);
    pthread_mutex_unlock(&g_mu);
    *made = 1;
    if (async) {
        pc_open();
        uint32_t smax = V.props.limits.maxComputeSharedMemorySize;
        if (V.cache_ctl && !(smax && cs->e->shared > smax)) {   // the cache-only try (one over the shared memory limit: cs_build says why it is not made)
            uint32_t pin = (cs->e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(DMSH_VK_STAGE_CS) : 0;
            VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
            int spill = 0; for (unsigned i = 0; i < cs->e->bind_count; i++) if (cs->b[i].kind == DMSH_VK_KIND_SHARED_SPILL) spill = 1;
            VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .flags = (spill ? VK_PIPELINE_CREATE_DISPATCH_BASE_BIT : 0) | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT,
                .layout = lay->layout, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = csm, .pName = "main"}};
            int shard_slot = -1;
            VkPipelineCache pc_use = ((umask | smask) && g_pc_retype) ? g_pc_retype
                                   : g_gpl_cache_on ? shard_pc_acquire(cs->key, &shard_slot)
                                   : g_pc;
            VkPipeline p = VK_NULL_HANDLE; uint64_t t0 = now_ns(); VkResult r = VK_ERROR_UNKNOWN;
            if ((umask | smask) && g_pc_retype && pc_use == g_pc_retype && g_gpl_cache_on) {
                r = vk.vkCreateComputePipelines(V.dev, g_pc_retype, 1, &ci, NULL, &p);   // 1: delta retype cache
                if (r != VK_SUCCESS || !p) {
                    p = VK_NULL_HANDLE;
                    VkPipelineCache shard_pc = shard_pc_acquire(cs->key, &shard_slot);
                    if (shard_pc != VK_NULL_HANDLE) {
                        r = vk.vkCreateComputePipelines(V.dev, shard_pc, 1, &ci, NULL, &p);   // 2: Boot #1 mini-shard
                        shard_pc_release(shard_slot); shard_slot = -1;
                        if (r == VK_SUCCESS && p) atomic_fetch_add_explicit(&g_retype_shard_hit, 1, memory_order_relaxed);
                        else { atomic_fetch_add_explicit(&g_retype_shard_miss, 1, memory_order_relaxed); p = VK_NULL_HANDLE; }
                    } else atomic_fetch_add_explicit(&g_retype_shard_miss, 1, memory_order_relaxed);
                }
            } else r = vk.vkCreateComputePipelines(V.dev, pc_use, 1, &ci, NULL, &p);
            shard_pc_release(shard_slot);   // always; -1 is a no-op
            uint64_t dns = now_ns() - t0;
            if (r != VK_SUCCESS) p = VK_NULL_HANDLE;
            pthread_mutex_lock(&g_mu);
            g_create_ns += dns;
            if (p) { g_ncs++; g_made++; g_frame_made++; g_probe_hit++; g_create_ms += dns / 1e6; g->p_mono = p; g->busy = 0; pthread_cond_broadcast(&g_busy_cv); atomic_fetch_add_explicit(&g_ram_rehydrate, 1, memory_order_relaxed); }
            else { g_probe_miss++; g_probe_miss_ns += dns; }
            pthread_mutex_unlock(&g_mu);
            if (p) { pw_note_cs(cs, umask, smask); return p; }
        }
        cs_queue(g, cs, lay, csm, umask, smask);
        atomic_fetch_add_explicit(&g_as_skips, 1, memory_order_relaxed); vka_pso_wait = 1;
        return VK_NULL_HANDLE;
    }
    cs_build(g, cs, lay, csm, umask, smask, prewarm, 0);
    return __atomic_load_n(&g->p_mono, __ATOMIC_ACQUIRE);
}
// A new compute entry g (busy) made and published. async: a frame-path miss made on an asynchronous thread (cs_queue), its time not the frame's
static void cs_build(gval *g, vka_shader *cs, const vka_layout *lay, VkShaderModule csm, uint64_t umask, uint64_t smask, int prewarm, int async) {
    pc_open();
    uint64_t t0 = now_ns();
    uint32_t pin = (cs->e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(DMSH_VK_STAGE_CS) : 0;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
    int spill = 0;   // shared memory in the spill buffer (vka_draw.c spill_dispatch): dispatched in parts with a base workgroup
    for (unsigned i = 0; i < cs->e->bind_count; i++) if (cs->b[i].kind == DMSH_VK_KIND_SHARED_SPILL) spill = 1;
    VkPipelineCreationFeedback fb_all = {0}, fb_st = {0};
    VkPipelineCreationFeedbackCreateInfo fbi = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, .pPipelineCreationFeedback = &fb_all,
                                                .pipelineStageCreationFeedbackCount = 1, .pPipelineStageCreationFeedbacks = &fb_st};
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .pNext = &fbi, .flags = spill ? VK_PIPELINE_CREATE_DISPATCH_BASE_BIT : 0, .layout = lay->layout,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = csm, .pName = "main"}};
    VkPipeline p = VK_NULL_HANDLE; VkResult r; int over = 0, from_shard = 0;
    uint32_t smax = V.props.limits.maxComputeSharedMemorySize;
    int shard_slot = -1;
    VkPipelineCache pc_use = ((umask | smask) && g_pc_retype) ? g_pc_retype
                           : g_gpl_cache_on ? shard_pc_acquire(cs->key, &shard_slot)
                           : g_pc;
    if (smax && cs->e->shared > smax && !VKA_ENV("DM_VK_CS_SHARED_OVER", 0)) {
        r = VK_ERROR_FEATURE_NOT_PRESENT; over = 1;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("compute program %016llx takes %u bytes of shared memory, the device at most %u: not made, its dispatches are skipped (the first 16 are logged; the count is in the pipeline line)", (unsigned long long)cs->key, cs->e->shared, smax);
    } else if (!csm) { r = VK_ERROR_INITIALIZATION_FAILED; }
    else {
        int bs = build_begin("compute pipeline", cs->key, 0);
        if ((umask | smask) && g_pc_retype && pc_use == g_pc_retype && g_gpl_cache_on && V.cache_ctl) {
            r = retype_probe_cs(&ci, g_pc_retype, &p);                  // 1: delta retype cache
            if (r != VK_SUCCESS || !p) {
                p = VK_NULL_HANDLE;
                VkPipelineCache shard_pc = shard_pc_acquire(cs->key, &shard_slot);
                if (shard_pc != VK_NULL_HANDLE) {
                    r = retype_probe_cs(&ci, shard_pc, &p);            // 2: Boot #1 mini-shard
                    shard_pc_release(shard_slot); shard_slot = -1;
                    if (r == VK_SUCCESS && p) { atomic_fetch_add_explicit(&g_retype_shard_hit, 1, memory_order_relaxed); from_shard = 1; }
                    else { atomic_fetch_add_explicit(&g_retype_shard_miss, 1, memory_order_relaxed); p = VK_NULL_HANDLE; }
                } else atomic_fetch_add_explicit(&g_retype_shard_miss, 1, memory_order_relaxed);
            }
        }
        if (!from_shard && !(r == VK_SUCCESS && p)) r = vk.vkCreateComputePipelines(V.dev, pc_use, 1, &ci, NULL, &p);   // 3: live compile
        build_end(bs);
    }
    shard_pc_release(shard_slot);   // always; -1 is a no-op
    uint64_t dns = now_ns() - t0;
    if (!prewarm && !r && p && !from_shard) {
        int rehy = record_compile_outcome(&fb_all, &fb_st, 1, pc_use != VK_NULL_HANDLE, dns, "compute pipeline", cs->key, 0);
        if ((umask | smask) != 0) {
            vka_svar_rec vr = {0};
            vr.shader_hash = cs->key;
            vr.stage = DMSH_VK_STAGE_CS;
            vr.tex_u = umask;
            vr.tex_s = smask;
            svar_record(&vr);
            if (!rehy && pc_use == g_pc_retype) atomic_store_explicit(&g_retype_dirty, 1, memory_order_relaxed);
        }
    }
    pthread_mutex_lock(&g_mu);
    if (over) { p = VK_NULL_HANDLE; g_ncs_over++; } else if (r) { p = VK_NULL_HANDLE; g_nfail++; } else { g_ncs++; g_made++; if (!prewarm && !async) g_frame_made++; }
    { g_create_ms += dns / 1e6; if (!prewarm && !async) g_create_ns += dns; }
    __atomic_store_n(&g->p_mono, p, __ATOMIC_RELEASE); __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE); pthread_cond_broadcast(&g_busy_cv);
    pthread_mutex_unlock(&g_mu);
    if (r && !over) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("compute pipeline %016llx: %d; its dispatches are skipped", (unsigned long long)cs->key, r); }
    { double ms_ = (now_ns() - t0) / 1e6; static _Atomic int n; if (!r && ms_ > 50 && atomic_fetch_add(&n, 1) < 200) VLOG("compute pipeline %016llx took %.0f ms%s", (unsigned long long)cs->key, ms_, prewarm ? " (prewarm)" : async ? " in the background (asynchronous)" : "");
      // (noted before the save, so the save that follows a long one has its key: until 2026-11-15 the save came first and the key waited for the next one)
      if (!prewarm && !over) pw_note_cs(cs, umask, smask);   // (one not made stays out of the prewarm list)
      if (!prewarm && !r && ms_ > 1000) vka_pso_save(2); }
}
VkPipeline vka_cs_pipeline(vka_shader *cs, const vka_layout *lay) { int made; return cs_obtain(cs, lay, 0, &made, 0, 0, 0); }
VkPipeline vka_cs_pipeline_frame(vka_shader *cs, const vka_layout *lay) { int made; return cs_obtain(cs, lay, 0, &made, 0, 0, 1); }
VkPipeline vka_cs_pipeline_img(vka_shader *cs, const vka_layout *lay, uint64_t umask, uint64_t smask) { int made; return cs_obtain(cs, lay, 0, &made, umask, smask, 1); }
#endif
