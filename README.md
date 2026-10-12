# AlphaCWWorldMotion

A separate visual motion mod for Cube World Alpha: slow cloud drift and gentle voxel-leaf sway. Made by DieuDeGlace with AI assistance from OpenAI ChatGPT/Codex.

Install `AlphaWorldMotion.dll` and `AlphaWorldMotion.ini` in the game's `Mods` directory, then restart through the Alpha mod loader. F6 toggles motion while the game has focus. Remove the DLL before the next launch to uninstall. This mod does not require the water mod and does not modify world data, collision, saves or game assets.

## Motion and seams

Clouds follow a slow, bounded horizontal drift, using their native draw transform. Leaves are selected from verified Alpha block type 8, never from their colors. Alpha material 11 is stationary ground/road terrain and is explicitly excluded; Steam snow-leaf IDs are not reused. A cached per-corner weight drives two added vertex-shader instructions. Native shader bytecode, lighting, vertex colors and face IDs remain intact.

Corners touching wood, rock, ground or other stationary materials are pinned. Unknown terrain data also pins a corner. Chunk-boundary corners remain pinned and movement ramps up over the next three blocks, so separately admitted chunks and different distance fades cannot split their common edge. This is intentionally conservative: crowns near chunk borders sway less. Trunks, reeds, buildings and characters remain stationary.

When AlphaCWShadows and CWSMTAA are loaded under their established DLL names, the motion hook waits for them and runs outside their draw hooks. Shadows capture the animated buffer, and their replay shaders receive the same displacement. TAA continues to recognize the preserved native shader metadata. Version 0.2 exports an explicit leaf replay function used by AlphaCWShadows 0.3, so caster and receiver draws apply the same displacement as the visible leaves. Cloud drift is captured once and never applied again during shadow replay. Cloud shadows are supplied by AlphaCWShadows 0.3 using a separate bounded map; install both updated DLLs.

## Performance

No extra draw calls, per-frame mesh uploads, new render targets or extra screen passes. Trigonometry runs once per frame on the CPU; leaves use two additional vertex-shader instructions. New meshes are prepared incrementally, processing at most 256 quads and approximately one millisecond of classification work per frame. Initial buffer copying and final upload are separate bounded admission costs, not covered by that classification timer.

Only nearby identity-transform terrain meshes are eligible. The expanded vertex cache is capped at 24 MiB and 64 entries; meshes unused for three seconds are discarded. A source buffer is limited to 512 KiB. Empty/non-leaf meshes are cached to avoid repeated inspection. Original buffers are never rewritten. Hardware-dependent FPS impact and dense-forest frame pacing are not yet established.

Newly visible trees may take time to begin swaying. Native cloud culling still applies, and drift reverses gradually rather than carrying clouds outside their generation area indefinitely. Existing native buffer boundaries and unfamiliar declarations/transforms retain native rendering. Unsupported shader register layouts also retain native rendering.

## Settings

Settings are read at startup from `Mods/AlphaWorldMotion.ini`.

| Setting | Default | Meaning |
| --- | --- | --- |
| Enabled, Clouds, Leaves | 1 | Independent motion switches |
| LeafAmplitudePercent | 12 | Hundredths of a block; 0-25 |
| LeafRange | 96 | Full motion range in blocks; fades over the next 32; 32-160 |
| CloudDriftDistance | 32 | Maximum X drift in blocks; 0-64; Y drift is 40% |
| WindSpeedPercent | 100 | Motion rate; 10-200 |
| CloudSpeedPercent | 200 | Cloud-only speed multiplier; 25-500; default twice the original drift speed |

Logs and counters are written to `Mods/AlphaWorldMotion.log`. The inspected Alpha native layout is required; this is a prototype, not a Steam-version DLL.

## Build and checks

Install Visual Studio Build Tools with Desktop development with C++ and a Windows SDK. Open an **x86 Native Tools Command Prompt** in this repository, then run:

```bat
build.cmd
build\tests\MotionRulesTest.exe
build\tests\MotionGpuTest.exe
```

The DLL is produced at `build/Mods/AlphaWorldMotion.dll`. GPU validation uses a hidden D3D9 window and requires an available graphics device. It never activates or captures other windows. Close exclusive fullscreen games if device creation fails. Tests cover native column selection, stationary anchors, shader models 2/3, shader metadata preservation, cloud transforms, native state restoration and device reset cleanup.

Source uses the checked-in `.clang-format` style: four-space indentation and a 100-column target. Format project `.cpp`/`.h` files with clang-format; preserve embedded shader strings.

See [VALIDATION.md](VALIDATION.md) for what has actually been verified. MIT licensed; no proprietary game code or assets are distributed.

## Alpha material correction (0.3)

Read-only sampling of native Alpha terrain confirmed green canopy material 8 and ground/road material 11. The original Steam-derived 8/11 classifier incorrectly animated Alpha roads. Only verified type 8 is now eligible, while 11 pins contacting corners as stationary terrain. This changes cached selection only and adds no per-frame work. Unverified snow-leaf IDs remain stationary. Cloud speed is unchanged in this correction.
