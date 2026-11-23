# Hogwarts Legacy (`0100F7E00C70E000`)

Snapshot v327680, SDK 17.5.4 (`SDK_libz-17_5_4-Release0`), Unreal Engine 4 (the `Phoenix` cook, `UE4CommandLine.txt` points at `Phoenix.uproject`), NVN. Modules `main`, `subsdk0` (`NintendoSDK_movie`), `sdk`; `rtld` is replaced by the host. The source is a 14 GB `.nup` read in place — the RomFS was not extracted into the repository or the home folder.

Status: first phone run done (2026-11-23) on a realme RMX3371 with Turnip: boots, 30 fps, audio, UI draws, but the lit 3D scene is almost black. The cause was traced to NaN from `InverseSqrt` of a zero normal in one of the game's pixel shaders; a general host patch is in place but has not been rebuilt or checked on the phone yet. The finding is below.

## Survey facts (2026-11-22)

- Title ID / program ID `0100F7E00C70E000`, base `v327680`, no updates and no add-ons in the `.nup`.
- Build IDs: `main` `0c00fc37b0610fbdb2cf73aed0cc73e7`, `subsdk0` `d0c48eef6dc2f1e7ac8b8dd5b5b03a60`, `sdk` `1ea0b9fe6cd6867b18e374a5d53d7950`.
- `main`: 31.6 M instructions decoded (2,454 undecodable), 78,173 x18 uses, 134 `mrs tpidr_el0`, bss 32.2 MB, 807 imports (all new to the host's profile survey), 305 exports. Hooks present: 865 of the host's 1,248; only `nvn` is imported directly.
- RomFS: 200 files. `Phoenix/Content/Paks/` holds 12 `.pak`, 13 `.utoc` and 13 `.ucas` (IoStore) containers plus 140 `.bk2` movies; shaders are expected in the UE4 shader code archives inside the paks, not loose at the RomFS root. No `Manifest_UFSFiles_*` listing the archive names was found.
- The memory keys are the survey's Unreal defaults; the 64 GB alias region follows the console's reservation, but the allocator cut has not been measured on a device.

## Shaders

`shader_split.py` reads the `.nup` in place (`<file>.nup:base/romfs` and `:base/exefs/main`) with `DM_UE_DECODE` and the `ooz` worker (`DM_OODLE_OOZ`, built from `dm/third_party/ooz`). The IoStore shader libraries decode locally on Linux — no Mac needed. The split finds **65,047 shaders** (5,597 compute, 9 geometry, 38,350 pixel, 21,091 vertex; 61,979 game and 3,068 global), with 21,091 variation pairs; the tables are in this profile. The Vulkan build made 83,431 SPIR-V modules (all `spirv-val`-valid); 2 of the 65,047 shaders could not be translated because they declare more bindings in one stage than the driver reports, so those draws will be missing until the binding limit is raised. The library is 737 MB (3.1 GB of SPIR-V before compression).

## Android pack

`pack_android.py` reads the `.nup` directly (`<file>.nup:base/romfs`); the work folder is beside the package (`<folder>/0100F7E00C70E000-work/android-pack`). The rewrite of `main` is the slow step (about 37 minutes), then the 703 MB library is copied in. The pack is 880 MiB and holds the rewritten `main` / `subsdk0` / `sdk`, the build-ID gates, and the shader library. The RomFS stays in the `.nup`.

## Phone run: dark lit scene (fix pending phone check, 2026-11-23)

On Turnip the game reaches its frame loop, runs at about 30 fps with audio and draws ~441 draws a frame. The UI renders correctly, but the lit world is almost black; brightening a screenshot shows the geometry is all there (character, walls, railings), only unlit. With the system driver the screen stays black and nothing renders (not investigated further at the reporter's request). There is no `[RED]` line beyond the ADPF hint failure, no missing-shader request (both `shaders/layer/req/` and `files/shader_req/req/` stayed empty apart from an unrelated older South Park dump), no texture-cache eviction and no skipped draws after the pipeline warm-up.

A frame capture at 40 s (frame 716, `dark.nvncap`, 770 MB) is a **loading screen** (a still castle image with a HINT line), not an in-game 3D frame. A capture at 90 s (frame 2146, `ingame.nvncap`, 880 MB) is the broken in-game frame: HUD and film grain draw, the 3D world is black. Its `TPIX` chunks were decoded directly (chunk header `cap_tpix` 168 bytes + one `cap_tsub` 48 bytes, then level-0 pixels). Of the four 1280x720 `NVN_FORMAT_R11G11B10F` targets, the early one (serial 5908) has **0 NaN** texels, while the two in-game scene targets (serials 26400 and 26423) have **433,858 NaN texels each (47% of the frame)** and serial 26422 has 63,149 NaN and 758,292 all-zero texels. A decoded view of serial 26400 still shows the character and walls, so geometry is drawn; the colour values are what is broken. This is the strongest lead for the dark picture: a tonemap fed NaN yields black while the unlit HUD is unaffected.

`DM_VK_NANSCAN` found a real fault: **about 60–120 NaN texels per frame in the 1280x720 `R11G11B10F` scene colour target** (serial near 5948), always starting near (1132, 344) and carried into the following passes. Whether those texels are what darkens the in-game picture is still unproven. `DM_VK_NANSCAN=2` with a one-draw step narrowed the first appearance to a draw that uses vertex shader `b9c0e2fde8bbda7c`; skipping pixel shader `8e4ad8a13590b1bd` did not remove the NaN, and the vertex shader is common to every NaN line. That vertex shader's module fetches float4 data from two textures (`vp_t_cb196610_1056805A` / `_1056805C`) and feeds it to `OpFma`; whether the NaN comes from the fetched data or from the translation is not established. A NaN reaching the scene colour can explain the dark frame if it contaminates the eye-adaptation downsample, as it does in other games ([NaN texels in mip chains](../../../docs/reference/rendering.md#mip-chain-pass-sampling-the-level-it-renders)), but that link is not proven here.

## Found on this game

- [InverseSqrt of zero on the Vulkan replay](../../../docs/reference/shaders.md#inversesqrt-of-zero-on-the-vulkan-replay): a general host patch clamps the operand, and the offline translator now bakes the same clamp into the library (Android chain `e4625cf8f816`). The library was rebuilt with `scripts/shader_fast_rebuild.py` (migrate hack): 39,597 of 65,047 entries re-translated (those with a scalar `InverseSqrt`), 25,448 migrated from the old library; the 706 MB library is in the pack. Not yet checked on the phone.

`DM_VK_NANSCAN=2` with a one-draw step at frame 1000 (in-game) named the first writer: **draw 281-282 of pass 0, vertex `f61fc44ee5ab4906` / pixel `36310803131ac0b6`** (both from `ShaderArchive-Phoenix_Chunk10`, UE5543 / UE5539). The next draw of the same pair raises the target from 10,088 to 182,180 NaN texels. The pixel module was extracted from the built Vulkan library and disassembled: it samples several textures, sanitises NaN UVs with `OpIsNan`, and contains a normalisation `Fma` → `InverseSqrt` (plus two `OpFDiv` by a support-buffer word). An `InverseSqrt` of zero/negative, or a division by zero, would produce Inf/NaN on Vulkan where Maxwell's RSQ may give a finite result. An input-texture scan (`DM_VK_NANSCAN_FS`) spent its whole line budget on frame lines and did not report, so whether a sampled texture or the arithmetic is the source is still open. The capture shows the geometry is drawn; only the values are broken.

Ruled out by A/B on the phone, all with the same result: `DM_VK_CS_ASYNC=0` + `DM_VK_PSO_ASYNC=0`; `DM_UE_TEXTURE_POOL=1000` (not applied: the rewriter does not find `r.Streaming.PoolSize` in this main, `ue_cvar.py` confirms it is not registered with the known pattern); `DM_VK_EAGER_TEX=1`; `DM_VK_SUBGROUP_PIN=0`; `DM_VK_PUSH_DEDUP=0` + `DM_VK_DRV_MEMO=0`; `DM_VK_TEX_TRACK=0`; a new save. The two shaders the build could not translate (`6d69c07dd8606f7e`, `d30cc1c32b57b33f`, both pixel) are not requested during play.

## Open

- Find the source of the NaN in the scene colour target: dump the float data fetched by `b9c0e2fde8bbda7c` (a frame capture with `DM_NVN_CAPTURE` plus an offline replay, or `DM_VK_NANSCAN` on that vertex stage) and check whether the guest data already holds NaN.
- Decide whether the NaN actually drives the exposure; if so, follow the general NaN → eye-adaptation path rather than adding a game rule.
- Measure the memory layout (the keys are still survey defaults).
- The two shaders that exceed the stage's binding limit are missing; decide whether to raise the limit.
