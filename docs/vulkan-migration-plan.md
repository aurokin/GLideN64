# Vulkan Migration Plan

## Goal

Replace the OpenGL rendering backend with a Vulkan backend while preserving GLideN64 behavior, compatibility, and performance.

## Non-goals

- Rewriting core N64 emulation logic unrelated to rendering.
- Changing plugin APIs exposed to emulators.
- Dropping OpenGL immediately (OpenGL remains as fallback during migration).

## Current state

- `graphics::Context` already abstracts rendering through `ContextImpl`.
- OpenGL is the only functional backend today.
- A Vulkan selection scaffold now exists; selecting Vulkan currently logs and falls back to OpenGL.

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
- Validate per-game correctness against OpenGL baseline captures.
- Build a regression suite: scene checksums/screenshots + frame timing snapshots.
- Resolve backend-specific behavior mismatches and precision issues.

6. Performance hardening
- Remove redundant barriers and optimize pipeline transitions.
- Batch descriptor updates and reduce pipeline churn.
- Tune upload paths, asynchronous compilation, and cache warmup behavior.

7. Rollout
- Introduce runtime backend selection in config/UI (default OpenGL first).
- Promote Vulkan to opt-in beta after parity thresholds are met.
- Switch default backend only after stability/perf targets hold.

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

## Exit criteria for Vulkan default

- Zero critical rendering regressions across maintained baseline titles.
- Stable frame pacing within agreed tolerance compared with OpenGL baseline.
- Shader cache correctness proven across clean cache and warm cache runs.
- No blocker-level driver issues on maintained target platforms.
