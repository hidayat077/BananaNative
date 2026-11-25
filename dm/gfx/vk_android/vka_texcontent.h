// Which texture first used as a sample takes its content from its pool storage (vka_tex.c storage_mode),
// free of Android and Vulkan headers so host unit tests can exercise the predicate.
// Switch 2 (NVN2): memory is coherent, so a sampled texture's content is its storage at first use and after
// (even when empty at first use, tracked for later CPU writes).
// Switch 1 (NVN1): memory is not coherent, so only storage that holds non-zero bytes at first use is uploaded;
// an all-zero storage keeps GPU-content behaviour so reused pool slots' leftovers are not uploaded.
#ifndef VKA_TEXCONTENT_H
#define VKA_TEXCONTENT_H
#include "../nvn/include/nvn.h"

static inline int vka_tc_sampled_content(int nvn2, unsigned texflags, unsigned poolflags, int has_layout, int compressed_fmt, int fmt_kind, int sampled, int has_data) {
    if (!sampled || has_layout || compressed_fmt || fmt_kind >= 3) return 0;
    if (poolflags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) return 0;
    if (texflags & (NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE)) return 0;
    if (nvn2) return !(texflags & NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    return has_data;
}
#endif
