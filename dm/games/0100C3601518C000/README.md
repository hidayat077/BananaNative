# Sid Meier's Civilization VII (`0100C3601518C000`)

Version 1.5.1 (`v1703936`), SDK 19.3.11, Firaxis's engine with Coherent Gameface / V8 and Wwise, on NVN. These module build IDs are from the 1.5.1 `.nup`: `main` (`e656e23d5435311081509ba2a2ae81d3`) and `sdk` (`ad9ee4518b1d8ca811fbdf35fc4f26d5`); the registered `MediaDecoders.NN.nro` is rewritten and loaded through `nn::ro`. The gameplay check below was on 1.5.0.1 (`v1638400`, `main` `5d9401301ab2bb4917bafd939e5b3dd8`, `sdk` `df7a3960c74c3a4803f826c587153252`).

**Mac gameplay tested, 2026-11-15:** first-run display options, main menu, leader/civilization selection, world generation, unit selection and the founder's settlement action (London, turn 1), then Quick Save. Audio plays with no underruns. The gameplay check stayed under ten minutes. iOS is blocked by address space, not yet supported; see [Memory](#memory).

## What it needs

| Setting | Why |
|---|---|
| `sys_resource 0x200000` | the NPDM declares 2 MB of system resource, enabling the SDK's virtual address memory |
| `app_pool 0xCD500000` | hardware application-pool budget; the game requests a 3.01 GB heap |
| `mem_reserve 0x300000000`, `mem_alias 0x218000000`, `mem_stack 0x10000000` | 3.125 GB heap region, 256 MB stacks, 8.375 GB alias region; the Mac automatically uses a 64 GB alias region with the same heap/stack partition |
| `depth_mode zero` | the game calls `nvnDeviceSetDepthMode` with near = 0 |

No game-specific code patches, host branches or new profile switches. The existing SDK matcher reduces the address-region node area; the existing general odd-stack-frame rule handles this build's four hand-written SVC blocks.

## Rendering

`ShaderAutoGen_Linux_NVN_FinalRelease.bs` holds 730 single-stage CIVBSF records in 1.5.1: 325 vertex, 309 pixel and 96 compute, with duplicates. The split finds **440 distinct shaders** (121 vertex, 224 pixel, 95 compute) with no `[RED]` line; the existing pixel-input cover supplies vertex padding, and the library's effect-to-stage pairing is still not decoded. The 1.5.0.1 Mac check below had 801 records and 481 distinct shaders; the two versions' keys have only one in common, so the 1.5.0.1 `shader_seen.tsv` selected set and `pso_manifest.tsv` prewarm seed were dropped rather than kept stale ([Firaxis CIVBSF shader libraries](../../../docs/reference/shaders.md#firaxis-civbsf-shader-libraries)).

Everything in the two paragraphs below is from the **1.5.0.1 Mac run**, not from 1.5.1: the prewarm and seen lists covered first-run menus, new-game setup and the first settlement; the shader map was compared with the runtime with no shader missing on that route; all 481 shaders were created at startup, and the two measured dual-source programs (`064c4ce6df409438`, `f2755f3bfebde977`) used their existing dual-source entry points.

The 1.5.0.1 Mac app build was 7.9 GB, chain `cd731f39944d`, shader inputs `20383d074091`; its rebuilt prewarm archive held 77 render and 20 compute pipelines with zero failures and no dual-source warning. The CIVBSF reader used for 1.5.1 was added later to this repo's `shader_split.py` with synthetic tests; no Mac or phone run has used the 1.5.1 tables yet.

On the Apple M6, the settled map held about 60 fps: frame time p50 16.66 ms, p95 17.04 ms, GPU p50 5.57 ms over a 300-frame window. Resident memory reached 3,330 MB, Metal allocations 2,828 MB, then stayed near that level in this short test. These are absolute measurements, not a performance comparison with another port.

## Memory

The game sets a 3.01 GB main heap. Its Coherent UI also needs V8's pointer-compression cage: `IsolateAllocator::InitializeOncePerProcess` (`main+0x2d05a50`) supplies a 4 GB reservation size and 4 GB alignment. `VirtualMemoryCage::InitReservation` (`+0x305d8b8`) first reserves 8 GB to find an aligned interior address, frees it, and retries 4 GB with that address as a hint. Coherent's `v8::base::OS::Allocate` (`+0x2a69af8`) discards the hint; AppUI's allocator (`+0x5f4964`) uses the SDK's address selection. Runtime probes show repeated 8 GB / 4 GB attempts, followed by an oversized reservation retained around the aligned cage.

The automatic region check measured an 8 GB region at alias +0.06 GB, opened through region +2.76 GB with about 6 MB mapped in that region. A 5 GB overall block with a 1.375 GB alias region cannot hold it. The profile records a 12 GB layout that holds the observed request; this is above the iPhone budget without extended virtual addressing. The normal Mac alias region fits it.

**Open, iOS:** correctly aligned Coherent/V8 allocation can avoid the doubled reservation, but even the 4 GB cage plus the main heap exceeds the current iPhone budget. A compact cage or allocator replacement must preserve V8's pointer compression, bounded allocator and GC assumptions. Neither a blind size clamp nor a generic `AllocateAddressRegion` that returns less memory is valid. No such workaround has been added.

## Mac route

Build with `scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> --platform mac` (or `--platform mac-app` for a standalone app). The work folder has `run.py`: `boot <log> <seconds>`, `step <shot> <buttons...>`, `ctl <line...>` and `stop`; it uses a dedicated `civ7host`, window, sound and Metal HUD. The build's `run/save` holds the tested quick save.

From the main menu: A on New Game; Right, A selects Ada Lovelace; A opens Great Britain's details, A selects it; A continues the default small-map setup. On Overview, Down, A chooses Launch Game. After generation, A begins; R skips the introductory help cards. On the map, A selects the starting hex, A opens the founder's actions, Right, A founds London. Plus, Down, A quick-saves. Touch input was not needed for this route.

The supplied folder has 42 licence-only DLC packages; the game displayed owned content in its pause menu. The base game and other updates have not been tested.

## Found on this game

- [Firaxis CIVBSF shader libraries](https://github.com/seedds/nxnative/blob/main/docs/reference/shaders.md#firaxis-civbsf-shader-libraries).
- [Mac prewarm pipelines reused after a packed shader library changes](https://github.com/seedds/nxnative/blob/main/docs/reference/shaders.md#mac-prewarm-pipelines-reused-after-a-packed-shader-library-changes): stamp lookup now follows the packed library.
- [Stack frames of an odd size](../../../docs/reference/rewriter.md#stack-frames-of-an-odd-size-sub-sp-sp-imm): existing general rule, no new cut in this port.
- [A written secondary blend source](../../../docs/reference/rendering.md#a-written-secondary-blend-source-nvn-dual-source-blending): two measured shaders (`064c4ce6df409438`, `f2755f3bfebde977`), originally in `dual_want.tsv`, now handled by `main0_u` in `<key>_bx`.
- **Resolved (needs phone confirmation): 1.5.1 main-menu crash** — on Turnip the 1.5.1 build booted and reached the frame loop but crashed at the menu after 20–80 s (`CRASH sig=11 addr=0 pc=main+0x33ba08`) when `Network::Friends::Manager::Update` tried to double its blocked-users vector to `0x800000` (`0x4c000000` bytes) and wrote to the returned null buffer. `NintendoProvider`'s constructor (`main+0x512494`) skips `RefreshBlockedUsers` (`main+0x5127b0`) when `nn::friends::NotificationQueue::Initialize` fails, leaving `m_blockedCount` (`+0xd00`) uninitialized on the heap. Fixed generally in `hle_friends.c` ([Friends notification queue initializes offline](../../../docs/reference/services.md#friends-notification-queue-initializes-offline)); the temporary workaround cuts in `patches.py` were removed.

## Android

The profile is in `android/app/gradle.properties`' `nxGames` and in the APK build workflows, with `status bring-up` in [`readiness.tsv`](readiness.tsv): the launcher warns on every phone. This packages the game but is **not** a claim that it boots on Android — there has been no phone run. The Mac's 12 GB address-space layout (V8 cage plus the 3.01 GB heap) may be the same obstacle on Android as on iOS; that is the first thing a bring-up run must check.

The supplied `.nup` is 1.5.1 (`v1703936`), whose module build IDs differ from the Mac-tested 1.5.0.1 build; the build row now names 1.5.1. `shader_split.py` was taught the CIVBSF container and the 1.5.1 entry layout (header revision `0x1118` with `0x50`-byte entries whose `+0x8` points to a `{data offset, length}` pair), so the tables in this profile are now split from the 1.5.1 `.nup` itself — no game data was copied into the repository. Android Prepare should now find all 440 shaders; this still needs a phone run to confirm.

## Open

- iOS support: the V8 cage and main heap exceed the address-space budget described above; no working IPA claimed.
- Android bring-up: 1.5.1 boots on Turnip and reaches the frame loop; the temporary workaround cuts have been removed, and the `hle_friends.c` `NotificationQueue::Initialize` fix still needs a phone run to confirm the menu crash is gone.
- The 1.5.1 shader tables come from the split alone: no Mac run has compiled them or checked them against the runtime, and the 1.5.0.1 selected-set and prewarm seeds were dropped. A first phone run (or Mac bring-up) should confirm Prepare finds all 440 shaders and that no program is missing from the map.
- The run logs a failed initial upload of a multisample Depth24 texture (`format 0x33`, target 5). Both GPU and CPU deswizzle reject multisample geometry; the Metal substitute also requires depth conversion. The observed map renders, but the upload's relevance and later shadow/rendering coverage remain unverified; no blanket skip was added.
- Quick Save was written and appeared in Load Game with the correct turn and civilization. A reload was started, but its return to the map was not verified before the ten-minute gameplay-testing budget ended.
- Later turns, age transitions, multiplayer, long-session memory stability, and full movie coverage have not been tested.
