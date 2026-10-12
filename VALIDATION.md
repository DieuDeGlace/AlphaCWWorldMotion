# Validation

2026-10-11, version 0.3: native Alpha material sampling confirms material 8 on green canopy and material 11 on ground/road terrain. CPU and hidden HAL D3D9 tests pass: 11 creates no animated buffer, original road vertices remain unchanged, road contacts pin leaves, and 8 still animates. Native colors/face IDs, visible-vs-shadow replay equivalence, cloud transform restoration and three device resets pass.

This patch does not increase cloud speed. Live road immobility still needs restart verification. Existing motion/cascade performance was reported good before the material correction.
