# RVK2 Video Plan (Single Source)

Last updated: 2026-03-03

## Mission

Achieve stable RVK2 video output that is:
1. Non-black.
2. Recognizable game image.
3. Motion-correct enough for active gameplay bring-up.

Accuracy-first. RVK2-only path. No legacy renderer fallback.

## Baseline

1. Local gate is green.
2. Stage/forensics instrumentation is active and useful.
3. Paper Mario parity metric is stable but still visually incorrect.
4. Deep-dive corpus is integrated under `docs/references/n64/deep-dive-pack/`.
5. Cycle hazard modeling now includes:
   - 1-cycle selector-bank behavior: combiner/blender now source second-cycle selector fields.
   - RH#001: 1-cycle `TEX1 -> next-pixel TEX0`.
   - RH#002: cycle2 second-cycle `TEX0 -> current TEX1`, `TEX1 -> next-pixel TEX0`.
   - cycle2 alpha-compare next-pixel combiner lookahead.
6. TMEM32 sampling now uses a single authoritative split-word decode path (no experimental mode matrix).
7. Blender selector `A=shade alpha` now consumes interpolated shade alpha instead of combiner-alpha approximation.
8. Synthetic texel fallback now renders a stable coordinate/state pattern instead of random noise.
9. VI origin selection now prefers in-range surfaces matching VI width when multiple candidates overlap.
10. Hidden coverage bit-plane is now persisted per surface and consumed by blender memory-coverage alpha paths.
11. TEXEL1 sampling from secondary tile descriptors (tile+1) is now wired into both cycle hazard paths.
12. Triangle Y edge values are now consumed as signed 14-bit s10.2 values in both render-plan bound derivation and executor rasterization.
13. Executor TMEM sampling is now bound to per-draw historical TMEM snapshots (instead of end-of-frame global TMEM state).

## Remaining Work Map (2.9%)

1. `P5` cycle semantics closure (combiner/blender/coverage/depth): **0.5%**
2. `P3` authoritative TMEM path closure (especially 32b): **1.4%**
3. `P4` raster/coefficient edge behavior: **0.2%**
4. `P2` present-source determinism polish: **0.5%**
5. `P6` VI finishing polish: **0.2%**

## Latest Batch (2026-03-03)

1. Closed major TMEM temporal-coherency gap:
   - runtime now captures deduplicated TMEM snapshots and binds them per draw packet.
   - executor now selects TMEM words per work item instead of sampling end-of-frame `TMEM`.
2. Validation signal jump:
   - Paper Mario candidate moved from near-black/noise to a high-energy textured frame (`rmse=0.284339`, `mae=0.138428` vs black-reference cache).
   - stage sweep now shows materially non-black `texel_raw/combiner_out/blender_out/final` outputs, confirming texture data is flowing through the pipe.
3. Local validation:
   - `./scripts/local_gate.sh` PASS (release+debug unit+conformance).
4. Closed blender `color_on_cvg` non-overflow write target:
   - non-overflow `color_on_cvg` path now writes blender `M` input (2B path) instead of hardcoded framebuffer memory color.
   - added conformance lock `testColorOnCvgWritesBlenderMInputConformance`.
5. Closed VI content-window over-letterboxing path:
   - removed second-pass content fit inside already aspect-resolved output, so VI now fills resolved output instead of reintroducing heavy center-band letterbox.
   - updated VI aspect unit expectations to lock fill behavior.

## Previous Batch (2026-03-03)

1. Closed signed-triangle-Y interpretation gap:
   - render-plan triangle bounds now sign-extend `YL/YM/YH` as signed 14-bit s10.2 values.
   - executor triangle raster now uses signed `YL/YM/YH` for edge reconstruction and area tests.
2. Updated trace-replay mirror (`scripts/rvk2_packet_trace_replay.py`) to preserve schema-v1 consistency with runtime math.
3. Added regression lock:
   - unit test `testTriangleSignedYBounds` verifies negative-Y triangle commands no longer explode bounds.
4. Validation:
   - `./scripts/local_gate.sh` PASS (release+debug unit+conformance).
   - `./build/release-vulkan-smoke/rvk2_unit_tests` PASS.
   - `./build/release-vulkan-smoke/rvk2_conformance_tests` PASS.
5. Fresh parity + forensics snapshot:
   - metrics changed (`rmse=0.202891`, `mae=0.068396`) vs prior black-reference delta baseline.
   - triangle path activated in live capture (`write_triangle_share=0.378955`, `stage_textured_triangle_share=0.374742`; previously near-zero in this path).
   - TMEM dominance retained (`stage_texel_source_tmem_rate=0.993263`), with small synthetic spill (`0.001063`) still to close.

1. Reworked tile-axis coordinate mapping to match documented RDP ordering:
   - shift in fixed-point (`s10.5`) before integer texel conversion,
   - tile-relative mapping via tile upper-left between shift and clamp/mirror/mask.
2. Corrected filtered sampling fractions to use transformed texture coordinates (instead of raw pre-transform fractions).
3. Enforced COPY-mode clamp behavior from N64 docs:
   - tile clamp bits are ignored in COPY phase while shift/mirror/mask still apply.
4. Added conformance lock:
   - `testCopyModeIgnoresTileClampConformance` now verifies copy-mode clamp-bit invariance under real TMEM sampling.
5. Removed negative-coordinate texel fallback holes:
   - negative tile-relative S/T no longer reject to fallback,
   - TMEM/RDRAM address math now uses signed bit-address accumulation, preserving wrapped addressing behavior.
6. Validation:
   - `./scripts/local_gate.sh` PASS (release+debug unit+conformance).
7. Fresh parity + forensics snapshot after transform batch:
   - metrics unchanged (`rmse=0.188514`, `mae=0.056238`),
   - forensics now shows fully TMEM-authoritative sampling in this capture (`tx_tmem=792216`, `tx_rdram=0`, `tx_synth=0`, `present_select=3`).

## Earlier Batch (2026-03-02)

1. Fixed first-cycle combiner `COMBINED` feedback hazard behavior:
   - first-cycle combiner now carries previous-pixel combined color feedback.
   - cycle2 alpha-compare next-pixel combiner lookahead now seeds from first-cycle combined feedback.
2. Added conformance coverage for cycle1 `COMBINED` feedback hazard behavior.
3. Corrected `color_on_cvg` blender semantics to match the RDP programming manual:
   - color writes are inhibited unless coverage overflows.
4. Added conformance coverage for `color_on_cvg` overflow bypass.
5. Corrected rectangle/scissor decode to pixel-space coordinates:
   - `SetScissor` and `TexRect/FillRect` edges now decode from 10.2 fixed-point into integer pixels.
6. Updated unit tests and trace-replay script (`scripts/rvk2_packet_trace_replay.py`) to stay schema-v1-consistent with the new decode semantics.
7. Fresh parity run with fresh trace+forensics now shows clear signal jump:
   - non-black candidate capture (reference remains black in this scenario),
   - render-work rect bounds collapse to framebuffer scale (`max_lrx`: fill=319, texrect=307, tri=320),
   - selected present surface width now near VI width (`321` vs prior `~1041`),
   - VI source/output luminance and non-black sample counts increased substantially.
8. Local gate remains green (release+debug unit+conformance).
9. Fixed present-selection history handoff consistency:
   - history-picked VI-origin surfaces no longer get mislabeled as `NoSurface`,
   - `present_select` distribution in fresh parity now reflects intent (`3=218`, `5=44`, `7=1`; removed prior `4` churn).
10. Added regression unit test coverage for history-backed VI-origin selection reason preservation.
11. Corrected tile axis clamp semantics:
   - tile `ULS/ULT/LRS/LRT` bounds are now applied only when clamp mode is set.
12. Fresh parity after clamp fix shows a stronger rendered signal jump (rmse `0.188514` vs prior `0.140786` against black reference).
13. Corrected `color_on_cvg` write gating to RDP manual semantics:
   - inhibit color writes unless coverage overflows.
14. Added RVK2 debug-isolation toggles for rapid hypothesis testing:
   - `REALITYVK_RVK2_DEBUG_DISABLE_CYCLE2_PREV_MEMORY`
   - `REALITYVK_RVK2_DEBUG_FORCE_BLEND_DIVIDE`
   - `REALITYVK_RVK2_DEBUG_SWAP_TMEM16`
   - `REALITYVK_RVK2_DEBUG_SWAP_TMEM4_NIBBLES`
   - `REALITYVK_RVK2_DEBUG_ALT_TMEM8_XOR`
   - `REALITYVK_RVK2_DEBUG_DISABLE_TRIANGLE_WRITES`
   - `REALITYVK_RVK2_DEBUG_DISABLE_TEXRECT_WRITES`
15. Diagnostic sweep outcome:
   - disabling cycle2 previous-memory behavior had no effect on this capture,
   - disabling triangles or texrects both significantly changes luma/error profile, confirming both raster paths still carry high-risk semantics.

## What Deep-Dive Changed In Our Plan

1. Hidden/coverage memory is a first-class correctness surface, not a side detail.
2. VI mid-frame register mutation is now a top-priority validation target.
3. Stress-ROM-driven validation should run in parallel with parity, not after parity.
4. paraLLEl implementation shortcuts are reference hints, not normative hardware truth.

## Batch Plan

## Batch A: RDP Semantic Closure (Largest Remaining Risk)

Goal: close dominant `b10/b11` mismatch classes and cycle hazards.

1. Add focused conformance matrices for:
   - `alpha_cvg_sel`, `cvg_x_alpha`, `cvg_dest`, `color_on_cvg`.
   - cycle1 vs cycle2 selector behavior under destination-sensitive blends.
2. Implement missing cycle2 hazard behaviors still unmodeled:
   - TEX0/TEX1 second-cycle hazard handling.
   - any remaining confirmed previous/next-pixel dependency surfaces.
3. Keep all changes forensics-visible (new counters if needed).
4. Validate with:
   - `./scripts/local_gate.sh`
   - parity run
   - stage sweep (`final`, `texel_raw`, `combiner_out`, `blender_out`, `vi_source`).

Expected outcome: first clear jump from noise-like output toward recognizable scene structure.

## Batch B: TMEM + Hidden Coverage Authority

Goal: remove ambiguity in texel source and coverage storage semantics.

1. Close TMEM32 correctness so experimental mode is no longer needed.
2. Add hidden coverage/bit-plane parity instrumentation hooks (diagnostic-level if needed).
3. Validate against targeted stress behaviors from repeater64 references:
   - RDRAM 9th-bit dependent behavior.
   - fill/sync corner behavior where relevant to RVK2 scope.
4. Re-run gate + parity + forensics and compare class deltas.

Expected outcome: texel-source and coverage behavior stop drifting between classes and scenes.

## Batch C: VI Dynamics + Present Determinism

Goal: stabilize final displayed output and motion behavior.

1. Add VI mid-frame register update micro-tests (scanline-shift/scale style effects).
2. Tighten field/serrate/interlace behavior under active frame transitions.
3. Finalize present-source policy edge cases (bootstrap and origin/type boundaries).
4. Keep deterministic behavior across repeated runs.

Expected outcome: stable recognizable moving image and deterministic frame selection.

## Update Contract

After each coding cycle:
1. Update this file only (do not split status across multiple roadmap docs).
2. Record:
   - completed items,
   - changed assumptions,
   - next actions.
3. Keep old implementation logs and artifacts under `build/`, not `docs/`.

## Completion Criteria For This Plan

1. No mostly-black parity capture retries across repeated candidate runs.
2. At least one target game intro scene is visually recognizable and moving.
3. Local gate remains green.
4. Remaining mismatches are quality/correctness deltas, not missing-image failures.
