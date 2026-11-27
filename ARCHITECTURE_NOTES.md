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
| `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT` | **Added (M3)** | [vka_pso.c](dm/gfx/vk_android/vka_pso.c), [vka_shader.c](dm/gfx/vk_android/vka_shader.c), [vka_draw.c](dm/gfx/vk_android/vka_draw.c) | 4-set layout (`DM_VK_GPL_PRECOMPILE=1` on Turnip): Set 0 pre-raster, Set 1 FS, Set 2 pool, Set 3 pool-fb; SPIR-V `DescriptorSet` remapped in `vka_spv_remap_sets`. |
| `vkMergePipelineCaches` | **Enabled (M4)** | [vka.h](dm/gfx/vk_android/vka.h), [vka_pso.c](dm/gfx/vk_android/vka_pso.c) | Merges Boot #1 per-worker `VkPipelineCache` instances into master `g_pc` before handle destruction. |

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

---

## 6. Milestone 4: Boot #1 Pre-Compile & Persistent GPL Cache (`DM_VK_BOOT_PRECOMPILE`)

- **Gate & Defaults:** `DM_VK_BOOT_PRECOMPILE` (restart-only) defaults to `1` on Mesa Turnip when `V.gpl_indep` is active, and `0` on other drivers (`vka_auto.h`, `vka_dev.c`). Fallback paths (`DM_VK_BOOT_PRECOMPILE=0`, classic render passes, non-Turnip drivers) remain untouched.
- **Container Format ([vka_gpl_cache.h](dm/gfx/vk_android/vka_gpl_cache.h)):**
  - Filename: `vk_gpl_cache_<TitleID>_<dmsh_hash>_<driver_uuid>.bin` in `$DM_TEMP` (stale files for the same title are pruned automatically on mismatch/driver swap).
  - 80-byte `vka_gpl_cache_hdr` (`GPLC` magic, `schema_version=1`, `vendor_id`, `device_id`, `driver_version`, `flags` with `COMPLETE` and `GL_DEPTH`, `pipeline_cache_uuid[16]`, `dmsh_vk_lib_hash`, `total_entries`, `compiled_entries`, `bitset_bytes`, `vk_cache_bytes`, `vk_cache_fnv`, `header_crc`) + resume bitset + raw `VkPipelineCache` blob.
  - Atomic checkpoint writes (`open(.tmp)` → `write` → `fsync` → `close` → `rename`) every 250 shaders or 10 s, plus on user skip/early stop.
- **Worker Pool & 8 GB RAM + ZRAM Hygiene (`vka_pso.c` `vka_boot_precompile_run`):**
  - Runs on `mtl_prepare()` before the first 3D frame (never on the Android UI thread).
  - `min(4, big_and_mid_cores)` workers, each with its own local `VkPipelineCache` merged periodically into master `g_pc` via `vkMergePipelineCaches` every `<= 100` shaders, destroying temporary `VkPipeline` and `VkShaderModule` handles immediately after merge.
  - `vka_precompile_target_workers()` steps down to 2 workers after 45 s (thermal) and throttles/stops cleanly if `MemAvailable < 600 MiB` or `/proc/self/maps >= 75%` of `vm.max_map_count`.
  - In-game cache saves are debounced (`>= 50` new entries and `>= 30 s` via `vka_precompile_debounce_due()`), with forced flush on `mtl_drain()`.
- **Non-Blocking Progress UI:** `nxhost_precompile_status()` / `nxhost_precompile_skip()` (`vk_android.h` → `nxlaunch.c` → `Native.java`) drive a 200 ms polled overlay in `GameActivity.java` with an optional **Skip / Play Now** button.

---

## 7. Milestone 5: Retype Leak Closure & Separated Telemetry

- **Dynamic Write-Mask Suppression (`vka_eds.h`, `vka_draw.c`):**
  - `vka_fout_retype_masks()` skips `VKA_FS_RETYPE` whenever a color attachment's effective write mask (`chmask & fcomp`) is `0`, letting EDS3 `vkCmdSetColorWriteMaskEXT` handle masked-off channels with zero SPIR-V rewriting.
- **Boot #1 Pre-Compile of Seeded Retypes & Alpha-Test Specializations (`vka_shader.c`, `vka_pso.c`):**
  - `precomp_pw_variants()` scans loaded prewarm seed records (`vk_prewarm.bin` / `.keys`) at the end of Boot #1 and pre-compiles known `VKA_FS_RETYPE` (`vka_shader_precomp_fout`), `alpha_func != 0` specializations, `vin_u`/`vin_s` (`vka_shader_precomp_vin`), and compute image retypes (`vka_shader_precomp_cs_img`) directly into `g_pc` before the final atomic checkpoint save.
- **Separated Runtime Telemetry (`vka_gpl_stats.h`, `vka_pso.c`, `vka_exec.c`, `vka_ctl.c`):**
  - Periodic replay logs (`shader telemetry:`) and `ctl: stats` emit `vka_gpl_telemetry_format()` with separated counters: `ir3_compiler_miss` (via `VkPipelineCreationFeedback` + `vka_classify_compile_feedback`), `gpl_fast_link_count`, `gpl_fast_link_avg_us`, `gpl_fast_link_p99_us`, `gpl_fast_link_max_us`, `pipeline_ram_rehydrate_count`, `frontend_shader_translate_count`, and per-path retype counts (`fout`, `vin`, `tex`, `cs_img`).

---

## 8. Runtime Shader Variant Manifest & Memory-Safe Delta Retype Cache (Audit + Fix)

### Audit Findings (Verified Before Editing)
1. **Why `vin` retypes trigger frequently under `VK_EXT_vertex_input_dynamic_state`:**
   - [dm/gfx/vk_android/vka_draw.c](dm/gfx/vk_android/vka_draw.c) (`vin_format()`, `vin_build()`) and [dm/gfx/vk_android/vka_spvpatch.c](dm/gfx/vk_android/vka_spvpatch.c) (`vka_spv_vin_retype()`) handle Maxwell's rule where integer vertex attributes (`UINT8..SINT32`) load raw integer bits into scalar registers for shader `OpBitcast` while `dmsh_vk.lib` declares every VS input as `vec4<float>` (`vin_types == DMSH_VK_TYPE_FLOAT`).
   - Because Vulkan prohibits feeding an integer `VkFormat` to a float shader input (and `USCALED`/`SSCALED` converts `5` to `5.0f` instead of bit-pattern `5`), each active `vin_u`/`vin_s` bit is strictly gated by `vs->e->vin_mask` and actual integer attribute formats bound by the game. Static SPIR-V inspection cannot safely guess unsigned vs. signed extension (`UINT` vs `SINT` on 8/16-bit attributes) or dynamic subset combinations without combinatorial bloat, so persisting exact runtime variants via the manifest + delta cache solves this cleanly across all games.
2. **Why `skipped 6` occurred during Boot #1 (`10349 / 10349` on No Man's Sky):**
   - `precomp_compile_entry()` in [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c) increments `skip_n` whenever `vka_shader_precomp_open()` ([dm/gfx/vk_android/vka_shader.c](dm/gfx/vk_android/vka_shader.c)) returns `0`: stages other than VS/FS/CS (`TCS`/`TES`/`GS`/`TS`/`MS`), `refusal()` entries, `FS` with `DMSH_VK_F_MESH_PRIM` when `!V.mesh`, or `CS` exceeding `maxComputeSharedMemorySize` / `vka_is_ndve_key()`. None of those skipped entries caused the Run #2 misses (`vin`, `tex`, `fout`, `cs_img`).
3. **Full Runtime Variant Key Inventory ([vka_shader_variants.h](dm/gfx/vk_android/vka_shader_variants.h)):**
   - `shader_hash` (`uint64_t`), `stage` (`DMSH_VK_STAGE_*`), `lib_variant` (`DMSH_VK_MOD_*`), `vin_u`/`vin_s` (`uint32_t`), `fout_u`/`fout_s` (`uint32_t`), `tex_u`/`tex_s` (`uint64_t`: per-stage graphics texture/image retype or compute `img_retype`), `alpha_func` (`SpecId 1`), `samples`, and `flags` (`VKA_SVAR_F_GL_DEPTH`, `VKA_SVAR_F_PFB_LATE`, `VKA_SVAR_F_A2C`, `VKA_SVAR_F_GS_CLAIM`).
   - Pool-read feedback instrumentation (`csm_pfb`) and independent-sets descriptor remapping (`vka_spv_remap_sets`) apply automatically inside `create_stage_module()`, while runtime `early_fragment_tests` control-block aliasing (`prog_key_alt`) resolves to its existing `dmsh_vk.lib` entry key.
4. **Partial-Resume `compiled == total` Edge Case & `.heavy` Persistence:**
   - Fixed `vka_boot_precompile_run()` (`rem == 0` early return) in [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c) so a partial checkpoint that reached `compiled == total` runs `precomp_pw_variants()` and stamps `VKA_GPL_CACHE_F_COMPLETE`.
   - Initialized `g_heavy_path` from the delta cache path when `g_gpl_cache_on` is active so long compiles persist on disk instead of falling back to `"this run only"`.

### Persistence & Startup Rehydration Architecture
- **Driver-Agnostic Variant Manifest ([vka_shader_variants.h](dm/gfx/vk_android/vka_shader_variants.h)):**
  - Stored as `vk_shader_variants_<TitleID>_<dmsh_hash>.bin` (`SVAR` v1 header + 56-byte `vka_svar_rec` entries + FNV checksums, atomic `fsync` + `rename` writes).
  - Survives driver updates (`pipelineCacheUUID` changes) so recorded runtime variants are automatically re-compiled on the new driver; pruned only when `dmsh_vk.lib` changes.
- **Read-Only Base Cache + Lightweight Delta Retype `VkPipelineCache` ([vka_gpl_cache.h](dm/gfx/vk_android/vka_gpl_cache.h), [vka_pso.c](dm/gfx/vk_android/vka_pso.c)):**
  - Boot #1 base `vk_gpl_cache_*.bin` stays strictly **read-only** in-game (`save_now()` never calls `vkGetPipelineCacheData` on it), eliminating 180+ MB RAM spikes and write amplification.
  - Runtime-retyped/specialized graphics stage libraries (`L_PRE`, `L_FS`, `L_PREFS`) and compute pipelines (`cs_img`) route through `g_pc_retype`, persisted atomically as `vk_gpl_retype_cache_<TitleID>_<dmsh_hash>_<driver_uuid>.bin` (`VKA_GPL_CACHE_F_DELTA`).
  - Stage indexing rule (`vka_svar_is_fs_retyped()`, `vka_svar_is_pre_retyped()`): `k->tex_u[5]` / `k->tex_s[5]` follow `DMSH_VK_STAGE_*` (`VS=0, TCS=1, TES=2, GS=3, FS=4`), whereas `P->sh[]` follows NVN stage order (`FS=1`), ensuring `tex`-only FS retypes (`tex_u[4] | tex_s[4]`) and `tex + fout` manifests record exact FS masks cleanly.
- **Startup Rehydration ([vka_shader.c](dm/gfx/vk_android/vka_shader.c), [vka_pso.c](dm/gfx/vk_android/vka_pso.c)):**
  - `vka_retype_variants_startup()` runs after loading Boot #2+ or finishing Boot #1: loads the manifest + delta cache, probes `g_pc_retype` (`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`) or compiles missing/driver-swapped variants via a RAM/maps-guarded worker pool (`vka_shader_precomp_variant()`), merges into `g_pc_retype`, and saves atomically.
- **Modified / Added Files:**
  - Added [dm/gfx/vk_android/vka_shader_variants.h](dm/gfx/vk_android/vka_shader_variants.h).
  - Updated [dm/gfx/vk_android/vka_gpl_cache.h](dm/gfx/vk_android/vka_gpl_cache.h), [dm/gfx/vk_android/vka_gpl_stats.h](dm/gfx/vk_android/vka_gpl_stats.h), [dm/gfx/vk_android/vka.h](dm/gfx/vk_android/vka.h), [dm/gfx/vk_android/vka_shader.c](dm/gfx/vk_android/vka_shader.c), [dm/gfx/vk_android/vka_pso.c](dm/gfx/vk_android/vka_pso.c), [dm/gfx/vk_android/vka_ctl.c](dm/gfx/vk_android/vka_ctl.c), [android/app/src/main/java/dev/nxnative/android/ShaderCacheUi.java](android/app/src/main/java/dev/nxnative/android/ShaderCacheUi.java), [dm/host/tests/t_android_gpl_cache.c](dm/host/tests/t_android_gpl_cache.c), [dm/host/tests/t_android_eds.c](dm/host/tests/t_android_eds.c), and [dm/host/tests/Makefile](dm/host/tests/Makefile).

---

## 9. Sharded GPL Store, Monolithic Background Upgrade, & Hot-Path Deduplication

- **Sharded v2 GPL Base Cache & Bounded LRU Cache Store (P0-2 & P1, [vka_gpl_cache.h](dm/gfx/vk_android/vka_gpl_cache.h), [vka_pso.c](dm/gfx/vk_android/vka_pso.c)):**
  - Replaces the single 400+ MB monolithic `VkPipelineCache` with a hash-bucketed sharded v2 `GPLC` container (`1..256` shards targeting ~160 entries/shard via `vka_gpl_shard_of()`).
  - **Boot #1 (Flat RAM & Resumable):** Each worker compiles one shard at a time into a local `VkPipelineCache`, destroys temporary `VkPipeline` handles, serializes `vkGetPipelineCacheData` (~1–4 MB), destroys the `VkPipelineCache`, appends atomically via `pwrite` + shard table update (`VKA_GPL_SHARD_DONE`), and moves on.
  - **Throttling Fix:** `vka_precompile_target_workers()` removes the premature 45 s `4 -> 2` clamp and retains full worker parallelism while `MemAvailable > 1500 MiB` and `maps < 50%`.
  - **Boot #2+ (Zero-Copy `mmap` + LRU `VkPipelineCache`):** Maps `vk_gpl_cache_*.bin` read-only (`PROT_READ, MAP_SHARED`), loads individual ~1–6 MB shard caches on demand with an LRU memory budget (`DM_VK_GPL_SHARD_BUDGET_MB`, default `32 MiB`, `16 MiB` under `DM_VK_RAM_SAVER >= 2`), and calls `madvise(..., MADV_DONTNEED)` after `vkCreatePipelineCache` to avoid retaining file/anonymous pages.
  - **Auxiliary Caches (`mmap` + `munmap`):** Delta retype (`vk_gpl_retype_cache_*.bin`) and monolithic (`vk_monolithic_cache_*.bin`) caches load via `vka_gpl_aux_cache_load_mmap()` and `munmap` immediately after `vkCreatePipelineCache`.
- **Hybrid→Monolithic Background Upgrade & Prewarm (P0-1, [vka_pso.c](dm/gfx/vk_android/vka_pso.c), [ShaderCacheUi.java](android/app/src/main/java/dev/nxnative/android/ShaderCacheUi.java)):**
  - Keeps GPL fast-link (`~30–40 us`) as a stutter-free bridge while queueing active pipelines (`DM_VK_GPL_OPT=1`) to `vk-psopt` (`DM_VK_PSO_SLOTS=1` on Turnip) to compile monolithic replacements into `g_pc_mono` (`vk_monolithic_cache_*.bin`).
  - On Boot #2+, `gfx_obtain()` / `gfx_build()` probe `g_pc_mono` (`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`) before GPL linking, and `pw_start()` prewarms `vk_pipelines-*.keys` into monolithic PSOs on background threads.
- **4-Set & EDS3 Hot-Path Deduplication (P0-1, [vka_draw.c](dm/gfx/vk_android/vka_draw.c)):**
  - Keys Set 0 push-descriptor deduplication on `lay->set0` instead of `lay->layout` so fragment shader changes with identical pre-raster bindings still hit Set 0 dedup/delta updates.
  - Caches Set 1 (`g_s1`) and bindless pool sets (`g_pool_bound`) per command buffer, eliminating redundant `set_alloc`, `vkUpdateDescriptorSets`, and `vkCmdBindDescriptorSets` calls.
  - Skips redundant `vkCmdSetLogicOpEXT` when `logicOpEnable == 0` and preserves prior `DY.beq[i]` entries for attachments with blending disabled.
- **Secondary Fixes (P2):**
  - P2-1: Calls `shuffle_plan()` before `cs` variant SPIR-V selection in `vka_shader_precomp_*` ([vka_shader.c](dm/gfx/vk_android/vka_shader.c)), runs `precomp_pw_variants()` after `vka_svar_manifest_load()`, and always flushes `vk_gpl_retype_cache_*.bin` before committing `vk_shader_variants_*.bin` (`svar_and_retype_save_locked()`).
  - P2-3: Deduplicates `input: on-screen controls:` log output in `nxhost_touch_controls_set()` ([input_android.c](dm/host/android/input_android.c)) so the layout editor only logs on actual changes.

---

## 10. Monolithic 1-Set Non-EDS3 Fast Path, Fixed Monolithic Cache Saving, & Mini-Shards

- **Decoupled Monolithic Cache Persistence (P0, [vka_pso.c](dm/gfx/vk_android/vka_pso.c)):**
  - Fixed Turnip stage-feedback quirk where `VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT` was set on `fb_all` even during 200+ ms cold monolithic compiles, which had left `g_mono_dirty` at `0` and caused `save_now()` to never serialize `g_pc_mono` while logging `delta_retype_cache_bytes` under `g_mono_cache_path`.
  - `full_pipeline()` now inspects per-stage feedback + compile duration (`!g_mono_cache_loaded || !all_st_hit || dns > 5 ms`), tracks `g_mono_made`, and `save_now()` serializes & logs `vk_monolithic_cache_*.bin` (`mono_cache_save_locked()`) and `vk_gpl_retype_cache_*.bin` (`svar_and_retype_save_locked()`) independently.
- **Steady-State Monolithic 1-Set Non-EDS3 Fast Path (P1, [vka.h](dm/gfx/vk_android/vka.h), [vka_shader.c](dm/gfx/vk_android/vka_shader.c), [vka_pso.c](dm/gfx/vk_android/vka_pso.c), [vka_draw.c](dm/gfx/vk_android/vka_draw.c)):**
  - Added un-remapped `mono_peer` shader modules (`vka_shader_mono_peer()`, `vka_shader_mono_scope()`) and compact 1-set pipeline layout (`vka_layout_mono_of()`), plus dual-handle `gval` (`p_mono` + `p_hybrid`) keyed on canonical `vka_gkey`.
  - Hybrid GPL pipelines (`p_hybrid`) continue using 4-set independent layout + EDS3 dynamic states as a zero-stutter bridge; once `vk-psopt` or `vk-prewarm` populates `p_mono`, `vka_gfx_pipeline_ex()` switches draws transparently to the 1-set compact layout with zero EDS3 `vkCmdSet*` emissions.
- **Mini-Shards & Cache Hygiene Fixes (P2, [vka_gpl_cache.h](dm/gfx/vk_android/vka_gpl_cache.h), [vka_pwlist.h](dm/gfx/vk_android/vka_pwlist.h), [vka_pso.c](dm/gfx/vk_android/vka_pso.c)):**
  - Scaled `vka_gpl_shard_count()` up to `2048` mini-shards (~24 shaders/shard, `SHARD_LRU_CAP = 64`) to shrink cold-shard frame-path open latency and minor page faults.
  - Fixed `vka_pw_rec_ok()` rejecting `gs_fx == 2` (transform-feedback capture pipelines) and matched `svar_compile_one()`'s `tf_fx` detection to `make_new()` so all `43/43` variants hit `g_pc_retype` cleanly.

---

## 11. Boot #1 Static SPIR-V Retype Heuristic & Same-Shard Runtime Rehydration

- **Static inference (Stages 2–3, [vka_spv_retype_infer.h](dm/gfx/vk_android/vka_spv_retype_infer.h), [vka_shader.c](dm/gfx/vk_android/vka_shader.c)):**
  - `vka_spv_infer_retype()` is a pure-C, Vulkan-free two-pass SPIR-V scanner: declarations before the first `OpFunction`, then function bodies. It infers `vin_u/vin_s` (location bits), `fout_u/fout_s` (colour-location bits) and `tex_u/tex_s` (stage-relative binding bits) from `OpBitcast` provenance, with signedness refined by signed-only consumers. Its `VSI_*` enum namespace cannot collide with `spirv.h`.
  - `DM_VK_BOOT_PRECOMPILE_RETYPE` (default `1`) enables `infer_precomp_retype()` inside `vka_shader_precomp_open()`. It scans the `DMSH_VK_MOD_BASE` words already produced by `lib_words()` — **no second decompression** — and applies `vin -> fout -> img` patches on a copy, in the exact same order as `vka_shader_precomp_variant()` and the runtime retype module builders: `lib_words -> prep_words -> retype patches -> create_stage_module`. The inference itself is read-only and runs before `prep_words` because `prep_words` does not change input locations or descriptor bindings.
  - The resulting `VkShaderModule` (`vka_precomp_sh.retype_mod`) is compiled into the **same per-shard `local_pc`** as the base stage libraries, so Turnip's per-shard `vk_gpl_cache_*.bin` blob stores base and retyped binaries keyed by SPIR-V hash without any shard-format change. `vka_shader_precomp_destroy()` destroys it with the base modules.
- **Runtime cache-only probe order (Stage 4, [vka_pso.c](dm/gfx/vk_android/vka_pso.c)):**
  - For retyped `L_PRE` / `L_FS` stage libraries (`pc_override == g_pc_retype`) and retyped compute pipelines, with `V.cache_ctl` (pipelineCreationCacheControl) on: **1)** probe `g_pc_retype` with `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`; **2)** on a miss, probe the shader's own mini-shard from `vk_gpl_cache_*.bin` with the same cache-only bit; **3)** only if both miss, do the existing live compile into `g_pc_retype`. `shard_pc_release()` runs on every path.
  - A mini-shard hit is a clean cache hit: it is not counted as `ir3_compiler_miss`, does not set `g_retype_dirty`, and does not append a redundant `vka_svar_rec` to `vk_shader_variants_*.bin` or a binary to `vk_gpl_retype_cache_*.bin`. Monolithic background optimisation (`vk-psopt`) and `vk_pipelines-*.keys` recording still happen normally.
  - `shader telemetry:` gained `retype_shard_hit=<N> retype_shard_miss=<M>` (appended in `vka_pso_telemetry_line()` so `vka_gpl_stats.h` and Boot #2+ cache formats stay untouched).
- **Invariants:** Boot #2+ `vk_monolithic_cache_*.bin`, `vk_gpl_retype_cache_*.bin`, the variant manifest format, non-retyped mini-shard lookups, and the `lib_make()` / `gpl_link()` rehydration behaviour for non-retyped stages are unchanged; `DM_VK_BOOT_PRECOMPILE_RETYPE=0` turns the inference pass off and leaves Stage 4 with zero shard probes.
