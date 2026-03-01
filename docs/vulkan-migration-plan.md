# Vulkan Migration Plan

## Goal

Replace the legacy rendering backend with a Vulkan backend while preserving RealityVK behavior, compatibility, and performance.

## Non-goals

- Rewriting core N64 emulation logic unrelated to rendering.
- Changing plugin APIs exposed to emulators.
- Preserving legacy renderer code parity in this branch (this branch is Vulkan-first and actively removes that legacy rendering code).

## Current state

- `graphics::Context` already abstracts rendering through `ContextImpl`.
- Vulkan backend bootstrap now initializes Vulkan instance/device/queue/surface/swapchain through Mupen vidext Vulkan callbacks.
- Vulkan render/resource/shader calls are still stubbed while subsystem ports are in progress.
- Legacy renderer compilation is disabled in this branch (`REALITYVK_BUILD_LEGACY_RENDERER=ON` now hard-fails configure).
- Shared renderer parameter constants are now backend-neutral in `Graphics/Parameters.cpp` (no legacy context dependency).
- Linux Mupen build with default settings now links **zero** legacy context files.
- Host bootstrap no longer depends on GLX proc lookups; Vulkan initialization now uses `VidExt_InitWithRenderMode` and `VidExt_VK_*` APIs.

## Legacy Renderer Retirement Tracker (Vulkan-first)

Immediate targets (in order):

1. Remove obsolete legacy host/video extension calls from runtime startup paths.
2. Keep Vulkan-present-only host path and tighten failure diagnostics.
3. Delete legacy threaded path, GLSL combiner path, and legacy context implementation from this branch once no remaining references exist.

Completed in this pass:

1. Shared parameter constants moved to backend-neutral `Graphics/Parameters.cpp`.
2. Mupen display bootstrap moved out of the legacy context tree to `Graphics/Host/mupen64plus/`.
3. Unconditional legacy renderer link dependency removed from default Linux Vulkan-first builds.

## Milestones

1. Foundation (backend bootstrap)
- Add real `vulkan::ContextImpl` with instance/device/surface/swapchain lifecycle.
- Build command buffers and queue submission model.
- Define backend-owned synchronization primitives (fences/semaphores) and frame pacing.

2. Resource model
- Implement Vulkan texture, buffer, and framebuffer abstractions matching current `ContextImpl` contracts.
- Add descriptor set strategy for combiner inputs and per-draw uniforms.
- Add memory allocator strategy (staging + device-local, reuse pools).

3. Shader pipeline
- Replace GLSL runtime path with Vulkan shader pipeline strategy (SPIR-V).
- Introduce shader cache key parity with current combiner key model.
- Implement persistent shader cache serialization/deserialization for Vulkan.

4. Draw path parity
- Port triangle/rect/line draw paths and blending/depth/scissor behavior.
- Port framebuffer copy/upscale/downscale and RDRAM interop dependent paths.
- Port depth handling and special paths used by problematic titles.

5. Feature parity and validation
- Validate per-game correctness against reference captures.
- Build a regression suite: scene checksums/screenshots + frame timing snapshots.
- Resolve backend-specific behavior mismatches and precision issues.

6. Performance hardening
- Remove redundant barriers and optimize pipeline transitions.
- Batch descriptor updates and reduce pipeline churn.
- Tune upload paths, asynchronous compilation, and cache warmup behavior.

7. Rollout
- Keep runtime/backend configuration Vulkan-first in this branch.
- Promote Vulkan to opt-in beta after parity thresholds are met.
- Switch default backend only after stability/perf targets hold.

8. Long-term host integration
- Maintain compatibility with Mupen64Plus plugin API while Vulkan matures.
- Add explicit host-integration adapters so backend internals can evolve without host breakage.

## Delivery cadence (agreed)

We will execute work in **2-phase blocks**, then do a focused refactor checkpoint before starting the next block.

### Rule

- Implement two phases.
- Stop for a simplification/refactor pass.
- Keep local gates green before and after each refactor.

### Planned blocks

Block A:
- Phase 2 (Resource model)
- Phase 3 (Shader pipeline)
- Refactor A: simplify resource/shader boundaries and remove temporary coupling.

Block B:
- Phase 4 (Draw path parity)
- Phase 5 (Feature parity and validation)
- Refactor B: simplify draw/FB paths and remove redundant compatibility workarounds that are unnecessary for Vulkan.

Block C:
- Phase 6 (Performance hardening)
- Phase 7 (Rollout)
- Refactor C: stabilize long-term architecture and isolate emulator/plugin-facing seams for future custom plugin work.

Block D:
- Phase 8 (Long-term host integration)
- Ongoing compatibility + distribution hardening
- Refactor D: isolate host adapters from renderer core so future custom plugin work is low-risk.

### Architectural bias

- Prioritize **feature parity over code parity** with legacy renderer paths.
- Internal APIs are changeable when it improves clarity and maintainability.
- Preserve external plugin behavior while we are still shipping in current integration targets.

## Block A execution notes (learning-based update)

### Implemented in current pass

- Phase 2 foundation:
  - Added Vulkan `ResourceModel` abstraction for texture/framebuffer/renderbuffer state.
  - Implemented Vulkan texture object lifecycle with image/view/sampler allocation and upload path (staging copy).
  - Wired texture/framebuffer/renderbuffer/bind-image API calls through the resource layer.
- Phase 3 foundation:
  - Added Vulkan shader key persistence (`ShaderKeyStorage`) for combiner-key caching.
  - Wired shader storage save/load hooks for Vulkan path.
- Refactor A execution:
  - Split `ResourceModel` into `TextureStore`, `FramebufferStore`, and `BindingState`.
  - Rewired `ContextImpl` to route texture, framebuffer/renderbuffer, and image/texture-unit bindings through those smaller modules.
  - Removed the monolithic resource module to keep ownership boundaries explicit.
  - Split texture upload mechanics into a dedicated `TextureUploader` module used by `TextureStore`.
  - Introduced `ProgramLibrary` + `DescriptorLayoutRegistry` boundaries for shader-module ownership and pipeline-layout ownership.

### What we learned

- The first resource layer was useful, but too broad as one module.
- `FramebufferStore` and `BindingState` are currently pure metadata and can remain independent from Vulkan device lifetimes.
- `TextureStore` is the only phase-2 store that truly depends on Vulkan device/queue/command-pool services today.
- Upload policy can now evolve independently because `TextureStore` no longer owns staging-buffer command submission logic.
- Program and descriptor layout ownership can evolve independently from draw submission and pipeline caching.
- Descriptor strategy cannot be finalized cleanly until textured draw packets are represented explicitly in the draw path.
- True shader binary cache value depends on real Vulkan combiner/pipeline compilation (not just key persistence).

### Refactor A (updated scope)

Refactor A now tracks completed vs remaining work:

1. Completed:
- `TextureStore` (images/views/samplers/uploads)
- `FramebufferStore` (attachments/renderbuffer metadata)
- `BindingState` (texture/image-unit bindings)

2. Completed:
- `TextureUploader` now owns staging uploads + layout transitions used by `TextureStore`.

3. Completed:
- `ProgramLibrary` boundary introduced for Vulkan program/shader-module ownership and combiner-program factory entrypoint.
- Descriptor layout registry is now separate from draw submission and pipeline-cache code.

4. Completed:
- Draw packet resource hooks are now wired end-to-end:
- Texture/image bindings are captured per packet through `BindingState`.
- Packet resource slots now use fixed unit-indexed arrays + masks (no per-draw slot vector allocations).
- Descriptor binder now consumes packet slots and binds Vulkan descriptor sets during draw encoding.
- Current color-only path remains intact while textured combiner pipeline work is still pending.

5. Deferred:
- Keep key persistence now
- Add binary cache only after real Vulkan combiner pipelines are active

## Block B execution notes (current)

### Phase 4 started

- Added a Vulkan textured baseline pipeline path while preserving the existing color-only path.
- `DrawVertex` now carries texture coordinates and barycentric payload needed for textured/combiner evolution.
- `ProgramLibrary` now owns two shader program variants:
  - basic color
  - basic textured (descriptor-set sampled)
- `PipelineCache` now keys pipelines by textured vs color-only draw packets and selects matching shader modules.
- Textured shader path now consumes per-draw push constants for combiner-derived behavior flags (`texture0`, `texture1`, `shade`).
- Draw submission now maps combiner tile usage (`usesTile(0/1)`) into Vulkan descriptor slots 0/1 so two-texture baseline behavior exists before full combiner parity.
- Descriptor updates now use per-frame slot caching to avoid redundant `vkUpdateDescriptorSets` calls on repeated bindings.

### Phase 5 bootstrap

- Linux local gates are now part of every textured-path integration step:
  - `REALITYVK_GRAPHICS_BACKEND=Vulkan ./scripts/local_gate.sh`
  - `./scripts/local_gate.sh`
- Added deterministic local smoke tooling:
  - `./scripts/local_smoke.sh` (scenario manifest + checksum comparison)
  - `REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh` (opt-in smoke stage inside local gate)
  - default smoke runner path now uses local LLM-specific Mupen runtime in `~/code/paper_mario`.
- Current observed status:
  - Reference smoke path is stable with committed baseline checksums.
  - Vulkan smoke launch path is now stable on this Linux host for the Paper Mario deterministic capture path after upload-layout fixes in Vulkan texture staging.
- Current smoke scaffolding is in place; baseline expansion and scenario coverage still need to grow to claim parity.

### Next in Block B

1. Move texrect/triangle sampling behavior from baseline texture multiply toward combiner parity (tile selection, dual-source usage, and combiner-controlled math).
2. Populate/lock reference baselines and expand maintained smoke scenarios beyond initial Paper Mario checkpoint coverage.
3. Start pruning GL-era compatibility workarounds only after Vulkan parity checks confirm they are unnecessary.

### Offscreen FBO trace study (2026-03-01)

New Vulkan trace controls were added to expose framebuffer workflow shape in real gameplay:

- `REALITYVK_VK_TRACE_FBO=1`
- `REALITYVK_VK_TRACE_FBO_VERBOSE=1` (optional, more draw enqueue detail)
- `REALITYVK_VK_TRACE_FBO_LIMIT=<N>` (default: 2000)

Representative run used:

- `scripts/paper_mario_smoke_runner.sh --backend Vulkan --rom /home/auro/code/paper_mario/baseline/papermario-clean.z64 --state /home/auro/code/mupen/mupen64plus-runtime/checkpoints/boot_title_003/boot_title_003.st --frames 0 --out /tmp/pm_vk_trace_boot003.png --preset full --scale-div 2`

Observed operation counts from trace logs:

- `draw_skip` (non-default draw FBO): **367**
- `bind`: 67
- `attach`: 30
- `blit_begin` / `blit_end`: 6 / 6
- `clear_color`: 11
- `present`: 11

Most skipped draws were offscreen rect/triangle work on non-default FBOs:

- dominant skipped draw FBO ids: `230`, `16`, `19`, `226`
- skipped primitives: mostly `rects` (346), plus `triangles` (21)
- final trace summary sample: `skippedOffscreenDraws=367`, `skippedOffscreenVertices=1861`

Key implication:

- The current Vulkan path now handles attach/clear/blit metadata and texture copies, but still drops the primary render-to-texture workload (`draw*` on non-default FBOs). This is now the highest-leverage parity gap.

Architecture decision for next implementation block:

1. Introduce a target-aware draw recorder that preserves draw packets per active draw framebuffer (not only default framebuffer).
2. Add an offscreen pass executor that can render packet batches into framebuffer-attached textures before swapchain present.
3. Keep texture clear/blit paths as supporting operations, but treat them as secondary to offscreen draw execution.
4. Validate with trace-backed checkpoints first, then fold into smoke/baseline parity gates.

### Offscreen draw execution prototype (2026-03-01 follow-up)

Implemented first working offscreen-draw bridge for non-default FBO draws:

- Non-default `drawTriangles` / `drawRects` / `drawLine` no longer auto-skip by default.
- Packets are finalized with current raster/depth/blend state (`DrawRecorder::applyStateToPacket`) and executed immediately into the currently bound draw FBO attachment set via a Vulkan offscreen pass.
- Temporary Vulkan render pass + framebuffer are built from current FBO attachments (`COLOR_ATTACHMENT0` + optional `DEPTH_ATTACHMENT`) and encoded with existing descriptor/pipeline/draw encoder infrastructure.
- Render target textures are marked readable again (`SHADER_READ_ONLY_OPTIMAL`) after offscreen execution.

Observed result after prototype on the previously traced checkpoint (`boot_title_003`):

- `skippedOffscreenDraws` reduced from `367` to `0`.
- No `draw_skip` events in trace for that scenario.

Current status:

- This removes the largest known structural blocker (dropped non-default FBO draw work).
- On a 18-checkpoint Paper Mario sample set, reference/candidate capture parity now matches on most states; remaining mismatches are concentrated in a small subset of live progression checkpoints.
- Next work should focus on offscreen pass correctness details (attachment load/store semantics, format/layout edge cases, and depth behavior), not on whether offscreen draws execute at all.

## Long-term integration roadmap (Mupen64Plus only)

### Objective

- Keep one Vulkan-first rendering core for the Mupen64Plus plugin path.

### Integration strategy

1. Host adapter boundary
- Introduce explicit host layer modules for:
  - video extension / window / surface interop
  - input timing hooks used by presentation pacing
  - filesystem paths for cache/config/save assets
- Keep renderer core host-agnostic; host layer only translates APIs/events.

2. Mupen64Plus stabilization track
- Preserve plugin ABI behavior and config semantics.
- Add compatibility tests for:
  - plugin lifecycle (`Startup`, `Shutdown`, ROM open/close, context resets)
  - surface re-create and resize paths
  - threaded + non-threaded presentation modes (where supported)

3. Packaging and config convergence
- Version shader/cache artifacts with host + backend signatures to avoid invalid reuse.
- Produce reproducible release artifacts and checksums for maintained host targets.

### Delivery order

1. Finish Vulkan parity and local Linux validation (Blocks A-C).
2. Add host adapter boundary and lifecycle tests.
3. Certify Mupen64Plus path first.
4. Expand to Windows path after Linux parity/stability targets hold.

### Exit criteria for host integration

- Mupen64Plus: no blocker regressions vs reference behavior on maintained test set.
- Shared Vulkan core remains single-source (no host-specific renderer forks).

## Workstreams and ownership

- Rendering backend core: device/swapchain/command submission.
- Shader/combiner translation: GLSL parity and cache.
- Framebuffer/depth behavior parity: high-risk game compatibility paths.
- Tooling and regression: capture/replay, checksum/screenshot verification.

## High-risk areas

- Framebuffer/depth emulation semantics and precision drift.
- Shader translation mismatches for uncommon combiner states.
- Synchronization hazards causing flicker, corruption, or stalls.
- Platform variance (Windows/Linux driver behavior differences).

## Local quality gates

Every change set should pass:

1. `./scripts/local_gate.sh`
2. At least one emulator smoke test on representative games.
3. No increase in known regression list for baseline titles.

## Spec-grounded validation assets (2026-03-01)

To reduce dependence on legacy GL behavior as a correctness oracle, we now keep:

- N64 local reference bundle: `docs/references/n64/README.md`
- Runtime contract checklist: `docs/n64-runtime-validation-checklist.md`
- Cross-core bug matrix and test candidates: `docs/n64-video-core-bug-matrix.md`
- Paper Mario runtime parity manifest: `tests/smoke/scenarios_paper_mario_runtime.tsv`
- Runtime validation run log: `docs/archive/runtime-validation-2026-03-01.md`

## Exit criteria for Vulkan default

- Zero critical rendering regressions across maintained baseline titles.
- Stable frame pacing within agreed tolerance compared with reference baseline.
- Shader cache correctness proven across clean cache and warm cache runs.
- No blocker-level driver issues on maintained target platforms.
