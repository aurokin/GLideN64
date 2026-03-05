# RVK2 Core Fix Execution Plan

Date started: 2026-03-04  
Owner: Codex (active execution)

## Objective
Fix missing textures and missing geometry in `paper_mario_intro` on Vulkan `rvk2`, prioritizing structural recovery (content appears) before fine visual parity.

## Acceptance Bar
- [ ] `candidate_non_black_ratio >= 0.90`
- [ ] `missing_without_write_ratio <= 0.35`
- [ ] `rmse <= 0.25`
- [ ] `mae <= 0.20`

## Lane Order (Locked)

### Lane 0: Present / VI-origin coherence
- [x] Reproduce executor-vs-shadow divergence in same-run mode.
- [x] Prove whether VI-matched history selection affects executor output.
- [x] Promote VI-matched history preservation into default present selection logic (no debug toggle required).
- [x] Validate with short deep shadow-oracle A/B (`20` frames, replay disabled).
- [x] Validate with full deep smoke archive checkpoint (no shadow).
- [ ] Follow-up: reduce present-size/present-hash replay mismatches once raster parity improves.

### Lane 1: LoadBlock TMEM addressing (`dxt` + odd/even interleave)
- [x] Audit current `LoadBlock` write-address logic against hardware-like mapping expectations.
- [x] Promote load-kind-aware TMEM row-XOR addressing into default executor path (no debug toggle required).
- [ ] Implement full canonical address mapping path (single source of truth).
- [x] Add telemetry fields for per-load mapping decisions (bounded/rate-limited).
- [ ] Validate with 10-frame shadow A/B.
- [ ] Validate with 20-frame shadow A/B.
- [x] Run deep checkpoint and archive comparison.

### Lane 2: LoadTLUT + CI path
- [ ] Audit TLUT load path for swap/alignment behavior.
- [ ] Implement/fix TLUT upload mapping + CI fetch assumptions.
- [ ] Add TLUT CRC + palette-mode telemetry.
- [ ] Validate with 10-frame and 20-frame shadow A/B.
- [ ] Run deep checkpoint and archive comparison.

### Lane 3: Tile/load mutation semantics
- [ ] Audit `SetTile`, `SetTileSize`, `LoadTile`, `LoadBlock`, `LoadTLUT` mutation and restore behavior.
- [ ] Correct unit semantics (`line` words vs bytes, load-time tile edits).
- [ ] Add targeted assertions/probes for tile state transitions.
- [ ] Validate with 10-frame and 20-frame shadow A/B.
- [ ] Run deep checkpoint and archive comparison.

## Execution Rules
- Use shadow-on only as structural oracle for inner-loop convergence.
- Do not land permanent fixes that require debug toggles.
- Use quick smoke for local iterations; deep smoke for lane checkpoints.
- Lane advancement requires structural metric improvement (`candidate_non_black_ratio`, `missing_without_write_ratio`).

## Progress Log

### 2026-03-04
- [x] Created execution plan and locked lane order.
- [x] Started lane 1 implementation.
- [x] Captured lane-1 deep baseline (`frames=10`) and archived telemetry bundle.
- [x] Added TMEM sample telemetry fields for `LoadBlock` context (`tile/uls/ult/lrs/lrt/dxt/span/qwords/estimated_words_per_line`).
- [x] Added TMEM sample TLUT telemetry (`tlut_applied`, lookup address, raw/decoded TLUT entries).
- [ ] Apply first canonical `LoadBlock` addressing behavior change.
- [x] Identified dominant CI+LUT failure: TMEM samples collapse to index `0xFF` while RDRAM probe remains non-black in the same writes.
- [x] Added temporary CI+LUT RDRAM-primary heuristic in executor (bring-up path; documented for later rollback/refinement).
- [x] Ran deep checkpoint after heuristic; metrics moved in the expected structural direction (`candidate_non_black_ratio +2.38pp` vs prior deep run).
- [x] Added same-run shadow-oracle attribution workflow (`shadow-present candidate` vs `executor-present dump`).
- [x] Identified present-selection override as a regression source: VI-matched history surfaces were being replaced by most-written live fallback.
- [x] Promoted VI-matched history preservation into default path (no permanent debug toggle needed).
- [x] Verified with short deep shadow-oracle A/B (`20` frames): executor-vs-shadow compare improved (`best_mae 0.238958 -> 0.228807`).
- [x] Ran full deep checkpoint (`paper_mario_intro.20260304-235955Z.63c167f0`) and confirmed suspected gap removal: `VI origin did not match selected present surface`.
- [ ] Continue next lane focus on dominant overwrite cluster (`op=fill combine=0x00FFFFFFFFFCF87C other_modes=0x00308C7F00000000`) and zero-shade triangle path.

### 2026-03-05
- [x] Ran quick-smoke TMEM XOR matrix; best default behavior matched combined load-kind-aware TMEM8+TMEM32 row-XOR probes.
- [x] Promoted load-kind-aware TMEM8/TMEM32 row-XOR behavior into default executor path (removed dependency on debug toggles for this fix).
- [x] Rebuilt and revalidated quick smoke on default path (`rmse=0.346580`, `mae=0.249116`, `candidate_non_black_ratio=0.766512`).
- [x] Ran deep checkpoint (`paper_mario_intro.20260305-003843Z.57f1559c`) and archive compare vs `20260304-235955Z`.
- [ ] Continue next lane focus on dominant fill overwrite cluster and packet-level raster/source divergence.

## Current Lane-1 Evidence Snapshot
- Deep run: `build/parity-runs/paper-mario/archive/paper_mario_intro.20260304-222205Z.21ceca95`
- Baseline metrics:
  - `candidate_non_black_ratio=0.675517`
  - `rmse=0.360563`
  - `mae=0.256704`
- Strongest bundle leads:
  - focus frame `9` has texrect traffic without triangles (UI-only symptom in focus frame)
  - replay state divergence starts early (`frame=9`)
  - present-size / selected-surface mismatches are recurrent in stateful replay
- CI+LUT telemetry finding (detailed overwrite probe):
  - `tex0_format=2,size=1,lut_mode=2` dominates textured black writes.
  - TMEM texel byte resolves to `0xFF` across sampled addresses, forcing TLUT index `255`.
  - TLUT lookup resolved to address `0x7FC` (`raw=0x0100`, decoded=`0x0001`) across those rows.
  - RDRAM probe for the same samples produced non-black values, indicating TMEM-side divergence rather than LUT decode collapse alone.
- Latest lane-1 checkpoint (row-XOR default promotion):
  - deep run: `build/parity-runs/paper-mario/archive/paper_mario_intro.20260305-003843Z.57f1559c`
  - archive compare vs `paper_mario_intro.20260304-235955Z.63c167f0`:
    - `rmse`: `0.350840 -> 0.346580` (`-0.004260`)
    - `mae`: `0.250593 -> 0.249116` (`-0.001477`)
    - `candidate_non_black_ratio`: `76.64% -> 76.65%` (`+0.01pp`)
    - suspected gaps: unchanged count (`12`)

## Checkpoint Log

| Time (local) | Lane | Change | Validation | Result |
|---|---|---|---|---|
| 2026-03-04 | Setup | Created plan doc | N/A | Active |
| 2026-03-04 22:20Z | Lane 1 | Deep baseline (`paper_mario_focus_deep --frames 10`) | Archive + bundle | Captured |
| 2026-03-04 22:27Z | Lane 1 | Added per-sample `LoadBlock` context telemetry in executor overwrite logs | `local_gate` + smoke | Completed |
| 2026-03-04 22:41Z | Lane 2 | Added TLUT lookup telemetry and traced CI+LUT black-write cluster | Detailed overwrite probe | Confirmed |
| 2026-03-04 22:52Z | Lane 2 | Applied temporary CI+LUT RDRAM-primary heuristic | Deep checkpoint + archive compare | Structural improvement; keep iterating |
| 2026-03-04 23:03Z | Lane 2 | Checkpoint commit/push (`70895391`) | pre-push gate | Pushed |
| 2026-03-04 23:40Z | Lane 0 | Shadow-oracle same-run attribution showed present-selection override divergence | short deep A/B (`20` frames) | Confirmed |
| 2026-03-04 23:50Z | Lane 0 | Preserve VI-matched history selection by default in executor present selection | build + short deep A/B | Promoted |
| 2026-03-04 23:59Z | Lane 0 | Full deep checkpoint (`paper_mario_intro.20260304-235955Z.63c167f0`) | archive compare vs `20260304-230122Z` | `VI origin mismatch` suspected-gap removed |
| 2026-03-05 00:38Z | Lane 1 | Promote TMEM8/TMEM32 load-kind row-XOR mapping into default executor path | quick-smoke matrix + deep checkpoint (`paper_mario_intro.20260305-003843Z.57f1559c`) | RMSE/MAE improved; suspected-gap set unchanged |
