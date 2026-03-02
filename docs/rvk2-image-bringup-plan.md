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
| P1 | Highest | In progress | 10% | Add frame-forensics layer so each bad frame is diagnosable. |
| P2 | Highest | In progress | 15% | Make present target selection deterministic and VI-origin authoritative. |
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

- Overall completion of this bring-up plan: **~12%**
- Active phase: **P2**
- Blockers: none (technical debt only)

## Latest Findings (Forensics Run)

Source: `REALITYVK2_FRAME_FORENSICS_FILE` during Paper Mario parity capture.

- Captured records: `155` frames.
- Present-selection modes observed:
  - `kExecutorPresentSelectionNoSurface (5)`: `44` frames.
  - `kExecutorPresentSelectionLastSurface (1)`: `38` frames.
  - `kExecutorPresentSelectionVIOriginRange (3)`: `73` frames.
- VI rejection observed:
  - `kVIRejectMissingSource (1)`: `44` frames.
  - `kVIRejectNone (0)`: `111` frames.
- Zero-size present frames: `44` (all correspond to no-surface/missing-source windows).
- Late stable frame example:
  - frame `126`: selected surface `0x005CE430`, writes `939279`, output `640x480`.

Implication:
- We now have precise visibility into when black output is “no render surface yet” vs “rendered but wrong content.”
- P2 should focus on tightening present target policy around VI-origin + surface activity epoch.

## Latest Findings (P2 Selection Pass)

After adding previous-surface support and bounded surface-history selection:

- Present selection mix shifted from mostly `last-surface` fallback to deterministic fallback classes:
  - `kExecutorPresentSelectionVIOriginRange (3)`: `73`
  - `kExecutorPresentSelectionMostWrittenFallback (4)`: `37`
  - `kExecutorPresentSelectionLastSurface (1)`: `1`
  - `kExecutorPresentSelectionNoSurface (5)`: `44`
- Zero-present frames remained `44`, but all are early bootstrap windows before any render surface exists.

Implication:
- Runtime now makes less arbitrary present-target choices once surfaces exist.
- Remaining visual corruption is increasingly likely inside content generation (TMEM/pixel path), not target selection.

## Update Log

| Date | Change | Notes |
| --- | --- | --- |
| 2026-03-02 | Initial plan created. | Derived from current RVK2 smoke/parity behavior and trace analysis. |
| 2026-03-02 | Added frame forensics instrumentation (P1, in progress). | Executor summary now records present-surface selection, per-surface write/work rankings, and VI rejection/resolved-state metadata; context can emit per-frame forensic records via `REALITYVK2_FRAME_FORENSICS_FILE`. |
| 2026-03-02 | Added deterministic present fallback improvements (P2, in progress). | Executor now supports no-work previous-surface fallback, bounded surface-history cache, and VI-origin selection against history before non-authoritative fallback. |
