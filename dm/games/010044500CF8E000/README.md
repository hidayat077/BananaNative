# Hellblade: Senua's Sacrifice (`010044500CF8E000`)

Base game 1.1.0 (the `.nup` meta block; NCM version 65536), SDK 7.3.0, NVN, Unreal Engine 4, with `NintendoSDK_movie` for its cutscenes. The dump is one base package, no updates and no add-ons: four code modules (`main`, `rtld`, `sdk`, `subsdk0`) and a RomFS of 106 files, 18.15 GB, one of them `HellbladeGame/Content/Paks/HellbladeGame-Switch.pak` (13.01 GB). The profile was made from a survey of the user's own `.nup`, read in place; **bring-up is still open** — no Mac, iPhone or Android run has been made.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `main` imports from `subsdk0` (NintendoSDK_movie) and `sdk`; load order main, subsdk*, sdk |
| `romfs_marker HellbladeGame/Content/Paks/HellbladeGame-Switch.pak` | `game.tsv` | the only large RomFS file, present in every copy of this game |
| `zip_store .bk2:.pak` | `game.tsv` | the 99 Bink video files and the pak are already compressed media |
| `mem_reserve`, `mem_alias`, `mem_stack`, `app_pool` | `game.tsv` | the survey's provisional Unreal split: `nninitStartup` reserves a 62 GB address region. The rewriter's engine-wide region gate (`dm/tools/ue_region.py`) cuts it down to what the alias area has room for; refine after a real boot |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB of system resource, enabling virtual address memory |
| `shader_groups` | `game.tsv` | the global shader cache, the engine's packages and the game's packages (see Shaders) |
| `status bring-up` | [`readiness.tsv`](readiness.tsv) | the launcher warns that the game may stop or draw wrongly; nothing has been tested on a phone yet |

No game-specific host hook or rewriter cut is needed at this stage. SDK 7.3.0 is much older than Mario's SDK 16: NVN object sizes, API versions and service layouts differ, so any boot failure must be checked against [NVN objects](../../../docs/reference/nvn-objects.md) rather than assumed to be the game's own code. The survey's rewrite-site census is in the work folder (see Survey).

## Pak and shaders

`HellbladeGame/Content/Paks/HellbladeGame-Switch.pak` is an **Unreal pak version 4**, 50,337 files, mount point `../../../`, uncompressed, with no `.ushaderbytecode` shader code archives. As in [Unreal Engine 4.18 shaders inline in cooked packages](../../../docs/reference/shaders.md#unreal-engine-418-shaders-inline-in-cooked-packages), the shaders are inside each material's cooked package and the engine's global shaders in `Engine/GlobalShaderCache-GLSL_SWITCH.bin` (5.3 MB); each is an `LSLG` record held in a zlib stream. Nothing here needs the game's own Oodle decoder, so `shader_split.py` reads the pak straight from the `.nup` on any machine. Unreal combines vertex and pixel shaders per draw, so the split pairs each vertex shader with a pixel-input cover.

The split reads **19,289 distinct shaders** (16,358 pixel, 2,707 vertex, 148 geometry, 76 compute) with no `[RED]` lines: 17,855 from the game's packages, 859 from the global shader cache, 544 from the engine's packages and 31 from `main`; it makes 2,707 variation pairs. This is the second game found with [Unreal Engine 4.18 shaders inline in cooked packages](../../../docs/reference/shaders.md#unreal-engine-418-shaders-inline-in-cooked-packages), and shows that such a version 4 pak need not have LZ5-compressed blocks — Hellblade's entries are stored.

## Survey

The survey did not extract the 19.5 GB `.nup`: `010044500CF8E000-work/make_survey_dump.py` beside it writes a folder dump whose RomFS files are **sparse** (real sizes and real 32-byte headers, no game data), so `build_ipa_from_nsp.py --survey-only` could read the ExeFS, the manifest, the rewrite-site census and the imports in place; `profile_guess.py` also read the `.nup`'s package paths directly. The report is `010044500CF8E000-work/survey-dump/010044500CF8E000_survey/survey.md`. The shader split then read the `.nup` directly; no game file was copied into the repository.

## Found on this game

- **Unreal Engine 4.18 shaders inline in cooked packages**: this game's uncompressed version 4 pak is the second example of the form, after DRAGON QUEST XI S; no tool change was needed ([shaders.md](../../../docs/reference/shaders.md#unreal-engine-418-shaders-inline-in-cooked-packages)).

## Status

Profile, shader tables, docs and the Android game list are in place; the docs / Android game-list checks pass. A code-only pack from the `.nup` (`pack_android.py --no-shaders`) rewrote all three modules with no `[RED]` lines, but the Vulkan shader library, the APK and any phone run have not been made. Nothing is known yet about boot, input, rendering or performance. Next: bring-up on the Mac, then a phone run on the system driver and a Turnip driver, as in [03-bringup](../../../docs/agents/03-bringup.md) and [06-dev-loop](../../../docs/agents/06-dev-loop.md#checking-a-change-for-android).
