# Validation

2026-10-11: MSVC x86 optimized build succeeds. CPU motion tests pass. Hidden HAL D3D9 tests pass using hardware vertex processing on this test run:

- Synthetic native Alpha columns select foliage and preserve position data, colors and face IDs in the immutable cached buffer.
- Cloud displacement respects scaled transforms and restores native constants exactly.
- Shader models 2 and 3 render displaced corners and stationary anchors correctly; CTAB metadata, native shader/buffer bindings and reserved constants are restored.
- Three device resets succeed after releasing motion resources.

Live Alpha visual appearance, seam behavior across real tree/chunk layouts, shadow/TAA interaction, Alt+Tab and dense-forest FPS remain to be tested. No claim of a guaranteed FPS target is made.

Version 0.2: explicit shadow replay produces identical test pixels to visible leaf motion for shader models 2 and 3. Original prototype performance was reported good in-game by the user; the new combined cloud-shadow build still needs a live FPS/appearance check.
