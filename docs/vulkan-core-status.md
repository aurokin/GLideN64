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
16. VI/presentation path now consumes live VI register snapshots and selects presented surface by exact/containing `VI_ORIGIN` match when available.
17. Primitive depth source path (`otherModes.depthSource` + `SetPrimDepth`) is now modeled in executor + replay with deterministic conformance coverage.
18. VI presenter now models deterministic gamma, divot, and interlace field behavior from VI status/register state.
19. Alpha compare state is now modeled in synthetic executor/replay and covered by conformance.
20. Depth compare/update mode bits are now modeled in synthetic executor/replay and covered by conformance.
21. Coverage mode flags (`cvgDest`, `cvgXAlpha`, `alphaCvgSel`, `colorOnCvg`, `forceBlender`, `blendMask`) are now modeled in synthetic executor/replay and covered by conformance.
22. VI presenter now models `VI_STATUS` gamma-dither and AA-mode filtering behavior with deterministic unit coverage.
23. VI presenter now applies VI type-aware decode behavior for 16bpp mode (`status.type=2`) with deterministic quantization.
24. VI presenter now applies interlace field selection in register-stepping space (field-aware Y stepping) for serrated mode.
25. VI presenter now enforces register-window edge clipping (out-of-range samples resolve to black) and blank output on invalid H/V windows.
26. VI presentation now applies origin-relative base pixel offsets only when `VI_ORIGIN` maps to a selected surface.
27. VI register-driven sampling now treats `VI_WIDTH` as source line stride for linear address generation.
28. VI presenter now models `VI_CTRL.DEDITHER_ENABLE` (bit 16) for 16bpp in compatible AA modes and treats `AA_MODE=REPLICATE` as the only no-resample path.
29. VI register path now hard-fails reserved/invalid control states (`TYPE=1`, `VI_WIDTH=0`) to deterministic blank output.
30. VI-specific conformance now covers filter-mode behavior and fail-safe control-state blanking at executor level.
31. VI register path now models `PIXEL_ADVANCE[15:12]` as deterministic horizontal subpixel sample offset.
32. VI register path now enforces type-aware `VI_WIDTH` line-stride alignment (16bpp `%4`, 32bpp `%2`) as deterministic fail-safe blanking.
33. Executor/replay color-surface writes now honor render-target pixel size semantics (`colorImageSize=2` quantized to deterministic RGBA5551-expanded output).
34. Executor/replay synthetic paths now model destination read enable (`otherModes.imageRead`) so combiner/blender/coverage destination participation is explicitly gated.
35. Executor/replay triangle depth compare/update now models `otherModes.depthMode` (`OPA`/`INTER`/`XLU`/`DEC`) with deterministic mode-specific behavior.
36. Executor/replay synthetic texture sampling now models texture-filter/bilerp behavior via deterministic bilinear neighborhood sampling for texrect and textured triangles.
37. Executor/replay combiner path now models `otherModes.combineKey` and `otherModes.convertOne` post-combine behavior.
38. Executor/replay blender path now models `otherModes.colorDither`, `otherModes.alphaDither`, and `otherModes.textureEdge` output behavior.
39. Executor/replay texture path now models `otherModes.texturePersp`, `otherModes.textureLOD`, `otherModes.textureDetail`, and `otherModes.textureLUT`.
40. VI conformance now includes interlace field-phase behavior at executor level.
41. Executor/replay blender path now models cycle-specific blend mux selector fields (`c1/c2 m1/m2 a/b`) with deterministic `aaEnable` + `pipelineMode` behavior.
42. Phase E contract work has started via new `rvk2_TextureReplacement` deterministic keying + `.htc` cache-key contract module with unit coverage.
43. Conformance now includes blend mux selector transitions and AA/pipeline mode transitions.
44. Phase E now includes deterministic texture-replacement store APIs (insert/lookup/ordered key iteration) and deterministic replacement sampling behavior.
45. Phase E now includes deterministic `.htc` cache read/write APIs (`RKVHTC1`) with unit coverage.
46. Executor texture path now supports optional replacement sampling via deterministic key/cache lookup (`REALITYVK_RVK2_TEX_REPLACEMENT`, `REALITYVK_RVK2_TX_HTC_PATH`, `REALITYVK_RVK2_TX_PACK_PATH`).
47. Phase E now includes deterministic pack ingest via `rkv2_pack_index_v1.tsv` (cache-key indexed raw RGBA entries) with unit + executor coverage.
48. Executor lifecycle now supports deterministic replacement reload/invalidation tokens and no longer reconstructs executor per present.
49. Replacement store now supports deterministic bounds policy (`max entries` / `max pixels`) applied at load time.
50. Texture-pack index tooling is now landed (`scripts/rvk2_texture_pack_index.py`) and local gate can validate pack indexes with opt-in knobs.
51. Executor replacement observability now reports loaded entries/pixels and replacement sample hit/miss counters, with env-gated runtime summary logging.
52. Executor/replay now model `SetScissor` field mode (even/odd line filtering) and mode-specific scissor edge semantics (lower-exclusive always, right-edge inclusive only in copy/fill).
53. Executor/replay now model scissor-aware dither Y indexing (`[2:1]` under explicit scissoring) with conformance coverage.
54. Executor/replay now model explicit texture-filter mode variants (point/bilerp/average/sharpen-style) with expanded conformance coverage.
55. Local gate remains clean across Release/Debug unit + conformance suites after the recent scissor/dither/filter semantic closure passes.
56. Executor combiner path now decodes and applies cycle-specific (`cycle1`/`cycle2`) selector fields from `SetCombine`, and conformance now asserts cycle2-selector isolation from cycle1 output.
57. Executor combiner path now models extended selector sources (`LOD_FRACTION`, `PRIM_LOD_FRAC`, `K5`, and alpha-selector lanes) and conformance now asserts these selector transitions alter output deterministically.
58. Executor coverage-gated write semantics now use cycle-aware destination alpha in cycle2 (using cycle1 output as coverage destination), with dedicated conformance coverage.
59. Submission-plan split classification coverage is now explicit in unit tests (`start`/`barrier`/`phase`/`cycle`/`render-target`/`scissor` + non-submittable unknown-phase path), closing Phase B semantic-batching test gaps.
60. Copy/fill phase behavior now bypasses alpha-compare and coverage-gated write suppression semantics (cycle-pipeline-only), with dedicated conformance coverage.
61. Texture replacement runtime control file support is now landed (`REALITYVK_RVK2_TX_CONTROL_FILE`) for live enable/disable, reload/invalidate token updates, and pack/cache path overrides without emulator restart.
62. Replacement visibility now supports stable per-frame summary file output (`REALITYVK_RVK2_TX_SUMMARY_PATH`) and maintainer control tooling (`scripts/rvk2_tx_control.py`).
63. Runtime cutover hardening started: context creation is now rvk2-only, runtime-switch fallback is removed, legacy draw pass-through in `rvk2::ContextImpl` is disabled, and no-work presents now clear to deterministic black.
64. Command ingest is now unconditional on rvk2 path (RSP/RDP/HLE/Turbo3D/T3DUX no longer guard on runtime-switch capture checks), and dead runtime-switch selector APIs were removed.
65. Legacy present fallback injection path is deleted from Vulkan present, and stale runtime-switch naming/config surfaces were renamed to trace-config (`rvk2_TraceConfig`) for rvk2-only operation clarity.
66. `rvk2::ContextImpl` now hard-noops high-frequency legacy draw-state mutators (`enable`, cull/depth/blend/viewport/scissor/polygon-offset setters), reducing dead legacy draw-recorder churn while preserving base-qualified presenter operations.
67. Replay strictness tightened for current schema-v1 render-work rows: 109-column `W` rows are no longer marked legacy and now participate in full declared-vs-computed mismatch checks.

## Phase Status

| Phase | Status | Notes |
| --- | --- | --- |
| A: Contracts and Determinism | Done | Schema, trace, replay, gate wiring, ADR baseline are in place. |
| B: Semantic Pipeline Backbone | Done | Draw semantic -> raster -> render-work -> submission -> executor path is deterministic and now has explicit split-classification coverage. |
| C: Core Rendering Correctness | Done | Fill/copy/texrect/triangle/depth/coverage/blend hazard behavior is closed for the scoped synthetic contract and covered by conformance. |
| D: VI and Presentation | Done | Register-driven source selection/scaling/filter/fail-safe behavior is implemented with unit + conformance + smoke coverage for current scoped contract. |
| E: Texture Replacement | Done | Deterministic key/cache contracts, store APIs, `.htc` IO, pack-index ingest, lifecycle controls, bounded load policy, optional executor sampling, runtime control-file UX, summary visibility, and maintainer tooling are landed. |
| F: Cutover and Deletion | In progress | Runtime/context fallback is removed, ingest is unconditional, and present fallback injection is deleted; full legacy-path deletion and cleanup are still open. |

## Completion Estimate

Estimated overall roadmap completion: **~98%**.

Heuristic phase weighting used for this estimate:
- A: 20%
- B: 20%
- C: 30%
- D: 15%
- E: 10%
- F: 5%

Estimated phase progress used:
- A: 100%
- B: 100%
- C: 100%
- D: 100%
- E: 100%
- F: 55%

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
26. Added VI register-window edge semantics:
   - register-driven sampling now treats out-of-range sample coordinates as clipped/black instead of edge-clamping
   - invalid VI H/V windows (`hEnd<=hStart` or unresolved `vEnd<=vStart`) now produce blank output instead of fallback sizing
   - unit coverage added for x/y sample overflow clipping and invalid window blank behavior
27. Tightened VI origin-mapped presentation semantics:
   - executor now resolves `VI_ORIGIN` to exact or containing render targets (address-range aware by color-image size)
   - presenter now applies origin-relative source pixel offset when and only when the selected surface matches `VI_ORIGIN`
   - unit coverage added for in-range `VI_ORIGIN` surface selection, origin-offset sampling, wrapped `vStart` handling, and no-match fallback behavior
28. Landed VI width-stride addressing semantics:
   - register-driven sampling now uses `VI_WIDTH` as line stride when mapping `(x,y)` sample positions to source linear addresses
   - source sampling validity now resolves through stride-aware linear bounds (including out-of-range row/offset clip-to-black behavior)
   - AA/divot neighborhood taps now operate on stride-aware linear neighbors for register-driven paths
   - unit coverage added for stride remapping (`VI_WIDTH < sourceWidth`) and stride overflow clipping (`VI_WIDTH > backing width`)
29. Expanded VI filter-mode semantics:
   - AA mode handling now follows VI mode intent (`REPLICATE` bypass only; other modes use deterministic resample kernels)
   - `VI_CTRL.DEDITHER_ENABLE` now applies a deterministic 8-neighbor correction path for 16bpp when AA mode is `AA_ALWAYS` or `REPLICATE`
   - dedither is intentionally inactive for incompatible modes (`AA_NEEDED`/`RESAMPLE`) to avoid unsupported behavior drift
   - unit coverage added for dedither-on/off divergence and AA-mode-gated dedither behavior
30. Hardened VI register fail-safe behavior:
   - reserved VI type (`TYPE=1`) now resolves to blank output instead of undefined decode behavior
   - register-driven presentation now requires non-zero `VI_WIDTH`; zero-width states deterministically blank
   - unit coverage added for reserved type and zero-width blank-output behavior
31. Added VI-focused conformance closure:
   - added executor-level conformance for dedither behavior in compatible modes and dedither inactivity in incompatible AA modes
   - added executor-level conformance for fail-safe blanking on reserved VI type and zero `VI_WIDTH`
   - integrated these checks into `rvk2_conformance_tests` and local gate
32. Added VI pixel-advance semantics and validation:
   - register-driven sampling now consumes `VI_CTRL.PIXEL_ADVANCE[15:12]` as a fixed horizontal subpixel offset
   - out-of-range pixel-advance samples deterministically clip to black through existing stride/bounds validation
   - unit coverage added for pixel-advance shift and overflow clip behavior
   - executor-level conformance added for pixel-advance hash/frame divergence and overflow clipping
33. Added VI stride-alignment fail-safe semantics:
   - register-driven VI now requires type-aligned `VI_WIDTH` scanline stride (16bpp `%4`, 32bpp `%2`) for deterministic presentation
   - invalid stride states now resolve to blank output instead of undefined sampling behavior
   - unit + executor conformance coverage added for invalid 16bpp/32bpp stride blanking behavior
34. Added render-target write-size semantics closure:
   - executor writes now encode to surface format by `colorImageSize` before storage (`16bpp -> RGBA5551` quantize/expand)
   - replay model mirrors identical target-size write behavior for deterministic trace/replay consistency
   - executor conformance coverage added for `colorImageSize`-driven output divergence with coverage invariance
35. Added B/C semantic closure for destination/depth/filter behavior:
   - executor/replay now gate destination color participation on `otherModes.imageRead` for combiner/blender/coverage paths
   - executor/replay now apply mode-specific depth compare/update rules from `otherModes.depthMode`
   - executor/replay now apply deterministic texture-filter/bilerp sampling for texrect + textured triangle paths
   - added conformance coverage for `imageRead`, `depthMode`, and texture-filter transitions
36. Added B/C mode-bit expansion pass:
   - executor/replay now model combine-key + convert-one combiner behavior (`otherModes.combineKey`, `otherModes.convertOne`)
   - executor/replay now model blender dither/edge behavior (`colorDither`, `alphaDither`, `textureEdge`)
   - executor/replay now model texture perspective/LOD/detail/LUT behavior (`texturePersp`, `textureLOD`, `textureDetail`, `textureLUT`)
   - added conformance coverage for combine-key/convert-one transitions, extended texture-mode transitions, and VI interlace field-phase behavior
37. Added B/C blender selector + AA/pipeline expansion and started Phase E contracts:
   - executor/replay now consume cycle-specific blend mux selector fields (`c1/c2 m1/m2 a/b`) in synthetic blender paths
   - executor/replay now consume `aaEnable` and `pipelineMode` in deterministic coverage/blend behavior
   - added conformance coverage for blend selector transitions and AA/pipeline transitions
   - landed `rvk2_TextureReplacement` deterministic key + `.htc` cache-key contract module with unit coverage
38. Extended Phase E texture replacement implementation:
   - added deterministic texture replacement store APIs (`insert`, `find`, ordered `keys`, and deterministic sampling helper)
   - added deterministic `.htc` cache write/read APIs (`RKVHTC1` header, versioned entry serialization)
   - integrated optional texture replacement sampling into executor texture path through deterministic key/cache lookup
   - added unit coverage for cache IO roundtrip and executor replacement sampling behavior
39. Added Phase E deterministic pack ingest path:
   - added `rkv2_pack_index_v1.tsv` ingest (`hi`, `lo`, `width`, `height`, `rgba_file`) into replacement store
   - executor now optionally loads pack index path (`REALITYVK_RVK2_TX_PACK_PATH`) and overlays pack entries over `.htc` cache entries
   - added unit coverage for direct pack ingest and executor pack-vs-cache replacement precedence
40. Added Phase E lifecycle + bounds controls:
   - executor now supports deterministic reload/invalidation tokens (`REALITYVK_RVK2_TX_RELOAD_TOKEN`, `REALITYVK_RVK2_TX_INVALIDATE_TOKEN`)
   - executor now keeps a persistent instance in `rvk2::ContextImpl` and updates config each present, avoiding unconditional per-frame replacement reloads
   - replacement store now exposes deterministic bounds policy (`REALITYVK_RVK2_TX_MAX_ENTRIES`, `REALITYVK_RVK2_TX_MAX_PIXELS`) applied after cache/pack ingest
   - added unit coverage for store limits and executor reload/invalidation behavior
41. Added Phase E pack-index tooling + gate ergonomics:
   - landed deterministic pack-index generator/validator utility (`scripts/rvk2_texture_pack_index.py`) for `rkv2_pack_index_v1.tsv`
   - added canonical ordering, size-contract checks, duplicate-key detection, and optional full-pack coverage validation
   - integrated optional pack-index validation stage into `scripts/local_gate.sh` via `REALITYVK_GATE_TX_PACK_*` knobs
   - documented tooling + gate integration in maintainer docs
42. Added Phase E replacement observability counters + runtime logging:
   - executor summary now reports replacement enable state, loaded replacement entry/pixel counts, and per-frame sample/hit/miss counts
   - replacement sample counters are wired directly at replacement lookup path for deterministic hit/miss accounting
   - added runtime env knob `REALITYVK_RVK2_TX_LOG_SUMMARY=1` to emit per-frame replacement summary logs in rvk2 presenter path
   - extended unit coverage to assert replacement summary counter behavior for baseline/cache/pack/combined replacement runs
43. Added B/C scissor semantics closure and conformance:
   - executor now applies `SetScissor` mode bit behavior for interlaced field filtering (even/odd scanline selection)
   - executor now applies mode-specific scissor edge behavior (lower edge exclusive in all phases, right edge exclusive in cycle phases and inclusive in copy/fill)
   - replay model now mirrors identical scissor semantics for deterministic trace consistency
   - conformance coverage now asserts phase edge behavior and field-mode filtering behavior
44. Added B/C scissor-aware dither indexing closure:
   - executor now applies scissor-aware Bayer Y indexing for dither (`y>>1` under explicit scissor)
   - replay model now mirrors identical dither indexing behavior for deterministic trace consistency
   - conformance coverage now asserts scissor-driven dither output divergence with stable write coverage
45. Added B/C explicit texture-filter mode closure:
   - executor/replay now differentiate texture-filter modes (`point`, `bilerp`, `average`, and sharpen-style mode)
   - texrect and textured-triangle sampling paths now consume explicit filter mode instead of binary filtered/unfiltered behavior
   - conformance coverage now asserts deterministic divergence across filter-mode transitions while preserving write coverage
46. Added B/C cycle-aware combiner selector closure:
   - executor combiner now decodes real cycle1/cycle2 selector fields from `combineMux` instead of synthetic rotated second-pass state
   - cycle2 path now consumes cycle1 intermediate output and applies cycle2 combiner/blender selectors explicitly
   - texture sampling seed no longer depends on `combineMux`, reducing non-semantic coupling
   - conformance now includes cycle2-selector isolation checks and local gate remains clean
47. Added B/C extended combiner selector-source closure:
   - executor combiner now preserves high-value selector behavior instead of truncating selectors to low 3 bits
   - combiner source modeling now includes extended color/alpha selector lanes (`LOD_FRACTION`, `PRIM_LOD_FRAC`, `K5`, and deterministic digest-backed lanes)
   - conformance now includes explicit extended-selector transitions and local gate remains clean
48. Added B/C cycle2 coverage destination closure:
   - cycle2 coverage-gated write checks now evaluate coverage destination against cycle1 output rather than pre-cycle destination state
   - rect/triangle write paths now thread phase-aware coverage destination color through synthetic phase evaluation
   - conformance now includes cycle1 vs cycle2 coverage-save behavior checks (`colorOnCvg + cvgDest=save`) and local gate remains clean
49. Added Phase B submission split-classification closure:
   - unit tests now directly cover submission split reasons for barrier/phase/cycle/render-target/scissor transitions
   - unit tests now cover same-state append aggregation counters and non-submittable unknown-phase filtering
   - deterministic batching-classification contract is now explicitly validated in local gate
50. Added Phase C copy/fill alpha+coverage bypass closure:
   - copy/fill phase write paths now bypass alpha-compare and coverage-gated write suppression behavior
   - conformance now includes copy/fill bypass validation under aggressive alpha/coverage mode settings
   - local gate remains clean across release/debug unit + conformance suites
51. Closed Phase E runtime operator + visibility UX:
   - landed control-file overrides for texture replacement (`REALITYVK_RVK2_TX_CONTROL_FILE`) consumed each present
   - control file now supports live enable/disable, cache/pack path overrides, bounds, and reload/invalidate tokens
   - landed stable per-frame replacement summary file output (`REALITYVK_RVK2_TX_SUMMARY_PATH`)
   - added maintainer control utility (`scripts/rvk2_tx_control.py`) and unit coverage for control-file config + lifecycle behavior
52. Started Phase F hard cutover:
   - context creation now always instantiates `rvk2::ContextImpl` (runtime switch fallback removed)
   - rvk2 trace/config ingestion is fixed to active runtime-only behavior (no runtime selector path)
   - `rvk2::ContextImpl` no longer forwards triangle/rect/line draws to legacy Vulkan draw submission
   - no-work presentation now clears to black rather than showing legacy draw output
53. Tightened Phase F ingest/runtime cleanup:
   - RSP/RDP/Turbo3D/T3DUX/hybrid HLE synthetic submission paths no longer branch on runtime-switch capture checks
   - rvk2 frame begin/capture emission paths are now unconditional in active runtime
   - removed dead runtime-switch selector APIs (`getRequestedRuntimePath`, `isRealityVK2Requested`, `shouldCaptureRDPTrace`)
54. Advanced Phase F present/trace cleanup:
   - deleted Vulkan present fallback candidate/injection path and associated env gate (`REALITYVK_VK_ENABLE_PRESENT_FALLBACK`)
   - removed fallback packet debug source classification from present packet metadata
   - renamed stale `rvk2_RuntimeSwitch` module to `rvk2_TraceConfig` to reflect rvk2-only trace/config responsibilities
55. Reduced dead legacy state churn in rvk2 runtime:
   - `rvk2::ContextImpl` now overrides high-frequency draw-state mutators to explicit no-op for runtime-facing calls
   - rvk2 presenter path remains intact via explicit base-qualified Vulkan calls in present/upload steps
   - local gate remains clean after this cutover-focused API behavior narrowing
56. Tightened replay validation for current render-work schema rows:
   - parser no longer tags 109-column `W` rows as legacy compatibility rows
   - declared render-work rows now hard-compare against computed replay output for current schema-v1 traces
   - local gate and python syntax checks remain clean after strictness tightening

## Current Bottlenecks

1. Visual parity threshold still fails on maintained Paper Mario metric (`rmse=0.259169` vs `0.25` gate target).
2. Legacy-derived renderer code still exists in-tree and is still compiled; runtime fallback is removed, but code deletion is incomplete.
3. Trace/replay still carries transitional compatibility paths that should be removed once cutover fixtures are refreshed.
4. Texture replacement is functionally closed, but pack/caching behavior still needs wider parity burn-in across additional title coverage.

## Next Coding Priorities

1. Continue Phase F: delete legacy-derived render execution paths and related dead code now that fallback runtime + switch-gating are removed.
2. Continue parity reduction on maintained Paper Mario comparison while preserving deterministic trace/replay contracts.
3. Tighten trace/replay strictness by retiring legacy row compatibility once smoke/parity fixtures are regenerated.
4. Expand parity burn-in coverage for texture replacement packs/caches now that Phase E runtime UX is closed.

## Remaining Work Split (Approx)

1. Phase F cutover/deletion (legacy path deletion/cleanup): **40%** of remaining work.
2. Parity stabilization and metric closure: **35%**.
3. Trace/replay strictness + schema tightening: **20%**.
4. Post-closure texture replacement burn-in/coverage expansion: **5%**.

## Remaining Work by Phase

1. **Phase F (Cutover and Deletion, in progress)**
   - Delete legacy-derived Vulkan/GLideN64 render execution paths now that runtime fallback is removed.
   - Prune transitional test/docs paths that only exist for dual-runtime support.
   - Remove stale runtime-switch policy/config surfaces that implied dual-path operation.
2. **Cross-phase parity + strictness work**
   - Reduce maintained Paper Mario parity metric to target threshold.
   - Retire trace/replay compatibility handling for legacy semantic row widths after fixture refresh.
   - Expand pack/cache replacement parity coverage beyond current maintained scenario set.

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
