# RVK2 Core Fix Execution Plan

Date started: 2026-03-04  
Owner: Codex (active execution)

## Objective
Fix missing textures and missing geometry in `paper_mario_intro` on Vulkan `rvk2`, prioritizing structural recovery first (Stage A) and image error convergence second (Stage B).

## Acceptance Bar
- [ ] `candidate_non_black_ratio >= 0.90`
- [ ] `missing_without_write_ratio <= 0.35`
- [ ] `rmse <= 0.25`
- [ ] `mae <= 0.20`

## Current Baseline (2026-03-05)
- Deep archive run: `paper_mario_intro.20260305-100328Z.cdf22eca`
- Compare base: `paper_mario_intro.20260305-065447Z.36a63c79`
- Metrics:
  - `candidate_non_black_ratio=0.775134`
  - `rmse=0.371346`
  - `mae=0.276243`
  - `candidate_mean_luma=0.297256`
- Deep delta (`065447 -> 100328`):
  - `candidate_non_black_ratio`: `+18.84pp`
  - `rmse`: `+0.035550`
  - `mae`: `+0.054247`
  - suspected gaps: `14 -> 13`

## Oracle Snapshot (Current Structural Signal)
- Deterministic shadow oracle: `build/parity-runs/paper-mario/shadow-oracle/20260305-094633Z`
- `frames=10`, retry `0`
- Off vs on:
  - shadow-off `candidate_non_black_ratio=0.673351`
  - shadow-on `candidate_non_black_ratio=0.885972`
- Missing-region attribution:
  - `missing_without_write_ratio=0.9123`
  - `missing_without_write_with_prior_write_ratio=1.0`
- Dominant prior packet neighborhood remains texrect/fill cluster around:
  - `combine=0x00FFFFFFFFFCF87C`
  - `other_modes=0x00208C7F00000000` / `0x00308C7F00000000`

## Lane Order (Locked)

### Lane A: Overwrite / combiner state-cluster (active)
- [x] Isolate first structural divergence with deterministic off-vs-on oracle.
- [x] Confirm ingress parity (`SHADOW_DRAW` off/on packet traces identical).
- [x] Stabilize present selection so VI-history-only surfaces no longer hard-lock stale output.
- [x] Add targeted telemetry around dominant texrect/fill packet neighborhood (state + stage + write-mask context).
- [x] Implement executor-path behavior fix for texrect/fill overwrite divergence.
- [x] Validate with quick smoke + deep smoke + oracle compare.

### Lane B: Canonical TMEM mapping (`LoadBlock` / `LoadTLUT` / CI)
- [x] Add TMEM snapshot and load-context telemetry hooks.
- [x] Keep load-kind XOR probes available via debug envs.
- [x] Promote load-kind-aware TMEM row XOR defaults for TMEM8/TMEM32 runtime sampling.
- [ ] Implement single canonical TMEM write/fetch mapping path (remove split legacy/probe behavior).
- [ ] Reconcile `dxt` progression + odd/even row behavior with runtime fetch logic.
- [ ] Revalidate CI/TLUT decode path against canonical mapping.

### Lane C: Triangle visibility / raster recovery
- [ ] Use focus-frame telemetry to prove whether missing geometry is decode/raster/drop vs overwrite.
- [ ] Add per-stage probes for triangle visibility failures in affected frames.
- [ ] Land raster visibility fix once Lane A/B no longer dominate missing regions.

### Lane D: Replay parity cleanup (after Stage-A gains)
- [ ] Reduce present-size/hash mismatch noise.
- [ ] Keep non-stateful replay as triage signal only.
- [ ] Promote stateful replay confidence after present-handoff/raster drift narrows.

### Lane E: Debug/perf hardening
- [ ] Ensure landed behavior fixes do not require persistent `REALITYVK_RVK2_DEBUG_*` toggles.
- [ ] Keep probes discoverable, documented, and disabled by default.
- [ ] Keep telemetry heavy-path scoped to deep profile.

## Execution Rules
- One behavioral lane per checkpoint commit.
- Run `./scripts/local_gate.sh` before each checkpoint commit.
- Run quick smoke for fast iteration, deep smoke for behavior checkpoints.
- Use oracle artifacts + archived metrics for decisions, not monitor screenshots alone.
- Keep shadow mode as oracle only; final behavior must come from executor path with shadow off.

## Progress Log

### 2026-03-04
- [x] Created plan and locked objective/acceptance bar.
- [x] Added deep telemetry and missing-region tooling for packet-attributed analysis.
- [x] Added deterministic off-vs-on shadow oracle workflow.
- [x] Identified dominant missing-without-write class as primary structural blocker.

### 2026-03-05
- [x] Confirmed off/on ingress parity (packet trace line count + SHA match).
- [x] Added TMEM post-write snapshot capture hooks and unit coverage.
- [x] Hardened deep telemetry tooling for large logs (streaming parse, lower RSS).
- [x] Added present-surface split in missing-region prior-hit attribution.
- [x] Fixed TMEM load-kind XOR probe toggles so probe flags are no longer no-op.
- [x] Landed present-selection policy change:
  - when VI origin maps only to history and a compatible live surface has writes, prefer live by default.
- [x] Revalidated with `local_gate.sh` and deep smoke checkpoint (`20260305-100328Z`).
- [x] Expanded overwrite log schema for focus-cluster diagnosis:
  - added cycle/alpha/cvg/blend/depth/color/tile/texture state fields to overwrite TSV rows.
  - added focus-cluster state ranking (`top_states`, `top_source_packets`, `dominant_state`) in telemetry bundle.
- [x] Captured full deep checkpoint with expanded telemetry (`paper_mario_intro.20260305-102706Z.b7c46e23`):
  - image metrics unchanged vs prior full deep baseline (expected telemetry-only change).
  - dominant fill-cluster signature now explicit in bundle output (`combine=0x00FFFFFFFFFCF87C`, `other=0x00308C7F00000000`, `cycle_type=3`, `fill_color=0x00010001`).
- [x] Probed non-black-aware present fallback ranking (20-frame deep) and reverted:
  - no structural movement in quick/deep probe metrics.
  - removed heuristic to keep executor behavior stable; retained telemetry improvements.
- [x] Landed targeted VI-history carry escalation for fallback-selected live surfaces:
  - strict carry now retries with visible-diff carry only when all are true:
    - `present_select=most_written_fallback`
    - VI-history candidate age is recent (`<=1`)
    - strict carry copied zero pixels
    - strict non-black diff potential exceeds threshold.
  - frame-forensics in oracle run `20260305-124849Z` confirms fallback frames now carry:
    - frames `18/20/22/24/26`: `selected_surface_vi_history_carry_copied` moved `0 -> 12572/14551/16159/17767/19372`.
  - quick smoke remained stable (`candidate_non_black_ratio=0.775134`).
  - 20-frame oracle moved `off candidate_non_black_ratio` from prior low runs (`0.574846`) to `0.586726`; missing-without-write remains dominant (`0.882528`).
- [x] Promoted load-kind-aware TMEM row XOR defaults (`TMEM8` + `TMEM32`) from probe-only to runtime defaults:
  - changed default runtime path to use load-kind-aware row XOR in texture sampling (`tmemLoadKind=tile/block/tlut`).
  - quick smoke improved image error while preserving structure:
    - `rmse=0.367426` (from `0.371448`)
    - `mae=0.274906` (from `0.276416`)
    - `candidate_non_black_ratio=0.775195` (from `0.775134`).
  - 20-frame oracle off lane improved image error (`rmse=0.333528`, `mae=0.221082`) with stable structure (`candidate_non_black_ratio=0.586726`).
  - missing-without-write attribution remains dominant (`0.882528`), so Lane A stays primary.

## Immediate Attack Plan
1. Quantify how much of missing-without-write remains on fallback-selected frames after VI-history carry escalation (`missing-region-focus.off-vs-shadow.json` + frame-forensics join).
2. Add overwrite-path telemetry tying dominant texrect/fill packets to destination surface address and write-mask class on the selected present surface.
3. Implement the next narrow correction in overwrite/combiner behavior for the dominant state signature (`combine=0x00FFFFFFFFFCF279`, `other=0x00000CFF00504340`) without relying on debug toggles.
4. Validate with quick smoke, then deep smoke checkpoint + archive compare.
5. Continue into canonical TMEM lane only after Lane A produces measurable Stage-A movement.
