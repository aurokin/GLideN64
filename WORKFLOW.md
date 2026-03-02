# RealityVK Workflow

This document captures the working loop I use to drive Vulkan parity work in this repo.

## Scope

- Primary gate: Paper Mario intro parity only.
- Comparison target: `GLideN64-upstream` plugin on upstream core.
- Candidate target: `RealityVK` Vulkan plugin on local core.
- Goal: feature parity over code parity.

## Test Contract (What We Are Comparing)

We are testing:

- Candidate: `RealityVK Vulkan` from this repo.
- Reference: `GLideN64-upstream`.

For Paper Mario parity, this is wired by default in:

- [paper_mario_parity.sh](/home/auro/code/gliden64/scripts/paper_mario_parity.sh)
  - reference plugin: `/home/auro/code/gliden64-upstream/build-release/plugin/Release/mupen64plus-video-GLideN64.so`
  - candidate plugin: `build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so`
  - reference core: `/home/auro/code/mupen/mupen64plus-core-upstream/projects/unix/libmupen64plus.so.2`
  - candidate core: `/home/auro/code/mupen/mupen64plus-core/projects/unix/libmupen64plus.so.2`

## Repository Map

Primary repos and runtimes used by this workflow:

- Working renderer repo:
  - `/home/auro/code/gliden64`
- Upstream renderer reference:
  - `/home/auro/code/gliden64-upstream`
- Runtime/tooling root:
  - `/home/auro/code/mupen`
- Candidate core:
  - `/home/auro/code/mupen/mupen64plus-core`
- Reference core:
  - `/home/auro/code/mupen/mupen64plus-core-upstream`
- Runtime launcher:
  - `/home/auro/code/mupen/mupen64plus-runtime`
- Agent/capture support:
  - `/home/auro/code/mupen/mupen64plus-ui-console`
- Future libretro integration target:
  - `/home/auro/code/mupen/mupen64plus-libretro-nx`
- Upstream RetroArch reference:
  - `/home/auro/code/mupen/RetroArch-upstream`

## ROM Locations

- Current Paper Mario gate ROM:
  - `/home/auro/code/paper_mario/baseline/papermario-built.z64`
- Broader N64 ROM set:
  - `/home/auro/code/n64_roms`

## Read-Only Policy

Filesystem note:

- The repos above are writable on this machine.

Workflow policy:

- Treat upstream repos as read-only references unless a task explicitly asks to edit them:
  - `/home/auro/code/gliden64-upstream`
  - `/home/auro/code/mupen/mupen64plus-core-upstream`
  - `/home/auro/code/mupen/RetroArch-upstream`
- Treat ROM assets as read-only inputs.

## Core Principles

1. Keep one stable visual gate.
2. Change one variable at a time.
3. Add instrumentation before broad refactors.
4. Promote behavior to default only after repeatable gains.
5. Keep every new behavior reversible with an env opt-out.

## Daily Loop

1. Clean runtime/process state.
2. Rebuild plugin.
3. Run baseline parity once.
4. Form one hypothesis.
5. Probe with targeted env flags or narrow code gate.
6. Run A/B or small matrix.
7. Keep only changes with consistent metric win and no regressions on the gate.
8. Promote probe to default (with disable env) when proven.
9. Re-run baseline and strict sweeps.
10. Update docs and keep logs/artifacts attributable.

## Process Hygiene

Before new runs:

```bash
pgrep -af "mupen64plus|launch.sh|agentctl.py|script -qec" || true
kill -9 <stale_pids_if_any> || true
```

Reason: stale launch wrappers can contaminate captures and debugging.

## Build + Baseline Commands

```bash
cmake --build build/release-vulkan-smoke -j$(nproc)
REALITYVK_PM_VISUAL_GATE=0 scripts/paper_mario_parity.sh
```

Baseline metrics output:

- `build/parity-runs/paper-mario/paper_mario_intro.metrics.json`

## Vulkan Backend Layout (Current)

Use this file split when navigating/refactoring Vulkan backend code:

- [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp)
  - High-level draw/present orchestration and packet flow.
- [vulkan_ContextImpl_InstanceDevice.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl_InstanceDevice.cpp)
  - Instance/surface/device/queue/sync bring-up and teardown.
- [vulkan_ContextImpl_Swapchain.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl_Swapchain.cpp)
  - Swapchain/render-pass/framebuffer/draw-resource lifecycle.
- [vulkan_ContextImpl_Internal.h](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl_Internal.h)
  - Shared internal Vulkan context state and helper utilities.
- [vulkan_CombinerDecode.*](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_CombinerDecode.cpp)
  - Encoded combiner selector expansion.
- [vulkan_CombinerClassify.*](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_CombinerClassify.cpp)
  - Combiner pattern classification.
- [vulkan_CombinerApply.*](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_CombinerApply.cpp)
  - Packet mutation from classified combiner intent.
- [vulkan_BlendMux.*](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_BlendMux.cpp)
  - Strict blend mux packing and intent tracing.
- [vulkan_PacketBuilder.*](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_PacketBuilder.cpp)
  - Packet base init and vertex assembly.
- [vulkan_PacketNormalize.*](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_PacketNormalize.cpp)
  - Packet normalization policy.

## Baseline Reset Workflow

Use separate commands for committed smoke checksums vs parity cache artifacts:

1. Update committed smoke baseline checksums:

```bash
REALITYVK_SMOKE_BACKENDS=Vulkan \
REALITYVK_SMOKE_PLUGIN_VULKAN=/home/auro/code/gliden64/build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so \
REALITYVK_SMOKE_UPDATE_BASELINES=1 \
./scripts/local_smoke.sh
```

2. Refresh Paper Mario parity reference capture cache (for A/B compare scripts):

```bash
REALITYVK_PM_REFRESH_REFERENCE=1 ./scripts/paper_mario_parity.sh
```

3. Verify baseline gate after refresh:

```bash
./scripts/local_smoke.sh
```

## Scripts Used

Main scripts used regularly:

- [paper_mario_parity.sh](/home/auro/code/gliden64/scripts/paper_mario_parity.sh)
  - Main single-run parity gate for Paper Mario.
- [paper_mario_smoke_runner.sh](/home/auro/code/gliden64/scripts/paper_mario_smoke_runner.sh)
  - Launches runtime, steps frames, captures framebuffer dumps/screenshots.
- [paper_mario_compare_view.sh](/home/auro/code/gliden64/scripts/paper_mario_compare_view.sh)
  - Builds and opens composed compare image panels.
- [local_gate.sh](/home/auro/code/gliden64/scripts/local_gate.sh)
  - Local gate wrapper.
- [local_smoke.sh](/home/auro/code/gliden64/scripts/local_smoke.sh)
  - Scenario smoke framework.
- [intro_video_compare.py](/home/auro/code/gliden64/scripts/intro_video_compare.py)
  - Multi-game sequence capture, frame metrics, and comparison video generation.

## Image Capture and Comparison Workflow

Primary flow (Paper Mario):

1. `paper_mario_parity.sh` resolves one scenario from:
   - [scenarios.tsv](/home/auro/code/gliden64/tests/smoke/scenarios.tsv)
2. For reference and candidate, it calls `paper_mario_smoke_runner.sh`.
3. Capture method is controlled explicitly:
   - framebuffer dump path (`dumpfb-preset`) with optional `flip_y`
   - screenshot fallback or forced screenshot path
4. Captures are written as `.ppm` and normalized `.png`.
5. Comparison computes:
   - `rmse`
   - `mae`
   - `max_abs_diff`
6. Outputs:
   - `paper_mario_intro.reference.png`
   - `paper_mario_intro.candidate.png`
   - `paper_mario_intro.diff.png`
   - `paper_mario_intro.metrics.json`
   - `paper_mario_intro.capture-context.json`

Quick visual review flow:

- `paper_mario_compare_view.sh` composes reference/candidate/diff (plus optional OpenGL panel) and opens the latest image.

## Video Capture and Comparison Workflow

Script:

- [intro_video_compare.py](/home/auro/code/gliden64/scripts/intro_video_compare.py)

How it works:

1. Loads game list manifest (TSV).
2. For each game:
   - boots runtime twice (reference and candidate plugins)
   - pauses/steps deterministically
   - captures frame sequence via `dumpfb-preset`
3. Computes per-frame diff metrics:
   - changed pixels
   - mean absolute diff
   - max absolute diff
4. Builds videos with `ffmpeg`:
   - `reference.mp4`
   - `candidate.mp4`
   - `side_by_side.mp4`
   - `diff.mp4`
   - `triptych.mp4`
5. Writes reports:
   - per-game `result.json`
   - `per_frame_metrics.json`
   - run-level `results.json`
   - markdown report `REPORT.md`

Important for this project:

- Run video comparisons with explicit plugin paths so reference is `GLideN64-upstream` and candidate is `RealityVK Vulkan`.
- Do not rely on script defaults if they point to a different reference plugin build.

Example command:

```bash
python3 scripts/intro_video_compare.py \
  --manifest tests/intro_video/games.tsv \
  --rom-root /home/auro/code/n64_roms \
  --runtime-root /home/auro/code/mupen \
  --reference-plugin /home/auro/code/gliden64-upstream/build-release/plugin/Release/mupen64plus-video-GLideN64.so \
  --candidate-plugin /home/auro/code/gliden64/build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so \
  --out-dir build/intro-video-compare
```

## Investigation Pattern

### 1. Isolate layer path

- Use focused trace toggles (example):
  - `REALITYVK_VK_DEBUG_TRACE_RT_HANDLE=<handle>`
  - `REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_HANDLES=<handles>`
  - `REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_LIMIT=<n>`

### 2. Confirm where mismatch is introduced

- Determine if mismatch is:
  - offscreen writer pass,
  - present compositing pass,
  - blend/combiner semantic path,
  - or capture/presentation orientation contract.

### 3. Add a narrow debug gate

- Implement small behavior switch under one env var.
- Do not broadly rewrite multiple systems at once.

### 4. Run minimal matrix

Typical matrix:

- baseline
- debug toggle ON
- related toggle ON/OFF combinations

Keep runs short and only for this gate.

### 5. Promote only proven wins

If the probe is repeatably better:

- make it default behavior
- add `REALITYVK_VK_DISABLE_...=1` opt-out
- document metrics before/after

## Promotion Criteria

A change is promoted when all are true:

1. Better baseline metrics on repeated runs.
2. No gate instability introduced.
3. Behavior stays reversible (disable env).
4. Trace evidence matches hypothesis.
5. Strict-path status remains understood (even if still regressed).

## Regression Safety

Always re-run after promotion:

```bash
# baseline
REALITYVK_PM_VISUAL_GATE=0 scripts/paper_mario_parity.sh

# strict fetch
REALITYVK_PM_VISUAL_GATE=0 \
REALITYVK_VK_EXPERIMENTAL_FB_FETCH_COLOR=1 \
REALITYVK_VK_STRICT_FB_FETCH_COLOR=1 \
scripts/paper_mario_parity.sh

# strict dual
REALITYVK_PM_VISUAL_GATE=0 \
REALITYVK_VK_EXPERIMENTAL_DUAL_SOURCE=1 \
REALITYVK_VK_STRICT_DUAL_SOURCE_BLEND=1 \
scripts/paper_mario_parity.sh
```

## Documentation Contract

After each meaningful change:

1. Update `docs/vulkan-core-status.md` with:
   - what changed,
   - exact metrics,
   - interpretation,
   - next step.
2. Update `docs/local-smoke.md` with new env controls.
3. Keep parity context visible in artifacts:
   - `paper_mario_intro.capture-context.json`
   - metrics JSON
   - trace logs when used.

## Delayed Work (Explicitly Later)

Deferred until later roadmap block:

- Shader parity edge-case closure.
- Offscreen skip-path closure.
- Broader runtime validation/checkpoint expansion beyond current Paper Mario gate.
- Mupen/RetroArch integration adjustments after renderer parity stabilizes.

## Monitoring Expectations

To monitor progress, track these expectations:

1. Baseline parity metric is reported after meaningful renderer changes.
2. Strict-path metrics are reported separately and not silently promoted.
3. Any promoted behavior has a disable env knob.
4. Runtime process hygiene is maintained (no stale launch sessions).
5. Docs are updated when behavior or metrics change:
   - `docs/vulkan-core-status.md`
   - `docs/local-smoke.md`
6. Artifact provenance remains clear via:
   - `paper_mario_intro.metrics.json`
   - `paper_mario_intro.capture-context.json`

## Current Known Effective Pattern

- Offscreen and present orientation/scaling issues should be solved using explicit runtime semantics, not tool-side implicit flipping.
- Final RT fullscreen compositing can require canonicalized fullscreen position/UV mapping to avoid sampling window drift.
- Border and main scene can come from different pass families; treat them as separate hypotheses even when they share final RT handle.

## What I Avoid

- Large speculative rewrites without measurements.
- Multi-game expansion before this single gate is stable.
- Long smoke loops when a single short capture answers the question.
- Leaving runtime processes alive between tests.
