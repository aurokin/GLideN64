# RVK2 Status

## Priority Order
1. Present/handoff coherence lane
- Goal: eliminate missing pixels that were written in prior frames but are unwritten in the focus frame.
- Evidence:
  - `missing_without_write_ratio=0.896015`
  - `missing_without_write_with_prior_write_ratio=1.0`
  - left segment remains most affected (`0.954816` unwritten ratio).
- Exit signal: prior-write overlap drops materially and missing-region dominance is no longer unwritten carry-forward.

2. Dominant texrect state-cluster correctness lane
- Goal: recover missing textures/geometry tied to the dominant texrect state in missing regions.
- Focus state:
  - `combine_mux=0x00FFFFFFFFFCF279`
  - `other_modes=0x00000CFF00504340`
  - `tile_line=50`
  - `texture_image_width=200`
- Exit signal: dominant-state hit share drops and missing-region structural boxes shrink.

3. Left-strip primitive/work coverage lane
- Goal: restore write coverage on the left side where missing coverage is highest.
- Evidence: left-segment missing pixels are disproportionately unwritten versus center/right.
- Exit signal: left-segment unwritten ratio trends toward center/right behavior.

4. VI/present cleanup lane (after structural recovery)
- Goal: reduce residual output drift after major scene elements are present.
- Exit signal: no meaningful visual regression between pre-present and final stage outputs for focus frames.

5. Debug/perf cleanup lane
- Goal: keep debug toggles probe-only and migrate stabilized behavior into non-debug paths.
- Exit signal: no accepted fix requires persistent debug-only toggles.

## Current Baseline (2026-03-04)
- Latest archive run: `paper_mario_intro.20260304-190750Z.10319b63`
- Archive index: `build/parity-runs/paper-mario/archive/index.tsv`
- Key metrics:
  - `rmse=0.365291`
  - `mae=0.278921`
  - `candidate_non_black_ratio=0.775134`
  - `candidate_mean_luma=0.297241`
- Tooling constraints in effect:
  - upstream `GLideN64` reference `dumpfb-preset` is black in agent-mode flow; treat reference non-black ratio as non-actionable in this workflow.
  - deep replay should be interpreted from stateful runs first; non-stateful replay remains low-confidence for first-divergence attribution.
