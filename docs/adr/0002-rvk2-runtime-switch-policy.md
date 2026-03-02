# ADR 0002: RealityVK2 Runtime Switch Policy

- Status: Accepted
- Date: 2026-03-02

## Context

RealityVK2 is being built in parallel with the current Vulkan path.
We need a predictable policy for selecting runtime behavior during development without destabilizing default execution.

## Decision

1. Runtime selection remains explicit and opt-in:
   - `REALITYVK_RENDER_PATH=rvk2`
   - `REALITYVK2_RENDER_PATH=rvk2`
2. If rvk2 execution path is requested but not fully implemented, runtime must warn and safely fall back to current Vulkan path.
3. Trace capture controls stay usable independent of default render path:
   - `REALITYVK2_CAPTURE_RDP_TRACE`
   - `REALITYVK2_TRACE_FILE`
   - `REALITYVK2_PACKET_TRACE_FILE`
4. Local gate smoke mode emits and replay-validates rvk2 packet traces by default.
5. Visual parity metrics are treated as signal during rebuild and may be non-blocking while core semantics are still under active replacement.

## Consequences

1. Development can move quickly without falsely presenting rvk2 as production-ready.
2. Capture/replay tooling remains available in both fallback and migration phases.
3. Runtime-path behavior remains explicit and testable.
