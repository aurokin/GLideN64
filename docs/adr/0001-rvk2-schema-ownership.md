# ADR 0001: RealityVK2 Schema Ownership and Version Policy

- Status: Accepted
- Date: 2026-03-02

## Context

RealityVK2 now emits deterministic trace records and packet dumps that are consumed by tooling and local gate checks.
Without an explicit schema contract, packet/state format drift can silently break validation tooling and make historical artifacts unusable.

## Decision

1. The canonical schema tag is `rvk2_schema_v1`.
2. Schema constants live in:
   - [rvk2_Types.h](/home/auro/code/gliden64/src/Graphics/RealityVK2/rvk2_Types.h)
   - `rvk2::kSchemaVersion`
   - `rvk2::kSchemaName`
3. Breaking schema changes are allowed before rvk2 default cutover.
4. Any schema-affecting change must update:
   - schema constants, if version/tag changes
   - replay tooling docs and status docs
   - local gate expectations if needed
5. Until cutover, schema ownership is held by the solo maintainer.

## Consequences

1. Tooling can assert schema identity instead of relying on implicit format assumptions.
2. Intentional breaking changes remain possible during Phase A/B without pretending backward compatibility.
3. Schema drift becomes explicit and reviewable.
