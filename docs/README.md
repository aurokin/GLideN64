# Documentation Map

This folder contains working docs for RealityVK Vulkan migration and parity work.

## Start Here

1. [Workflow](/home/auro/code/gliden64/WORKFLOW.md)
2. [Parity Flow vs Upstream](/home/auro/code/gliden64/docs/realityvk-vs-upstream-flow.md)
3. [Local Smoke Guide](/home/auro/code/gliden64/docs/local-smoke.md)

## Active Operations

- [Local Smoke Guide](/home/auro/code/gliden64/docs/local-smoke.md)
  - Commands, env vars, capture behavior, and parity outputs.
- [Local CI Guide](/home/auro/code/gliden64/docs/local-ci.md)
  - Local gate/CI workflow.
- [Upstream Flow and Roadmap](/home/auro/code/gliden64/docs/realityvk-vs-upstream-flow.md)
  - Current metrics, implemented deltas, in-progress items, deferred batch.
- [Vulkan Migration Plan](/home/auro/code/gliden64/docs/vulkan-migration-plan.md)
  - Broader migration phases.
- [Vulkan Backend Intent Map](/home/auro/code/gliden64/docs/vulkan-backend-intent.md)
  - Module boundaries and agent-safe edit points tied to N64 semantics.

## Runtime Validation and Test Planning

- [Runtime Validation Checklist](/home/auro/code/gliden64/docs/n64-runtime-validation-checklist.md)
- [Video Core Bug Matrix](/home/auro/code/gliden64/docs/n64-video-core-bug-matrix.md)
- [Runtime Validation Snapshot (2026-03-01)](/home/auro/code/gliden64/docs/archive/runtime-validation-2026-03-01.md)

## References

- [N64 References Index](/home/auro/code/gliden64/docs/references/n64/README.md)
  - Programming manuals and architecture references used for parity analysis.

## Archive

- [runtime-validation-2026-03-01.md](/home/auro/code/gliden64/docs/archive/runtime-validation-2026-03-01.md)

## Notes

- Generated artifacts are disposable and live under `build/`.
- Current practice is to keep only latest parity artifacts and cache references.
