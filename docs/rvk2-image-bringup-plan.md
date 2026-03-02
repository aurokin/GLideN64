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

- Overall completion: **~55%**
- Active focus: **P4/P5 with stage-tap-guided isolation**
- Blockers: none (debug debt only)

## Immediate Work Queue

1. Run short parity/smoke captures per stage-view mode (`final`, `texel_raw`, `combiner_out`, `blender_out`, `vi_source`) and compare behavior transitions.
2. Use `vi_source` to answer one hard question first: whether selected source surfaces already contain coherent scene structure.
3. If `vi_source` is coherent: prioritize P6/VI mapping and filtering correctness.
4. If `vi_source` is not coherent: prioritize P4/P5 raster-stage corrections before more VI work.
5. Keep TMEM32 experimental work behind explicit opt-in until stage outputs are stable and interpretable.

## Update Log

| Date | Change | Notes |
| --- | --- | --- |
| 2026-03-02 | Plan compacted for operational use. | Removed stale narrative sections; kept active signals and execution queue. |
| 2026-03-02 | Added stage-tap diagnostics in executor pipeline. | `REALITYVK_RVK2_DEBUG_STAGE_VIEW` now supports `final`, `texel_raw`, `combiner_out`, `blender_out`, and `vi_source`. |
| 2026-03-02 | Added VI-source direct-present debug path. | `vi_source` mode bypasses VI processing and presents selected source surface pixels directly for isolation. |
| 2026-03-02 | Validation: `./scripts/local_gate.sh` passed. | Unit + conformance + plugin builds all green after stage-tap changes. |
