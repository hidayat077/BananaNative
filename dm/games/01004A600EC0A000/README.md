# Immortals Fenyx Rising (`01004A600EC0A000`)

Base game 1.3.4 (the `.nup` meta block; NCM version 720896), SDK 10.4.0, NVN, Ubisoft's Anvil engine with Havok and Wwise. The dump is one base package, no updates and no add-ons: three code modules (`main`, `rtld`, `sdk`) and a RomFS of 615 files, 16.35 GB — 372 `.forge` world archives, 132 `.webm` videos, 109 Wwise `.pck` files and the root `ShaderDataNVN.bin`. The profile was made from a survey of the user's own `.nup`, read in place; **bring-up is still open** — no Mac, iPhone or Android run has been made.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | `main` imports only from `sdk`; there is no `subsdk` on this build |
| `romfs_marker ShaderDataNVN.bin` | `game.tsv` | the root shader archive, present in every copy of the game |
| `zip_store .forge:.pck:.webm` | `game.tsv` | those files are already compressed or media |
| `mem_reserve`, `mem_alias`, `mem_stack`, `app_pool` | `game.tsv` | the survey's provisional split: the NPDM declares 8 MB of system resource and `main` never sets a heap, so the game maps its memory itself in a 4.375 GB alias region; refine after a real boot |
| `sys_resource 0x800000` | `game.tsv` | the NPDM declares 8 MB of system resource, enabling virtual address memory |
| `shader_groups ShaderDataNVN=global` | `game.tsv` | every shader comes from the one code-only archive (see Shaders) |
| `status bring-up` | [`readiness.tsv`](readiness.tsv) | the launcher warns that the game may stop or draw wrongly; nothing has been tested on a phone yet |

SDK 10.4.0 is much older than Mario's SDK 16: NVN object sizes, API versions and service layouts differ, so any boot failure must be checked against [NVN objects](../../../docs/reference/nvn-objects.md) rather than assumed to be the game's own code. The survey's rewrite-site census is in the work folder (see Survey).

## Shaders

`ShaderDataNVN.bin` (27.8 MB) is a set of Ubisoft **GPUCODEONLY** code-only archives. Each is a 0x10-byte header followed by a code area, a four-byte record and the control section that names the code length at `+0x704`. Graphics code areas begin with `0x12345678`; compute ones are headerless and their stage comes from the control's thread group at `+0x748`. The first blob holds headed code and the later ones headerless code; both pair by the same rule. This needed a new general reader in `shader_split.py` ([shaders.md](../../../docs/reference/shaders.md#ubisofts-gpucodeonly-shader-archives)), with `test_gpu_codeonly.py` covering both forms.

The split reads **2,578 distinct shaders** (1,388 pixel, 180 vertex, 95 geometry, 7 domain, 3 hull, 905 compute) with no `[RED]` lines: 4,259 control sections pair with their code, of which 1,681 are copies of a key found earlier. There are no vertex / pixel variation pairs: every program is a single stage, so the split supplies no pairing table.

## Survey

The survey did not extract the 17 GB `.nup`: `01004A600EC0A000-work/make_survey_dump.py` beside it writes a folder dump whose RomFS files are **sparse** (real sizes and real 32-byte headers, no game data), so `build_ipa_from_nsp.py --survey-only` could read the ExeFS, the manifest, the rewrite-site census and the imports in place; `profile_guess.py` also read the `.nup`'s package paths directly. The report is `01004A600EC0A000-work/survey-dump/01004A600EC0A000_survey/survey.md`. The shader split then read the `.nup` directly; no game file was copied into the repository.

## Found on this game

- **Ubisoft's GPUCODEONLY shader archives**: first game with this container; the reader in `shader_split.py` is general ([shaders.md](../../../docs/reference/shaders.md#ubisofts-gpucodeonly-shader-archives)).

## Status

Profile, shader tables, docs and the Android game list are in place; the docs / Android game-list checks and all 83 shader-split Python tests pass. A code-only pack from the `.nup` (`pack_android.py --no-shaders`) rewrote both modules with no `[RED]` lines, but the Vulkan shader library, the APK and any phone run have not been made. Bring-up on the Mac and a phone run on the system driver and a Turnip driver are still open, as in [03-bringup](../../../docs/agents/03-bringup.md) and [06-dev-loop](../../../docs/agents/06-dev-loop.md#checking-a-change-for-android).
