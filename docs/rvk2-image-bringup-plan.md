# RVK2 Image Bring-Up Plan

Last updated: 2026-03-02

## Goal

Bring RVK2 from black/noise output to stable, game-representative image output by replacing remaining synthetic rendering behavior with N64/RDP/VI-grounded logic.

## Scope Rules

- Accuracy first, then performance.
- Vulkan + RVK2 path only.
- Plan is allowed to evolve as new evidence is collected.
- Every coding cycle updates this document with:
  - what changed,
  - what was learned,
  - what changed in priorities.

## Current Leads

1. Executor still uses synthetic/pseudo shading and texture paths for output.
2. TMEM model tracks metadata but does not yet serve real texel data for rendering.
3. Present-surface selection can still pick non-authoritative render targets in some frame windows.
4. Depth participation is weak in captured scenes (very low/no active depth-tested paths observed).
5. VI path can still reject/zero frames when resolved state is invalid or mismatched.

## Phases

Total remaining work represented here: 100%.

| ID | Priority | Status | Weight | Outcome |
| --- | --- | --- | --- | --- |
| P1 | Highest | Planned | 10% | Add frame-forensics layer so each bad frame is diagnosable. |
| P2 | Highest | Planned | 15% | Make present target selection deterministic and VI-origin authoritative. |
| P3 | Highest | Planned | 25% | Replace pseudo texture sampling with real TMEM-backed sampling. |
| P4 | High | Planned | 20% | Replace pseudo triangle color path with coefficient-driven raster evaluation. |
| P5 | High | Planned | 25% | Implement real combiner/blender/depth/coverage semantics for cycle 1/2. |
| P6 | Medium | Planned | 5% | Final VI framing and output conformance polish after core content path is real. |

## Immediate Execution Order

1. P1: instrument frame forensics first.
2. P2: lock present-surface selection and VI-origin mapping.
3. P3 + P4: build real content generation path.
4. P5: close correctness for blend/depth/coverage behavior.
5. P6: final VI/output polish.

## Live Progress

- Overall completion of this bring-up plan: **0%**
- Active phase: **P1**
- Blockers: none (technical debt only)

## Update Log

| Date | Change | Notes |
| --- | --- | --- |
| 2026-03-02 | Initial plan created. | Derived from current RVK2 smoke/parity behavior and trace analysis. |

