# RealityVK2 Program Status (2026-03-02)

This is the single execution tracker for the rewrite.

## Snapshot

1. `rvk2` runtime path and scaffolding are integrated in build + local gate.
2. Command ingest capture is live for LLE, HLE display-list loops, and Turbo3D/T3DUX bypass paths.
3. Schema is frozen as `rvk2_schema_v1` (breaking still allowed before cutover).
4. Deterministic packet trace + replay are live and gate-integrated.
5. Replay now supports multicore execution (`--jobs`, default all cores).
6. Replay check is currently clean on maintained smoke trace: `frames=124 failed=0 warned=0`.
7. Draw semantic -> raster op -> render work -> submission batch pipeline is implemented and traced.
8. Software executor is implemented for fill/texrect/copy and triangle coverage-mask behavior.
9. HLE synthetic RDP submission is live for `gDP`, `gSPTriangle`, DMA triangles, and screen-space triangle paths.
10. Visual parity is still not at target threshold (`rmse=0.487984` on current Paper Mario compare).

## Phase Status

| Phase | Status | Notes |
| --- | --- | --- |
| A: Contracts and Determinism | Done | Schema, trace, replay, gate wiring, ADR baseline are in place. |
| B: Semantic Pipeline Backbone | In progress | Semantic/raster/render/submission/executor pipeline exists, but semantic coverage is incomplete. |
| C: Core Rendering Correctness | In progress | Fill/texrect/triangle backbone exists in executor path; full combiner/blender/depth/hazard correctness not closed. |
| D: VI and Presentation | In progress | Basic deterministic VI presenter is wired; full VI-accurate behavior not implemented. |
| E: Texture Replacement | Not started | Hi-res pack + `.htc` rewrite path not implemented yet. |
| F: Cutover and Deletion | Not started | `rvk2` is not default and legacy-derived paths still exist. |

## Completed Recently

1. Centralized synthetic triangle packet packing in `rvk2_SyntheticTriangle.h`.
2. Unified triangle capture across `gSPTriangle`, `drawDMATriangles`, and `drawScreenSpaceTriangle`.
3. Added/updated unit coverage for synthetic triangle packet packing and semantic decode.
4. Added replay multicore support and gate knob:
   - CLI: `--jobs`
   - gate env: `REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS`
5. Reduced replay CPU overhead in rect/triangle pixel loops.

## Current Bottlenecks

1. Semantic completeness gap in combiner/blender/depth interactions.
2. No dedicated synthetic conformance suite yet (beyond current unit and smoke layers).
3. Visual parity threshold still fails on maintained Paper Mario metric.
4. Texture replacement stack (`hi-res` + `.htc`) is not started.

## Next Coding Priorities

1. Expand semantic decode coverage for remaining high-impact RDP opcodes.
2. Build targeted synthetic conformance scenes for blend/depth/coverage edge cases.
3. Close executor semantic fidelity gaps for cycle behavior and hazard ordering.
4. Tighten replay performance further only if gate latency regresses materially.
5. Start Phase E interface contracts for texture replacement keying and `.htc` flow.

## Local Gate Contract

Default gate:

```bash
./scripts/local_gate.sh
```

Smoke + replay gate:

```bash
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
```

Control replay workers:

```bash
REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS=0 ./scripts/local_gate.sh
```

`0` means auto/all cores; use `1` to force single-process replay.
