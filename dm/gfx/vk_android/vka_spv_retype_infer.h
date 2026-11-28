// Static heuristic scanner for the SPIR-V retypes the replay can apply at pipeline time.
// Pure C, no Vulkan headers: usable by dm/host/tests/t_android_gpl_cache.c and the Android
// replay. Every VSI_* constant is prefixed so this header can be included beside spirv.h
// or a local enum. The scanner only *infers* candidates; it never edits the words it reads.
#ifndef VKA_SPV_RETYPE_INFER_H
#define VKA_SPV_RETYPE_INFER_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t vin_u, vin_s;   // vertex input location bits 0..31 (disjoint)
    uint32_t fout_u, fout_s; // fragment output location bits 0..7 (disjoint)
    uint64_t tex_u, tex_s;   // stage-relative binding bits 0..63 (disjoint)
} vka_spv_retype_hint;

enum {
    VSI_OpUndef = 1, VSI_OpName = 5, VSI_OpMemberName = 6, VSI_OpExtInstImport = 11, VSI_OpExtInst = 12,
    VSI_OpMemoryModel = 14, VSI_OpEntryPoint = 15, VSI_OpExecutionMode = 16, VSI_OpCapability = 17,
    VSI_OpTypeInt = 21, VSI_OpTypeFloat = 22, VSI_OpTypeVector = 23, VSI_OpTypeImage = 25,
    VSI_OpTypeSampledImage = 27, VSI_OpTypePointer = 32, VSI_OpTypeFunction = 33,
    VSI_OpConstant = 43, VSI_OpConstantComposite = 44, VSI_OpConstantNull = 46,
    VSI_OpFunction = 54, VSI_OpFunctionEnd = 56, VSI_OpVariable = 59, VSI_OpLoad = 61, VSI_OpStore = 62,
    VSI_OpAccessChain = 65, VSI_OpInBoundsAccessChain = 66, VSI_OpVectorShuffle = 79,
    VSI_OpCompositeConstruct = 80, VSI_OpCompositeExtract = 81, VSI_OpCompositeInsert = 82,
    VSI_OpCopyObject = 83, VSI_OpSampledImage = 86, VSI_OpImageSampleImplicitLod = 87,
    VSI_OpImageSampleExplicitLod = 88, VSI_OpImageFetch = 95, VSI_OpImageGather = 96,
    VSI_OpImageRead = 98, VSI_OpImageWrite = 99, VSI_OpImage = 100,
    VSI_OpConvertFToU = 109, VSI_OpConvertFToS = 110, VSI_OpConvertSToF = 111, VSI_OpConvertUToF = 112,
    VSI_OpFNegate = 127, VSI_OpFAdd = 129, VSI_OpFSub = 131, VSI_OpFMul = 133, VSI_OpFDiv = 136,
    VSI_OpFRem = 137, VSI_OpFMod = 141, VSI_OpVectorTimesScalar = 142, VSI_OpDot = 148,
    VSI_OpBitcast = 124, VSI_OpSDiv = 135, VSI_OpSRem = 138, VSI_OpSMod = 139,
    VSI_OpSGreaterThan = 173, VSI_OpSGreaterThanEqual = 175, VSI_OpSLessThan = 177,
    VSI_OpSLessThanEqual = 179, VSI_OpShiftRightArithmetic = 195, VSI_OpSelect = 169,
    VSI_OpLoopMerge = 246, VSI_OpSelectionMerge = 247, VSI_OpLabel = 248, VSI_OpBranch = 249,
    VSI_OpBranchConditional = 250, VSI_OpSwitch = 251, VSI_OpKill = 252, VSI_OpReturn = 253,
    VSI_OpReturnValue = 254, VSI_OpUnreachable = 255,
    VSI_OpDecorate = 71, VSI_OpMemberDecorate = 72, VSI_OpDecorateId = 332,
    VSI_OpDecorateString = 5632, VSI_OpMemberDecorateString = 5633,
    VSI_DecLocation = 30, VSI_DecBinding = 33, VSI_DecDescriptorSet = 34,
    VSI_SCUniformConstant = 0, VSI_SCInput = 1, VSI_SCOutput = 3, VSI_SCFunction = 7,
    VSI_NONE = 0xffu, VSI_AMBIG = 0xfeu
};

static inline int vka_spv_signed_consumer(uint32_t op) {
    return op == VSI_OpSDiv || op == VSI_OpSRem || op == VSI_OpSMod || op == VSI_OpConvertSToF ||
           op == VSI_OpShiftRightArithmetic || op == VSI_OpSLessThan || op == VSI_OpSGreaterThan ||
           op == VSI_OpSLessThanEqual || op == VSI_OpSGreaterThanEqual;
}

// vka_spvpatch.c:551-557's texel_read_op(): reads whose result is the texel vector.
static inline int vka_spv_texel_read_op(uint32_t op) {
    return op == VSI_OpImageFetch || op == VSI_OpImageRead || op == VSI_OpImageSampleImplicitLod ||
           op == VSI_OpImageSampleExplicitLod || op == VSI_OpImageGather;
}

// Genuine float arithmetic / conversion / comparison. The propagation ops (Bitcast,
// CopyObject, Composite*, Select, Store) are handled before this test and are not listed.
static inline int vka_spv_float_consumer(uint32_t op) {
    return op == VSI_OpFNegate || op == VSI_OpFAdd || op == VSI_OpFSub || op == VSI_OpFMul ||
           op == VSI_OpFDiv || op == VSI_OpFRem || op == VSI_OpFMod || op == VSI_OpVectorTimesScalar ||
           op == VSI_OpDot || op == VSI_OpConvertFToU || op == VSI_OpConvertFToS ||
           (op >= 180 && op <= 191) || op == VSI_OpExtInst;   // OpFOrdEqual .. OpFUnordGreaterThanEqual
}

// w: SPIR-V words, nw: word count, tex_base: first binding of the stage's 64-binding range.
// want_vin / want_tex / want_fout select which families to look for. 1 = a hint was found.
static inline int vka_spv_infer_retype(const uint32_t *w, size_t nw, uint32_t tex_base,
                                       int want_vin, int want_tex, int want_fout,
                                       vka_spv_retype_hint *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!w || nw < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 20)) return 0;

    // One heap block: six uint32_t tables for 4-byte alignment, then eighteen uint8_t tables.
    // Never a stack VLA, and one free() on every path.
    size_t nb = bound;
    void *buf = malloc(nb * (6 * sizeof(uint32_t) + 18));
    if (!buf) goto done;
    uint32_t *loc = (uint32_t *)buf, *bind = loc + nb, *set = bind + nb, *si_img = set + nb;
    uint32_t *ptr_ty = si_img + nb, *rt = ptr_ty + nb;
    uint8_t *fty = (uint8_t *)(rt + nb), *ity = fty + nb, *isgn = ity + nb;
    uint8_t *img_float = isgn + nb, *ptr_sc = img_float + nb, *p_vin = ptr_sc + nb;
    uint8_t *p_fout = p_vin + nb, *p_tex = p_fout + nb, *v_vin = p_tex + nb;
    uint8_t *tex_obj = v_vin + nb, *tex_fetch = tex_obj + nb, *c_vin = tex_fetch + nb;
    uint8_t *c_tex = c_vin + nb, *c_sgn = c_tex + nb, *ff_int = c_sgn + nb;
    uint8_t *loc_vin = ff_int + nb, *loc_tex = loc_vin + nb, *local_ptr = loc_tex + nb;
    for (size_t i = 0; i < nb; i++) {
        // vka_spvpatch.c:568 leaves an undecorated DescriptorSet as ~0u and vka_spvpatch.c:600
        // requires set == 0: an undecorated binding is NOT set 0, so keep ~0u here too.
        loc[i] = bind[i] = set[i] = ~0u;
        si_img[i] = ptr_ty[i] = rt[i] = 0;
        fty[i] = ity[i] = isgn[i] = img_float[i] = 0;
        ptr_sc[i] = VSI_NONE;
        p_vin[i] = p_fout[i] = p_tex[i] = VSI_NONE;
        v_vin[i] = tex_obj[i] = tex_fetch[i] = VSI_NONE;
        c_vin[i] = c_tex[i] = ff_int[i] = VSI_NONE;
        c_sgn[i] = 0;
        loc_vin[i] = loc_tex[i] = VSI_NONE;
        local_ptr[i] = 0;
    }

#define VSI_LEN_OK(I, LEN) do { \
        uint32_t len_ = (LEN); \
        if (!len_ || len_ > nw - (I)) goto done; \
    } while (0)
// rt[] maps an SSA id to its result TYPE. Only int-typed results are useful (the fout
// bitcast source); recording a merge/branch/store/type instruction there would corrupt it.
#define VSI_RT_CONST(I, OP, LEN) do { \
        uint32_t i_ = (uint32_t)(I), op_ = (OP), len_ = (LEN); \
        if (len_ >= 3 && w[i_ + 1] < bound && w[i_ + 2] < bound && ity[w[i_ + 1]] && \
            (op_ == VSI_OpConstant || op_ == VSI_OpConstantComposite || \
             op_ == VSI_OpConstantNull || op_ == VSI_OpUndef)) \
            rt[w[i_ + 2]] = w[i_ + 1]; \
    } while (0)
#define VSI_RT_BODY(I, OP, LEN) do { \
        uint32_t i_ = (uint32_t)(I), op_ = (OP), len_ = (LEN); \
        if (len_ >= 3 && w[i_ + 1] < bound && w[i_ + 2] < bound && ity[w[i_ + 1]] && \
            op_ != VSI_OpStore && op_ != VSI_OpImageWrite && op_ != VSI_OpDecorate && \
            op_ != VSI_OpMemberDecorate && op_ != VSI_OpName && op_ != VSI_OpMemberName && \
            op_ != VSI_OpEntryPoint && op_ != VSI_OpExecutionMode && op_ != VSI_OpFunction && \
            op_ != VSI_OpFunctionEnd && op_ != VSI_OpTypeInt && op_ != VSI_OpTypeFloat && \
            op_ != VSI_OpTypeVector && op_ != VSI_OpTypeImage && op_ != VSI_OpTypeSampledImage && \
            op_ != VSI_OpTypePointer && op_ != VSI_OpTypeFunction && op_ != VSI_OpCapability && \
            op_ != VSI_OpMemoryModel && op_ != VSI_OpExtInstImport && op_ != VSI_OpLoopMerge && \
            op_ != VSI_OpSelectionMerge && op_ != VSI_OpLabel && op_ != VSI_OpBranch && \
            op_ != VSI_OpBranchConditional && op_ != VSI_OpSwitch && op_ != VSI_OpKill && \
            op_ != VSI_OpReturn && op_ != VSI_OpReturnValue && op_ != VSI_OpUnreachable) \
            rt[w[i_ + 2]] = w[i_ + 1]; \
    } while (0)

    // Pass 1: the whole word stream is length-checked; declarations are read up to the first function.
    size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        VSI_LEN_OK(i, len);
        if (op == VSI_OpFunction && first_fn == nw) first_fn = i;
        if (i < first_fn || op == VSI_OpFunction) {
            if (op == VSI_OpDecorate && len >= 4 && w[i + 1] < bound) {
                if (w[i + 2] == VSI_DecLocation) loc[w[i + 1]] = w[i + 3];
                else if (w[i + 2] == VSI_DecBinding) bind[w[i + 1]] = w[i + 3];
                else if (w[i + 2] == VSI_DecDescriptorSet) set[w[i + 1]] = w[i + 3];
            } else if (op == VSI_OpTypeInt && len >= 4 && w[i + 2] == 32 && w[i + 1] < bound) {
                ity[w[i + 1]] = 1; isgn[w[i + 1]] = (uint8_t)(w[i + 3] ? 1 : 0);
            } else if (op == VSI_OpTypeFloat && len >= 3 && w[i + 2] == 32 && w[i + 1] < bound) {
                fty[w[i + 1]] = 1;
            } else if (op == VSI_OpTypeVector && len >= 4 && w[i + 1] < bound && w[i + 3] >= 2 && w[i + 3] <= 4) {
                uint32_t el = w[i + 2];
                if (el < bound && fty[el]) fty[w[i + 1]] = (uint8_t)w[i + 3];
                if (el < bound && ity[el]) { ity[w[i + 1]] = (uint8_t)w[i + 3]; isgn[w[i + 1]] = isgn[el]; }
            } else if (op == VSI_OpTypeImage && len >= 9 && w[i + 1] < bound) {
                // sampled type is w[i+2], as in vka_spvpatch.c's img[k].f[0]
                img_float[w[i + 1]] = (uint8_t)(w[i + 2] < bound && fty[w[i + 2]]);
            } else if (op == VSI_OpTypeSampledImage && len >= 3 && w[i + 1] < bound && w[i + 2] < bound) {
                si_img[w[i + 1]] = w[i + 2];
            } else if (op == VSI_OpTypePointer && len >= 4 && w[i + 1] < bound && w[i + 3] < bound) {
                ptr_sc[w[i + 1]] = (uint8_t)w[i + 2]; ptr_ty[w[i + 1]] = w[i + 3];
            } else if (op == VSI_OpVariable && len >= 4 && w[i + 1] < bound && w[i + 2] < bound) {
                uint32_t pt = w[i + 1], id = w[i + 2], sc = w[i + 3];
                uint32_t ty = (pt < bound && ptr_sc[pt] == sc) ? ptr_ty[pt] : 0;
                if (ty >= bound) ty = 0;
                if (want_vin && sc == VSI_SCInput && loc[id] < 32 && fty[ty]) p_vin[id] = (uint8_t)loc[id];
                else if (want_fout && sc == VSI_SCOutput && loc[id] < 8 && fty[ty]) p_fout[id] = (uint8_t)loc[id];
                else if (want_tex && sc == VSI_SCUniformConstant && set[id] == 0 && bind[id] != ~0u &&
                         bind[id] >= tex_base && bind[id] - tex_base < 64) {
                    uint32_t im = ty;
                    if (im && si_img[im] && si_img[im] < bound) im = si_img[im];   // combined image + sampler
                    if (im && im < bound && img_float[im]) p_tex[id] = (uint8_t)(bind[id] - tex_base);
                }
            }
        }
        VSI_RT_CONST(i, op, len);
        i += len;
    }

    uint32_t vu = 0, vs = 0, fu = 0, fs = 0, vin_float_used = 0;
    uint64_t tu = 0, ts = 0, tex_float_used = 0;

    // Pass 2: function bodies, in instruction order (SSA definitions precede their uses).
    // The second iteration carries SC_FUNCTION local provenance back across branches/loops.
    for (int pass = 0; pass < 2; pass++) {
    for (size_t i = first_fn; i < nw;) {
        uint32_t op = w[i] & 0xffffu, len = w[i] >> 16;
        VSI_LEN_OK(i, len);
        VSI_RT_BODY(i, op, len);
        if (op == VSI_OpCopyObject && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && w[i + 3] < bound) {
            uint32_t s = w[i + 3], d = w[i + 2];
            if (p_vin[s] != VSI_NONE) p_vin[d] = p_vin[s];
            if (p_fout[s] != VSI_NONE) p_fout[d] = p_fout[s];
            if (p_tex[s] != VSI_NONE) p_tex[d] = p_tex[s];
            if (local_ptr[s]) local_ptr[d] = 1;
            if (v_vin[s] != VSI_NONE) v_vin[d] = v_vin[s];
            if (tex_obj[s] != VSI_NONE) tex_obj[d] = tex_obj[s];
            if (tex_fetch[s] != VSI_NONE) tex_fetch[d] = tex_fetch[s];
            if (c_vin[s] != VSI_NONE) { c_vin[d] = c_vin[s]; c_sgn[d] = c_sgn[s]; }
            if (c_tex[s] != VSI_NONE) { c_tex[d] = c_tex[s]; c_sgn[d] = c_sgn[s]; }
            if (ff_int[s] != VSI_NONE) ff_int[d] = ff_int[s];
        } else if (op == VSI_OpVariable && len >= 4 && w[i + 1] < bound && w[i + 2] < bound &&
                   w[i + 3] == VSI_SCFunction && ptr_sc[w[i + 1]] == VSI_SCFunction) {
            local_ptr[w[i + 2]] = 1;   // a Function-scope local pointer
        } else if ((op == VSI_OpAccessChain || op == VSI_OpInBoundsAccessChain) && len >= 4 &&
                   w[i + 1] < bound && w[i + 2] < bound && w[i + 3] < bound) {
            uint32_t s = w[i + 3], d = w[i + 2];
            if (p_vin[s] != VSI_NONE) p_vin[d] = p_vin[s];
            if (p_fout[s] != VSI_NONE) p_fout[d] = p_fout[s];
            if (p_tex[s] != VSI_NONE) p_tex[d] = p_tex[s];
            if (local_ptr[s]) local_ptr[d] = 1;
        } else if (op == VSI_OpLoad && len >= 4 && w[i + 2] < bound && w[i + 3] < bound) {
            uint32_t s = w[i + 3], d = w[i + 2];
            if (p_vin[s] != VSI_NONE) v_vin[d] = p_vin[s];
            if (p_tex[s] != VSI_NONE) tex_obj[d] = p_tex[s];
            if (local_ptr[s]) {   // vk_android_spv.py:323-328: SC_FUNCTION locals carry provenance too
                if (loc_vin[s] != VSI_NONE && loc_vin[s] != VSI_AMBIG) v_vin[d] = loc_vin[s];
                if (loc_tex[s] != VSI_NONE && loc_tex[s] != VSI_AMBIG) tex_fetch[d] = loc_tex[s];
            }
        } else if ((op == VSI_OpSampledImage || op == VSI_OpImage) && len >= 4 &&
                   w[i + 2] < bound && w[i + 3] < bound) {
            if (tex_obj[w[i + 3]] != VSI_NONE) tex_obj[w[i + 2]] = tex_obj[w[i + 3]];
        } else if (vka_spv_texel_read_op(op) && len >= 5 && w[i + 2] < bound && w[i + 3] < bound) {
            if (tex_obj[w[i + 3]] != VSI_NONE) tex_fetch[w[i + 2]] = tex_obj[w[i + 3]];
        } else if ((op == VSI_OpCompositeExtract || op == VSI_OpVectorShuffle) && len >= 5 &&
                   w[i + 2] < bound && w[i + 3] < bound) {
            uint32_t d = w[i + 2], s = w[i + 3];
            if (v_vin[s] != VSI_NONE) v_vin[d] = v_vin[s];
            if (tex_fetch[s] != VSI_NONE) tex_fetch[d] = tex_fetch[s];
            if (c_vin[s] != VSI_NONE) { c_vin[d] = c_vin[s]; c_sgn[d] = c_sgn[s]; }
            if (c_tex[s] != VSI_NONE) { c_tex[d] = c_tex[s]; c_sgn[d] = c_sgn[s]; }
            if (ff_int[s] != VSI_NONE) ff_int[d] = ff_int[s];
            if (op == VSI_OpVectorShuffle && w[i + 4] < bound) {
                uint32_t t = w[i + 4];
                if (v_vin[d] == VSI_NONE && v_vin[t] != VSI_NONE) v_vin[d] = v_vin[t];
                if (tex_fetch[d] == VSI_NONE && tex_fetch[t] != VSI_NONE) tex_fetch[d] = tex_fetch[t];
                if (c_vin[d] == VSI_NONE && c_vin[t] != VSI_NONE) { c_vin[d] = c_vin[t]; c_sgn[d] = c_sgn[t]; }
                if (c_tex[d] == VSI_NONE && c_tex[t] != VSI_NONE) { c_tex[d] = c_tex[t]; c_sgn[d] = c_sgn[t]; }
                if (ff_int[d] == VSI_NONE && ff_int[t] != VSI_NONE) ff_int[d] = ff_int[t];
            }
        } else if (op == VSI_OpSelect && len >= 6 && w[i + 2] < bound && w[i + 4] < bound && w[i + 5] < bound) {
            uint32_t d = w[i + 2], a = w[i + 4], b = w[i + 5];   // condition is w[i+3]
            if (v_vin[a] != VSI_NONE) v_vin[d] = v_vin[a]; else if (v_vin[b] != VSI_NONE) v_vin[d] = v_vin[b];
            if (tex_fetch[a] != VSI_NONE) tex_fetch[d] = tex_fetch[a]; else if (tex_fetch[b] != VSI_NONE) tex_fetch[d] = tex_fetch[b];
            if (c_vin[a] != VSI_NONE) { c_vin[d] = c_vin[a]; c_sgn[d] = c_sgn[a]; }
            else if (c_vin[b] != VSI_NONE) { c_vin[d] = c_vin[b]; c_sgn[d] = c_sgn[b]; }
            if (c_tex[a] != VSI_NONE) { c_tex[d] = c_tex[a]; c_sgn[d] = c_sgn[a]; }
            else if (c_tex[b] != VSI_NONE) { c_tex[d] = c_tex[b]; c_sgn[d] = c_sgn[b]; }
            if (ff_int[a] != VSI_NONE) ff_int[d] = ff_int[a]; else if (ff_int[b] != VSI_NONE) ff_int[d] = ff_int[b];
        } else if (op == VSI_OpCompositeConstruct && len >= 4 && w[i + 2] < bound) {
            uint32_t d = w[i + 2];
            uint8_t tag = VSI_NONE;
            for (uint32_t k = 3; k < len; k++) if (w[i + k] < bound && ff_int[w[i + k]] != VSI_NONE) {
                if (tag == VSI_NONE || ff_int[w[i + k]] == 2) tag = ff_int[w[i + k]];
            }
            if (tag != VSI_NONE) ff_int[d] = tag;
        } else if (op == VSI_OpCompositeInsert && len >= 6 && w[i + 2] < bound && w[i + 3] < bound && w[i + 4] < bound) {
            // w[i+5..] are literal indices, NOT SSA ids: only Object and Composite carry provenance.
            uint8_t tag = ff_int[w[i + 3]];
            if (tag == VSI_NONE) tag = ff_int[w[i + 4]];
            if (tag != VSI_NONE) ff_int[w[i + 2]] = tag;
        } else if (op == VSI_OpBitcast && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && w[i + 3] < bound) {
            uint32_t rty = w[i + 1], d = w[i + 2], s = w[i + 3];
            if (ity[rty]) {   // target is int (scalar or vector)
                uint8_t sg = isgn[rty];
                if (want_vin && v_vin[s] != VSI_NONE) {
                    c_vin[d] = v_vin[s]; c_sgn[d] = sg;
                    if (sg) vs |= 1u << v_vin[s]; else vu |= 1u << v_vin[s];
                }
                if (want_tex && tex_fetch[s] != VSI_NONE) {
                    c_tex[d] = tex_fetch[s]; c_sgn[d] = sg;
                    if (sg) ts |= 1ull << tex_fetch[s]; else tu |= 1ull << tex_fetch[s];
                }
            }
            uint32_t sty = rt[s];   // source SSA id's type
            if (want_fout && sty < bound && ity[sty] && fty[rty])
                ff_int[d] = (uint8_t)(isgn[sty] ? 2 : 1);
        } else if (op == VSI_OpStore && len >= 3 && w[i + 1] < bound && w[i + 2] < bound) {
            uint32_t dst = w[i + 1], src = w[i + 2];
            if (local_ptr[dst]) {   // conflicting tags from different locations/bindings clear the slot
                if (v_vin[src] != VSI_NONE) loc_vin[dst] = (loc_vin[dst] == VSI_NONE || loc_vin[dst] == v_vin[src]) ? v_vin[src] : VSI_AMBIG;
                if (tex_fetch[src] != VSI_NONE) loc_tex[dst] = (loc_tex[dst] == VSI_NONE || loc_tex[dst] == tex_fetch[src]) ? tex_fetch[src] : VSI_AMBIG;
            }
            if (want_fout && p_fout[dst] != VSI_NONE && ff_int[src] != VSI_NONE) {
                if (ff_int[src] == 2) fs |= 1u << p_fout[dst];
                else fu |= 1u << p_fout[dst];
            }
        } else if (vka_spv_float_consumer(op)) {
            // vk_android_spv.py:308-345: a value also used by float math is not integer bits.
            for (uint32_t k = 3; k < len; k++) if (w[i + k] < bound) {
                uint32_t id = w[i + k];
                if (v_vin[id] != VSI_NONE && v_vin[id] != VSI_AMBIG) vin_float_used |= 1u << v_vin[id];
                if (tex_fetch[id] != VSI_NONE && tex_fetch[id] != VSI_AMBIG) tex_float_used |= 1ull << tex_fetch[id];
            }
        } else if (vka_spv_signed_consumer(op)) {
            for (uint32_t k = 3; k < len; k++) if (w[i + k] < bound) {
                uint32_t id = w[i + k];
                if (c_vin[id] != VSI_NONE) { vs |= 1u << c_vin[id]; c_sgn[id] = 1; }
                if (c_tex[id] != VSI_NONE) { ts |= 1ull << c_tex[id]; c_sgn[id] = 1; }
            }
        }
        i += len;
    }
    }

    vu &= ~vin_float_used; vs &= ~vin_float_used;
    tu &= ~tex_float_used; ts &= ~tex_float_used;
    out->vin_u = vu & ~vs;
    out->vin_s = vs;
    out->fout_u = fu & ~fs;
    out->fout_s = fs;
    out->tex_u = tu & ~ts;
    out->tex_s = ts;

done:
    free(buf);
#undef VSI_RT_BODY
#undef VSI_RT_CONST
#undef VSI_LEN_OK
    return (out->vin_u | out->vin_s | out->fout_u | out->fout_s | out->tex_u | out->tex_s) != 0;
}

#endif
