# LEGO Star Wars: The Skywalker Saga (`010042D00D900000`)

Update 1.10.0 (v589824), Nintendo SDK 12.3.6, Traveller's Tales' engine, NVN. On an M4 Pro with macOS 27.2.1, the game reaches Episode IV, “Boarding Party.” The corridor, characters, in-game text and subtitles render. The host plays audio and reloads a save in the first slot.

## What it needs

- [The profile](game.tsv) loads `main`, `subsdk1` for movies and `sdk`. The NPDM declares zero system resource. The game opens a 3.13 GiB heap in the 3.25 GiB guest heap region. The game sets an upper-left window origin and zero-to-one depth. `depth_mode zero` supplies that depth mode to pipeline prebuilds.
- [The base and update shader map](shader_map.tsv) covers 667 executable shaders and 97,648 RomFS shaders. With the 17 supplied add-ons, the NSP script derives a map with 984 more shader keys. All 99,299 shaders built for Mac on `e641898384f7`.

## Frame rate

- [The profile](game.tsv) uses `fps_target 60` and `present_interval display`. The game asks NVN to present every two refreshes. The existing host pacing mode presents once per refresh at 60 Hz and the game's updates follow elapsed time. The player's **Frame rate** setting can select 30 fps.
- On the M4 Pro with a 60 Hz display, the stationary Boarding Party corridor went from a median 33.34 ms per frame with the original profile to 16.67 ms with display pacing. The title montage reached the same scenes at six matched wall-clock checkpoints between 52 and 146 seconds after the first present in the 30 and 60 fps Mac apps.

## Found on this game

- **Resolved** — [TT Games DAT and executable shaders](../../../docs/reference/packing.md#tt-games-shaders-are-missing-from-the-executable-and-dat-archives): the readers are now in this fork's `shader_split.py`, so Prepare's split opens the CC4 DAT indices and the loose control sections of the 1.10.0 `.nup` (the packer's `DM_OODLE_OOZ` worker on a PC, `OodleService` on the phone). Upstream also resolves [DFLT-compressed add-on shaders](https://github.com/seedds/nxnative/blob/main/docs/reference/packing.md#dflt-shader-members-in-tt-games-dlc); this snapshot has no add-ons.
- **Resolved** — [Float views for integer buffer images](https://github.com/seedds/nxnative/blob/main/docs/reference/rendering.md#float-views-for-integer-buffer-images) and [Source-only blending on floating-point targets](https://github.com/seedds/nxnative/blob/main/docs/reference/rendering.md#source-only-blending-on-floating-point-targets). The latter restored the 3D picture after the opening crawl.
- **Resolved** — [GPU copy destinations in protected pool chunks](https://github.com/seedds/nxnative/blob/main/docs/reference/nvn-objects.md#gpu-writes-dropped-by-a-pool-buffer-made-over-write-protected-pages), [3D copies mirrored to texture storage](../../../docs/reference/nvn-objects.md#gpu-copies-into-textures-uploaded-from-their-storage), and [Buffer copies over the storage of textures uploaded from it](https://github.com/seedds/nxnative/blob/main/docs/reference/nvn-objects.md#buffer-copies-over-the-storage-of-textures-uploaded-from-it), found during earlier bring-ups.

## Mac validation

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac-app --gpu none --no-prebake` and open the app. Start a new game, select Episode IV, and continue to the “Boarding Party” corridor. From a save made there, use Continue Game and confirm the first slot.

## Android

The profile is in `android/app/gradle.properties`' `nxGames` and in the APK build workflows, with `status bring-up` in [`readiness.tsv`](readiness.tsv): the launcher warns on every phone. This packages the game but is **not** a claim that it boots on Android — there has been no phone run. The user's `.nup` holds base v589824 (1.10.0) only: its `main` (`ec593a5f9552100a9fe95c4ca9e34873`), `sdk` (`26dfae95ca29ae3c49dfd84d69f8b7fd`) and `subsdk1` (`c21e6cdb086c7734345cb677057749f0`) build IDs match the registered build row, so Prepare can use it directly. The 17 add-ons the Mac notes used are not in that snapshot. The profile reserves a 4 GiB guest region and opens a 3.13 GiB heap, which a phone has to supply.

## Open

- Later chapters, local co-op and iOS gameplay await testing.
- Android bring-up: the game is packaged in the APK but has no phone run; check its 3.13 GiB heap request first before calling it playable.
