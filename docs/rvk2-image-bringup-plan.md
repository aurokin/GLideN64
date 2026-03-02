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

- Overall completion: **~57%**
- Active focus: **P4/P5 with stage-tap-guided isolation**
- Blockers: none (debug debt only)

## Latest Stage Sweep (Paper Mario)

Source artifacts:
- `build/parity-runs/paper-mario/paper_mario_intro.<mode>.candidate.{ppm,png}`
- `build/parity-runs/paper-mario/paper_mario_intro.<mode>.metrics.json`

Observed mode groups (rebuilt `build/release-vulkan-smoke` plugin):
- Group A (identical outputs): `final` + `blender_out`
- Group B (identical outputs): `texel_raw` + `combiner_out`
- Group C (unique output): `vi_source`

Key implication:
- For this scene/frame slice, combiner is currently behaving as pass-through from texel stage, while blender/late pixel controls are where output diverges from texel.
- `vi_source` divergence confirms VI processing is materially changing the frame; it remains useful as a source-surface isolation view, not as a parity target by itself.

## Immediate Work Queue

1. Add focused forensics counters around blender/coverage decisions (alpha A/B selectors, coverage resolve, dither mode, force_blender/AA interactions) for active draw packets.
2. Correlate `final` vs `texel_raw` divergence with packet classes to identify the first high-impact blender/coverage mismatch.
3. Use `vi_source` captures to validate source-surface coherence per frame window before spending more cycles on VI polish.
4. Keep TMEM32 experimental work behind explicit opt-in until blender/coverage behavior is better constrained.
5. After blender closure, re-run stage sweep and check whether `combiner_out` still collapses onto `texel_raw`.

## Update Log

| Date | Change | Notes |
| --- | --- | --- |
| 2026-03-02 | Completed rebuilt-plugin stage sweep (`final`, `texel_raw`, `combiner_out`, `blender_out`, `vi_source`). | `final==blender_out`, `texel_raw==combiner_out`, and `vi_source` was unique; this narrows immediate work to blender/coverage semantics. |
| 2026-03-02 | Plan compacted for operational use. | Removed stale narrative sections; kept active signals and execution queue. |
| 2026-03-02 | Added stage-tap diagnostics in executor pipeline. | `REALITYVK_RVK2_DEBUG_STAGE_VIEW` now supports `final`, `texel_raw`, `combiner_out`, `blender_out`, and `vi_source`. |
| 2026-03-02 | Added VI-source direct-present debug path. | `vi_source` mode bypasses VI processing and presents selected source surface pixels directly for isolation. |
| 2026-03-02 | Validation: `./scripts/local_gate.sh` passed. | Unit + conformance + plugin builds all green after stage-tap changes. |
