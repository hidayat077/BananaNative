# Hogwarts Legacy (`0100F7E00C70E000`)

Snapshot v327680, SDK 17.5.4 (`SDK_libz-17_5_4-Release0`), Unreal Engine 4 (the `Phoenix` cook, `UE4CommandLine.txt` points at `Phoenix.uproject`), NVN. Modules `main`, `subsdk0` (`NintendoSDK_movie`), `sdk`; `rtld` is replaced by the host. The source is a 14 GB `.nup` read in place — the RomFS was not extracted into the repository or the home folder.

Status: profile only. Nothing has been run on a phone yet.

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

## Open

- Build an APK with this title ID (the CI workflow's `nxGames` now lists it) and try it on the phone.
- Boot on a phone and fix bring-up from the symptom index; measure the memory layout.
- The two shaders that exceed the stage's binding limit are missing; decide whether to raise the limit.
