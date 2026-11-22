# South Park: The Fractured but Whole (`01008F2005154000`)

Snapshot v327680 (display version 1.05), SDK 3.5.1, Unreal Engine 4 with Havok and Wwise, on NVN. Ubisoft. Modules `main`, `subsdk0` (GLSLC, NVN's run-time shader compiler) and `sdk`; `rtld` is replaced by the host. The dump came from a plaintext NSP-Box NSP, which is a complete snapshot of v327680 rather than the base game alone.

Status: plays on a phone (2026-11-22, Turnip, 1080x2400, about 30 fps, 579 draws a frame). No Mac was used; both bring-up fixes are in the profile and the shader library, not in `env.txt` or a side layer. The profile was started from the survey's provisional keys, with [`0100AE00096EA000`](../0100AE00096EA000/README.md) (Hyrule Warriors: Definitive Edition — SDK 3.5.1 and a GLSLC `subsdk0`) as the closest reference.

## What it needs

| Setting | Where | Why |
|---|---|---|
| SDK 3's NVN object sizes and service layouts | [`game.tsv`](game.tsv), host | SDK 3.5.1, far older than Mario's 16.2; the host was converted to SDK 16 sizes. Check [NVN objects](../../../docs/reference/nvn-objects.md) one by one |
| `modules main subsdk0 sdk` | `game.tsv` | what `main` imports, from the survey; `subsdk0` is GLSLC |
| `pad_layout full` | `game.tsv` | a 3D Unreal game; the camera is on the right stick |
| `zip_store .sdfdata` | `game.tsv` | SDF data is already compressed; storing it avoids recompressing ~17 GB |
| 5 GB reserve, 4.375 GB alias, `app_pool 0xCD500000`, `sys_resource 0x1000000` | `game.tsv` | the survey's guess from the NPDM (16 MB system resource, virtual address memory, no heap set by `main`); **not measured** |

## Files and shaders

- RomFS: 3,010 files, 16.96 GB. Almost all of it (`camel/sdf`, 3,008 files) is `*.sdfdata`; the top level has two boot images. `romfs_marker` is `camel/sdf/nx/data/_FileList.txt`.
- There is no BNSH / BAHS at the RomFS root and no shader container the survey recognises. The shaders may be inside the `.sdfdata` archives or compiled by the game's own GLSLC in `subsdk0`; nothing is known yet. Finding them is the first graphics task ([shaders](../../../docs/reference/shaders.md), [04](../../../docs/agents/04-graphics.md)).
- `main` imports `nvnBootstrapLoader` and NVN's `nv::InitializeGraphics` / `nv::SetGraphicsAllocator`, so NVN is the graphics API. `nso_syms.py` reports 554 new-only imports in `main` and 629 of the host's 1,248 HLE hooks present in this `sdk`.
- Rewrite census (`insn_census.py`): `main` 8,169,668 instructions, 12,449 x18 uses, 556 `mrs tpidr_el0`, no `svc`; `sdk` 1,014,306 instructions and 106 `svc`; `subsdk0` 1,900,963 instructions, 4,659 x18 uses.

## Found on this game

General shader change first needed here; it applies to every game:

- **Shaders:** [Whole-asset zlib records in Snowdrop SDF slices](../../../docs/reference/shaders.md#whole-asset-zlib-records-in-snowdrop-sdf-slices): `shader_split.py` found nothing in the `.sdfdata` slices until it learned the `0xabcd00d4` framing. A full split finds 4,604 shaders (75 compute, 457 pixel, 4,072 vertex); the 27 `.mcache` assets of `sdf-A-0000.sdfdata` alone hold 27 of them (7 compute, 17 pixel, 3 vertex). The `.mshader` entries still point at offsets that are not zlib streams, so those assets may be missing from the 4,604.

Game-specific bring-up, both resolved:

- **Startup:** the Snowdrop page allocator's unchecked reservation crashed at `main+0x17fb4b0`, writing `0x20000ffff` in `DT_INIT`, with the same address as [Separate virtual ranges in Snowdrop's page allocator](../../../docs/reference/memory.md#separate-virtual-ranges-in-snowdrops-page-allocator). The survey's 5 GB / 4.375 GB layout left no room for the first `AllocateAddressRegion`; the profile now gives the game a Switch-sized 64 GB alias region in a 66 GB block (`mem_reserve` / `mem_alias`). `snowdrop_pools.py` does not recognise this older build's encoding, so the general cut does not apply, but the layout is the hardware one the engine expects.
- **Black screen after the logo:** pixel shader `a19dd0ddaa9e08ce` was absent from the library and the game drew 5 draws a frame. It was captured with `DM_NVN_SHDUMP`, then put in `<dump>/updates/327680/nvn_shdump` and picked up by `shader_split.py` (`nvn_dumps`), so the main library now holds all 4,605 shaders. The next run drew 579 draws a frame with no missing-shader line. The shader is still likely one of the undecoded `.mshader` assets; decoding those would make the capture unnecessary on a fresh dump.

## Open

- Decode the `.mshader` assets of `sdf-A-0001.sdfdata` (not zlib; likely Oodle) so a fresh dump does not need the saved shader.
- Measure the memory layout now that the alias region is the hardware one.
- Confirm a longer play session on the system driver as well as Turnip.
