# RealityVK2 Rebuild Plan

## Objective

Build a new Vulkan-first N64 video core (`rvk2`) that is more accurate, deterministic, and easier to validate than the GLideN64-derived path.

This is a reimplementation, not an incremental port.

## Product Scope (Kept)

1. 4:3 and 16:9 output scaling.
2. Hi-res texture pack support.
3. `.hts` texture cache support.

Everything else is optional and must justify itself against correctness.

## Hard Rules

1. N64 semantics first, backend convenience second.
2. Determinism is required for every major pipeline stage.
3. Vulkan executes semantic packets; Vulkan does not define semantics.
4. No game-specific workaround without a linked reproducible test.
5. New behavior must be traceable from command to output.

## Target Architecture

1. `rvk2::CommandStream`
2. `rvk2::RSPFrontend`
3. `rvk2::RDPState`
4. `rvk2::TMEMModel`
5. `rvk2::DrawSemantic`
6. `rvk2::RasterPipeline`
7. `rvk2::RenderPlan`
8. `rvk2::SubmissionPlan`
9. `rvk2::Executor`
10. `rvk2::VIRenderer`
11. `rvk2::TextureReplacement`
12. `rvk2::VulkanBackend`
13. `rvk2::Validation`

## Delivery Phases

### Phase A: Contracts and Determinism

1. Build `rvk2` scaffolding and runtime selection.
2. Freeze schema (`rvk2_schema_v1`) with deterministic traces.
3. Land replay and local gate integration.

Exit:

1. Stable deterministic replay on maintained smoke scenarios.

### Phase B: Semantic Pipeline Backbone

1. Build draw semantic extraction.
2. Build raster-op IR.
3. Build render-work IR.
4. Build submission batching.
5. Build software executor as semantic oracle.

Exit:

1. Deterministic semantic pipeline from command -> executor output.

### Phase C: Core Rendering Correctness

1. Fill/copy correctness closure.
2. Texrect correctness closure.
3. Triangle coverage + interpolation + cycle behavior closure.
4. Depth/coverage/blend hazard closure.

Exit:

1. Synthetic correctness suite green for implemented behaviors.

### Phase D: VI and Presentation

1. VI shaping semantics (register-accurate policy).
2. Production 4:3/16:9 presentation contract.

Exit:

1. Deterministic VI tests and scenario-level stability.

### Phase E: Texture Replacement Layer

1. Deterministic replacement keying.
2. Hi-res pack integration.
3. `.hts` compatibility and cache behavior.

Exit:

1. Replacement remains deterministic and mode-safe.

### Phase F: Cutover and Deletion

1. Make `rvk2` default path.
2. Remove dead GLideN64-derived execution paths.
3. Do not retain runtime fallback paths.

Exit:

1. Local gate + smoke + replay + targeted conformance all stable.

## Validation Stack

1. Command decode/state-latch unit tests.
2. Semantic replay tests.
3. Synthetic pixel behavior tests.
4. Deterministic scenario smoke tests.
5. Cross-core visual comparison as secondary signal only.

## Operating Contract

1. Canonical docs are:
   - `docs/vulkan-core-rebuild-plan.md`
   - `docs/vulkan-core-status.md`
2. Architectural decisions go in `docs/adr/`.
3. Keep planning docs compact; no parallel roadmap documents.
