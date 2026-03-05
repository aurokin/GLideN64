# RVK2 Workflow

## Primary Objective
- Fix missing textures and missing geometry in `paper_mario_intro` (title background, characters, checkerboard stage) on Vulkan `rvk2`.
- Keep every render-behavior change backed by deterministic parity artifacts and deep telemetry.

## Acceptance Bar
- Stage A (structural coverage):
  - `candidate_non_black_ratio >= 0.90`
  - `missing_without_write_ratio <= 0.35`
  - left-segment `missing_without_write_ratio <= 0.60`
  - no present-surface hard fault in telemetry bundle.
- Stage B (image deviation):
  - `rmse <= 0.25`
  - `mae <= 0.20`
  - no large missing-region boxes outside the ignored blink hotspot.
- Stage C (scene parity):
  - title-screen scene elements are present and stable in side-by-side monitoring output.
  - deep telemetry no longer flags carry-forward/present-handoff as dominant failure class.

## Reference / Ground-Truth Assumptions
- Scenario: `paper_mario_intro` from `tests/smoke/scenarios.tsv`.
- Reference plugin: upstream `GLideN64`.
- Candidate plugin: this repository (`RealityVK`).
- Deterministic capture path: `paper_mario_smoke_runner.sh` + `agentctl dumpfb-preset`.
- Current constraint: upstream `GLideN64` does not emit usable agent-mode `dumpfb-preset` frames in this workflow (reference capture is expected black); parity script auto-disables reference non-black validation for this path.
- Press Start blink hotspot is ignored by default diff box: `238,245,482,380` (720x540 space).
- Source of truth for decisions: deep archive metrics + telemetry bundle, not monitor screenshots alone.
- Legacy note: older archives may contain `screenshot_*` reference captures; refresh with `REALITYVK_PM_REFRESH_REFERENCE=1` when migrating old caches.

## Decision Lock (2026-03-04)
- Inner-loop oracle: accepted `shadow-on` candidate output for structural texture/geometry pacing.
- Fix order lock:
  1. `LoadBlock` TMEM addressing (`dxt` progression + odd/even interleave)
  2. `LoadTLUT` / CI palette path
  3. tile/load mutation semantics (`SetTileSize` restoration correctness)
- Temporary fallback toggles are allowed for bring-up only; they must be removed or intentionally promoted before final acceptance.
- Lane advancement gate: require movement in `candidate_non_black_ratio` + missing-region metrics; do not use RMSE-only wins.

## Current Diagnosis Snapshot (2026-03-04)
- Latest non-shadow deep archive run:
  - `paper_mario_intro.20260304-235955Z.63c167f0`
- Metrics:
  - `rmse=0.350840`
  - `mae=0.250593`
  - `candidate_non_black_ratio=0.766445`
  - `candidate_mean_luma=0.269865`
- Checkpointed present-path fix:
  - VI-matched history selection is preserved by default when live fallback would break VI-origin coherence.
  - Archive compare (`20260304-230122Z -> 20260304-235955Z`) removed suspected gap:
    - `VI origin did not match selected present surface`
- Remaining dominant leads:
  - black-write stream still clusters around:
    - `op=fill`, `combine=0x00FFFFFFFFFCF87C`, `other_modes=0x00308C7F00000000`
  - top source packets still report zero shade RGB in overwrite-heavy paths.
  - replay still reports present-size/hash divergence, indicating upstream raster/source mismatch remains.

## Standard Execution Loop
1. Quick smoke for fast regression check:
```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_PROFILE=basic \
REALITYVK_PM_VISUAL_GATE=0 \
./scripts/paper_mario_parity.sh
```
2. Deep smoke for render/present behavior changes (stateful replay defaults on in deep profile):
```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_PROFILE=deep \
REALITYVK_PM_VISUAL_GATE=0 \
./scripts/paper_mario_parity.sh
```
   - Deep profile default hygiene:
     - candidate-plugin telemetry freshness preflight is enforced.
     - non-archived telemetry clutter is pruned before run (`REALITYVK_PM_TELEMETRY_PRUNE_ENABLE=1`).
3. Compare latest two deep archives:
```bash
python3 scripts/rvk2_archive_compare.py \
  --index build/parity-runs/paper-mario/archive/index.tsv \
  --scenario paper_mario_intro
```
4. Apply one targeted lane from `docs/status.md`.
5. Re-run deep smoke and verify metric + `suspected_gaps` movement.
6. Commit and push incremental checkpoints with required co-author attribution.

## Fast Iteration Helpers
```bash
./scripts/paper_mario_iterate.sh --preset fast --runs 1 --frames 20
./scripts/paper_mario_focus_deep.sh --frames 120
./scripts/paper_mario_shadow_ab.sh --frames 20
```

## Shadow Oracle Loop (Same-Run)
- Use when reference dumpfb is non-actionable and you need executor-local attribution:
1. Run deep with shadow present and executor dump enabled:
```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_PROFILE=deep \
REALITYVK_PM_VISUAL_GATE=0 \
REALITYVK_RVK2_SHADOW_DRAW=1 \
REALITYVK_RVK2_SHADOW_PRESENT=1 \
./scripts/paper_mario_parity.sh
```
2. Compare same-run `candidate` (shadow-present) vs `executor-present` artifact:
- candidate: `build/parity-runs/paper-mario/paper_mario_intro.candidate.png`
- executor dump: `build/parity-runs/paper-mario/telemetry/paper_mario_intro.candidate.executor-present.ppm`
3. Use this pair for missing-region attribution before changing raster/TMEM lanes.

## Telemetry Hygiene
- Manual prune command:
```bash
./scripts/paper_mario_telemetry_cleanup.sh
```
- Disable default pre-run prune for one run:
```bash
REALITYVK_PM_TELEMETRY_PRUNE_ENABLE=0 ./scripts/paper_mario_parity.sh
```
- Dry-run prune preview:
```bash
REALITYVK_PM_TELEMETRY_PRUNE_DRY_RUN=1 ./scripts/paper_mario_parity.sh
```
