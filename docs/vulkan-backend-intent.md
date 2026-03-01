# Vulkan Backend Intent Map

## Goal

Keep Vulkan code easy to edit by separating:

1. N64 render semantics (RDP combiner/blender intent)
2. Packet-level behavior adaptation (heuristics/special passes)
3. Vulkan execution/orchestration (instance/device/swapchain/submit)

## N64 semantic anchors

Use these as the primary contract when adjusting behavior:

- `docs/references/n64/SGI_RDP_Command_Summary.pdf`
- `docs/references/n64/n64brew_Reality_Display_Processor_Commands.html`
- `docs/references/n64/n64brew_Reality_Display_Processor_Pipeline.html`

Combiner equation to preserve conceptually:

- `(A - B) * C + D` for each cycle, then cycle composition rules.

RDP pipeline staging to preserve conceptually:

- combiner stage (color/alpha equations)
- blender stage (coverage/alpha/framebuffer combine decisions)

## Module boundaries

- `src/Graphics/VulkanContext/vulkan_ContextImpl.cpp`
  - Owns high-level draw/present orchestration and N64-facing API dispatch.
  - Should not own Vulkan instance/device/swapchain lifecycle internals.
  - Should not own detailed combiner decode/classification/apply logic.
  - Should not own strict blend-mux packing logic.
- `src/Graphics/VulkanContext/vulkan_ContextImpl_InstanceDevice.cpp`
  - Owns Vulkan instance/surface/device/queue/sync lifecycle.
  - Keeps startup/shutdown sequencing explicit and isolated.
- `src/Graphics/VulkanContext/vulkan_ContextImpl_Swapchain.cpp`
  - Owns swapchain/render-pass/framebuffer lifecycle and draw-resource setup.
  - Centralizes swapchain recreate/destroy behavior.
- `src/Graphics/VulkanContext/vulkan_ContextImpl_Internal.h`
  - Owns shared internal context state (`VulkanState`) and Vulkan helper utilities.
  - Shared by lifecycle translation units; not for external backend consumers.
- `src/Graphics/VulkanContext/vulkan_CombinerHeuristics.cpp`
  - Owns inferred combiner-program behavior and shader-flag resolution.
  - Operates on combiner key semantics, not packet mutation.
- `src/Graphics/VulkanContext/vulkan_CombinerDecode.*`
  - Owns mux-selector expansion (`encoded selector -> G_GCI_* semantic input`).
- `src/Graphics/VulkanContext/vulkan_CombinerClassify.*`
  - Owns pattern detection for direct/modulate/add/alpha-scale/two-cycle post-modulate.
  - Keeps classification pure over decoded combiner terms.
- `src/Graphics/VulkanContext/vulkan_CombinerApply.*`
  - Owns `DrawPacket` mutation for combiner-driven overrides.
  - Owns combiner debug/env-flag override controls.
- `src/Graphics/VulkanContext/vulkan_BlendMux.*`
  - Owns strict blend-mux packet packing and debug instrumentation.
  - Represents blender-stage intent separately from combiner-stage intent.
- `src/Graphics/VulkanContext/vulkan_ReadbackAdapters.*`
  - Owns `PixelReadBuffer`/`ColorBufferReader` Vulkan adapter classes.
  - Keeps readback adapter behavior outside `ContextImpl`.
- `src/Graphics/VulkanContext/vulkan_PacketBuilder.*`
  - Owns packet base initialization and vertex population helpers.
  - Keeps draw packet assembly details outside `ContextImpl`.
- `src/Graphics/VulkanContext/vulkan_PacketNormalize.*`
  - Owns packet position normalization policy and fallback transforms.
  - Keeps viewport/scissor/color-image normalization rules outside `ContextImpl`.
- `src/Graphics/VulkanContext/vulkan_SpecialPrograms.cpp`
  - Owns special shader program adapters (texrect/depth-fog/gamma/fxaa/text) and rect-pass special packet flag mapping.
  - Central place for dynamic type checks of special shader program kinds.

## Agent edit rules

1. If a change is about combiner selector mapping, edit `vulkan_CombinerDecode.*` first.
2. If a change is about combiner pattern detection, edit `vulkan_CombinerClassify.*` first.
3. If a change is about packet mutation due to combiner patterns, edit `vulkan_CombinerApply.*` first.
4. If a change is about strict blend mux, edit `vulkan_BlendMux.*` first.
5. If a change is about readback buffer adapter behavior, edit `vulkan_ReadbackAdapters.*` first.
6. If a change is about packet assembly or packet vertex initialization, edit `vulkan_PacketBuilder.*` first.
7. If a change is about packet normalization policy/fallback transforms, edit `vulkan_PacketNormalize.*` first.
8. If a change is about texrect/depth-fog/gamma/fxaa/text behavior, edit `vulkan_SpecialPrograms.*` first.
9. Keep `ContextImpl` focused on orchestration and data flow between modules.
10. Prefer adding small pure helper functions over expanding `ContextImpl` branches.
11. Treat env-flag behavior as debug/iteration scaffolding; keep defaults aligned with validated parity paths.

## Refactor roadmap (agent-oriented)

Phase 1 (completed):

1. Extract combiner/blender semantics from `ContextImpl` into `vulkan_CombinerHeuristics.*`.
2. Extract special shader adapters + rect special-pass mapping into `vulkan_SpecialPrograms.*`.

Phase 2 (completed):

1. Split combiner heuristics into `decode`, `classify`, and `apply` files.
2. Keep all env-flag gates in `apply` so debug control is centralized.
3. Reuse decode mapping in inference path so selector expansion has one source.

Phase 2.5 (completed):

1. Split strict blend-mux packing into `vulkan_BlendMux.*`.
2. Keep combiner- and blender-stage responsibilities separate by file/module.

Phase 3 (completed):

1. Extract readback adapters (`PixelReadBuffer`/`ColorBufferReader`) into `vulkan_ReadbackAdapters.*`.
2. Keep readback module pure adapter logic; Vulkan texture transfer stays in `TextureStore`.

Phase 4 (completed):

1. Extract packet-assembly helpers from `ContextImpl` draw paths into `vulkan_PacketBuilder.*`.
2. Keep builder responsible for `DrawPacket` population only (no Vulkan API calls).
3. Move normalization policy and transform-mode selection into explicit helper functions.
4. Completed slices:
5. packet base init + vertex population moved to `vulkan_PacketBuilder.*`,
6. position normalization policy moved to `vulkan_PacketNormalize.*`.

Phase 5 (completed):

1. Split `ContextImpl` Vulkan lifecycle into:
2. `vulkan_ContextImpl_InstanceDevice.cpp`,
3. `vulkan_ContextImpl_Swapchain.cpp`,
4. with shared state/helpers in `vulkan_ContextImpl_Internal.h`.
5. Keep `ContextImpl` as orchestrator that sequences module calls and error handling.

Phase 6 (completed):

1. Add a small N64-intent trace surface:
2. combiner classification trace (`decode -> classify -> apply decision`) with packet id hooks.
3. blend-mux trace (`mux1/mux2/params`) with bounded counters.
4. Keep traces opt-in via env flags and local-gate safe by default.

### Phase 6 trace controls

All Phase 6 traces are opt-in and bounded:

- `REALITYVK_VK_TRACE_COMBINER_INTENT=1`
  - Enables packet-level combiner intent trace including decoded active-cycle terms, classification result, and final apply decision.
- `REALITYVK_VK_TRACE_COMBINER_INTENT_LIMIT=<N>`
  - Caps combiner intent trace lines (default: `256`).
- `REALITYVK_VK_TRACE_BLEND_MUX=1`
  - Enables strict blend-mux intent trace (`mux1/mux2/params`, cycle, texrect, decision).
- `REALITYVK_VK_TRACE_BLEND_MUX_LIMIT=<N>`
  - Caps blend-mux intent trace lines (default: `256`).

### Trace interpretation quick guide

Combiner intent trace (`REALITYVK_VK_TRACE_COMBINER_INTENT=1`) is structured as:

- `id=<packetId>`: packet correlation key across draw path logs.
- `mux`/`cycle`: raw combiner key anchor from RDP state.
- `decode=[c:A,B,M,D a:A,B,M,D]`: expanded active-cycle inputs from N64 combiner equation.
- `classify=[...]`: classifier outputs (`direct/mod/add/alpha/alphaScale/twoCycle`).
- `apply=<decision>`: final packet mutation branch used by `vulkan_CombinerApply`.
- `flags=...`: resulting shader flag mask after apply.
- `count=<n>`: bounded power-of-two emission counter for repeated identical signatures.

Blend-mux intent trace (`REALITYVK_VK_TRACE_BLEND_MUX=1`) is structured as:

- `id=<packetId>`: packet correlation key.
- `mux1/mux2/params`: packed blender-stage control values sent with strict blend mode.
- `cycle`/`texrect`: RDP cycle mode and rect classification context.
- `decision`: one of `disabled`, `filtered_mux2`, `filtered_params`, `applied`.
- `count=<n>`: bounded repeated-signature counter.

Agent workflow for intent debugging:

1. Start with a failing frame and collect packet id from present/composite logs.
2. Locate `VK combiner intent` lines for that packet id to confirm decode/classify/apply chain.
3. If strict blend is expected, locate matching `VK blend mux intent` line and verify `decision=applied`.
4. If combiner decision looks correct but output differs, inspect packet bindings/normalization traces next.

## Current status summary (2026-03-01)

1. Combiner semantics now have explicit decode/classify/apply boundaries.
2. Blend-mux packet packing is isolated from combiner apply logic.
3. Readback adapter classes are isolated from `ContextImpl`.
4. Packet base init and vertex population are isolated in `vulkan_PacketBuilder.*`.
5. Packet normalization policy is isolated in `vulkan_PacketNormalize.*`.
6. `ContextImpl` now consumes modular helpers for:
7. inferred combiner program creation,
8. shader-flag resolution,
9. combiner packet mutation,
10. strict blend-mux packet state,
11. readback adapter creation,
12. packet assembly helpers,
13. packet normalization policy,
14. special program detection.
15. Local gate passes after each split.
16. Vulkan lifecycle is split into focused translation units with shared internal state.
17. Intent traces are opt-in, bounded, and packet-id-correlated.

## Completion criteria per phase

1. `./scripts/local_gate.sh` passes.
2. No behavior changes outside intended module boundary.
3. `ContextImpl.cpp` line count decreases and orchestration readability improves.
4. New files have one primary responsibility and clear naming by N64 intent or Vulkan system.
