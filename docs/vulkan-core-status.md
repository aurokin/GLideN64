# RealityVK2 Program Status (2026-03-02)

This is the single execution tracker for the rewrite.

## Snapshot

1. `rvk2` runtime path and scaffolding are integrated in build + local gate.
2. Command ingest capture is live for LLE, HLE display-list loops, and Turbo3D/T3DUX bypass paths.
3. Schema is frozen as `rvk2_schema_v1` (breaking still allowed before cutover).
4. Deterministic packet trace + replay are live and gate-integrated.
5. Replay now supports multicore execution (`--jobs`, default all cores).
6. Replay check is currently clean on maintained smoke trace: `frames=124 failed=0 warned=0`.
7. Draw semantic -> raster op -> render work -> submission batch pipeline is implemented and traced.
8. Software executor now consumes triangle shade/texture/z coefficient semantics, including deterministic triangle depth compare/update.
9. HLE synthetic RDP submission is live for `gDP`, `gSPTriangle`, DMA triangles, and screen-space triangle paths.
10. Visual parity is still not at target threshold (`rmse=0.259169` vs `0.25` gate on current Paper Mario compare).
11. Packet trace/replay now carries variable-length command payload words beyond legacy inline limits.
12. Synthetic executor now applies explicit combiner + blender stages (including destination-dependent blending and texrect flip sampling), and replay model is synchronized.
13. Mixed-state stress conformance now covers rapid phase/depth/coverage/combiner/scissor transitions and batch-segmentation determinism.
14. Packet trace semantic rows (`S`) now emit full coefficient schema (72 columns) and replay parser validates the expanded schema while preserving legacy compatibility.
15. Synthetic texture sampling now consumes expanded texture/tile/load state for texrect and textured triangles (executor + replay aligned).
16. VI/presentation path now consumes live VI register snapshots and selects presented surface by `VI_ORIGIN` when available.
17. Primitive depth source path (`otherModes.depthSource` + `SetPrimDepth`) is now modeled in executor + replay with deterministic conformance coverage.
18. VI presenter now models deterministic gamma, divot, and interlace field behavior from VI status/register state.
19. Alpha compare state is now modeled in synthetic executor/replay and covered by conformance.
20. Depth compare/update mode bits are now modeled in synthetic executor/replay and covered by conformance.
21. Coverage mode flags (`cvgDest`, `cvgXAlpha`, `alphaCvgSel`, `colorOnCvg`, `forceBlender`, `blendMask`) are now modeled in synthetic executor/replay and covered by conformance.
22. VI presenter now models `VI_STATUS` gamma-dither and AA-mode filtering behavior with deterministic unit coverage.
23. VI presenter now applies VI type-aware decode behavior for 16bpp mode (`status.type=2`) with deterministic quantization.
24. VI presenter now applies interlace field selection in register-stepping space (field-aware Y stepping) for serrated mode.

## Phase Status

| Phase | Status | Notes |
| --- | --- | --- |
| A: Contracts and Determinism | Done | Schema, trace, replay, gate wiring, ADR baseline are in place. |
| B: Semantic Pipeline Backbone | In progress | Semantic/raster/render/submission/executor pipeline exists, but semantic coverage is incomplete. |
| C: Core Rendering Correctness | In progress | Fill/texrect/triangle backbone exists in executor path; full combiner/blender/depth/hazard correctness not closed. |
| D: VI and Presentation | In progress | VI register-driven source selection/scaling + gamma/divot/interlace behavior are wired; full VI filtering/parity behavior remains incomplete. |
| E: Texture Replacement | Not started | Hi-res pack + `.htc` rewrite path not implemented yet. |
| F: Cutover and Deletion | Not started | `rvk2` is not default and legacy-derived paths still exist. |

## Completion Estimate

Estimated overall roadmap completion: **~65%**.

Heuristic phase weighting used for this estimate:
- A: 20%
- B: 20%
- C: 30%
- D: 15%
- E: 10%
- F: 5%

Estimated phase progress used:
- A: 100%
- B: 74%
- C: 65%
- D: 60%
- E: 0%
- F: 0%

## Completed Recently

1. Centralized synthetic triangle packet packing in `rvk2_SyntheticTriangle.h`.
2. Unified triangle capture across `gSPTriangle`, `drawDMATriangles`, and `drawScreenSpaceTriangle`.
3. Added/updated unit coverage for synthetic triangle packet packing and semantic decode.
4. Added replay multicore support and gate knob:
   - CLI: `--jobs`
   - gate env: `REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS`
5. Reduced replay CPU overhead in rect/triangle pixel loops.
6. Landed payload-contract closure for `P` rows:
   - runtime captures full command payload words
   - packet trace emits payload count + payload words
   - replay parser/hash replayer supports variable-length payload rows
   - added unit coverage for `>6` payload word capture/hash behavior
7. Landed full triangle semantic coefficient decode for opcodes `0x08..0x0F`:
   - shade / texture / z coefficient groups now decoded from expanded command payload mapping
   - semantic hash/replay contract updated to include new coefficient fields
   - unit coverage added for extended coefficient decode path
8. Landed coefficient propagation and executor consumption end-to-end:
   - raster + render work packets carry triangle coefficient groups
   - executor/replay now use these fields for triangle shade/texture and depth behavior
   - raster/render hashes and trace `R`/`W` rows updated to include coefficient fields
   - unit coverage added for coefficient propagation and executor depth/shade/texture effects
9. Added dedicated synthetic conformance suite target (`rvk2_conformance_tests`) and local gate integration:
   - cycle/phase classification conformance
   - blend sensitivity conformance
   - depth ordering conformance
   - scissor/coverage conformance
10. Expanded conformance matrix and replay-model alignment:
   - combiner mux conformance
   - blend destination dependency conformance
   - texrect flip sampling conformance
   - render target isolation conformance
   - replay executor model updated to match synthetic combiner/blender + texrect-flip behavior
   - smoke replay check remains clean (`frames=124 failed=0 warned=0`)
11. Added phase-aware synthetic execution and conformance closure for cycle modes:
   - executor/replay now apply explicit phase behavior for `Cycle1`, `Cycle2`, `Copy`, and `Fill`
   - cycle2 second-pass synthetic path is deterministic and trace-replay aligned
   - new conformance coverage for copy destination-bypass, cycle2 distinction, and fill-phase blend/combiner invariance
   - maintained smoke replay remains clean (`frames=124 failed=0 warned=0`)
12. Expanded hazard-corner conformance and depth-path rules:
   - depth compare/update now constrained to cycle phases (`Cycle1`/`Cycle2`) in executor and replay
   - conformance coverage added for depth participation by phase (`Cycle1`/`Cycle2` active, `Copy` bypass)
   - conformance coverage added for depth alias fallback (`depthImageAddress=0`) and isolated depth-surface behavior
   - conformance coverage added for coverage-modulation blend flag behavior
   - maintained smoke replay remains clean (`frames=124 failed=0 warned=0`)
13. Added mixed-state stress conformance matrix:
   - deterministic stress scene with rapid state transitions across phase/depth/combiner/blender/scissor/render-target switches
   - conformance checks for single-batch vs split-batch execution invariance
   - conformance checks for depth isolation, copy-phase depth bypass, coverage modulation, cycle2 combiner sensitivity, scissor tightening, and offscreen-target isolation
   - local gate remains clean with the expanded matrix (`rvk2 conformance tests: PASS`)
14. Tightened semantic trace/replay strictness:
   - `S` rows now emit full semantic coefficient payload instead of legacy compact rows
   - replay parser now accepts and validates 72-column semantic rows (legacy 14/26/37 still accepted)
   - smoke replay remains clean with expanded schema (`frames=124 failed=0 warned=0 strict=0`)
15. Broadened non-triangle texture-state behavior and conformance:
   - executor/replay texture sampling now mixes texture image format/size/width, tile descriptor state, and TMEM load metadata
   - tile mask/shift/mirror/clamp and tile bounds now affect synthetic texture coordinate behavior
   - added conformance coverage for texrect sensitivity to tile descriptor transitions, tile bounds, texture image state, and TMEM load state
   - smoke replay remains clean after state-expansion changes (`frames=124 failed=0 warned=0 strict=0`)
16. Landed VI register-driven presenter behavior:
   - `ContextImpl` snapshots live VI registers per present and passes them through executor config
   - executor now prefers `VI_ORIGIN` surface for presentation when present in rendered surfaces
   - VI presenter now samples/scales with VI register start/step parameters while preserving 4:3 / 16:9 output modes
   - unit coverage added for VI register sampling and VI-origin presentation selection
17. Landed primitive depth-source conformance closure:
   - render-work now carries `depthSource`, `primDepthZ`, and `primDepthDelta`
   - executor/replay depth evaluation now honors primitive-depth source path
   - render-work hashing/replay model updated to include primitive-depth fields
   - conformance coverage added for pixel-depth vs primitive-depth behavior divergence
18. Extended VI register behavior with gamma flag modeling:
   - presenter now applies deterministic gamma transform when VI gamma bit is enabled
   - unit coverage added for gamma-on vs gamma-off output/hash divergence
19. Extended VI register behavior with divot + interlace field modeling:
   - presenter now applies deterministic horizontal divot median filtering when `VI_STATUS_DIVOT_ENABLED` is set
   - presenter now applies deterministic serrate field sampling from `VI_V_CURRENT_LINE` when `VI_STATUS_SERRATE_ENABLED` is set
   - unit coverage added for divot and interlaced sampling divergence
20. Landed alpha-compare semantic closure:
   - render-work now carries decoded `alphaCompare` state from `SetOtherModes`
   - executor/replay now gate color writes through deterministic alpha-compare behavior (including blend-threshold and dithered mode)
   - render-work hashing/replay model updated to include alpha-compare state
   - conformance coverage added for alpha-threshold reject/accept behavior
21. Landed depth compare/update mode closure:
   - render-work now carries decoded depth-compare/depth-update mode bits from `SetOtherModes`
   - executor/replay depth path now honors compare-only, update-only, and compare+update combinations
   - render-work hashing/replay model updated to include depth-mode state bits
   - conformance coverage added for depth compare/update mode behavior divergence
22. Landed coverage-mode semantic closure:
   - render-work now carries decoded coverage/blender interaction bits (`cvgDest`, `blendMask`, `cvgXAlpha`, `alphaCvgSel`, `colorOnCvg`, `forceBlender`)
   - executor/replay now model deterministic coverage destination behavior and coverage-gated color-write behavior
   - synthetic blender/replay path now incorporates coverage-mode weights/alpha effects and force-blender state
   - render-work hashing/replay model updated to include coverage-mode fields
   - conformance coverage added for coverage-mode flag behavior divergence
23. Extended VI register behavior with gamma-dither + AA mode modeling:
   - presenter now applies deterministic gamma-dither perturbation when `VI_STATUS_GAMMA_DITHER_ENABLE` is set
   - presenter now applies deterministic VI AA-mode filtering for AA modes `1` and `2`
   - unit coverage added for gamma-dither and AA-mode hash/pixel divergence
24. Extended VI type decode behavior:
   - presenter now applies deterministic VI type-aware decode for 16bpp path (`status.type=2`)
   - sampled source pixels are quantized to 5/5/5/1-expanded RGBA before downstream VI filtering
   - unit coverage added for type2 vs type3 hash divergence and quantized sample expectation
25. Tightened interlace register-step behavior:
   - presenter now applies interlace field offset before fixed-point Y step conversion in register-driven mode
   - this aligns serrate sampling with register-space stepping instead of post-sample row doubling
   - unit coverage added for interlaced field phase behavior with non-zero `yStart` and non-1.0 `yStep`

## Current Bottlenecks

1. Semantic completeness gap remains for cycle-accurate combiner/blender/depth/cvg behavior versus real RDP.
2. Visual parity threshold still fails on maintained Paper Mario metric.
3. VI path now includes register-driven source selection/scaling and gamma/divot/interlace, but still lacks additional VI parity/filters and full register-accurate edge behavior.
4. Texture replacement stack (`hi-res` + `.htc`) is not started.
5. Conformance matrix is broad, but additional hazard corners remain as semantic coverage expands.

## Next Coding Priorities

1. Continue non-triangle semantic closure for remaining high-impact state interactions not yet modeled in synthetic combiner/blender.
2. Extend VI path toward additional register-accurate VI parity/filter behavior beyond current gamma/divot/interlace modeling.
3. Start Phase E interface contracts for texture replacement keying and `.htc` flow.
4. Continue adding targeted hazard-corner conformance where semantic gaps are discovered.
5. Keep reducing Paper Mario parity delta while preserving deterministic trace/replay contracts.

## Remaining Work Split (Approx)

1. Core rendering correctness closure (cycle-accurate combiner/blender/depth/cvg): **33%** of remaining work.
2. Semantic + non-triangle behavior coverage closure: **23%**.
3. VI/presentation register-accurate behavior: **17%**.
4. Texture replacement (`hi-res` + `.htc`) contracts + implementation: **18%**.
5. Trace/replay strictness + schema tightening + final cutover cleanup: **9%**.

## Local Gate Contract

Default gate:

```bash
./scripts/local_gate.sh
```

Smoke + replay gate:

```bash
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
```

Control replay workers:

```bash
REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS=0 ./scripts/local_gate.sh
```

`0` means auto/all cores; use `1` to force single-process replay.
