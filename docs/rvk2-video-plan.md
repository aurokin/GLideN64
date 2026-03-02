# RVK2 Video Plan (Single Source)

Last updated: 2026-03-02

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
5. Cycle2 hazard approximations now include TEX1 next-pixel combiner sourcing and cycle2 alpha-compare next-pixel combiner lookahead.

## Remaining Work Map (28%)

1. `P5` cycle semantics closure (combiner/blender/coverage/depth): **9%**
2. `P3` authoritative TMEM path closure (especially 32b): **9%**
3. `P4` raster/coefficient edge behavior: **5%**
4. `P2` present-source determinism polish: **3%**
5. `P6` VI finishing polish: **2%**

## Latest Batch (2026-03-02)

1. Added cycle2 combiner TEX hazard approximation in executor:
   - cycle2 `TEX1` now uses next-pixel TEX0 approximation.
2. Added cycle2 alpha-compare hazard approximation:
   - cycle2 alpha compare now uses next-pixel first-cycle combiner output approximation.
3. Added conformance coverage for cycle2 TEX selector hazard behavior.
4. Kept local gate green after this batch.

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
