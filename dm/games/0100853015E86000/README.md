# No Man's Sky (`0100853015E86000`)

Base game v5308416 (display 7.10.0), SDK 20.5.17, Hello Games' own engine with Havok and Wwise, on NVN. Modules `main` and `sdk`; `rtld` is replaced by the host. The package is a `.nup` read in place: its RomFS is 99 `NMSARC*.pak` archives (3.71 GB), with no BNSH / BAHS at the top level.

Status: surveyed, profiled, shader-mapped and packed for Android. The first phone run aborted at startup on the heap/used-memory mismatch; the profile now reports the modules' size and needs a rebuilt APK and a confirming run. A full split finds 10,349 distinct shaders (3,548 vertex, 6 geometry, 6,666 pixel, 129 compute); the Vulkan library translated all of them and every module passed `spirv-val --target-env vulkan1.3`. The rewriter patched `main` (2.0 MB, 74 gates) and `sdk` (0.7 MB, 109 gates) for Android. `sdk` is build `0023a1b0…`, the same build [STEINS;GATE RE:BOOT](../01008BC024FDE000/README.md) ships, so that profile is the closest reference for the SDK layer.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main sdk` | [`game.tsv`](game.tsv) | what the survey found in the ExeFS; there is no `subsdk0` |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | the survey's guess from `main`'s heap request and the NPDM's zero system resource, as the other games without virtual address memory; **not measured** |
| `romfs_marker NMSARC.Shaders.pak` | `game.tsv` | a file in every copy of this version's RomFS, to tell it from another version's data |
| `mem_used 0x6000000` | `game.tsv` | the rewritten modules are 92 MB, so the default 64 MB of used memory made the game's `SetHeapSize(pool − used)` fail at boot ([Heap sized from the application pool less used memory](../../../docs/reference/memory.md#heap-sized-from-the-application-pool-less-used-memory)) |

## Survey

- Code: `main` 32.5 MB of text, 41.7 / 6.9 / 1.5 MB segments, 26.4 MB bss, build ID `157fa464786a74d276a8ec7da9d5a385`; `sdk` build `0023a1b06130cd795f7bc4613772af0a`. `main.npdm` reports program ID `0100853015E86000` and 0 MB of system resource.
- SDK strings: `NintendoSDK_nnSdk-20_5_17`, `NintendoSDK_NVN-20_5_17`, libcurl and libz of the same version. `sdk` imports 863 of the host's 1,248 HLE hook names; `main` imports 510 symbols new to the compared game, among them `nv::InitializeGraphics`, `nv::SetGraphicsAllocator`, `nvnBootstrapLoader` and the `nn::ro` loader.
- Rewrite-site census (`insn_census.py`): `main` 10,928,830 instructions, 44,530 x18 uses, 809 `mrs tpidr_el0`; `sdk` 1,674,226 instructions, 58 `svc` (one of each number 1–0x1d, 0x1f, 0x21–0x2f except 0x2b, 0x2e, 0x30, 0x31, 0x37–0x3f, …), 15,571 x18 uses, 124 `mrs tpidrro_el0`, 43 `mrs cntpct_el0`.
- All 99 RomFS files are `.pak`; the largest are `NMSARC.audio.pak` (1.65 GB), `NMSARC.MeshPlanetBIOMES.pak` (436 MB), `NMSARC.MeshCommon.pak` (425 MB), `NMSARC.TexUI.pak` (102 MB) and `NMSARC.Shaders.pak` (22 MB). The survey reads no BNSH / BAHS header at the root.
- The survey was made without extracting the RomFS: the `.nup`'s code modules were copied (39 MB) and a manifest of path, size and the first 32 bytes of each RomFS file was fed to the repository's own survey code. The report lives in the game's work folder, beside the `.nup`, outside the repository.

## Shaders

- The 99 `.pak` files are Hello Games' HGPAK archives: a header (`HGPAK\0\0\0`, version 2, file count, chunk count, compressed flag, data offset), a 32-byte-per-file index (MD5 of the path, offset, decompressed size), an array of compressed chunk sizes, then the data in 0x20000-byte Oodle chunks on Switch. The first member is a CRLF-separated manifest of the other 21,166 paths.
- `NMSARC.Shaders.pak` holds 21,117 `.nvb` files (and 49 GLSL source `.bin` files). Each `.nvb` is one NVN program: its control section starts with magic `0x98761234`, holds its own length at `+0x20` and the code area's length at `+0x704`; the code area starts at magic `0x12345678` (graphics) or `0x12345679` (compute), with the SPH word at `+0x30`. The stage comes from the SPH word, so no name parsing is needed.
- [Whole-file Oodle chunks in Hello Games' HGPAK archives](../../../docs/reference/shaders.md#whole-file-oodle-chunks-in-hello-games-hgpak-archives): `shader_split.py` now reads HGPAK members and `.nvb` records through `oodle()` (`DM_OODLE_OOZ` on a PC), and skips paks whose manifest has no `.nvb`, so the 1.65 GB audio pak is not inflated. A chunk whose stored size is already 0x20000 is kept whole, not sent to the decoder.
- The full split writes `shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv` and `pso_pairs.tsv` into the profile. The map's sources name the `.nvb` members inside `NMSARC.Shaders.pak`, so a targeted split can read them straight from the `.nup` too.
- The Android Vulkan library has 18,475 modules (10,349 base, 5,589 `uint`, 2,525 `uint_mixed`, 12 `shuffle_lds`) in 136.1 MB (559.7 MB of SPIR-V before compression); chain `a3d8757f9bcd`, Vulkan chain `0f9e17c20bc3`, inputs `ab8fa1812c3e`. The pack is 222.7 MiB; the RomFS stays in the game's `.nup`.

## Found on this game

- **SIGSEGV at `addr=0x200` (`main+0x2483248`) and `nvnDeviceRegisterFastClearColorui` stub, fixed in host (awaiting phone retest):** `nvnTextureBuilderGetStorageClass` and `nvnTextureGetStorageClass` returned `0`, causing the engine to treat sparse virtual-pool allocations as non-sparse, call `nvnMemoryPoolMap` on the `0xf40000000`-byte `VIRTUAL | CPU_NO_ACCESS` pool (`NULL`), and pass `NULL + 0x200` to `memcpy` when uploading a 16x16x1 3-level texture ([Texture storage classes are non-zero](../../../docs/reference/nvn-objects.md#texture-storage-classes-are-non-zero)); `nvnDeviceRegisterFastClearColori` and `nvnDeviceRegisterFastClearColorui` are now implemented in `hle_nvn.c`.
- **SIGSEGV writing into `argv[0]`, resolved:** with the heap fixed, the second phone run reached `nnMain` and faulted at `main+0x1300e0` (`lr=main+0x1300dc`), writing into `libnxhost+0x92e70`, a read-only host string holding `"game"`. The game lowercases its argument strings in place; the host had handed it a literal instead of writable memory ([An argument string the host keeps read-only](../../../docs/reference/services.md#an-argument-string-the-host-keeps-read-only)). `argv[0]` is now a writable `static char[]` in `dm/host/main.c`. Needs a rebuilt APK and a confirming run.
- **Startup abort, resolved:** the first phone run aborted in `nninitStartup` at `main+0x2614c58` with `nn::diag` Result `0x1003`. The log's `[mem] SetHeapSize 0xc9400000 + modules 0x5c00000 > app pool 0xcd500000 (used memory reported 0x4000000 + bss 0)` is the known [heap-sized-from-used-memory case](../../../docs/reference/memory.md#heap-sized-from-the-application-pool-less-used-memory): the game takes its heap as pool − used memory, but the host checks the request against the real 92 MB of rewritten modules. The profile now reports `mem_used 0x6000000` (96 MB), as [Catherine: Full Body](../0100BF00112C0000/README.md) does for the same module size. Needs a rebuilt APK and a confirming run.

## Open

- Measure the memory layout on a phone, and check that the 4 GB block is large enough for the game's heaps.
- Test the packed game on a phone with the system driver and Turnip; the workflow now builds the test APK with this title ID as well.
- Check the 6 geometry shaders and the 129 compute shaders in a run: their thread groups and shared memory come from the control sections and have no PC version to compare with.
