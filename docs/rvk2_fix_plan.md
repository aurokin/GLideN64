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

### Lane 1: LoadBlock TMEM addressing (`dxt` + odd/even interleave)
- [x] Audit current `LoadBlock` write-address logic against hardware-like mapping expectations.
- [ ] Implement canonical address mapping path (single source of truth).
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

## Checkpoint Log

| Time (local) | Lane | Change | Validation | Result |
|---|---|---|---|---|
| 2026-03-04 | Setup | Created plan doc | N/A | Active |
| 2026-03-04 22:20Z | Lane 1 | Deep baseline (`paper_mario_focus_deep --frames 10`) | Archive + bundle | Captured |
| 2026-03-04 22:27Z | Lane 1 | Added per-sample `LoadBlock` context telemetry in executor overwrite logs | `local_gate` + smoke | Completed |
| 2026-03-04 22:41Z | Lane 2 | Added TLUT lookup telemetry and traced CI+LUT black-write cluster | Detailed overwrite probe | Confirmed |
| 2026-03-04 22:52Z | Lane 2 | Applied temporary CI+LUT RDRAM-primary heuristic | Deep checkpoint + archive compare | Structural improvement; keep iterating |
| 2026-03-04 23:03Z | Lane 2 | Checkpoint commit/push (`70895391`) | pre-push gate | Pushed |
