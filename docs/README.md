# Maintainer Documentation

## Canonical Docs

- `../README.md`: project overview and maintainer entry points.
- `../AGENTS.md`: required agent operating rules and smoke/deep policies.
- `local-ci.md`: build/gate/smoke/replay commands.
- `workflow.md`: objective, acceptance bar, and execution loop.
- `status.md`: prioritized fix lanes and current baseline snapshot.

## Reference Corpus

- `references/n64/README.md`: local N64 hardware/reference corpus.

## RVK2 Operating Policy

- Runtime render path is `rvk2` only.
- Runtime fallback renderer paths are not supported.
- Packet/trace schema contract is `rvk2_schema_v1` (`src/Graphics/RealityVK2/rvk2_Types.h`).
- During active bring-up, schema-breaking changes are allowed when tooling and docs are updated in the same change.

## Documentation Hygiene

- Keep docs short and maintainer-focused.
- Delete superseded docs instead of archiving duplicates.
- Keep logs and generated artifacts in `build/`, not `docs/`.
