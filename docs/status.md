# RVK2 Status

## Priority Order
1. Temporal surface-content divergence lane
- Goal: eliminate conflicting non-black content across rotating present surfaces.
- Evidence:
  - `missing_without_write_with_prior_write_ratio=1.0` with strong overlap across prior color-image targets.
  - History-merge probe (`REALITYVK_RVK2_DEBUG_HISTORY_MERGE_LOG`) shows `potential_black_fill=0` while `potential_nonblack_diff` remains high (roughly `18k..51k` pixels per candidate surface pair).
  - Forcing static present surfaces (`0x00583430`, `0x005A8C30`, `0x005CE430`) worsens parity vs default selection.
- Exit signal: cross-surface non-black divergence drops materially and missing-region dominance no longer comes from prior-frame state clusters.

2. Left-strip primitive coverage lane
- Goal: recover missing writes on the left segment where geometry remains absent.
- Evidence: left-segment missing pixels are predominantly unwritten relative to center.
- New ingress correlation: frontend rect ingress spans `x≈0..260` with `60` rect calls, but dominant focus-frame texrect work still concentrates inside `x=60..260` and leaves left-strip deficits unresolved.
- Exit signal: left-segment write coverage approaches center/right segment behavior.

3. Dominant texrect state-cluster correctness lane
- Goal: fix texture/color correctness for dominant texrect render state.
- Focus state:
  - `combine_mux=0x00FFFFFFFFFCF279`
  - `other_modes=0x00000CFF00504340`
  - `tile_line=50`
  - `texture_image_width=200`
- Exit signal: missing-region texrect-dominance leads and structural-box count decline together.

4. VI/present cleanup lane (after structural recovery)
- Goal: tighten final image mapping and reduce residual stage hash drift.
- Exit signal: no meaningful visual regression between `vi_source` and `final` stage outputs for focus frames.

5. Debug/perf cleanup lane
- Goal: keep debug toggles probe-only and move stabilized behavior to non-debug paths.
- Exit signal: no required fix depends on persistent debug-only env toggles.

## Current Baseline
- Latest archive run: `paper_mario_intro.20260303-232934Z.ca92fb7a`
- Archive index: `build/parity-runs/paper-mario/archive/index.tsv`
- Key metrics:
  - `rmse=0.367518`
  - `mae=0.280595`
  - `candidate_non_black_ratio=0.744977`
  - `candidate_mean_luma=0.281491`
