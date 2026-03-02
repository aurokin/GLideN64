# RVK2 Image Bring-Up Plan

Last updated: 2026-03-02

## Objective

Move RVK2 from unstable noise output to stable N64-representative image output using only hardware-grounded logic.

## Rules

- Accuracy first, performance later.
- RVK2/Vulkan path only.
- Keep this plan concise and operational.
- Update this file every coding cycle with what changed and next actions.

## Current State

- Renderer path is RVK2-only and local gate is green.
- We now get changing image output, but final image is still not game-correct.
- Present-source selection is mostly deterministic in active frames; bootstrap no-surface windows still exist and are expected.
- TMEM-backed sampling is active for major paths, with remaining correctness risk concentrated in 32b addressing semantics and pixel pipeline behavior.
- Most likely remaining fault domain is stage semantics (texel -> combiner -> blender -> coverage/depth -> VI).

## Active Diagnostics

- `REALITYVK_RVK2_DEBUG_STAGE_VIEW`:
  - `final` (default): normal RVK2 present path.
  - `texel_raw`: write texel-stage color after normal geometry/depth/cvg acceptance.
  - `combiner_out`: write combiner result before blender.
  - `blender_out`: write blender result.
  - `vi_source`: bypass VI filtering/scaling and present selected source surface directly.
- Existing forensics capture remains active path for frame-level counters and selection metadata.

## Phases

Total remaining work represented here: 100%.

| ID | Priority | Status | Weight | Outcome |
| --- | --- | --- | --- | --- |
| P1 | Highest | Completed | 10% | Frame forensics and diagnosability in place. |
| P2 | Highest | In progress | 15% | Deterministic present-source policy aligned with VI origin/type. |
| P3 | Highest | In progress | 25% | Authoritative TMEM sampling closure (especially 32b correctness). |
| P4 | High | In progress | 20% | Coefficient/raster behavior closure for triangles and rect paths. |
| P5 | High | In progress | 25% | Combiner/blender/depth/coverage semantics closure for cycle 1/2. |
| P6 | Medium | Planned | 5% | Final VI conformance polish after core pixel correctness. |

## Progress Snapshot

- Overall completion: **~67%**
- Active focus: **P5 blender/coverage closure with class-bucket guidance**
- Blockers: none (debug debt only)

## Latest Pipeline Class Signal (Paper Mario)

Source artifacts:
- `build/parity-runs/paper-mario/paper_mario_intro.<mode>.candidate.{ppm,png}`
- `build/parity-runs/paper-mario/paper_mario_intro.metrics.json`

Using rebuilt `build/release-vulkan-smoke` plugin + `REALITYVK2_FRAME_FORENSICS_FILE`:

- Parity metrics remain:
  - `rmse = 0.017274`
  - `mae = 0.002231`
- Dominant work/write classes:
  - `work_texrect_share = 0.525`
  - `work_triangle_share = 0.475`
  - `work_textured_share = 0.964`
  - `write_texrect_share = 0.559`
  - `write_triangle_share = 0.441`
- Stage delta rates are high (active shading path, not pass-through):
  - `stage_texel_to_combiner_delta_rate = 0.471`
  - `stage_combiner_to_blender_delta_rate = 0.910`
  - `stage_texel_to_final_delta_rate = 0.947`
- Dominant mismatch classes:
  - `b10 (force_blend|coverage)` mostly texrect writes
  - `b11 (cycle2|force_blend|coverage)` mostly triangle writes
- Texel source split:
  - `stage_texel_source_tmem_rate = 0.424`
  - `stage_texel_source_rdram_rate = 0.483`
  - replacement/synthetic writes: `0`

## Latest Forensics Signal (Blender/Coverage Telemetry)

Using `REALITYVK2_FRAME_FORENSICS_FILE` + `scripts/rvk2_forensics_summary.py` on Paper Mario:

- `blend_enabled_rate = 1.0`
- `blend_force_rate = 1.0`
- `blend_coverage_zero_rate ≈ 0.560`
- `blend_coverage_overflow_rate ≈ 0.113`
- Dominant alpha selector distribution:
  - A selector: `s0 ≈ 0.709`, `s3 ≈ 0.291`
  - B selector: `s0 ≈ 0.709`, `s2 ≈ 0.291`

Implication:
- Blender path is always active in this capture, and force-blend + coverage classes dominate final writes.
- Immediate debugging should stay focused on coverage/blender semantics in classes `b10` and `b11`, with TMEM/RDRAM split validation per class.

## Latest Blender Semantics Pass

- Switched forced-blender path toward documented no-divide behavior using 5-bit alpha weights.
- Added divide vs no-divide telemetry:
  - `blend_divide_rate = 0.0`
  - `blend_nodivide_rate = 1.0` (Paper Mario active frames)
- Parity improved after this pass:
  - `rmse: 0.017899 -> 0.017274`
  - `mae: 0.002317 -> 0.002231`
- Quick stage sweep still shows:
  - `final == blender_out`
  - `texel_raw` remains distinct from final

Implication:
- No-divide forced blender remains the active path and should stay the baseline while we refine coverage destination/input semantics.
- Remaining gap appears to be policy-level (coverage/blender inputs), not missing textured work.

## Immediate Work Queue

1. Close class `b10` (`force_blend|coverage`) coverage destination semantics in texrect path.
2. Close class `b11` (`cycle2|force_blend|coverage`) cycle2 coverage handoff semantics for triangles.
3. Reconcile TMEM vs RDRAM source influence by class to prevent policy drift between texrect and triangle paths.
4. Keep TMEM32 experimental modes opt-in until class `b11` parity improves.
5. Re-run full stage sweep (`final/texel_raw/combiner_out/blender_out/vi_source`) after each major coverage policy change.

## Update Log

| Date | Change | Notes |
| --- | --- | --- |
| 2026-03-02 | Added class-bucket pipeline diagnostics and texel-source attribution for active writes. | New forensics fields now report stage deltas by packet class, textured work/write shares, texel source split (`TMEM` vs `RDRAM`) by class, and op-kind work/write distributions. |
| 2026-03-02 | Landed per-surface 3-bit coverage buffer in executor path (conformance-compatible defaults). | Coverage destination no longer derives from destination alpha color directly; local gate remains green and telemetry now reflects coverage policy changes in dominant force-blend classes. |
| 2026-03-02 | Removed semantic gate that forcibly disabled textured draws on zeroed tile descriptor. | Texture intent now follows opcode/packet semantics; telemetry confirms active textured work/write paths in Paper Mario capture. |
| 2026-03-02 | Implemented forced-blender no-divide semantics pass and validated improvement. | Local gate stayed green; Paper Mario parity improved (`rmse 0.017274`, `mae 0.002231`) and telemetry confirms forced no-divide path dominance in active frames. |
| 2026-03-02 | Added `scripts/rvk2_forensics_summary.py` and validated telemetry end-to-end. | Local gate + parity capture confirmed non-zero blender/coverage counters in frame-forensics output; current Paper Mario run shows blender forced on for all blended pixels and high coverage-zero share. |
| 2026-03-02 | Added blender/coverage/depth telemetry counters to executor + frame forensics output. | New per-frame fields include combiner/blender op counts, alpha/coverage reject counts, depth eval/reject/update counts, dither activity, texture-edge/convert-one alpha forces, and blend alpha selector histograms. |
| 2026-03-02 | Completed rebuilt-plugin stage sweep (`final`, `texel_raw`, `combiner_out`, `blender_out`, `vi_source`). | `final==blender_out`, `texel_raw==combiner_out`, and `vi_source` was unique; this narrows immediate work to blender/coverage semantics. |
| 2026-03-02 | Plan compacted for operational use. | Removed stale narrative sections; kept active signals and execution queue. |
| 2026-03-02 | Added stage-tap diagnostics in executor pipeline. | `REALITYVK_RVK2_DEBUG_STAGE_VIEW` now supports `final`, `texel_raw`, `combiner_out`, `blender_out`, and `vi_source`. |
| 2026-03-02 | Added VI-source direct-present debug path. | `vi_source` mode bypasses VI processing and presents selected source surface pixels directly for isolation. |
| 2026-03-02 | Validation: `./scripts/local_gate.sh` passed. | Unit + conformance + plugin builds all green after stage-tap changes. |
