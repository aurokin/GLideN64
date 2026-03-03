# Vulkan Core Status and Gap Plan

## Scope

- Runtime path is `rvk2` only.
- Current gap-closure scope is intentionally narrow: `paper_mario_intro` only.
- Goal of this phase: pinpoint missing RVK2 logic with deterministic evidence and map each gap to visible output issues.

## Current Focus

- Stabilize and explain all remaining RVK2 mismatches in `paper_mario_intro`.
- Keep CI/smoke workflow in `docs/local-ci.md`; keep logic-gap workflow here.
- Use `screenshot` as the upstream reference baseline and `dumpfb-preset` for RVK2 candidate capture.
- Keep content metrics (`*_non_black_ratio`, `*_mean_luma`) as guardrails while tightening fidelity.

## Operating Principle

- Findings drive what gets fixed.
- Debug steps are support tooling to confirm or refine findings.
- If a debug result does not map to a finding, it does not create priority by itself.

## Findings Baseline (gaps to close)

0. Reference parity baseline is near-black in current environment.
- Impact: RMSE/MAE mostly measure "is candidate drawing anything" instead of fidelity to a meaningful frame.
- Anchors: `scripts/paper_mario_parity.sh` metrics/capture context output, `build/parity-runs/paper-mario/paper_mario_intro.reference.png`.
- Current status: resolved for current harness by using screenshot capture for reference plugin; keep regression checks in place.

1. TMEM fallback behavior is synthetic for unsupported decode paths.
- Impact: missing/incorrect texture regions can resolve to diagnostic pattern instead of expected data.
- Anchors: `src/Graphics/RealityVK2/rvk2_Executor.cpp` (`sampleCITextureFromTMEM`, synthetic fallback path), `src/Graphics/RealityVK2/rvk2_ConformanceTests.cpp` (`testUnsupportedTMEMDecodeUsesSyntheticConformance`).

2. Texture detail/LOD behavior is incomplete.
- Impact: texture detail, lod-driven coordinate behavior, and filtering edge cases can diverge.
- Anchors: `src/Graphics/RealityVK2/rvk2_Executor.cpp` (`applyTextureCoordinateModes`, `applyTextureDetailModeColor`).
- Current status: detail-mode color transform is now implemented; LOD coordinate model and mode interactions still need hardware-faithful validation.
- Current signal: after synthetic-triangle perspective packing correction, disabling perspective correction is neutral/worse in `paper_mario_intro`; LOD coordinate path remains inactive/low-signal for this scenario.

3. Raster/combiner/blender/depth path is a synthetic approximation, not full hardware-accurate span pipeline.
- Impact: edge coverage, blending, z compare/update, and pixel ownership can drift from N64 behavior.
- Anchors: `src/Graphics/RealityVK2/rvk2_Executor.cpp` (`writeTriangle`, `applySyntheticCombiner`, `applySyntheticBlender`, `passesSyntheticDepthCompare`).
- Current signal: in `paper_mario_intro`, `combiner_out` remains materially closer to screenshot reference than `blender_out`/`final`; the dominant remaining downstream mismatch was cycle-2 triangle second-pass memory-color sourcing, now default-fixed with opt-out debug override.

4. Some mode-bit behavior is partial or non-authoritative.
- Impact: toggles such as pipeline/convert/key can change output in ways that are not fully spec-faithful.
- Anchors: `src/Graphics/RealityVK2/rvk2_Executor.cpp` (`isPipelineModeEnabled`, `isCombineKeyEnabled`, `isConvertOneEnabled`), `src/Graphics/RealityVK2/rvk2_RDPState.cpp`.

5. VI path implements core filters but not full register/timing semantics.
- Impact: scanline-sensitive behavior, interrupt semantics, and mid-frame register effects can be incorrect.
- Anchors: `src/Graphics/RealityVK2/rvk2_VIRenderer.cpp/.h`, `docs/references/n64/deep-dive-pack/docs/extracted/cheatsheets/verification_checklist.md`.

6. Hidden/coverage handling is local surface bookkeeping, not full shared 9th-bit memory model.
- Impact: AA/resample behavior and coverage interactions can mismatch in subtle edge regions.
- Anchors: `src/Graphics/RealityVK2/rvk2_Executor.cpp` (surface `coverage`/`hiddenCoverage` paths), `docs/references/n64/deep-dive-pack/report.md`.

7. Validation layer checks structural sanity, not deep semantic correctness.
- Impact: many logic regressions can still pass basic validation.
- Anchors: `src/Graphics/RealityVK2/rvk2_Validation.cpp`.

8. HLE synthetic triangle submission can diverge from raw command-stream semantics.
- Impact: display-list/microcode-specific behavior may not match LLE-equivalent RDP packets.
- Anchors: `src/gSP.cpp`, `src/GraphicsDrawer.cpp`, `src/Graphics/RealityVK2/rvk2_SyntheticTriangle.h`.
- Current status: perspective-coordinate packing mismatch in synthetic textured triangles is now fixed (pack `S/T` as pre-divided by synthetic `W` when `TP_PERSP` is enabled); broader HLE vs LLE drift remains in scope.

## Fix Backlog (from findings)

1. Baseline hygiene and content-first gating.
- Why first: without a meaningful reference frame, fidelity metrics can mislead prioritization.
- Done when: each run records content metrics (`*_non_black_ratio`, `*_mean_luma`) and RVK2 can be tracked independently of black-reference RMSE noise.

2. TMEM unsupported decode and synthetic fallback behavior.
- Why second: creates direct texture correctness loss and synthetic pixel output.
- Done when: targeted scenes no longer rely on synthetic texel source for intended TMEM-decodable paths.

3. Texture detail/LOD correctness.
- Why third: directly affects textured output stability and fidelity.
- Done when: detail/lod transitions are explained by hardware-faithful behavior, not placeholder logic.

4. Combiner/blender/depth/coverage fidelity.
- Why fourth: broad visual impact and frequent mismatch source.
- Done when: stage-local mismatches in combiner/blender/depth paths are closed for current scope.

5. VI register/timing semantics and hidden/coverage interactions.
- Why fifth: final scanout correctness depends on it after upstream raster gaps are closed.
- Done when: `vi_source` to `final` path mismatches are attributable only to known deferred items.

6. HLE synthetic triangle parity hardening.
- Why sixth: removes submission-path drift and improves confidence that fixes are truly RDP-faithful.
- Done when: HLE-heavy failures no longer require synthetic-path-specific exceptions.

## Active Attack Plan (2026-03-03)

1. Keep one canonical repro and one telemetry bundle.
- Command: `REALITYVK_PM_DEEP_TELEMETRY=1 ./scripts/paper_mario_parity.sh`
- Bundle anchor: `build/parity-runs/paper-mario/telemetry/paper_mario_intro.telemetry.bundle.json`
- Rule: no multi-variant emulator reruns for the same hypothesis unless a single-run artifact is missing.

2. Collapse replay noise before touching executor behavior.
- Reconciled replay present dimensions to declared trace size (reduces VI-model false positives).
- Added row-level first-diff diagnostics (semantic/raster/render-work/submission).
- Added computed state component summaries (`rdp_hash`, `tmem_hash`, key color/tmem state) on `state_hash` mismatch.

3. Prioritize earliest deterministic divergence.
- Work from first mismatching frame forward (currently frame 2).
- Use first-diff fields to target state carry/sync/tile/load provenance before broad shader/pipeline edits.

4. Keep geometry vs texture diagnosis explicit in bundle output.
- Geometry visibility gate: `coverage_ratio_vs_reference`.
- Texture/detail gate: `luma_ratio_vs_reference`.
- Replay-classifier gate: `error_kind_counts` from replay JSON.

5. Tighten gate behavior for deep telemetry mode.
- Deep mode now supports stateful replay (`REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL=1`).
- Gate can reuse parity-generated replay JSON to avoid duplicate replay passes in deep mode.

6. Drive fixes from one-run deviation artifacts.
- Deep telemetry now auto-emits:
  - `paper_mario_intro.deviation/overlay.png` + `boxes.json` + `summary.json` for pixel-localized mismatch boxes.
  - `paper_mario_intro.candidate.command-census.json` for triangle/texrect/fill command-family evidence.
- Rule: every new hypothesis must point to one mismatch box and one command-family clue before executor edits.

### Immediate Signals (latest deep bundle)

- `coverage_ratio_vs_reference=0.7229095423` (geometry/visibility deficit persists).
- `luma_ratio_vs_reference=0.5881004580` (texture/detail deficit persists).
- Replay failure family remains dominated by hash drift, but present-size mismatch class is removed after replay sizing reconciliation.
- Stateful replay on early frames (`1..30`) remains reduced to executor present-hash drift (18/30), indicating most prior row/state mismatches were frame-state carry artifacts.
- Deep replay now attaches frame-forensics VI context to present-hash mismatches (`forensics_present_hash`, `present_select`, `vi_reject`, `vi_use_regs`) so mismatch provenance is explicit in a single run.
- Latest deep replay telemetry adds VI stage hashes (`vi_hash_decode/filter/gdither`) and raw selected-surface hash (`selected_surface_hash`) to every frame-forensics row.
- In the latest Paper Mario deep run, all forensics-covered present mismatches (`56/56`) show:
  - `selected_surface_hash` mismatch (replay vs forensics)
  - `vi_hash_decode` mismatch
  - `vi_hash_filter` mismatch
  - `vi_hash_gdither` mismatch
- Interpretation: mismatch origin is now localized pre-VI (source surface/raster reconstruction), not only VI post-processing.

### Ground-Truth Deviation Playbook Loop (Paper Mario Intro)

1. Run one canonical deep telemetry capture.
- Command: `REALITYVK_PM_DEEP_TELEMETRY=1 ./scripts/paper_mario_parity.sh`

2. Triage mismatch shape first, then command mix.
- Read `paper_mario_intro.deviation/overlay.png` and `boxes.json`.
- Read `paper_mario_intro.candidate.command-census.md`.

3. Use command census to split upstream vs raster hypotheses.
- `texrect/fill present + triangles absent` in focus frame: prioritize RSP/DL submission/microcode path.
- `triangles present + missing geometry` in focus frame: prioritize triangle raster, scissor, Z, blend/alpha, or wrong color-image target.

4. Use pre-VI proof to keep scope locked.
- If `selected_surface_hash` mismatch is present with `vi_hash_*` mismatch, continue treating divergence as pre-VI origin.
- Do not start VI tuning until selected-surface parity is stable.

5. Instrument next where evidence is strongest.
- Add primitive-indexed bbox/hash telemetry around first failed frame and mismatch box coordinates.
- Prioritize command/state checkpoints around replay first-failed frame (`command-census` focus frame).

6. Validate by shrinking structural mismatch, not speckle.
- Primary success metric: `summary.json` box count/area drops over iterations.
- Secondary metric: `coverage_ratio_vs_reference` and `luma_ratio_vs_reference` trend upward.

## Progress Log

### 2026-03-03

- Deep telemetry upgrade pass completed for Paper Mario intro:
  - Replay now accepts frame-forensics TSV (`--forensics-file`) and emits present-path provenance on mismatch (`forensics_present_hash`, `vi_*`, `present_select`).
  - Deep telemetry replay hooks now auto-pass forensics context in `paper_mario_parity.sh` and `local_gate.sh`.
  - Replay present scaling was aligned to executor full-frame sampling (no synthetic letterbox in replay hash path).
- Latest single-run deep telemetry (`REALITYVK_PM_DEEP_TELEMETRY=1`, stateful replay):
  - Replay summary: `frame_count=126`, `failed_count=114`, `warning_count=0`.
  - Remaining replay failures are now isolated to present hash family:
    - `executor_present_hash mismatch` (114)
    - `forensics_present_hash` context emitted on 56 frames
  - No semantic/raster/render-work/submission mismatch classes remain in this stateful deep run.
  - Forensics VI signals: `vi_valid_rate=1.0`, `vi_use_register_rate=1.0`, `vi_reject_rate=0.0`, `present_select_share=s3:0.990991,s7:0.009009`.
- Finding #1 first implementation pass completed:
  - Added RDRAM fallback decode attempt when TMEM decode rejects in `rvk2_Executor`.
  - Added best-effort YUV16 RDRAM texel decode path (U Y0 V Y1 pair unpack) for fallback.
  - Kept TMEM as primary source; fallback order is now `TMEM -> RDRAM -> synthetic`.
- Added conformance coverage:
  - New test: `testUnsupportedTMEMDecodeUsesRdramFallbackWhenAvailableConformance` in `rvk2_ConformanceTests.cpp`.
  - Existing synthetic fallback test retained for no-RDRAM conditions.
- Short validation run:
  - `build/release-vulkan-smoke/rvk2_conformance_tests` passed.
  - `build/release-vulkan-smoke/rvk2_unit_tests` passed.
- Short `paper_mario_intro` parity check after change:
  - `./scripts/paper_mario_parity.sh` still fails visual gate (`rmse=0.318331`, limit `0.25`).
  - Forensics (`REALITYVK2_FRAME_FORENSICS_FILE`) showed `tx_rdram=0`, `tx_synth=0`, `tx_tmem=965016`.
  - Interpretation: current mismatch is not driven by TMEM-reject fallback in this scene; prioritize findings #2/#3 next.
- Baseline integrity check:
  - Confirmed maintained reference is effectively black: `reference_non_black_ratio=0.000010`, `reference_mean_luma=0.000004`.
  - Candidate is non-empty: `candidate_non_black_ratio=0.646911`, `candidate_mean_luma=0.200497`.
  - Interpretation: in this environment, parity RMSE is currently a content-presence signal more than a fidelity signal.
- Packet replay strict pass status:
  - `rvk2_packet_trace_replay.py --strict` summary: `frame_count=234`, `failed_count=233`.
  - High-frequency classes: `state_hash mismatch` (233 frames), semantic/raster/render/submission hash mismatches (222 frames).
  - Executor-specific drift is patterned: `executor_present_width/height mismatch` on 111 frames, often with `executor_surface_count mismatch` (`declared=1`, `computed=2`).
  - Interpretation: replay evidence remains useful for classifying failure families, but not yet a pass/fail gate for plugin correctness in this phase.
- Stage-local sweep (`REALITYVK_RVK2_DEBUG_STAGE_VIEW`) with visual gate disabled:
  - `texel_raw`: `rmse=0.335925`
  - `combiner_out`: `rmse=0.322363`
  - `blender_out`: `rmse=0.318331`
  - `vi_source`: `rmse=0.321412`
  - `final`: `rmse=0.318331`
  - Interpretation: first major divergence appears at/before `texel_raw`; later stages do not introduce the primary mismatch in current content-first baseline.
- Harness hardening completed in `scripts/paper_mario_parity.sh`:
  - Added content metrics to visual output JSON (`reference_non_black_ratio`, `candidate_non_black_ratio`, `reference_mean_luma`, `candidate_mean_luma`).
  - Added opt-in cached-reference content validation via `REALITYVK_PM_VALIDATE_CACHED_REFERENCE_CAPTURE=1`.
- Content-first toggle isolation (visual gate disabled):
  - Baseline: `candidate_non_black_ratio=0.646911`, `candidate_mean_luma=0.200497`.
  - `REALITYVK_RVK2_DEBUG_DISABLE_TRIANGLE_WRITES=1`: `candidate_non_black_ratio=0.314038`, `candidate_mean_luma=0.134408`.
  - `REALITYVK_RVK2_DEBUG_DISABLE_TEXRECT_WRITES=1`: `candidate_non_black_ratio=0.376507`, `candidate_mean_luma=0.076210`.
  - `REALITYVK_RVK2_DEBUG_DISABLE_CYCLE2_PREV_MEMORY=1`: no measurable change from baseline.
  - Interpretation: visible output is currently dominated by triangle + texrect paths; cycle2-prev-memory toggle is not a primary driver in this scenario.
- Texture detail mode first implementation completed:
  - Implemented non-zero `applyTextureDetailModeColor` transform in `rvk2_Executor` (detail modes 1/2/3).
  - Added conformance test `testTextureDetailColorTransformConformance` with fixed packet identity to isolate detail-bit effect.
  - Short validation: `rvk2_conformance_tests` and `rvk2_unit_tests` both passed after change.
- Post-detail-change parity snapshot (`final`, visual gate disabled):
  - `rmse=0.308398` (previous `0.318334`), `mae=0.179985`.
  - Content metrics: `candidate_non_black_ratio=0.635175`, `candidate_mean_luma=0.190995`.
  - Interpretation: content-first score moved in the expected direction, but black-reference limitation remains.
- Post-detail-change texture-stage check:
  - `texel_raw`: `rmse=0.338058`, `candidate_non_black_ratio=0.572634`, `candidate_mean_luma=0.211704`.
  - Interpretation: first divergence remains at/before `texel_raw`; next gap focus stays on coordinate/LOD and TMEM decode interactions.
- TMEM addressing sensitivity check at `texel_raw`:
  - Baseline: `rmse=0.335928`.
  - `REALITYVK_RVK2_DEBUG_SWAP_TMEM16=1`: no measurable change.
  - `REALITYVK_RVK2_DEBUG_SWAP_TMEM4_NIBBLES=1`: mismatch worsened (`rmse=0.378741`).
  - `REALITYVK_RVK2_DEBUG_ALT_TMEM8_XOR=1`: negligible change (`rmse=0.335588`).
  - Interpretation: primary texel-stage gap is unlikely to be 16-bit TMEM endianness; 4-bit nibble mapping matters but current default looks directionally better.
- Post-detail-change strict replay snapshot:
  - New trace replay summary: `frame_count=126`, `failed_count=125` (`--strict`).
  - Error families remain dominated by state/hash row mismatches; executor present-size mismatch persists on 57 frames.
  - Interpretation: replay still serves as mismatch classifier, not a pass gate for this phase.
- Coordinate-mode isolation with new debug toggles:
  - Added executor toggles: `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD=1` and `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_LOD_COORD=1`.
  - `final` stage:
    - baseline `rmse=0.308398`
    - `disable_persp`: `rmse=0.366254` (large change)
    - `disable_lod`: no measurable change from baseline
  - `texel_raw` stage:
    - baseline `rmse=0.338058`
    - `disable_persp`: `rmse=0.442120` (large change)
    - `disable_lod`: no measurable change from baseline
  - Interpretation: texture perspective correction is an active high-impact path in this scenario; LOD coordinate adjustment path is likely inactive in `paper_mario_intro`.
- Reference capture viability probe:
  - Upstream/reference plugin captures remained fully black at `120`, `200`, and `300` stepped frames.
  - Flip mode probe (`DUMPFB_FLIP_Y=0`) was also fully black.
  - Interpretation: baseline limitation is likely capture-path/plugin integration, not simply an early-frame sample point.
- Reference capture method split implemented:
  - `paper_mario_smoke_runner.sh` now supports `REALITYVK_SMOKE_CAPTURE_METHOD={dumpfb-preset,screenshot}`.
  - `paper_mario_parity.sh` defaults to `REFERENCE_CAPTURE_METHOD=screenshot` and `CANDIDATE_CAPTURE_METHOD=dumpfb-preset`.
  - Fresh parity run with reference screenshot capture produced non-black reference (`reference_non_black_ratio=0.945702`, `reference_mean_luma=0.440622`).
  - Interpretation: baseline is now meaningful for fidelity comparison; continue using content metrics as secondary guardrails.
- 60-frame upstream capture probe (same ROM/core/plugin, method-only delta):
  - `screenshot` capture at 60 frames: `non_black_ratio=0.945702`, `mean_luma=0.440622`.
  - `dumpfb-preset` capture at 60 frames: `non_black_ratio=0.000000`, `mean_luma=0.000000`.
  - Artifact paths:
    - `build/parity-runs/paper-mario/probes/paper_mario_intro.reference.60f.screenshot.ppm`
    - `build/parity-runs/paper-mario/probes/paper_mario_intro.reference.60f.dumpfb.ppm`
  - Interpretation: user observation is confirmed; visible upstream content exists in first 60 frames and black reference was strictly a `dumpfb`-path artifact.
- Screenshot-baseline stage probe snapshot (`REALITYVK_PM_REFERENCE_CAPTURE_METHOD=screenshot`):
  - `final`: `rmse=0.425197`, `mae=0.341833`.
  - `texel_raw`: `rmse=0.434403`, `mae=0.349503`.
  - `combiner_out`: `rmse=0.436229`, `mae=0.351595`.
  - `texel_raw` with `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD=1`: `rmse=0.431538`, `mae=0.345829`.
  - `final` with `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD=1`: `rmse=0.409024`, `mae=0.323094`.
  - Interpretation: with a valid reference baseline, RVK2 perspective-coordinate path now looks directionally suspicious (likely over-correction/precision mismatch) while LOD coordinate path remains low-signal in this scene.
- Candidate capture method equivalence probe (`paper_mario_intro`, 120 frames, same plugin/core):
  - `dumpfb` repeatability: `dumpfb_a vs dumpfb_b` was bit-identical (`rmse=0`, `mae=0`).
  - `dumpfb` vs `screenshot` (flip0): close but not equal (`rmse=0.034973`, `mae=0.004463`, exact pixel match `0.918915`).
  - `dumpfb` vs `screenshot` (flip1): clearly mismatched orientation (`rmse=0.357821`, `mae=0.250405`).
  - Interpretation: for candidate parity, `dumpfb` and `screenshot` are not equivalent captures; keep candidate baseline on deterministic `dumpfb-preset`.
- Capture orientation auto-resolution implemented:
  - `paper_mario_smoke_runner.sh` now accepts `REALITYVK_SMOKE_SCREENSHOT_FLIP_Y={0,1,auto}` with `auto` default.
  - `auto` resolves to inverse of `REALITYVK_SMOKE_DUMPFB_FLIP_Y` so screenshot orientation tracks dumpfb orientation.
  - `paper_mario_parity.sh` now mirrors the same policy via `REALITYVK_PM_SCREENSHOT_FLIP_Y={0,1,auto}` (default `auto`).
  - Validation probe (`paper_mario_intro`, 120 frames):
    - `DUMPFB_FLIP_Y=1 + SCREENSHOT_FLIP_Y=auto`: `rmse=0.034973`, `mae=0.004463`.
    - `DUMPFB_FLIP_Y=0 + SCREENSHOT_FLIP_Y=auto`: `rmse=0.034973`, `mae=0.004463`.
  - Interpretation: method-level orientation mismatch is now controlled centrally and no longer requires manual flip pairing.
- Emulator output orientation control implemented in RVK2 present path:
  - `src/Graphics/RealityVK2/rvk2_ContextImpl.cpp` present fullscreen quad now supports `REALITYVK_RVK2_PRESENT_FLIP_Y`.
  - Default is enabled (`REALITYVK_RVK2_PRESENT_FLIP_Y=1`) to align on-screen output with dumpfb-oriented expectation.
  - Override is available for regression checks (`REALITYVK_RVK2_PRESENT_FLIP_Y=0` restores legacy orientation).
  - Toggle verification (`paper_mario_intro`, screenshot capture):
    - `present_flip0 vs present_flip1`: `rmse=0.356900`, `mae=0.249270`.
    - `present_flip0 vs vertical_flip(present_flip1)`: `rmse=0.058894`, `mae=0.009010`.
  - Interpretation: present-stage Y orientation is now explicitly controllable at runtime and materially changes emulator output.
- Post-present-flip baseline check:
  - `./scripts/paper_mario_parity.sh` (visual gate disabled, cached screenshot reference): `rmse=0.402675`, `mae=0.300835`.
  - Content metrics remained stable/non-empty (`candidate_non_black_ratio=0.635193`, `candidate_mean_luma=0.191392`).
  - Interpretation: change integrates with existing harness and improves current screenshot-reference parity score.
- Comparison-script determinism pin:
  - `paper_mario_parity.sh` now pins candidate present orientation via `REALITYVK_PM_RVK2_PRESENT_FLIP_Y` (default `1`) and forwards it as `REALITYVK_RVK2_PRESENT_FLIP_Y`.
  - Interpretation: parity runs stay stable even if RVK2 present-flip defaults are changed later.
- Synthetic triangle perspective packing correction implemented:
  - `src/Graphics/RealityVK2/rvk2_SyntheticTriangle.h` now packs textured `S/T` as `S_over_W` / `T_over_W` when synthetic submit path is under `texturePersp`.
  - Synthetic submit callers now pass current perspective mode explicitly (`src/gSP.cpp`, `src/GraphicsDrawer.cpp`).
  - Added regression coverage: `testSyntheticTrianglePerspectivePacking` in `src/Graphics/RealityVK2/rvk2_UnitTests.cpp`.
  - Short validation: `rvk2_unit_tests` and `rvk2_conformance_tests` both passed after change.
- Post-fix perspective toggle probe (`paper_mario_intro`, screenshot reference, visual gate disabled):
  - `final` baseline: `rmse=0.404369`, `mae=0.302727`.
  - `final` with `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD=1`: `rmse=0.406381`, `mae=0.304720`.
  - `texel_raw` baseline: `rmse=0.420730`, `mae=0.318123`.
  - `texel_raw` with `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD=1`: `rmse=0.421522`, `mae=0.319756`.
  - Interpretation: perspective-disable no longer improves parity in this scenario, which indicates the previous synthetic perspective-domain mismatch has been materially reduced.
- Post-fix stage sweep snapshot (`REALITYVK_RVK2_DEBUG_STAGE_VIEW`):
  - `texel_raw`: `rmse=0.420730`, `candidate_non_black_ratio=0.573313`.
  - `combiner_out`: `rmse=0.394545`, `candidate_non_black_ratio=0.561188`.
  - `blender_out`: `rmse=0.404369`, `candidate_non_black_ratio=0.484905`.
  - `vi_source`: `rmse=0.401642`, `candidate_non_black_ratio=0.464306`.
  - `final`: `rmse=0.404369`, `candidate_non_black_ratio=0.484905`.
  - Interpretation: dominant remaining gap family in this baseline appears downstream of combiner (blender/coverage/depth/write behavior), not texture perspective coordinate math.
- Blender-path isolation probes added and run (debug-only toggles in `rvk2_Executor`):
  - Added toggles: `REALITYVK_RVK2_DEBUG_BYPASS_BLENDER`, `REALITYVK_RVK2_DEBUG_DISABLE_BLEND_MEMORY_COLOR_SOURCE`, `REALITYVK_RVK2_DEBUG_DISABLE_BLENDER_DITHER`, `REALITYVK_RVK2_DEBUG_DISABLE_IMAGE_READ`, plus coverage-control probes.
  - `bypass_blender`: `rmse=0.394545`, `mae=0.294528` (improves over baseline `0.404369`).
  - `bypass_blender` + `vi_source`: `rmse=0.386952`, `mae=0.287600`.
  - `disable_coverage_controls` and `disable_color_on_cvg_inhibit`: no measurable change from baseline.
  - `disable_blender_dither`: no meaningful RMSE gain.
  - `disable_blend_memory_color_source`: strongest gain (`rmse=0.390649`, `mae=0.292547`).
  - `disable_image_read` alone regressed (`rmse=0.418881`), so image-read gating itself is not the root mismatch.
  - Interpretation: memory-color path is involved, but this probe alone does not identify whether the gap is selector usage or cycle timing/aliasing.
- Primitive split on memory-color probe:
  - `disable_texrect`: `rmse=0.483519`; `disable_texrect + disable_blend_memory_color_source`: `rmse=0.459261` (large relative gain).
  - `disable_triangle`: `rmse=0.435205`; `disable_triangle + disable_blend_memory_color_source`: `rmse=0.439480` (slight regression).
  - Interpretation (corrected): memory-color probe gain is triangle-cycle2-dominant (`disable_texrect` leaves triangles), not texrect-dominant.
- Attempted default texrect-cycle1 memory-blend heuristic was reverted:
  - Heuristic trial regressed baseline (`rmse=0.407050`, `candidate_non_black_ratio=0.466024`) versus baseline (`rmse=0.404369`, `candidate_non_black_ratio=0.484905`).
  - Reverted to baseline behavior; this branch is now superseded by cycle2 second-pass memory alias fix below.
- Packet/mode census for texrect work (`rvk2.packet.rootcause.tsv`):
  - Texrect render-work rows are dominated by two modes:
    - `phase=1 cycle=0`: count `3285`, selectors `c1(P,A,M,B)=(0,0,1,0)`, force blender on, image-read on.
    - `phase=3 cycle=2`: count `1938`, copy path baseline mode.
  - Interpretation: texrect path did not explain the strongest memory-color probe gain; triangle cycle2 remained the primary suspect.
- Additional alpha hypothesis probe:
  - Added debug probe `REALITYVK_RVK2_DEBUG_FORCE_TEXEL_ALPHA_OPAQUE=1` (texrect path only).
  - Result regressed (`rmse=0.408929`), so dominant mismatch is not explained by simple texel-alpha underflow in this scene.
- Cycle2 second-pass memory alias probe and fix:
  - Added toggle `REALITYVK_RVK2_DEBUG_CYCLE2_SECOND_PASS_MEMORY_FROM_CYCLE1`.
  - Probe with toggle enabled: `rmse=0.381233`, `mae=0.283354` (large gain over `0.404369`).
  - Primitive attribution with toggle enabled:
    - `disable_texrect` (triangle-only): `rmse=0.459261` (improves from `0.483519`).
    - `disable_triangle` (texrect-only): `rmse=0.435186` (neutral vs `0.435205`).
  - Forensics deltas vs old baseline:
    - class11 (`cycle2+force+coverage`) `c2b`: `36563 -> 32963` (large reduction).
    - class10 (`cycle1+force+coverage`) changed opposite direction but smaller impact.
  - Implemented as default behavior:
    - cycle2 second-pass blender `M` input now aliases cycle1 output by default.
    - opt-out for regression checks: `REALITYVK_RVK2_DEBUG_CYCLE2_SECOND_PASS_MEMORY_FROM_CYCLE1=0`.
  - Post-fix baseline (`paper_mario_intro`, screenshot reference): `rmse=0.381233`, `mae=0.283354`, `candidate_non_black_ratio=0.635604`, `candidate_mean_luma=0.286474`.
  - Validation: `rvk2_unit_tests`, `rvk2_conformance_tests`, and `./scripts/local_gate.sh` passed.
- Texture mode census refinement from packet trace (`rvk2.packet.rootcause.tsv`):
  - Draw-mode distribution (textured):
    - texrect cycle0: `n=3285`, dominant tile/mode `tile=(0,3)`, `tf=0`, `tlut=0` (plus minor `(3,1)` cases).
    - texrect cycle2 copy lane: `n=1938`, `tile=(2,1)`, `tf=0`, `tlut=2`.
    - triangle cycle1 lane: `n=2793` textured rows, dominated by `tf=2`, `tp=1`, with tile sets `(4,0)`, `(0,3)`, and `(2,0)` (`tlut=2` subset).
  - Interpretation: texture filtering semantics are active and high-impact in triangle lane; TLUT-heavy path is primarily `tf=0` texrect cycle2 + subset of cycle1 triangles.
- 32-bit TMEM decode probe matrix (debug-only, no default behavior change):
  - Added probes:
    - `REALITYVK_RVK2_DEBUG_TMEM32_DIRECT_LINEAR=1`
    - `REALITYVK_RVK2_DEBUG_TMEM32_XOR02=1`
    - `REALITYVK_RVK2_DEBUG_TMEM32_PACK_HIGH_TO_LOW=1`
  - Results (`final`, screenshot reference):
    - baseline: `rmse=0.381233`
    - `PACK_HIGH_TO_LOW`: `rmse=0.403061` (regression)
    - `XOR02`: `rmse=0.382166` (near-neutral/slight regression)
    - `DIRECT_LINEAR`: `rmse=0.402775` (regression)
    - `DIRECT_LINEAR + PACK_HIGH_TO_LOW`: `rmse=0.413366` (regression)
  - Additional check:
    - `texel_raw` baseline `0.420730` vs `DIRECT_LINEAR` `0.437816` (regression).
  - Interpretation: current default 32-bit decode path remains directionally correct for this scenario; no default TMEM32 addressing/packing change landed.
- TLUT attribution refinement:
  - `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_LUT_APPLY=1` regressed baseline (`rmse=0.401727` vs `0.381233`).
  - Split:
    - triangle-only (`disable_texrect`): `0.459261 -> 0.476492` with LUT disabled (regression).
    - texrect-only (`disable_triangle`): unchanged (`0.435205` with/without LUT disable).
  - Interpretation: TLUT application is required for the active triangle subset; it is not the dominant texrect mismatch driver in this scene.
- Landed texture filter-mode mapping fix in RVK2 executor:
  - Root cause: `applyTextureFilterMode` mapping did not match project N64 constants (`G_TF_BILERP=2`, `G_TF_AVERAGE=3`), and mode `3` used a synthetic sharpen path.
  - Change:
    - `mode 2 -> bilerp`
    - `mode 3 -> average`
    - removed synthetic sharpen behavior from authoritative path.
  - Post-fix parity (`paper_mario_intro`, screenshot reference):
    - `final`: `rmse=0.378566`, `mae=0.290226` (improves from `0.381233`).
    - `texel_raw`: `rmse=0.413381`, `mae=0.321891` (improves from `0.420730`).
  - Primitive split after fix:
    - `disable_texrect` (triangle-only): `rmse=0.458961` (slight gain from `0.459261`).
    - `disable_triangle` (texrect-only): `rmse=0.431324` (gain from `0.435205`).
  - Validation after landing:
    - `rvk2_unit_tests`: PASS
    - `rvk2_conformance_tests`: PASS
    - `./scripts/local_gate.sh`: PASS
- Extended screenshot-baseline probe matrix after filter fix (`paper_mario_intro`):
  - Baseline (`final`): `rmse=0.378566`, `mae=0.290226`, `candidate_non_black_ratio=0.682901`.
  - `texel_raw/combiner/blender/final` sweep:
    - `texel_raw`: `0.413381`
    - `combiner_out`: `0.387603`
    - `blender_out`: `0.378566`
    - `final`: `0.378566`
  - Interpretation: dominant residual mismatch remains in texel stage; blender/final are currently closer than texel/combiner for this baseline.
- TMEM4 nibble-swap split matrix (debug probe only):
  - `texel_raw` improved with swap:
    - all writes: `0.413381 -> 0.399347`
    - triangle-only: `0.478398 -> 0.466218`
    - texrect-only: unchanged (`0.435472`)
  - `final` regressed with swap:
    - all writes: `0.378566 -> 0.386973`
    - triangle-only: `0.458961 -> 0.465825`
    - texrect-only: unchanged (`0.431324`)
  - Interpretation: nibble order is not a safe default flip; keep current default and treat this as a path interaction signal.
- Coverage/blender/cycle2 control probes (all with screenshot reference):
  - `REALITYVK_RVK2_DEBUG_DISABLE_COVERAGE_CONTROLS=1`: no measurable change.
  - `REALITYVK_RVK2_DEBUG_DISABLE_COLOR_ON_CVG_INHIBIT=1`: no measurable change.
  - `REALITYVK_RVK2_DEBUG_DISABLE_CYCLE2_PREV_MEMORY=1`: no measurable change.
  - `REALITYVK_RVK2_DEBUG_CYCLE2_SECOND_PASS_MEMORY_FROM_CYCLE1=1`: no measurable change.
  - `REALITYVK_RVK2_DEBUG_BYPASS_BLENDER=1`: regressed (`0.387603`).
  - `REALITYVK_RVK2_DEBUG_DISABLE_BLEND_MEMORY_COLOR_SOURCE=1`: regressed (`0.384951`).
  - `REALITYVK_RVK2_DEBUG_DISABLE_IMAGE_READ=1`: regressed (`0.391407`).
  - `REALITYVK_RVK2_DEBUG_FORCE_BLEND_DIVIDE=1`: slight regression (`0.381098`).
  - Interpretation: current missing-content gap is not driven by these post-texel control paths in this scenario.
- Coordinate-path confirmation after synthetic-perspective and filter fixes:
  - `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_LOD_COORD=1`: no measurable change (final and texel).
  - `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD=1`: slight regression (final `0.380661`; texel `0.415371`).
  - Interpretation: no high-value remaining fix signal in LOD/perspective toggles for `paper_mario_intro`.
- Texel alpha-force split probe:
  - triangle-only + `REALITYVK_RVK2_DEBUG_FORCE_ALL_TEXEL_ALPHA_OPAQUE=1`: unchanged.
  - texrect-only + force opaque: regressed (`0.431324 -> 0.434609`, non-black down).
  - full frame + force opaque: regressed (`0.389236`).
  - Interpretation: blanket texel-alpha forcing is not the missing-content fix.
- Trace-replay caveat discovered during row-level diff:
  - For latest frame replay, declared render-work rows have key fields zeroed (`other_modes`, `fill_color`, `sync_epoch`, several mode-derived booleans) while recomputed rows are non-zero; row hash mismatch remains expected.
  - Interpretation: for mode-level root-cause decisions in this phase, prioritize live executor forensics + parity metrics over declared trace-row field values.
- Current narrowed hypothesis after this probe set:
  - Remaining visible gaps are texture-stage and split across both primitive families:
    - triangle lane (TLUT-active subset),
    - texrect lane (non-TLUT dominant subset).
  - Next high-value work is texture decode/source attribution refinement by active format/size/mode buckets, not additional blender/coverage toggles.
- Added live texture-attribution counters to executor forensics:
  - New `REALITYVK2_FRAME_FORENSICS_FILE` fields:
    - `tx_filter_mode{0..3}`
    - `tx_lut_mode{0..3}`
    - `tx_fmt{0..4}_samples`
    - `tx_size{0..3}_samples`
    - `tx_fs_f{fmt}_s{size}`
    - `tx_fs_lut_f{fmt}_s{size}`
  - Purpose: mode/format attribution now comes from live executor samples rather than inferred packet-row fields.
- Live mode-bucket readout (`paper_mario_intro`, baseline final frame):
  - filter distribution:
    - `mode2=449556`
    - `mode3=515460`
  - LUT distribution:
    - `mode0=792216`
    - `mode2=172800`
  - active format/size buckets:
    - `f0s2=321672` (`lut=0`)
    - `f0s3=454648` (`lut=0`)
    - `f2s0=115200` (`lut=115200`)
    - `f3s1=73496` (`lut=0`)
- Primitive split with live counters:
  - triangle-only (`disable_texrect`):
    - filter: `mode2` only (`449556`)
    - active buckets: `f0s2`, `f0s3`, `f2s0(lut)`
  - texrect-only (`disable_triangle`):
    - filter: `mode3` only (`515460`)
    - active buckets: `f0s2`, `f0s3`, `f3s1`
  - Interpretation: texture mismatch ownership is now clearly split by primitive family and filter mode.
- Targeted filter-mode A/B probes (debug-only):
  - Added temporary toggles:
    - `REALITYVK_RVK2_DEBUG_TEXTURE_FILTER_MODE3_BILERP=1`
    - `REALITYVK_RVK2_DEBUG_TEXTURE_FILTER_MODE2_AVERAGE=1`
  - Results:
    - mode3->bilerp regressed (`final 0.380871`, `texel 0.420278`, rect-only `0.435193`).
    - mode2->average regressed (`final 0.378929`, `texel 0.413841`, tri-only `0.459261`).
  - Interpretation: current mode mapping (`2=bilerp`, `3=average`) remains directionally correct; residual texture gap is likely in decode/addressing nuances inside active format-size buckets, not high-level mode mapping.
- Validation after instrumentation-only changes:
  - `rvk2_unit_tests`: PASS
  - `rvk2_conformance_tests`: PASS
  - `./scripts/local_gate.sh`: PASS
- Added bucket-selective texture isolation gate in executor:
  - New debug env: `REALITYVK_RVK2_DEBUG_TEXTURE_BUCKET_MASK`.
  - Token format:
    - `f<fmt>s<size>` (allow bucket regardless of LUT usage),
    - `f<fmt>s<size>l` or `f<fmt>s<size>lut` (allow LUT-only samples for bucket),
    - `f<fmt>s<size>n` or `f<fmt>s<size>nolut` (allow non-LUT-only samples for bucket),
    - comma-separated list; `all`/`*` enables all; `off`/`none` disables mask.
  - Masked-out texture samples return opaque black in texel stage so parity deltas can be attributed to selected buckets.
  - New forensics fields:
    - `tx_mask_allow`
    - `tx_mask_reject`
- Full-frame bucket isolation matrix (`paper_mario_intro`, screenshot reference, visual gate off, non-black capture gate off):
  - baseline: `rmse=0.378566`, `non_black=0.682901`.
  - `f0s2`: `rmse=0.510760`, `non_black=0.082757`, `mask_allow=321672`, `mask_reject=643344`.
  - `f0s3`: `rmse=0.414914`, `non_black=0.495355`, `mask_allow=454648`, `mask_reject=510368`.
  - `f3s1`: `rmse=0.506866`, `non_black=0.108305`, `mask_allow=73496`, `mask_reject=891520`.
  - `f2s0l`: `rmse=0.491245`, `non_black=0.222006`, `mask_allow=115200`, `mask_reject=849816`.
  - Interpretation: `f0s3` is the dominant “stuff on screen” carrier; missing-content candidates concentrate in non-dominant buckets (`f0s2`, `f3s1`, `f2s0(lut)`).
- Primitive-split bucket isolation:
  - triangle-only baseline (`disable_texrect`): `rmse=0.458961`.
    - `tri_f0s3`: `0.480005` (closest single-bucket case),
    - `tri_f2s0l`: `0.491497`,
    - `tri_f0s2`: `0.511203`.
  - texrect-only baseline (`disable_triangle`): `rmse=0.431324`.
    - `rect_f0s3`: `0.439671` (closest single-bucket case),
    - `rect_f3s1`: `0.503655`,
    - `rect_f0s2`: `0.510822`.
  - Interpretation: both primitive families are currently anchored by `f0s3`; next highest-priority root-cause pass should target decode/addressing differences in `f0s2` + (`f3s1` texrect lane, `f2s0(lut)` triangle lane).
- Texel-stage bucket isolation confirmation (`REALITYVK_RVK2_DEBUG_STAGE_VIEW=texel_raw`):
  - baseline: `rmse=0.413381`.
  - `f0s3`: `0.418713` (closest single-bucket case).
  - `f0s2`: `0.509577`.
  - `f3s1`: `0.503326`.
  - `f2s0l`: `0.511441`.
  - Interpretation: bucket ranking holds at texel stage, so current missing-content signal is texture-source/decode lane specific rather than downstream combiner/blender behavior.
- Trace/replay observability hardening:
  - `W` trace rows now emit expanded render-work state (138 columns; legacy 109 still accepted by replay parser).
  - Added replay `--stateful-frames` mode to carry RDP/TMEM state across frames for stricter mismatch classification.
  - Result: state/hash/row false positives drop in short sequential replay windows; remaining failures are concentrated in executor-present classes.
- TLUT byte-semantics fix landed in RVK2 executor:
  - IA16 TLUT decode now treats entries as `A:I` byte order.
  - RGBA16 TLUT decode now byte-swaps TMEM word before `RGBA5551` channel unpack.
  - Validation: `rvk2_unit_tests`, `rvk2_conformance_tests`, and `./scripts/local_gate.sh` passed.
  - `paper_mario_intro` parity deltas (screenshot reference):
    - final baseline: `rmse=0.375718`, `mae=0.286012`, `candidate_non_black_ratio=0.683657`.
      - prior baseline: `rmse=0.378566`, `mae=0.290226`, `candidate_non_black_ratio=0.682901`.
    - `texel_raw` baseline: `rmse=0.389317` (prior `0.413381`).
    - `f2s0l` isolation: `rmse=0.489039` (prior `0.491245`).
    - `f3s1` isolation: unchanged (`rmse=0.506866`).
- Deviation playbook now supports blink-aware structural modes:
  - Added `missing_non_black` mode (`reference non-black && candidate black`) and `extra_non_black` mode (`candidate non-black && reference black`).
  - Deep telemetry default now uses `missing_non_black` with an ignore box over the blinking `Press Start` hotspot (`238,245,482,380` at 720x540).
  - New metrics include analyzed/ignored pixel counts plus missing/extra non-black counts and ratios.
  - Current structural lead from one-frame analysis: dominant missing region remains top-left screen coverage (not center blink noise); RDP scissor is full-screen across render work, so clipping is not the primary culprit.
- Added missing-region focus census (`scripts/rvk2_missing_region_focus.py`) and deep-telemetry integration:
  - Maps deviation boxes from post-VI capture space back to source-space and intersects them with render-work bounds in the capture frame.
  - Current `paper_mario_intro` result (`frame 126`, source box `x=[3..259], y=[0..186]`):
    - texrect hits: `57/59` (`96.6%`)
    - triangle hits: `10/53` (`18.9%`)
    - dominant intersecting bucket: `texrect:f0s3` (`56` hits), with minor `texrect:f3s1` (`1` hit).
  - Interpretation: highest-priority forward fix lane is texrect-side `f0s3` texture decode/addressing/presentation behavior inside the missing top-left region.
- Executor surface bootstrap telemetry/fix lane (new):
  - Per-address surface history bootstrap remains opt-in in RVK2 executor:
    - `REALITYVK_RVK2_DEBUG_ENABLE_SURFACE_HISTORY_BOOTSTRAP=1`
    - Behavior: when a color-image address reappears, initialize the working surface from cached prior-frame contents (format/size/width compatible) instead of hard-zero.
  - Paper Mario parity smoke now enables this bootstrap by default for the candidate path:
    - `REALITYVK_PM_RVK2_ENABLE_SURFACE_HISTORY_BOOTSTRAP=1` (default)
    - `REALITYVK_PM_RVK2_ENABLE_CROSS_SURFACE_BOOTSTRAP=1` (default)
    - Both can be set to `0` to disable during A/B probes.
  - Added optional cross-surface bootstrap/merge hook (kept opt-in):
    - `REALITYVK_RVK2_DEBUG_ENABLE_CROSS_SURFACE_BOOTSTRAP=1`
    - Behavior: allow fallback/merge from previously selected present surface for multi-buffer partial redraw experiments.
  - Rationale: `paper_mario_intro` shows persistent partial-write behavior (`present_select=3`, selected surface often from history, live writes on selected surface `0`), so “start from black every frame” under-fills large regions.
  - Measured impact (`paper_mario_intro`, screenshot reference):
    - baseline: `rmse=0.375718`, `candidate_non_black_ratio=0.683657`, `candidate_mean_luma=0.259130`
    - surface-history bootstrap enabled: `rmse=0.368224`, `candidate_non_black_ratio=0.741795`, `candidate_mean_luma=0.280509`
    - cross-surface bootstrap additionally enabled: `rmse=0.367523`, `candidate_non_black_ratio=0.744787`, `candidate_mean_luma=0.281487`
  - Quality gates:
    - `rvk2_unit_tests`: PASS
    - `rvk2_conformance_tests`: PASS
  - Current interpretation:
    - bootstrap materially reduces missing-content area, but residual left-strip coverage gap remains (`left-column occupancy unchanged`), so upstream command/geometry lane still needs targeted investigation.
- Missing-region focus telemetry v2 (address-history + segment occupancy):
  - `scripts/rvk2_missing_region_focus.py` now emits:
    - frame-local write coverage (`left/center/right` segment ratios),
    - per-color-image-address coverage stats,
    - short history-window (`N=3` frames by default) address rotation and per-address missing-box coverage,
    - missing-pixel write attribution (`missing_with_write` vs `missing_without_write`) mapped to source-space.
  - `scripts/rvk2_telemetry_bundle.py` now consumes these fields and emits direct leads for:
    - rotating target buffers across adjacent frames,
    - non-present target dominance in missing-region coverage,
    - texrect-vs-triangle dominance in missing boxes.
- Missing-region focus telemetry v3 (history-owner attribution + deeper single-run coverage):
  - `scripts/rvk2_missing_region_focus.py` now also emits:
    - `work_hit_stats` (`hit_total`, emitted sample count, truncation count) so chronology truncation is explicit.
    - history-owner attribution for missing pixels:
      - `missing_without_current_with_prior_write` vs `missing_without_current_without_prior_write`,
      - prior-frame overlap by color-image target (`prior_address_overlap_rows`),
      - present-surface overlap vs dominant prior overlap target.
    - prior-window address write stats (`history_window.prior_address_write_stats`) and prior frame ids.
  - `scripts/paper_mario_parity.sh` deep telemetry now defaults to high-coverage missing-region sampling:
    - `REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_MAX_HIT_SAMPLES=4096`
    - `REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_HISTORY_WINDOW=4`
    - `REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_MAX_ADDRESS_OVERLAP=16`
  - `scripts/rvk2_telemetry_bundle.py` now emits additional leads when:
    - unwritten missing pixels are mostly explained by prior-frame writes (carry-forward/handoff dependency),
    - unwritten missing pixels are not explained by recent history (absent primitive coverage),
    - multiple prior targets strongly overlap unwritten missing pixels (cross-surface composition requirement).
- Deep-smoke archival (new default behavior):
  - `scripts/paper_mario_parity.sh` now archives each deep telemetry run under:
    - `build/parity-runs/paper-mario/archive/<scenario>.<utc-stamp>.<git-sha>/`
  - Archive includes:
    - core compare artifacts (`reference/candidate`, diff, side-by-side image, capture context, metrics),
    - telemetry outputs (trace/packet replay, forensics, command census, missing-region focus, bundle),
    - deviation playbook directory.
  - Indexing and metadata:
    - append-only TSV index at `build/parity-runs/paper-mario/archive/index.tsv`,
    - per-run metadata JSON at `<run>/run_meta.json`,
    - latest symlink at `build/parity-runs/paper-mario/archive/paper_mario_intro.latest`.
  - Control knobs:
    - `REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE=0/1` (default `1`)
    - `REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE_ROOT=<dir>`
    - `REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE_INDEX=<file>`
- New deep run (`paper_mario-exp-handoff-lane`, bootstrap + cross-surface + `REALITYVK_RVK2_DEBUG_DISABLE_VI_HISTORY_PRESENT=1`):
  - `rmse=0.368067`, `candidate_non_black_ratio=0.742217`, `candidate_mean_luma=0.280745`.
  - Missing-region focus (`frame 126`) still shows mixed failure modes:
    - write coverage in missing boxes: `0.6166` overall,
    - segment source-box write ratios: `left=0.3129`, `center=0.8183`, `right=0.8228`.
  - Dominant missing-region texrect lane is still `f0s3` (`56/57` texrect write-hit samples), with dominant texrect state:
    - `tile_line=50`, `tile_tmem=0`, `cycle_type=1-cycle`.
  - Missing-pixel attribution (new):
    - source missing pixels: `17189`
    - covered by any write bounds: `2620` (`15.24%`)
    - not covered by any write bounds: `14569` (`84.76%`)
    - left segment unwritten ratio: `95.14%` (center `63.98%`)
  - Interpretation: center/right mismatches are more likely texrect texture/color correctness (writes occur but still diverge), while left-side deficit still has a write-coverage gap.
- TMEM32 decode sanity probes (quick smoke, bootstrap + cross-surface):
  - `REALITYVK_RVK2_DEBUG_TMEM32_DIRECT_LINEAR=1`: `rmse=0.378957` (worse).
  - `REALITYVK_RVK2_DEBUG_TMEM32_XOR02=1`: `rmse=0.368355` (slightly worse).
  - `REALITYVK_RVK2_DEBUG_TMEM32_PACK_HIGH_TO_LOW=1`: `rmse=0.403117` (much worse).
  - Interpretation: current default TMEM32 decode path remains the best-known baseline; dominant residual is unlikely to be solved by global TMEM32 addressing/packing toggles.
- Texture-source ordering probe:
  - Added diagnostic toggle: `REALITYVK_RVK2_DEBUG_FORCE_TEXTURE_RDRAM_PRIMARY=1`.
  - Result (`paper_mario_intro`, bootstrap + cross-surface): unchanged metrics versus current best (`rmse=0.367523`, `candidate_non_black_ratio=0.744787`, `candidate_mean_luma=0.281487`).
  - Interpretation: dominant remaining mismatch is not explained by TMEM-vs-RDRAM source priority.
- Missing-region state-dominance counters:
  - Missing-region focus now emits per-op write-hit dominant state maps (`combine_mux`, `other_modes`, `tile_line`, `texture_image_width`, etc.).
  - Current `paper_mario_intro` dominant texrect state in missing boxes:
    - `combine_mux=0x00FFFFFFFFFCF279` (`56/57`),
    - `other_modes=0x00000CFF00504340` (`57/57`),
    - `tile_line=50`, `texture_image_width=200` (`56/57`).
  - Interpretation: remaining mismatch is concentrated in one texrect render-state cluster, so next fix lane should target this exact state class instead of broad texture subsystem toggles.
- Present-handoff override probe:
  - Added diagnostic toggle: `REALITYVK_RVK2_DEBUG_PREFER_LIVE_SURFACE_OVER_HISTORY=1`.
  - Result (`paper_mario_intro`, bootstrap + cross-surface): unchanged versus current best (`rmse=0.367523`, `candidate_non_black_ratio=0.744787`).
  - Interpretation: for this capture frame, forcing a live-surface preference over history selection does not change the visual outcome; residual gap remains in the dominant texrect state cluster.

## Debug Support Matrix (maps steps to findings)

- `Baseline integrity/content-first`: Step 1 capture run; treat `reference_non_black_ratio` near zero as baseline-limited and prioritize candidate content metrics.
- `Baseline capture viability`: Step 1 frame-count probe (for example `120/200/300`) and flip probe (`REALITYVK_PM_DUMPFB_FLIP_Y=0/1`) to distinguish early-frame issues from capture-path issues.
- `Reference capture method split`: use `REALITYVK_PM_REFERENCE_CAPTURE_METHOD=screenshot` with `REALITYVK_PM_SCREENSHOT_DIR` and keep candidate on `dumpfb-preset`.
- `Capture method sanity`: when upstream appears black, run same-frame method A/B (`screenshot` vs `dumpfb-preset`) before treating it as render failure.
- `Candidate capture equivalence`: do not swap candidate to screenshot for parity metrics unless explicitly re-baselined; method introduces measurable per-pixel drift even when visually close.
- `Flip policy`: keep `SCREENSHOT_FLIP_Y=auto` unless a manual orientation override is intentionally under test.
- `Emulator output orientation`: use `REALITYVK_RVK2_PRESENT_FLIP_Y` for runtime on-screen orientation fixes; capture-tool flips should not be used as substitutes for present-path correctness.
- `TMEM decode/fallback`: Step 3 (`texel_raw`) + Step 4 TMEM toggles; verify with `tx_tmem`, `tx_synth`, and stage-source counters in forensics TSV.
- `Texture bucket attribution`: use live counters (`tx_filter_mode*`, `tx_lut_mode*`, `tx_fs_f*_s*`, `tx_fs_lut_f*_s*`) to identify the active decode/filter buckets before changing texture logic.
- `Bucket isolation`: use `REALITYVK_RVK2_DEBUG_TEXTURE_BUCKET_MASK` with `REALITYVK_PM_REQUIRE_NON_BLACK_CAPTURE=0` to isolate active buckets (`f0s2`, `f0s3`, `f3s1`, `f2s0l`) and rank impact before code changes.
- `Texture filter mode semantics`: derive active `TEXTFILT` modes from packet-trace census before adjusting sampler math; when `tf=2` dominates, verify bilerp path (`G_TF_BILERP`) instead of average/sharpen placeholders.
- `Filter A/B probes`: use temporary mode remap toggles only as diagnostics; keep default mapping unless both final and texel stages improve together.
- `Texture detail/LOD`: Step 3 (`texel_raw`) then `combiner_out`; check whether mismatch appears before blender/VI.
- `Texture perspective sanity`: compare baseline vs `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD=1` at `texel_raw` and `final`; if disabling improves RMSE consistently, prioritize perspective divide/precision and tile-shift modeling; if disabling is neutral/worse after synthetic-triangle packing fix, deprioritize perspective and shift focus to downstream blend/coverage behavior.
- `Synthetic triangle perspective packing`: for HLE-heavy scenes, validate that `TP_PERSP` state is forwarded into synthetic submit and that packed `S/T/W` produce stable or improved baseline without relying on `DISABLE_TEXTURE_PERSP_COORD`.
- `Combiner/blender/depth/coverage`: Step 3 (`combiner_out`, `blender_out`, `final`) + Step 4 write-disable and cycle2-memory toggle (`REALITYVK_RVK2_DEBUG_CYCLE2_SECOND_PASS_MEMORY_FROM_CYCLE1=0/1`).
- `Coverage/blender neutrality checks`: if coverage/cycle2/blender-control probes are flat or regressive against baseline, treat post-texel pipeline as lower priority and return to texel-source/decode attribution.
- `Mode-bit behavior`: Step 2 replay strictness + Step 3 stage sweep; confirm bit toggle produces expected stage-local effect only.
- `Trace-row reliability`: if declared render-work rows show zeroed mode fields while live executor counters are non-zero, use replay hashes as structural classifiers only and avoid using declared row values as mode truth.
- `VI semantics`: Step 3 compare `vi_source` vs `final`; if `vi_source` matches and `final` fails, classify as VI gap.
- `Hidden/coverage model`: Step 3 (`blender_out`/`final`) + forensics coverage/depth counters; look for edge-only diffs.
- `Validation blind spots`: Step 2 can pass replay while visual mismatch remains; treat as expected and continue stage isolation.
- `HLE synthetic triangle drift`: Step 4 triangle write toggle and provenance review; correlate failures with HLE triangle-heavy scenes.

## RVK2 Gap Debug Plan (`paper_mario_intro`)

### 1. Capture deterministic parity baseline

```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_VISUAL_GATE=0 \
REALITYVK_PM_REFERENCE_CAPTURE_METHOD=screenshot \
REALITYVK_PM_CANDIDATE_CAPTURE_METHOD=dumpfb-preset \
./scripts/paper_mario_parity.sh
```

Track:
- `build/parity-runs/paper-mario/paper_mario_intro.metrics.json`
- `build/parity-runs/paper-mario/paper_mario_intro.diff.png`
- `build/parity-runs/paper-mario/paper_mario_intro.capture-context.json`

Interpret baseline first:
- If `reference_non_black_ratio` is near zero, classify run as content-first.
- In content-first mode, use candidate non-black/luma metrics as primary "is RVK2 drawing" gate.
- Optional strict cached-reference check: `REALITYVK_PM_VALIDATE_CACHED_REFERENCE_CAPTURE=1`.
- If using screenshot reference capture, keep `REALITYVK_PM_SCREENSHOT_FLIP_Y` fixed across runs for determinism (`auto` is deterministic for fixed `REALITYVK_PM_DUMPFB_FLIP_Y`).

### 2. Collect frame forensics and packet replay evidence

```bash
mkdir -p build/debug/paper_mario_intro

REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK2_FRAME_FORENSICS_FILE=build/debug/paper_mario_intro/frame.forensics.tsv \
REALITYVK2_CAPTURE_RDP_TRACE=1 \
REALITYVK2_PACKET_TRACE_FILE=build/debug/paper_mario_intro/rvk2.packet.tsv \
./scripts/paper_mario_parity.sh

python3 scripts/rvk2_packet_trace_replay.py \
  --input build/debug/paper_mario_intro/rvk2.packet.tsv \
  --json-out build/debug/paper_mario_intro/rvk2.packet.replay.json \
  --jobs 0 --strict
```

Use this pass to classify failures:
- command/state/replay mismatches
- raster/executor mismatches
- VI selection/scanout mismatches

### 3. Stage-localize first divergence

Run parity repeatedly with:
- `REALITYVK_RVK2_DEBUG_STAGE_VIEW=texel_raw`
- `REALITYVK_RVK2_DEBUG_STAGE_VIEW=combiner_out`
- `REALITYVK_RVK2_DEBUG_STAGE_VIEW=blender_out`
- `REALITYVK_RVK2_DEBUG_STAGE_VIEW=vi_source`
- `REALITYVK_RVK2_DEBUG_STAGE_VIEW=final`

Interpretation:
- First bad at `texel_raw`: TMEM decode/addressing/coords/LUT/detail gaps.
- First bad at `combiner_out`: combiner selector or key/convert behavior gaps.
- First bad at `blender_out`: blend/coverage/depth interaction gaps.
- `vi_source` matches but `final` fails: VI post-processing/register semantics gaps.

### 4. Isolate logic classes with toggles

Apply one toggle at a time:
- `REALITYVK_RVK2_DEBUG_DISABLE_TRIANGLE_WRITES=1`
- `REALITYVK_RVK2_DEBUG_DISABLE_TEXRECT_WRITES=1`
- `REALITYVK_RVK2_DEBUG_DISABLE_CYCLE2_PREV_MEMORY=1`
- `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_PERSP_COORD=1`
- `REALITYVK_RVK2_DEBUG_DISABLE_TEXTURE_LOD_COORD=1`
- `REALITYVK_RVK2_DEBUG_SWAP_TMEM16=1`
- `REALITYVK_RVK2_DEBUG_SWAP_TMEM4_NIBBLES=1`
- `REALITYVK_RVK2_DEBUG_ALT_TMEM8_XOR=1`
- `REALITYVK_RVK2_DEBUG_TMEM32_DIRECT_LINEAR=1`
- `REALITYVK_RVK2_DEBUG_TMEM32_XOR02=1`
- `REALITYVK_RVK2_DEBUG_TMEM32_PACK_HIGH_TO_LOW=1`
- `REALITYVK_RVK2_DEBUG_CYCLE2_SECOND_PASS_MEMORY_FROM_CYCLE1=0/1`
- `REALITYVK_RVK2_DEBUG_FORCE_TEXEL_ALPHA_OPAQUE=1`

Keep only hypotheses that move metrics/diff signature consistently.

### 5. Maintain issue-to-root-cause log

Record per mismatch:
- artifact path
- first bad stage
- symptom category (`TMEM`, `Combiner`, `Blender/Coverage/Depth`, `VI`)
- suspected source file/function
- next proving test

### 6. Exit criteria for this phase

- Deterministic repro for `paper_mario_intro`.
- RVK2 content-first gate is stable (`candidate_non_black_ratio` and `candidate_mean_luma` stay above chosen floor).
- `rvk2_packet_trace_replay.py --strict` failures are reduced and each mismatch class has explicit owner/function.
- Every remaining mismatch is tagged with a concrete suspected logic gap and owner file/function.
