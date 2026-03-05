# RVK2 Status

## Priority Order
1. Overwrite/combiner state-cluster lane (current top target)
- Goal: resolve dominant black-write cluster and zero-shade divergence in the executor path.
- Focus signature: `op=fill`, `combine=0x00FFFFFFFFFCF87C`, `other_modes=0x00308C7F00000000`.
- Exit signal: bundle no longer flags dominant overwrite cluster + zero-shade source packets.

2. TMEM + CI/TLUT correctness lane
- Goal: close remaining texture decode/addressing gaps (`LoadBlock`/`LoadTLUT`/CI palette paths).
- Exit signal: missing texture regions stop tracking TMEM/CI-heavy packet clusters in shadow-oracle diffs.

3. Triangle visibility/raster lane
- Goal: remove UI-only symptom in focus frames and recover missing geometry path.
- Exit signal: command/replay evidence no longer reports texrect-only focus windows for structural frames.

4. Replay parity cleanup lane
- Goal: reduce present-size/hash divergence noise so stateful replay becomes a high-confidence regression gate.
- Exit signal: replay failures stop clustering on `executor_present_hash`/size mismatches.

5. Debug/perf hardening lane
- Goal: keep fixes on default runtime path and keep debug toggles probe-only.
- Exit signal: no accepted behavior fix depends on `REALITYVK_RVK2_DEBUG_*`.

## Current Diagnosis Snapshot
- Present-selection coherence checkpoint landed:
  - VI-matched history selection is now preserved by default when live-surface fallback would break VI-origin coherence.
  - Suspected gap removed in archive compare: `VI origin did not match selected present surface`.
- TMEM load-kind row-XOR checkpoint landed:
  - TMEM8/TMEM32 load-kind-aware row XOR is now default executor behavior (no debug env toggle required).
  - Deep archive compare shows directional image-metric improvement with no new suspected gaps.
- Shadow-oracle evidence (same-run, 20-frame deep, replay disabled):
  - executor-vs-shadow `best_mae` improved `0.238958 -> 0.228807`.
  - executor-vs-shadow `best_rmse` improved `0.366680 -> 0.360818`.
- Remaining high-value leads are now upstream of final present handoff (overwrite cluster + raster/source divergence).

## Current Baseline (2026-03-05)
- Latest non-shadow deep archive run: `paper_mario_intro.20260305-003843Z.57f1559c`
- Comparison baseline for this checkpoint: `paper_mario_intro.20260304-235955Z.63c167f0`
- Archive index: `build/parity-runs/paper-mario/archive/index.tsv`
- Latest key metrics:
  - `rmse=0.346580`
  - `mae=0.249116`
  - `candidate_non_black_ratio=0.766512`
  - `candidate_mean_luma=0.268280`
- Delta vs baseline (`235955 -> 003843`):
  - `rmse`: `-0.004260`
  - `mae`: `-0.001477`
  - `candidate_non_black_ratio`: `+0.01pp`
  - suspected gaps: unchanged (count `12`)

## Tooling Constraints
- Upstream `GLideN64` `dumpfb-preset` capture remains black in agent-mode flow; treat reference non-black ratio as non-actionable.
- Use stateful replay for deterministic class attribution; non-stateful replay is triage-only.
- For executor-vs-shadow attribution, prioritize same-run comparison (`shadow-present candidate` vs `executor-present dump`) to avoid run-to-run drift.
