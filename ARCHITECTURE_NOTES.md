# Architecture Notes: Zero-Stutter Vulkan Shader & GPL Pipeline (Adreno / Mesa Turnip)

Phase 0 codebase audit for implementing zero in-game `ir3` shader compilation on Android ARM64 targeting **Adreno GPUs + Mesa Turnip**, while preserving fallback compatibility for non-Turnip / stock Qualcomm drivers.

> [!IMPORTANT]
> **Hard Architecture Constraints:**
> - **Excluded extensions:** `VK_EXT_shader_object` and `VK_KHR_pipeline_binary` are intentionally NOT used (unsupported on upstream Mesa Turnip).
> - **LTO memory rule:** `VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT` must remain OFF by default (`DM_VK_GPL_LTO=0`) to avoid NIR/IR payload RAM spikes and Android LMK kills.
> - **Fallback preservation:** Classic PSO paths and non-EDS3 drivers (e.g., stock Qualcomm) remain intact behind runtime capability detection and `DM_VK_*` flags.

---

## 1. Shader Library (`dmsh_vk.lib`) & Runtime Shader Layer

- **Format & ABI:** [dm/gfx/nvn/dmsh_abi_vk.h](dm/gfx/nvn/dmsh_abi_vk.h) defines `dmsh_vk_header`, `dmsh_vk_entry`, `dmsh_vk_bind`, and `dmsh_vk_module`, versioned by `DMSH_VK_CHAIN` and `DMSH_VK_ABI` in [dm/gfx/nvn/dmsh_chain_vk.h](dm/gfx/nvn/dmsh_chain_vk.h).
- **Loader & Shader Cache:** [dm/gfx/vk_android/vka_shlib.c](dm/gfx/vk_android/vka_shlib.c) memory-maps `<pack>/shaders/v-<DMSH_VK_CHAIN>/dmsh_vk.lib` (or compatible ABI fallback), and [dm/gfx/vk_android/vka_shader.c](dm/gfx/vk_android/vka_shader.c) instantiates `VkShaderModule` handles via `vka_shader_make()` and `vka_prog_shaders()`.
- **Offline Coverage:** For a game with a complete prepared pack, **100% of Maxwell/NVN2 -> SPIR-V translation** happens ahead of time (`vk_android_build.py`). Every base stage module (VS, TCS, TES, GS, FS, CS, TS, MS) and pre-built variant (`DMSH_VK_MOD_UINT`, `DMSH_VK_MOD_UINT_MIXED`, `DMSH_VK_MOD_DUAL`, `DMSH_VK_MOD_SHUFFLE_LDS`, `DMSH_VK_MOD_TF`) is already present in `dmsh_vk.lib` at boot.
- **On-Demand Layer:** If a pack is missing shaders (or empty), `vka_shader.c` (`layer_request()`, `layer_map()`) writes `.ctl`/`.code` requests to `$DM_TEMP/shader_layer/` (or `DM_VK_LAYER_DIR`) for `shader_layer.py` to translate asynchronously in `:pack`.
- **Gap for M4:** Currently, `vka_shader.c` only creates `VkShaderModule` lazily at first draw (`vka_prog_shaders()`), and `vka_shlib_check_all()` (`DM_VK_SHLIB_CHECK=1`) is a diagnostic check rather than a Boot #1 pre-compiler with worker pools, `vkMergePipelineCaches`, atomic checkpoints, and Android progress UI.

---

## 2. `VkPipelineLayout` Construction & M3 Independent-Sets Plan

- **Current State ([dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c), `layout_slow()` / `vka_layout_of()`):**
  - Per [dm/gfx/nvn/dmsh_abi_vk.h](dm/gfx/nvn/dmsh_abi_vk.h), all per-draw resources for every graphics stage share **Set 0** (`stage * DMSH_VK_STAGE_BINDINGS + n`, where `DMSH_VK_STAGE_BINDINGS = 64`), **Set 1** holds the bindless texture/sampler pools (`vka_bindless_layout()`), and **Set 2** holds the pool-read feedback bitmap (`vka_pfb_layout()`).
  - Because `L->set0` combines the bindings of **all** active stages (`VS + TCS + TES + GS + FS`) into a single `VkDescriptorSetLayout`, `L->layout` (`VkPipelineLayout`) is unique per `(VS, FS, ...)` shader combination.
  - **Root Cause of In-Game GPL Misses:** Both `prekey.layout` and `fskey.layout` in `vka_pso.c` (`gpl_link()`) include `P->ci.layout`. Whenever gameplay encounters a new `(VS, FS)` pairing, `L->layout` changes, causing `PRE_RASTERIZATION_SHADERS` and `FRAGMENT_SHADER` GPL cache lookups to miss and re-invoke `ir3` compilation even if both individual shaders were compiled earlier.
- **Planned M3 Design (Zero `dmsh_vk.lib` ABI Breakage):**
  - Use `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT` with **4 descriptor sets** (well within `maxBoundDescriptorSets >= 7` on Adreno/Turnip):
    - **Set 0:** Pre-rasterization stage bindings (`VS/TCS/TES/GS` or `TS/MS`), using push descriptors when `ndesc <= V.max_push`.
    - **Set 1:** Fragment stage bindings (`FS`), allocated from the per-submission descriptor pool (`set_alloc()`).
    - **Set 2:** Bindless texture & sampler pool (`vka_bindless_layout()`).
    - **Set 3:** Pool-read feedback bitmap (`vka_pfb_layout()`).
  - Remap `DescriptorSet` decorations in SPIR-V during `VkShaderModule` creation in [dm/gfx/vk_android/vka_spvpatch.c](dm/gfx/vk_android/vka_spvpatch.c) (FS Set 0 -> Set 1; Pool Set 1 -> Set 2; PoolFB Set 2 -> Set 3), keeping existing `dmsh_vk.lib` packs valid without rebuilding shaders.

---

## 3. Pipeline Key (`vka_gkey`), Hashing & Prewarm List

- **Struct Definition ([dm/gfx/vk_android/vka_gkey.h](dm/gfx/vk_android/vka_gkey.h)):**
  - `cfmt[8]`, `dfmt`, `sfmt` (`uint32_t` VkFormats)
  - `ncolor`, `samples`, `topo`, `poly_mode`, `depth_clamp`, `a2c`, `logic_en`, `logic_op`, `gl_depth`, `fs_variant`, `alpha_func`, `gs_fx` (`uint8_t` flags/enums)
  - `vin_u`, `vin_s` (`uint32_t` vertex input retype masks)
  - `tex_u[5]`, `tex_s[5]` (`uint64_t` per-stage texture/image retype masks)
  - `att[8]` (`{en, src, dst, op, srca, dsta, opa, mask}`, 8 bytes each = 64 bytes)
  - `vin_static` (`uint32_t`)
  - `va[32]` (12 bytes each = 384 bytes) and `vb[33]` (12 bytes each = 396 bytes) — only hashed/compared when `vin_static != 0` (`offsetof(vka_gkey, va)` otherwise).
- **Full Lookup Key (`gkey_full` in [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c)):** `{ vka_shader *sh[VKA_NSTAGE]; VkPipelineLayout layout; vka_gkey k; }`.
- **Hash Function (`vka_pw_fnv` in [dm/gfx/vk_android/vka_pwlist.h](dm/gfx/vk_android/vka_pwlist.h)):** 8-byte-step FNV-1a mix (`0x9E3779B97F4A7C15ull` + `x ^= x >> 29`) with `VKA_PW_FNV0` seed.
- **On-Disk Persistence:** `NXVKPW2` format (`vk_pipelines-<driver>.keys` and `dm/games/<id>/vk_prewarm.bin`) verified by `vka_pw_layout()`.
- **Canonical vs. Runtime Key Separation (M2, [dm/gfx/vk_android/vka_eds.h](dm/gfx/vk_android/vka_eds.h)):**
  - `vka_gkey` struct layout is preserved unchanged so all `vk_prewarm.bin` seeds remain compatible.
  - **Canonical Key:** Holds the full NVN render state for disk `.keys` storage and non-EDS3 fallback drivers.
  - **Runtime Key (`vka_eds_runtime_key`):** Zeroes out fields handled dynamically by active EDS2/EDS3 bits (`depth_clamp`, `poly_mode`, `samples`, `a2c`, `logic_en`, `logic_op`, `att[].en`, `att[].src..opa`, `att[].mask`) before `g_gfx` hashing/lookup and GPL key construction.

---

## 4. Vulkan Extension & Feature Audit

| Capability | Status | Location | Notes |
| :--- | :--- | :--- | :--- |
| `VK_KHR_dynamic_rendering` (1.3 core) | **Enabled** | [vka_dev.c](dm/gfx/vk_android/vka_dev.c) | Required at device init (`f13.dynamicRendering`). |
| `VK_EXT_extended_dynamic_state` (1.3 core) | **Enabled** | [vka_dev.c](dm/gfx/vk_android/vka_dev.c), [vka_draw.c](dm/gfx/vk_android/vka_draw.c) | Cull, front face, topology, depth/stencil test/write/op/bounds. |
| `VK_EXT_extended_dynamic_state2` | **Core + Ext** | [vka_dev.c](dm/gfx/vk_android/vka_dev.c), [vka_draw.c](dm/gfx/vk_android/vka_draw.c) | Core 1.3 covers `rasterizerDiscardEnable`, `depthBiasEnable`, `primitiveRestartEnable`. Optional `VK_EXT_extended_dynamic_state2` adds `logicOpEXT` and `patchControlPointsEXT`. |
| `VK_EXT_vertex_input_dynamic_state` | **Enabled** | [vka_dev.c](dm/gfx/vk_android/vka_dev.c), [vka_draw.c](dm/gfx/vk_android/vka_draw.c) | `V.vin_dyn`, emits `vkCmdSetVertexInputEXT` (off by default on Qualcomm < 512.800). |
| `VK_EXT_extended_dynamic_state3` | **Added (M2)** | [vka_dev.c](dm/gfx/vk_android/vka_dev.c), [vka_eds.h](dm/gfx/vk_android/vka_eds.h) | Gated by `DM_VK_EDS3` (default `1` on Turnip, `0` elsewhere). Covers 9 feature bits (`depthClampEnable`, `polygonMode`, `rasterizationSamples`, `sampleMask`, `alphaToCoverageEnable`, `logicOpEnable`, `colorBlendEnable`, `colorBlendEquation`, `colorWriteMask`). |
| `VK_EXT_graphics_pipeline_library` | **Enabled** | [vka_dev.c](dm/gfx/vk_android/vka_dev.c), [vka_pso.c](dm/gfx/vk_android/vka_pso.c) | 4-part GPL (`L_VI`, `L_PRE`, `L_FS`, `L_FO`), `RETAIN_LTO` off by default. |
| `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT` | **Planned (M3)** | [vka_pso.c](dm/gfx/vk_android/vka_pso.c) | Currently used only in diagnostic `pre_probe()`; M3 promotes it to the main layout path. |
| `vkMergePipelineCaches` | **Planned (M4)** | [vka.h](dm/gfx/vk_android/vka.h) | Needed for Boot #1 per-worker `VkPipelineCache` merging. |

---

## 5. Runtime Shader `retyped` & Specialization Paths

All runtime SPIR-V patching paths that can cause late `VkShaderModule` / `ir3` compiles during gameplay:

1. **Fragment Output Retyping (`VKA_FS_RETYPE`):**
   - [dm/gfx/vk_android/vka_draw.c](dm/gfx/vk_android/vka_draw.c) (`draw_body()`, `g_fout_retype`): detects float FS outputs targeting `UINT`/`SINT` attachments without a pre-baked `MOD_UINT`/`MOD_UINT_MIXED` variant.
   - [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c) (`fout_masks()`, `pstate_fill()`): calls `vka_shader_fout_module()` -> `vka_spv_fout_retype()` in [dm/gfx/vk_android/vka_spvpatch.c](dm/gfx/vk_android/vka_spvpatch.c).
2. **Vertex Input Retyping (`vin_u` / `vin_s`):**
   - [dm/gfx/vk_android/vka_draw.c](dm/gfx/vk_android/vka_draw.c) (`vin_format()`, `vin_build_kept()`): integer vertex attributes feeding float VS inputs.
   - [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c) (`pstate_fill()`): calls `vka_shader_vin_module()` -> `vka_spv_vin_retype()` in [dm/gfx/vk_android/vka_spvpatch.c](dm/gfx/vk_android/vka_spvpatch.c).
3. **Texture & Image Retyping (`tex_u` / `tex_s`, `img_retype`):**
   - Graphics: [dm/gfx/vk_android/vka_draw.c](dm/gfx/vk_android/vka_draw.c) (`tex_masks()`), [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c) (`vka_shader_tex_module()`).
   - Compute: [dm/gfx/vk_android/vka_draw.c](dm/gfx/vk_android/vka_draw.c) (`img_retype()`), [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c) (`vka_cs_pipeline_img()`), [dm/gfx/vk_android/vka_shader.c](dm/gfx/vk_android/vka_shader.c) (`vka_shader_img_module()`).
4. **Pool-Read Feedback & Early/Late Depth Twins:**
   - [dm/gfx/vk_android/vka_shader.c](dm/gfx/vk_android/vka_shader.c) (`csm_pfb()`, `vka_shader_pfb_pick()`): when `a2c != 0` or `alpha_func != 0`, switches from the `EarlyFragmentTests` module to its late-test twin.
5. **Geometry Shader Claim / Link & Specialization Constants:**
   - [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c) (`pstate_fill()`): `vka_shader_gs_module()`, `vka_shader_gs_link_module()`, `DMSH_VK_SPEC_GL_DEPTH` (`SpecId 0`), and `DMSH_VK_SPEC_ALPHA_FUNC` (`SpecId 1`).
