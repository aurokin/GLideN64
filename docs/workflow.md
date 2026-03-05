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

## Decision Lock (2026-03-05)
- Inner-loop oracle: use `shadow-on` only for same-run structural attribution (not as an acceptance target).
- Fix order lock:
  1. overwrite/combiner state cluster (`fill` + texrect packet neighborhood)
  2. canonical TMEM mapping (`LoadBlock`/`LoadTLUT`/CI path)
  3. triangle visibility/raster recovery
  4. replay parity cleanup once structure is recovered
- Temporary fallback toggles are allowed only as probes and must not be required for a landed fix.
- Lane advancement gate: require movement in `candidate_non_black_ratio` and missing-region attribution metrics; do not accept RMSE-only wins.

## Current Diagnosis Snapshot (2026-03-05)
- Latest non-shadow deep archive run:
  - `paper_mario_intro.20260305-100328Z.cdf22eca`
- Metrics:
  - `rmse=0.371346`
  - `mae=0.276243`
  - `candidate_non_black_ratio=0.775134`
  - `candidate_mean_luma=0.297256`
- Present policy checkpoint:
  - when VI origin maps only to history and a compatible live surface has writes, executor now prefers live by default.
  - probe override remains available: `REALITYVK_RVK2_DEBUG_KEEP_VI_MATCHED_HISTORY_SELECTION=1`.
- Deterministic oracle snapshot (`20260305-094633Z`, `frames=10`, retry `0`):
  - shadow-on `candidate_non_black_ratio=0.885972`
  - shadow-off `candidate_non_black_ratio=0.673351`
  - `missing_without_write_ratio=0.9123`, `missing_without_write_with_prior_write_ratio=1.0`
- Remaining dominant leads:
  - missing-region prior hits still cluster in texrect/fill neighborhood around
    `combine=0x00FFFFFFFFFCF87C` and `other_modes=0x00208C7F00000000/0x00308C7F00000000`.
  - replay still reports present-size/hash divergence, but this is currently treated as a secondary signal until Stage-A structure improves.
  - TMEM load-kind row-XOR stays probe-only (debug toggles), pending full canonical TMEM mapping.

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
1. Run the combined oracle workflow (off + on + diff + packet attribution):
```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_PROFILE=deep \
REALITYVK_PM_VISUAL_GATE=0 \
./scripts/paper_mario_shadow_oracle.sh --frames 20 --retry-count 0
```
2. Review generated artifacts under `build/parity-runs/paper-mario/shadow-oracle/<stamp>/oracle-compare`.
3. Prioritize `missing_without_write_ratio` and top `missing_with_write_packet_hits` from
   `missing-region-focus.off-vs-shadow.json`.
4. If needed, run the manual same-run variant below for narrow experiments.
5. Run deep with shadow present and executor dump enabled:
```bash
REALITYVK_PM_SCENARIO_ID=paper_mario_intro \
REALITYVK_PM_PROFILE=deep \
REALITYVK_PM_VISUAL_GATE=0 \
REALITYVK_RVK2_SHADOW_DRAW=1 \
REALITYVK_RVK2_SHADOW_PRESENT=1 \
./scripts/paper_mario_parity.sh
```
6. Compare same-run `candidate` (shadow-present) vs `executor-present` artifact:
- candidate: `build/parity-runs/paper-mario/paper_mario_intro.candidate.png`
- executor dump: `build/parity-runs/paper-mario/telemetry/paper_mario_intro.candidate.executor-present.ppm`
7. Use this pair for missing-region attribution before changing raster/TMEM lanes.

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
