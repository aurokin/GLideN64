# ADR 0002: RealityVK2 Runtime Cutover Policy

- Status: Accepted
- Date: 2026-03-02

## Context

RealityVK2 migration is now at the hard-cutover stage.
Runtime execution through alternate paths is no longer desired.

## Decision

1. Runtime path is fixed to `rvk2` only; runtime switch is removed.
2. Context creation always instantiates `rvk2::ContextImpl`.
3. Previous draw pass-through in `rvk2::ContextImpl` is removed.
4. If no rvk2 present frame is produced, presentation clears to deterministic black instead of using previous draw output.
5. Trace output controls remain available:
   - `REALITYVK2_TRACE_FILE`
   - `REALITYVK2_PACKET_TRACE_FILE`
6. Local gate smoke mode emits and replay-validates rvk2 packet traces by default.
7. Visual parity metrics remain non-final until post-cutover parity closure.

## Consequences

1. Only one runtime execution path remains, reducing ambiguity and masking.
2. Regressions in rvk2 are immediately visible instead of being hidden by alternate rendering behavior.
3. Capture/replay tooling remains available during cutover hardening and parity closure.
