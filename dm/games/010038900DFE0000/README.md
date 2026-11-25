# What Remains of Edith Finch (`010038900DFE0000`)

Base game 1.0.0, SDK 7.3.0, NVN, Unreal Engine 4, Wwise. The dump (a `.nup` made from the user's own console) holds one base package, no updates and no add-ons: five ExeFS modules (`main`, `rtld`, `sdk`, `subsdk0`) and a RomFS of five files, 2.23 GB, one of them `FinchGame-Switch.pak`. The profile was made from a survey of those files; **bring-up is still open** — no Mac, iPhone or Android run has been made.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | `main` imports from `subsdk0` (NintendoSDK_movie) and `sdk`; load order main, subsdk*, sdk |
| `romfs_marker FinchGame/Content/Paks/FinchGame-Switch.pak` | `game.tsv` | the only large RomFS file, present in every copy of this game |
| `mem_reserve`, `mem_alias`, `mem_stack`, `app_pool` | `game.tsv` | the survey's provisional Unreal split: `nninitStartup` reserves a 62 GB address region. The rewriter's engine-wide region gate (`dm/tools/ue_region.py`) cuts it down to what the alias area has room for; refine after a real boot |
| `sys_resource 0x5000000` | `game.tsv` | the NPDM declares 80 MB of system resource, enabling virtual address memory |
| `shader_groups` | `game.tsv` | the game's and the engine's shader code archives (see Shaders) |
| `status bring-up` | [`readiness.tsv`](readiness.tsv) | the launcher warns that the game may stop or draw wrongly; nothing has been tested on a phone yet |

No game-specific host hook or rewriter cut is needed at this stage. SDK 7.3.0 is much older than Mario's SDK 16: NVN object sizes, API versions and service layouts differ, so any boot failure must be checked against [NVN objects](../../../docs/reference/nvn-objects.md) rather than assumed to be the game's own code.

## Pak and shaders

`FinchGame/Content/Paks/FinchGame-Switch.pak` is an **Unreal pak version 5**, 46,574 files, mount point `../../../`, unencrypted index, Zlib blocks. It names two shader code archives, `shaderarchive-finchgame-glsl_switch.ushaderbytecode` (19,097 shaders) and `shaderarchive-global-glsl_switch.ushaderbytecode` (1,399), both in **format version 1** with every shader a Zlib stream that inflates to an `LSLG` record. This is the same kind of single-stage shader as [Octopath Traveler 0](../01005270232F2000/README.md)'s version 2 archives; Unreal combines vertex and pixel shaders per draw, so the split pairs each vertex shader with a pixel-input cover.

The split finds **19,694 distinct shaders** (17,364 pixel, 1,794 vertex, 511 compute, 25 geometry; 18,297 in the game's archive and 1,397 in the global one) with zero `[RED]` lines, and 1,794 variation pairs. [Early Unreal Engine 4 pak version 5 and shader archives version 1 (Zlib)](../../../docs/reference/shaders.md#early-unreal-engine-4-pak-version-5-and-shader-archives-version-1-zlib) says what the general tool changes were: `uepak.py` was taught both formats, and decodes this game's Zlib shaders locally with no Mac Oodle job. The `test_uepak*` unit tests still pass.

## Survey

The survey did not extract the 2.39 GB pak: `010038900DFE0000-work/make_survey_dump.py` beside the `.nup` writes a folder dump whose RomFS files are **sparse** (real sizes and real 32-byte headers, no game data), so `build_ipa_from_nsp.py --survey-only` could read the ExeFS, the manifest, the rewrite-site census and the imports in place. The report is `010038900DFE0000-work/survey-dump/010038900DFE0000_survey/survey.md`. The shader split then read the `.nup` directly; no game file was copied into the repository.

## Status

Profile, shader tables, docs and the Android game list are in place; the docs / Android game-list checks pass. Nothing is known yet about boot, input, rendering or performance. Next: bring-up on the Mac, then a phone run on the system driver and a Turnip driver, as in [03-bringup](../../../docs/agents/03-bringup.md) and [06-dev-loop](../../../docs/agents/06-dev-loop.md).
