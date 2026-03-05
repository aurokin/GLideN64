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
- Present-selection policy checkpoint landed:
  - when VI origin maps only to history but a compatible live surface has writes, executor now prefers live by default.
  - temporary probe override remains available: `REALITYVK_RVK2_DEBUG_KEEP_VI_MATCHED_HISTORY_SELECTION=1`.
- Structural movement after the default policy flip:
  - quick smoke moved `candidate_non_black_ratio` from `0.766435` to `0.775134`.
  - deep compare (`20260305-065447Z -> 20260305-100328Z`) moved `candidate_non_black_ratio` from `58.67%` to `77.51%` and reduced suspected gaps (`14 -> 13`).
  - image-error metrics regressed (`rmse`/`mae`), so Stage-A structure remains the primary gate.
- TMEM load-kind row-XOR status:
  - load-kind-aware TMEM8/TMEM32 XOR paths are now default runtime behavior and remain overrideable via debug envs.
  - latest 20-frame checks improved image error with structure stable:
    - quick: `rmse=0.367426`, `mae=0.274906`, `candidate_non_black_ratio=0.775195`
    - oracle off lane: `rmse=0.333528`, `mae=0.221082`, `candidate_non_black_ratio=0.586726`.
- Focused deep overwrite telemetry lead (run `20260305-125600Z`):
  - dominant texrect-copy packet cluster (`combine=0x00FFFFFFFFFCF87C`, `other=0x00208C7F00000000`) shows repeated TMEM-vs-RDRAM probe divergence in CI8+TLUT sampling rows (`tex0_tmem_fetch_variant=2`, `tex0_tmem_load_kind=tile`).
  - dominant missing-with-write texrect cluster (`combine=0x00FFFFFFFFFCF279`, `other=0x00000CFF00504340`) still diverges under TMEM fetch variant `4`.
- Focus-cluster telemetry expansion landed:
  - overwrite TSV rows now include cycle/alpha/cvg/blend/depth/color/tile/texture state fields for each logged write.
  - telemetry bundle now emits `overwrite_summary.focus_clusters.{fill,texrect}.{dominant_state,top_states,top_source_packets}`.
  - latest full deep shows fill-cluster dominant signature explicitly (`combine=0x00FFFFFFFFFCF87C`, `other_modes=0x00308C7F00000000`, `cycle_type=3`, `fill_color=0x00010001`).
- Shadow-oracle evidence (same-run, 20-frame deep, replay disabled):
  - executor-vs-shadow `best_mae` improved `0.238958 -> 0.228807`.
  - executor-vs-shadow `best_rmse` improved `0.366680 -> 0.360818`.
- Present-selection live-proxy follow-up landed:
  - when VI matches history-only and a compatible live surface is present, executor now prefers the most-written compatible live surface.
  - latest oracle moved slightly (`candidate_non_black_ratio` `0.584560 -> 0.586726`) but dominant missing-without-write gap persisted.
- VI-history carry escalation checkpoint landed (fallback-only, executor default path):
  - when strict VI-history carry finds large non-black diff potential but copies `0`, fallback-selected frames now retry with visible-diff carry under a tight age/diff gate.
  - oracle frame-forensics (`20260305-124849Z`) confirms carry activation on fallback frames `18/20/22/24/26` (`copied=12572/14551/16159/17767/19372`).
  - quick smoke remained stable (`candidate_non_black_ratio=0.775134`); 20-frame oracle off lane stayed in the prior movement band (`0.586726`) and missing-without-write remains dominant (`0.882528`).
- Remaining high-value leads are now upstream of final present handoff (overwrite cluster + raster/source divergence).
- Deterministic shadow-oracle attribution (`frames=10` or `20`, retry `0`) now has a one-command workflow:
  - `./scripts/paper_mario_shadow_oracle.sh --frames 20 --retry-count 0`
  - Latest oracle snapshot (`20260305-124849Z`, `frames=20`): shadow-on `candidate_non_black_ratio=0.945481` vs executor-off `0.586726`.
  - Oracle missing attribution remains dominated by missing-without-write (`0.882528`) with prior-write coverage (`1.0`).
- Shadow draw forwarding side-effect check:
  - `REALITYVK_RVK2_SHADOW_DRAW=1` with `REALITYVK_RVK2_SHADOW_PRESENT=0` is byte-identical to shadow-off executor output.
  - no-op forwarding is not the current structural blocker.
- Conformance stability correction:
  - restored history-carry bootstrap/merge to debug-gated defaults after narrowing the live-proxy change.
  - `local_gate.sh` is green again (rvk2 unit + conformance PASS).
- TMEM capture alignment checkpoint landed:
  - Runtime now exposes explicit post-write TMEM snapshot capture, wired after executed `LoadTile/LoadBlock/LoadTLUT` in both `RDP` list and synthetic `gDP` load paths.
  - Unit coverage added for render-work snapshot index linkage (`testRuntimeTMEMWriteSnapshotCapture`).
  - Latest 20-frame oracle remains unchanged on structural gap metrics, so snapshot timing is not yet the primary blocker.
- Focus-cluster telemetry coverage fix landed:
  - frame-forensics summaries now fall back to overwrite-derived focus metrics when in-core frame counters report zero.
  - deep parity forensics summary generation now passes overwrite log input automatically.
  - telemetry bundle focus-cluster source is explicit (`forensics` vs `overwrite_log`) so analysis remains attributable.
- Deep telemetry bundle stability fix landed:
  - overwrite/triangle parsing now streams line-by-line in `rvk2_telemetry_bundle.py` (no full-file `read_text().splitlines()` allocation).
  - validated against the prior 12GB overwrite-log failure case (`shadow-oracle/20260305-071322Z/off`): bundle regeneration now completes.
- Deep forensics-summary stability/perf fix landed:
  - overwrite fallback parser now streams line-by-line and fast-filters for `focus_cluster=1/2`.
  - overwrite fallback parse is skipped unless forensics focus counters are zero.
  - 12GB overwrite fallback timing improved from ~2m16s to ~1m18s with low RSS.
- Shadow-off vs shadow-on command ingress parity confirmed:
  - packet traces are byte-identical (same SHA256 + line count) in oracle run `20260305-070621Z`.
  - divergence is downstream in executor generation/present behavior, not command ingestion.
- Missing-region prior-hit attribution is now split by present-surface address:
  - `rvk2_missing_region_focus.py` emits prior packet-hit lists on-present vs off-present.
  - this removes dominant non-present full-screen fill packets from masking present-surface texrect history clusters.
- Same-address bootstrap promotion probe result:
  - default-on probe (20-frame quick A/B) showed no structural movement; reverted to default-off to avoid noise.
- Non-black-aware present fallback probe result:
  - 20-frame deep probe showed no structural movement; reverted to keep runtime selection behavior stable.

## Current Baseline (2026-03-05)
- Latest non-shadow deep archive run: `paper_mario_intro.20260305-102706Z.b7c46e23`
- Comparison baseline for this checkpoint: `paper_mario_intro.20260305-100328Z.cdf22eca`
- Archive index: `build/parity-runs/paper-mario/archive/index.tsv`
- Latest key metrics:
  - `rmse=0.371346`
  - `mae=0.276243`
  - `candidate_non_black_ratio=0.775134`
  - `candidate_mean_luma=0.297256`
- Delta vs baseline (`100328 -> 102706`):
  - `rmse`: `+0.000000`
  - `mae`: `+0.000000`
  - `candidate_non_black_ratio`: `+0.00pp`
  - suspected gaps: unchanged (`13`)

## Tooling Constraints
- Upstream `GLideN64` `dumpfb-preset` capture remains black in agent-mode flow; treat reference non-black ratio as non-actionable.
- Use stateful replay for deterministic class attribution; non-stateful replay is triage-only.
- For executor-vs-shadow attribution, prioritize same-run comparison (`shadow-present candidate` vs `executor-present dump`) to avoid run-to-run drift.
