#pragma once
#include <stdint.h>

// Whether an unaligned buffer texture read is copied into aligned scratch for its texel buffer view (vka_tbuf.c). A view must start at a multiple of the
// device's minTexelBufferOffsetAlignment, which NVN does not require of a pool offset. A copy cannot carry a write back to guest memory, so writes are
// never copied; a buffer too large for a per-draw snapshot reads as an empty one instead. Host only: no ABI or chain version changes with this rule.
static inline int vka_tbuf_copy(int writes, uint64_t bytes, uint64_t off, uint32_t al) {
    return al > 1 && off % al != 0 && !writes && bytes && bytes <= (1u << 20);
}
