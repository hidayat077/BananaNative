# Dying Light: Platinum Edition (`01008C8012920000`)

Base game 1.0.5, SDK 12.3.3, NVN, Techland's own engine with FMOD. The profile was made from a survey of the user's own `.nup` (read in place, no extraction). Bring-up on Android is still open: nothing below has been validated on a phone.

## What the dump holds

- One `base` package, NCM version `v327680`, with ExeFS and RomFS. The `.nup` carries the game's meta block (title, publisher Techland, version 1.0.5).
- Modules `main` (`8c93b930348c978708ae9e6bc0c9cca9`), `subsdk0` (NintendoSDK_movie, `434729719bfacada3bd0ed4842a10e55`), `sdk` (`dfa12b7019fffbe725fa4b4f1335988c`) and `rtld`. `main` imports from `subsdk0`, so the movie module is kept; `rtld` is replaced by the host.
- The NPDM declares 12 MB of system resource and `main` never sets a heap. The first phone run (2026-11-28) trapped at `main+0x732854` (SIGILL, the game's own abort) after `VIRTUAL_MEMORY_ALLOCERROR_OUT_OF_PHYSICAL_MEMORY`: the game's allocator asked for an 8 GiB virtual region (`x21 = 0x200000000`) which the survey's default 4.375 GB alias region could not hold. `game.tsv` now uses a Switch-sized 64 GB alias region, like other games whose engines reserve large address ranges; this is a virtual reservation only, still unvalidated on the next boot.
- RomFS: 583 files, about 16.28 GB. The base package also holds the game's DLC folders (`DW_DLC0` … `DW_DLC49`), with `DW/` carrying the game data.
- Techland `.pak` archives are ZIPs; `shader_split.py`'s generic ZIP reader finds the game's GLSLC outputs in `DW/Data/optimized_nvn.mp`. The `.ppfx` files are the engine's text shader scripts, not NVN binaries.

## Files and shaders

- `shader_split.py` from the `.nup` finds **5,957 shaders**: 10 compute, 4,151 pixel, 1,796 vertex, all in `DW/Data/optimized_nvn.mp`. The vertex programs are single-stage and pair with one cover pixel shader (`12bf4a91921a5e08`).
- `shader_groups`: `/DW/=dw`. `shader_files.tsv` names the one file the targeted Android split must read.
- `romfs_marker` is `DW/Data1.pak`, a small file the dump holds; change it if another build is found that shares it.

## Open

- No phone run: the memory layout, `romfs_marker`, module set and shader coverage are survey values, not measured.
- `depth_mode` is unset (gl); set it to `zero` only if disassembly shows `nvnDeviceSetDepthMode` with near = 0.
- The DLC folders are part of the base package; whether the launcher should expose them as add-on content has not been checked.
- Anything about rendering, frame rate, audio and input is unknown until the game boots.
