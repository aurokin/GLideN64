# RVK2 Workflow

## Primary Objective
- Fix missing textures and missing geometry in `paper_mario_intro` (title background, characters, checkerboard stage) on Vulkan `rvk2`.
- Preserve deterministic smoke telemetry and archived comparison evidence for every deep run.

## Acceptance Bar
- Stage A (structural coverage):
  - `candidate_non_black_ratio >= 0.90`
  - `missing_without_write_ratio <= 0.35`
  - left-segment `missing_without_write_ratio <= 0.60`
  - telemetry bundle has no present-surface hard fault.
- Stage B (image deviation):
  - `rmse <= 0.25`
  - `mae <= 0.20`
  - no large missing-region boxes outside the ignored blink hotspot.
- Stage C (ship-quality parity for this scene):
  - major scene elements are present and stable in side-by-side monitor view.
  - deep telemetry no longer reports carry-forward/handoff as dominant failure class.

## Reference / Ground-Truth Assumptions
- Scenario: `paper_mario_intro` from `tests/smoke/scenarios.tsv`.
- Reference capture:
  - upstream `GLideN64`
  - screenshot method (`screenshot_scale2_flip0`).
- Candidate capture:
  - this repo `RealityVK`
  - deterministic `dumpfb-preset` path.
- Blink handling:
  - ignore `Press Start` hotspot via default diff ignore box `238,245,482,380` (720x540).
- Comparison source of truth:
  - archived deep run metrics + telemetry bundle, not only on-screen screenshots.

## Current Diagnosis Snapshot (2026-03-03)
- Latest deep smoke metrics:
  - `rmse=0.367523`
  - `mae=0.280598`
  - `candidate_non_black_ratio=0.744787`
  - `candidate_mean_luma=0.281487`
- Missing attribution:
  - `source_missing_pixels=17189`
  - `missing_with_write_pixels=2620` (`15.24%`)
  - `missing_without_write_pixels=14569` (`84.76%`)
  - `missing_without_write_with_prior_write_ratio=1.0`
  - `missing_without_write_without_prior_write_ratio=0.0`
- Dominant texrect state cluster in missing region:
  - `combine_mux=0x00FFFFFFFFFCF279`
  - `other_modes=0x00000CFF00504340`
  - `tile_line=50`
  - `texture_image_width=200`
- New ingress/work correlation (deep run `paper_mario_intro.20260303-232934Z.ca92fb7a`):
  - `ing_tri_calls=0` while `work_tri=53` (triangle work is RVK2 command-ingestion only, not frontend draw-call ingress).
  - `ing_rect_calls=60`, `ing_rect_texrect_calls=60`, `ing_rect_bounds=[x:-0.975..260.000, y:-0.974..223.000]`.
  - Focus-frame render-work texrects remain concentrated in `x=60..260` (source width 320), consistent with persistent left-strip missing coverage.
- Interpretation:
  - primary deficit is carry-forward/handoff composition plus missing left-strip write coverage.
  - texrect state-cluster correctness remains the main pixel-quality lane once coverage is restored.

## Standard Execution Loop
1. Run quick smoke for fast regression check.
2. Run deep smoke for structural diagnostics when render/present behavior changes.
3. Compare latest archive index row to prior run.
4. Apply one targeted fix lane from `docs/status.md`.
5. Re-run deep smoke and confirm metric + suspected-gap movement.
6. Commit and push incremental checkpoints with required attribution.
