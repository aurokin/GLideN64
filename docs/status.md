# RVK2 Status

## Priority Order
1. TMEM `LoadBlock` correctness lane
- Goal: match RDP TMEM addressing semantics for `LoadBlock` (`dxt` stepping + odd/even interleave/swap).
- Exit signal: structural texture/geometry reappearance improves with shadow-off and missing-region unwritten pressure drops.

2. TLUT / CI decode correctness lane
- Goal: fix `LoadTLUT` and CI palette usage (including unaligned/palette ordering edge cases).
- Exit signal: CI-backed textures stop presenting as black/corrupt and dominant missing boxes shrink.

3. Tile/load mutation semantics lane
- Goal: ensure load command side effects and `SetTileSize` restore semantics match hardware expectations.
- Exit signal: no recurring geometry/texture holes tied to load-phase tile state leakage.

4. Present/handoff coherence lane
- Goal: eliminate missing pixels that were written in prior frames but are unwritten in the focus frame.
- Evidence:
  - `missing_without_write_ratio=0.896015`
  - `missing_without_write_with_prior_write_ratio=1.0`
  - left segment remains most affected (`0.954816` unwritten ratio).
- Exit signal: prior-write overlap drops materially and missing-region dominance is no longer unwritten carry-forward.

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
