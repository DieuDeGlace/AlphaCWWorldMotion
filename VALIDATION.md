# Validation

2026-10-11, version 0.3: native Alpha material sampling confirms material 8 on green canopy and material 11 on ground/road terrain. CPU and hidden HAL D3D9 tests pass: 11 creates no animated buffer, original road vertices remain unchanged, road contacts pin leaves, and 8 still animates. Native colors/face IDs, visible-vs-shadow replay equivalence, cloud transform restoration and three device resets pass.

The user confirmed that ground/roads are stationary after restarting with the material correction. Existing motion/cascade performance was reported good.

Version 0.3.1 changes only the shipped cloud-speed preset from 200 to 250, a 25% increase. The DLL is unchanged; leaf amplitude/rate and cloud-shadow draw/update limits are unchanged. The compiled clamp accepts 250. Settings apply on the next launch.
