// Extended dynamic state (EDS2 / EDS3) capability mask, canonical -> runtime pipeline key masking, and per-command-buffer dirty-state bits (vka_dev.c, vka_pso.c, vka_draw.c).
// Vulkan-free so host unit tests (dm/host/tests/t_android_eds.c) can test key masking and dirty-bit transitions directly.
#ifndef VKA_EDS_H
#define VKA_EDS_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "vka_gkey.h"

// Optional dynamic states beyond core Vulkan 1.3 EDS1/EDS2 (stored in vka_dev.eds_mask)
enum {
    VKA_EDS_LOGIC_OP           = 1u << 0,    // VK_EXT_extended_dynamic_state2: extendedDynamicState2LogicOp
    VKA_EDS_PATCH_CP           = 1u << 1,    // VK_EXT_extended_dynamic_state2: extendedDynamicState2PatchControlPoints
    VKA_EDS_DEPTH_CLAMP        = 1u << 2,    // VK_EXT_extended_dynamic_state3: depthClampEnable
    VKA_EDS_POLYGON_MODE       = 1u << 3,    // VK_EXT_extended_dynamic_state3: polygonMode
    VKA_EDS_RAST_SAMPLES       = 1u << 4,    // VK_EXT_extended_dynamic_state3: rasterizationSamples
    VKA_EDS_SAMPLE_MASK        = 1u << 5,    // VK_EXT_extended_dynamic_state3: sampleMask
    VKA_EDS_ALPHA_TO_COV       = 1u << 6,    // VK_EXT_extended_dynamic_state3: alphaToCoverageEnable
    VKA_EDS_LOGIC_OP_ENABLE    = 1u << 7,    // VK_EXT_extended_dynamic_state3: logicOpEnable
    VKA_EDS_COLOR_BLEND_ENABLE = 1u << 8,    // VK_EXT_extended_dynamic_state3: colorBlendEnable
    VKA_EDS_COLOR_BLEND_EQ     = 1u << 9,    // VK_EXT_extended_dynamic_state3: colorBlendEquation
    VKA_EDS_COLOR_WRITE_MASK   = 1u << 10,   // VK_EXT_extended_dynamic_state3: colorWriteMask
    VKA_EDS_TOPO_UNRESTRICTED  = 1u << 11,   // dynamicPrimitiveTopologyUnrestricted: non-tessellation topology is dynamic

    VKA_EDS2_MASK = VKA_EDS_LOGIC_OP | VKA_EDS_PATCH_CP,
    VKA_EDS3_MASK = VKA_EDS_DEPTH_CLAMP | VKA_EDS_POLYGON_MODE | VKA_EDS_RAST_SAMPLES | VKA_EDS_SAMPLE_MASK |
                    VKA_EDS_ALPHA_TO_COV | VKA_EDS_LOGIC_OP_ENABLE | VKA_EDS_COLOR_BLEND_ENABLE |
                    VKA_EDS_COLOR_BLEND_EQ | VKA_EDS_COLOR_WRITE_MASK,
};

// Produces the runtime pipeline key *out from the canonical key *canon by zeroing every render-state field handled dynamically by eds_mask.
// keep_a2c: preserve canon->a2c when a fragment shader has a pool-feedback early/late depth twin that depends on alphaToCoverage.
static inline void vka_eds_runtime_key(const vka_gkey *canon, uint32_t eds_mask, int keep_a2c, vka_gkey *out) {
    size_t n = canon->vin_static ? sizeof *out : offsetof(vka_gkey, va);
    memset(out, 0, sizeof *out);
    memcpy(out, canon, n);
    if ((eds_mask & VKA_EDS_TOPO_UNRESTRICTED) && canon->topo != 10 /* VK_PRIMITIVE_TOPOLOGY_PATCH_LIST */) out->topo = 0;
    if (eds_mask & VKA_EDS_DEPTH_CLAMP) out->depth_clamp = 0;
    if (eds_mask & VKA_EDS_POLYGON_MODE) out->poly_mode = 0;
    if (eds_mask & VKA_EDS_RAST_SAMPLES) out->samples = 0;
    if ((eds_mask & VKA_EDS_ALPHA_TO_COV) && !keep_a2c) out->a2c = 0;
    if (eds_mask & VKA_EDS_LOGIC_OP_ENABLE) out->logic_en = 0;
    if (eds_mask & VKA_EDS_LOGIC_OP) out->logic_op = 0;
    for (int i = 0; i < 8; i++) {
        if (eds_mask & VKA_EDS_COLOR_BLEND_ENABLE) out->att[i].en = 0;
        if (eds_mask & VKA_EDS_COLOR_BLEND_EQ) {
            out->att[i].src = 0; out->att[i].dst = 0; out->att[i].op = 0;
            out->att[i].srca = 0; out->att[i].dsta = 0; out->att[i].opa = 0;
        }
        if (eds_mask & VKA_EDS_COLOR_WRITE_MASK) out->att[i].mask = 0;
    }
}

// Per-command-buffer dynamic state dirty bits (vka_draw.c DY.dirty)
enum {
    VKA_DYN_VP          = 1ull << 0,
    VKA_DYN_SC          = 1ull << 1,
    VKA_DYN_CULL        = 1ull << 2,
    VKA_DYN_FF          = 1ull << 3,
    VKA_DYN_TOPO        = 1ull << 4,
    VKA_DYN_DT          = 1ull << 5,
    VKA_DYN_DW          = 1ull << 6,
    VKA_DYN_DC          = 1ull << 7,
    VKA_DYN_DBT         = 1ull << 8,
    VKA_DYN_STT         = 1ull << 9,
    VKA_DYN_SOP         = 1ull << 10,
    VKA_DYN_SMASK       = 1ull << 11,
    VKA_DYN_DBE         = 1ull << 12,
    VKA_DYN_DB          = 1ull << 13,
    VKA_DYN_RD          = 1ull << 14,
    VKA_DYN_BC          = 1ull << 15,
    VKA_DYN_LW          = 1ull << 16,
    VKA_DYN_PR          = 1ull << 17,
    VKA_DYN_FBL         = 1ull << 18,
    VKA_DYN_DCLAMP      = 1ull << 19,
    VKA_DYN_PMODE       = 1ull << 20,
    VKA_DYN_PATCH_CP    = 1ull << 21,
    VKA_DYN_RSAMP       = 1ull << 22,
    VKA_DYN_SAMPLE_MASK = 1ull << 23,
    VKA_DYN_A2C         = 1ull << 24,
    VKA_DYN_LOGEN       = 1ull << 25,
    VKA_DYN_LOGOP       = 1ull << 26,
    VKA_DYN_BLEND_EN    = 1ull << 27,
    VKA_DYN_BLEND_EQ    = 1ull << 28,
    VKA_DYN_WRITE_MASK  = 1ull << 29,
    VKA_DYN_ALL         = (1ull << 30) - 1ull,
};

static inline void vka_dyn_dirty_all(uint64_t *dirty) { *dirty = VKA_DYN_ALL; }
static inline int vka_dyn_is_dirty(uint64_t dirty, uint64_t bit) { return (dirty & bit) != 0; }
static inline void vka_dyn_clear(uint64_t *dirty, uint64_t bit) { *dirty &= ~bit; }

#endif
