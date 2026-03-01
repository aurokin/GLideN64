# RealityVK vs GLideN64 Upstream: Logic Flow Comparison

Goal: identify what is missing in RealityVK compared to upstream GLideN64 behavior.

## Current Signal (2026-03-01)

Paper Mario intro, true-stock reference path (`GLideN64 + core-upstream`, reference screenshot capture):

- Baseline RealityVK path: `rmse=0.259169`, `mae=0.164378`
- Strict experimental fetch/blend paths (current behavior):
  - `STRICT_FB_FETCH_COLOR`: `rmse=0.320348`, `mae=0.214847`
  - `STRICT_DUAL_SOURCE_BLEND`: `rmse=0.320349`, `mae=0.215135`
  - Combined strict path: `rmse=0.320349`, `mae=0.215135`

Interpretation:
- RT-texrect Y correction improved baseline materially, but large parity gap remains.
- Strict fetch/blend paths currently regress this gate and are not the next merge-up candidate.
- Older strict-path figures in this document (for example `rmse=0.088289`) are from the stale-reference period and are historical-only.

## 1) Shared Front-Half Flow (Mostly Parity)

Both repos still run the same high-level emulation-to-render dispatch path:

1. Core calls plugin entrypoints.
2. `ProcessRDPList` dispatches to `RDP_ProcessRDPList`.
3. `UpdateScreen` dispatches to `VI_UpdateScreen`.
4. VI/framebuffer logic decides what to render/swap.

Evidence (same in both repos):

- RealityVK:
  - [CommonAPIImpl_common.cpp](/home/auro/code/gliden64/src/common/CommonAPIImpl_common.cpp:164)
  - [RDP.cpp](/home/auro/code/gliden64/src/RDP.cpp:571)
  - [VI.cpp](/home/auro/code/gliden64/src/VI.cpp:243)
- Upstream:
  - [CommonAPIImpl_common.cpp](/home/auro/code/gliden64-upstream/src/common/CommonAPIImpl_common.cpp:164)
  - [RDP.cpp](/home/auro/code/gliden64-upstream/src/RDP.cpp:571)
  - [VI.cpp](/home/auro/code/gliden64-upstream/src/VI.cpp:243)

So missing behavior is not primarily in RDP command ingest/dispatch; it is in backend execution.

## 2) Backend Flow Comparison

### Upstream (OpenGL)

```text
Core VidExt_Init (GL)
  -> OpenGL context + attributes
  -> Context::init() creates opengl::ContextImpl
  -> GraphicsDrawer/Texrect/Combiner call Context APIs
  -> opengl_ContextImpl routes to GL state/shader/fbo systems
  -> CoreVideo_GL_SwapBuffers
```

Evidence:

- GL init/swap/readback path:
  - [mupen64plus_DisplayWindow.cpp](/home/auro/code/gliden64-upstream/src/Graphics/OpenGLContext/mupen64plus/mupen64plus_DisplayWindow.cpp:80)
  - [mupen64plus_DisplayWindow.cpp](/home/auro/code/gliden64-upstream/src/Graphics/OpenGLContext/mupen64plus/mupen64plus_DisplayWindow.cpp:150)
  - [mupen64plus_DisplayWindow.cpp](/home/auro/code/gliden64-upstream/src/Graphics/OpenGLContext/mupen64plus/mupen64plus_DisplayWindow.cpp:244)
- Context impl selection:
  - [Context.cpp](/home/auro/code/gliden64-upstream/src/Graphics/Context.cpp:29)
- Full shader/combiner/special passes:
  - [opengl_ContextImpl.cpp](/home/auro/code/gliden64-upstream/src/Graphics/OpenGLContext/opengl_ContextImpl.cpp:398)
  - [opengl_ContextImpl.cpp](/home/auro/code/gliden64-upstream/src/Graphics/OpenGLContext/opengl_ContextImpl.cpp:416)
  - [glsl_SpecialShadersFactory.cpp](/home/auro/code/gliden64-upstream/src/Graphics/OpenGLContext/GLSL/glsl_SpecialShadersFactory.cpp:957)

### RealityVK (Vulkan)

```text
Core VidExt_InitWithRenderMode(VULKAN)
  -> ContextFactory always creates vulkan::ContextImpl
  -> GraphicsDrawer/Texrect/Combiner call same Context APIs
  -> vulkan_ContextImpl converts draw calls to DrawPackets
  -> default FB packets queued, offscreen packets executed immediately
  -> present() submits Vulkan command buffers + swapchain present
```

Evidence:

- Vulkan init/present/readback path:
  - [mupen64plus_DisplayWindow.cpp](/home/auro/code/gliden64/src/Graphics/Host/mupen64plus/mupen64plus_DisplayWindow.cpp:57)
  - [mupen64plus_DisplayWindow.cpp](/home/auro/code/gliden64/src/Graphics/Host/mupen64plus/mupen64plus_DisplayWindow.cpp:133)
  - [mupen64plus_DisplayWindow.cpp](/home/auro/code/gliden64/src/Graphics/Host/mupen64plus/mupen64plus_DisplayWindow.cpp:242)
- Context impl selection:
  - [ContextFactory.cpp](/home/auro/code/gliden64/src/Graphics/ContextFactory.cpp:8)
  - [Context.cpp](/home/auro/code/gliden64/src/Graphics/Context.cpp:39)
- Vulkan draw/present:
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2296)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2371)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2900)

## 3) Missing/Partial Implementation Areas in RealityVK

### A. Real combiner/shader program behavior is not implemented yet

RealityVK currently uses inferred-key combiner objects (not full backend-compiled combiner programs) instead of mux-accurate generated programs.

Evidence:

- Inferred combiner program class and creation:
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:167) (`VulkanInferredCombinerProgram`)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2497) (`createCombinerProgram`)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2525) (`createDepthFogShader`)
- Program library currently defers combiner creation to a callback factory (no Vulkan combiner compiler):
  - [vulkan_ProgramLibrary.h](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ProgramLibrary.h:136)
- Shader flags are still packet-driven (instead of upstream combiner-generated programs), but now include multiple special-pass controls:
  - [vulkan_DrawShaderConfig.h](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_DrawShaderConfig.h:7)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:582)

Impact:

- Combiner mux semantics are not equivalent to upstream GLSL combiner program generation.
- Some special passes are now Vulkan shader-backed (texrect variants, gamma, FXAA, text, depth-fog), but combiner-driven shader generation is still missing.

### B. Readback baseline exists; broader validation is still pending

RealityVK no longer uses zero-fill dummy readback on the primary path. Pixel and color readers now pull data from Vulkan textures via `TextureStore::readTexture`.

Evidence:

- Readback implementations:
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2302) (`createPixelReadBuffer`)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2405) (`createColorBufferReader`)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2386) / [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2470) (readback debug markers)
- Shared color attachment resolver used by readback and framebuffer paths:
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:814)

Impact:

- Readback is now usable as a local gate signal (no longer an obvious stub).
- Remaining uncertainty is in coverage, not baseline existence: depth readback edge-cases, MSAA/resolve semantics, and multi-attachment scenarios still need targeted validation ROMs.

### C. Capability flags disable important higher-level paths

RealityVK still advertises several advanced features as unsupported, which keeps some framebuffer/depth/blending logic on fallback paths.

Evidence:

- RealityVK feature flags:
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:3834)
- Upstream feature flags:
  - [opengl_ContextImpl.cpp](/home/auro/code/gliden64-upstream/src/Graphics/OpenGLContext/opengl_ContextImpl.cpp:510)
- Feature use sites:
  - [FrameBuffer.cpp](/home/auro/code/gliden64/src/FrameBuffer.cpp:382)
  - [FrameBuffer.cpp](/home/auro/code/gliden64/src/FrameBuffer.cpp:479)
  - [GraphicsDrawer.cpp](/home/auro/code/gliden64/src/GraphicsDrawer.cpp:608)
  - [GraphicsDrawer.cpp](/home/auro/code/gliden64/src/GraphicsDrawer.cpp:710)

Impact:

- Core copy/cache path flags are now enabled (`BlitFramebuffer`, `TextureBarrier`, `ShaderProgramBinary`), so Vulkan exercises more of the normal framebuffer flow.
- Remaining disabled advanced paths (`WeakBlitFramebuffer`, `FramebufferFetchDepth`, `FramebufferFetchColor`, `ImageTextures`, `DualSourceBlending`) still limit depth-compare and blend parity in edge cases.

### D. Remaining API/behavior deltas are narrower now

Key point:

- Previous no-op hooks (`setBlendColor`, `setPolygonOffset`, `textureBarrier`, `isError`, `isFramebufferError`) are now active and wired to Vulkan state/validation paths.
- Current deltas are primarily semantic parity (combiner translation, advanced depth/blend paths), not missing API plumbing for those hooks.

### E. Draw path constraints remain; offscreen/copy robustness improved

Evidence:

- Indexed draw path now supports `UNSIGNED_BYTE`/`UNSIGNED_SHORT`/`UNSIGNED_INT` (no longer hard-limited to `UNSIGNED_SHORT`).
- Offscreen execution still has explicit failure/skip paths (instrumented):
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2631)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2769)
- Attachment selection is now resolver-based (instead of hard-coded `COLOR_ATTACHMENT0`) across key paths:
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:1796) (`blitFramebuffers`)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:2784) (`executeOffscreenDrawPacket`)
  - [vulkan_ContextImpl.cpp](/home/auro/code/gliden64/src/Graphics/VulkanContext/vulkan_ContextImpl.cpp:3406) (present fallback candidate)
- Current Paper Mario smoke remains visually aligned with cached reference:
  - [paper_mario_intro.metrics.json](/home/auro/code/gliden64/build/parity-runs/paper-mario/paper_mario_intro.metrics.json)

Impact:

- Offscreen/copy behavior is less brittle to non-zero color attachments.
- Remaining risk is deeper semantic parity (combiner/special shaders/depth-copy modes), not only “attachment 0” plumbing.

## 4) Practical Interpretation For “What We’re Missing”

If the observed Vulkan output is “zoomed/partial/missing layers,” the most likely missing parity areas now are:

1. Real combiner/shader semantics (currently reduced to simple flag-driven textured/color pipelines).
2. Feature-gated framebuffer/depth paths (`FramebufferFetchDepth`, `ImageTextures`, `DualSourceBlending`).
3. Remaining special-shader edge-case parity and validation (now that texrect/depth-copy/gamma/FXAA/text/depth-fog paths exist).
4. Remaining offscreen edge-cases under complex depth-copy and multipass patterns.

## 5) Current Open Work Order

1. Active now: combiner parity expansion. Move from usage inference + direct-output overrides into more canonical mux patterns (shade/texture/constant blends), then toward mux-accurate translation.
2. Active now: advanced capability-path bring-up. Keep `WeakBlitFramebuffer`/`ImageTextures` enabled by default and iterate on fetch/blend feature support behind explicit experimental gating.
3. Deferred batch (later): shader parity closure + offscreen edge-case closure + runtime validation expansion.
   This batch includes depth-fog/special-pass edge refinement, multipass/depth-copy skip-path cleanup, and broader checkpoint/scenario coverage.

## 6) 2026-03-01 Progress Snapshot

Note:
- Historical entries in this section that report `rmse=0.011766` / `mae=0.000274` came from a stale cached-reference period and should be treated as superseded by item 77 below.

1. Readback is real (texture-backed) and instrumented.
2. Offscreen draw execution now tolerates non-zero color attachment selection.
3. Offscreen depth handling now keeps depth attached on size mismatch by clamping render extent to the shared intersection when depth is actually needed.
4. Blit and present-fallback texture source selection now use the same attachment resolver.
5. `setDrawBuffers` is now tracked per framebuffer and consulted by color-attachment resolution (active attachments preferred before fallback).
6. Depth blit now has explicit failure diagnostics/counters and hardened preconditions (attachment/texture/format/region checks plus normalized depth-copy extents).
7. Paper Mario smoke runner now supports strict depth-copy checks and per-run depth summary JSON artifacts.
8. Paper Mario cached-reference parity is currently within gate on the latest Vulkan build (`rmse=0.011766`, `mae=0.000274` (stale cached reference)).
9. Special-shader contract pass is now partially closed: Vulkan special program hooks no longer use a single dummy type, and color+depth texrect copy now explicitly requests both texture slots (plus copy path `s1/t1` UV initialization for dual-texture sampling).
10. Texrect draw special behavior now has a Vulkan shader-side path for alpha-test and filter mode control (3-point vs standard bilinear) via draw push constants.
11. Color+depth copy special behavior now emits explicit depth-from-texture1 semantics in the Vulkan textured shader path (`gl_FragDepth` from tex1 when flagged).
12. First combiner semantic translation slice is active: canonical `FILL`/`COPY` cycle types now map directly to shader flag intent before generic usage inference.
13. Post/text shader parity moved forward in Vulkan textured shader path:
    - Gamma correction is now handled via a dedicated special flag + push-constant gamma level.
    - FXAA now has a Vulkan shader-side fast edge-filter path controlled by a special flag.
    - Text drawing now uses shader-side text-color uniforms (push constants) with gamma-adjusted glyph intensity.
14. Depth-fog special path is now wired in Vulkan textured shader flow:
    - Dedicated depth-fog special program type and packet flag.
    - Shader-side depth->ZLUT->palette alpha reconstruction with fog RGB push constants.
    - Depth-fog packet binding now pulls `ZLUTTex`/`PaletteTex` descriptor slots in addition to depth texture.
15. Textured Vulkan shader sources now have a local tracked source + regeneration path:
    - [basic_textured.vert](/home/auro/code/gliden64/src/Graphics/VulkanContext/shaders/basic_textured.vert)
    - [basic_textured.frag](/home/auro/code/gliden64/src/Graphics/VulkanContext/shaders/basic_textured.frag)
    - [regenerate_basic_textured_shaders.sh](/home/auro/code/gliden64/scripts/regenerate_basic_textured_shaders.sh)
16. Dummy combiner scaffolding has been removed from Vulkan:
    - `DummyCombinerProgram` replaced by `VulkanInferredCombinerProgram`.
    - Shader-flag canonical `FILL/COPY` handling is now key-driven instead of dummy-type-driven.
    - Dead combiner-builder stub API (`isCombinerProgramBuilderObsolete`/`resetCombinerProgramBuilder`) has been removed from the Vulkan-only path.
17. Previous Vulkan no-op state hooks were replaced with active behavior:
    - `setBlendColor` now feeds Vulkan dynamic blend constants.
    - `setPolygonOffset` + `POLYGON_OFFSET_FILL` now feed Vulkan dynamic depth bias.
    - `textureBarrier` now issues a conservative synchronization point and dirties cached state.
    - `resetShaderProgram` now dirties cached state to force rebinding.
    - `isError`/`isFramebufferError` now report runtime/backend validity instead of constant `false`.
18. Capability parity moved forward:
    - `BlitFramebuffer` and `ShaderProgramBinary` are now enabled in Vulkan capability reporting.
    - `TextureBarrier` stays enabled and active through explicit Vulkan-side sync/state dirties.
19. Vulkan indexed draw parity moved forward:
    - `drawTriangles` now accepts `UNSIGNED_BYTE`, `UNSIGNED_SHORT`, and `UNSIGNED_INT` element types.
20. Vulkan combiner usage inference moved forward:
    - `VulkanInferredCombinerProgram` now infers `usesLOD()` from combiner LOD selectors and `usesHwLighting()` from combiner-key HW-light capability + shade usage.
21. Default-target depth blit contract was hardened:
    - `blitFramebuffers` now avoids partial side-effects on unsupported default-depth blit requests and cleanly falls back to higher-level copy paths.
22. Framebuffer validation now accepts depth-only targets:
    - `isFramebufferError()` no longer rejects valid depth-only FBO configurations when color attachments are intentionally absent.
23. Runtime validation asset hygiene update:
    - `tests/smoke/scenarios_paper_mario_runtime.tsv` checkpoint paths were updated to the new runtime root under `~/code/mupen`.
24. Runtime capture determinism for state-based checkpoints was improved:
    - `scripts/paper_mario_smoke_runner.sh` now enforces deterministic paused/load/paused flow.
    - Added post-load settle stepping (`REALITYVK_SMOKE_SETTLE_FRAMES_AFTER_LOAD`, default `1`) before scenario stepping/capture.
    - Repeated `pm_boot_title_003` runs are now stable in both Reference and Candidate paths under repeat smoke checks.
25. Runtime checkpoint parity status (Paper Mario) is currently clean on the maintained set:
    - `tests/smoke/scenarios_paper_mario_runtime.tsv` passes Reference vs Candidate comparison.
    - No active checkpoint-level divergence is currently reported on that set.
26. Strict intro gate remains clean after the latest parity/refactor batch:
    - `REALITYVK_PM_REQUIRE_NO_DEPTH_BLIT_FAIL=1`
    - `REALITYVK_PM_REQUIRE_DEPTH_BLIT_STATS=1`
    - Current metrics: `rmse=0.011766`, `mae=0.000274` (stale cached reference), `max_abs_diff=1.000000`.
27. Combiner semantic inference moved one step closer to upstream encoding behavior:
    - `VulkanInferredCombinerProgram` now uses combiner input expansion tables (encoded mux selectors -> `G_GCI_*` inputs) instead of narrow numeric heuristics.
    - Tile/shade/LOD usage flags now derive from expanded combiner inputs across both cycles.
28. Combiner semantic application moved one step beyond usage inference:
    - Vulkan draw packet assembly now detects direct solid-color mux cases (`(A-B)*0 + D`) for primitive/environment outputs.
    - In those canonical cases, packet colors are overridden from `gDP.primColor`/`gDP.envColor` and texture sampling is disabled for the draw.
    - Paper Mario strict intro gate remains passing after this change.
29. Combiner direct-output handling expanded to additional canonical cases:
    - Direct shade (`(A-B)*0 + SHADE`) now forces untextured shade output.
    - Direct texel outputs (`(A-B)*0 + TEXEL0/TEXEL1`) now force single-texture output without shade modulation.
    - Paper Mario strict intro gate remains passing with these additions.
30. Advanced capability bring-up is now split into default-safe vs experimental paths:
    - `WeakBlitFramebuffer` and `ImageTextures` are enabled by default when Vulkan core is ready.
    - `N64DepthWithFbFetchDepth`, `FramebufferFetchColor`, and `DualSourceBlending` are gated behind `REALITYVK_VK_EXPERIMENTAL_FETCH_BLEND=1`.
    - Combined experimental fetch/blend paths still diverge from cached reference and remain opt-in until semantics are closed.
31. Combiner canonical-pattern expansion moved forward:
    - Added constant*texel modulation detection for primitive/environment with texel0/texel1 (`(A-B)*C + D` canonical zero-B/zero-D modulate forms).
    - Vulkan packets now map these cases to textured+shade output with constant color injected via vertex color.
32. Combiner alpha-path handling moved forward for direct textured outputs:
    - Added direct-alpha interpretation for canonical direct-texture color cases (`texel` color with direct alpha source).
    - Direct alpha sources `primitive/env/one/zero/shade` now map to Vulkan textured draws via alpha-only shade modulation (RGB preserved, alpha sourced correctly for these canonical cases).
33. Capability bring-up diagnostics are now per-feature:
    - Experimental flags can now be toggled independently via:
      - `REALITYVK_VK_EXPERIMENTAL_FB_FETCH_DEPTH=1`
      - `REALITYVK_VK_EXPERIMENTAL_FB_FETCH_COLOR=1`
      - `REALITYVK_VK_EXPERIMENTAL_DUAL_SOURCE=1`
    - Strict behavior toggles are separated from capability toggles:
      - `REALITYVK_VK_STRICT_FB_FETCH_COLOR=1`
      - `REALITYVK_VK_STRICT_DUAL_SOURCE_BLEND=1`
    - Current Paper Mario intro signal:
      - baseline (no experimental toggles): `rmse=0.011766`, `mae=0.000274` (stale cached reference)
      - experimental capability toggles (default compatibility behavior): same as baseline
      - strict fetch/blend behavior (`STRICT_FB_FETCH_COLOR` + `STRICT_DUAL_SOURCE_BLEND`): `rmse=0.088289`, `mae=0.012818`
34. Dual-source blending backend plumbing moved forward:
    - Vulkan blend-factor mapping now includes `SRC1_COLOR`, `ONE_MINUS_SRC1_COLOR`, `SRC1_ALPHA`, `ONE_MINUS_SRC1_ALPHA`.
    - Vulkan capability for `DualSourceBlending` now requires both explicit experimental opt-in and device support (`dualSrcBlend` feature enabled at device creation).
35. Capability-path stabilization layer is now in place for incremental bring-up:
    - When experimental fetch/blend capability flags are enabled without strict toggles, RealityVK keeps ordinary blending behavior to preserve the stable Paper Mario gate path.
    - Strict behavior can be turned on explicitly for targeted debugging and semantic implementation passes.
36. Combiner canonical-pattern expansion now includes constant*shade cases:
    - Added primitive/environment * shade detection for canonical zero-B/zero-D modulate forms.
    - Vulkan packets now map these to untextured shade draws with per-vertex constant scaling (preserving shade variation while applying primitive/environment modulation).
37. Combiner alpha handling for canonical constant*texel modulate cases was tightened:
    - Direct-alpha overrides are now applied on these modulate paths as well (not only direct-texel output paths), so canonical texel-color + direct alpha sources route through the same textured alpha override logic.
38. Dual-source shader plumbing advanced one step:
    - Vulkan textured fragment shader now emits an explicit secondary output (`location=0,index=1`) so the pipeline contract exists for dual-source blend factors.
    - Current secondary output is still neutral (`vec4(0.0)`), so strict experimental dual-source behavior remains in the same divergence band and still requires full shader-blender semantic implementation.
39. Strict-path isolation signal is now clearer:
    - `REALITYVK_VK_EXPERIMENTAL_DUAL_SOURCE=1` + `REALITYVK_VK_STRICT_DUAL_SOURCE_BLEND=1` alone yields `rmse=0.088289`, `mae=0.012818`.
    - `REALITYVK_VK_EXPERIMENTAL_FB_FETCH_COLOR=1` + `REALITYVK_VK_STRICT_FB_FETCH_COLOR=1` alone yields the same metrics.
    - This indicates both strict-path divergences currently collapse to the same underlying missing semantic layer (mux-driven shader blending / framebuffer-fetch-equivalent behavior), not separate independent regressions on Paper Mario intro.
40. Strict blend-mux packet plumbing is now explicit in Vulkan draw packets:
    - Per-draw packed blend mux state (`c1/c2` mux selectors + force/cycle/cvg parameters) is now attached to packets and forwarded via fragment push constants when strict blend mode is active.
    - Strict blend-mux is disabled for texrect/special-pass programs to avoid contaminating non-combiner passes.
41. Vulkan shader now has a strict blend-mux evaluation stage:
    - Added shader-side mux unpack/select helpers and a first strict blender pass that computes secondary output factors from packed RDP blend mux state.
    - Current strict implementation intentionally keeps primary color on combiner output and focuses on destination-factor output while full shader-blender parity is still in progress.
42. Combiner canonical-pattern expansion now includes additive constant forms:
    - Added canonical detection for `(source - 0) * 1 + constant` forms (primitive/environment with source in texel0/texel1/shade).
    - Vulkan now maps these to source sampling plus shader-side constant add (clamped), using packet-provided constant color.
43. Current Paper Mario intro signal after this batch:
    - baseline gate remains unchanged: `rmse=0.011766`, `mae=0.000274` (stale cached reference).
    - strict dual-source / strict fb-fetch / combined strict paths remain at `rmse=0.088289`, `mae=0.012818`.
    - Result: implementation surface for strict paths grew, but measurable strict-path parity has not improved yet on the intro gate.
44. Strict blend-mux approximation trial (no gain):
    - Tried replacing neutral `LAST_FRAG` assumptions with a self-color surrogate (`lastFragColor := current combiner output`) in strict blend evaluation.
    - Paper Mario strict metrics remained unchanged (`rmse=0.088289`, `mae=0.012818`), so this approximation does not move parity and should be treated as temporary scaffolding only.
45. Combiner coverage instrumentation is now available for targeted parity work:
    - Added optional unresolved-combiner tracking under `REALITYVK_VK_DEBUG_COMBINER_COVERAGE=1`.
    - This enables data-driven prioritization of the next canonical combiner translation slices without changing default behavior.
46. Combiner canonical coverage on Paper Mario intro was expanded from collected unresolved keys:
    - Added two-cycle post-modulate handling for canonical forms where cycle1 is `(COMBINED - 0) * (PRIMITIVE|ENV) + 0` with alpha passthrough (`COMBINED`), including:
      - base cycle0 `texel0*shade` with alpha `texel0Alpha*shadeAlpha`
      - base cycle0 `texel0*shade` with alpha `texel0Alpha`
      - base cycle0 direct `shade`
    - Added direct textured-alpha scale detection for `(TEXEL - 0) * (PRIM_ALPHA|ENV_ALPHA|SHADE_ALPHA|1) + 0` cases.
47. Shader-side shade control now supports RGB-only modulation:
    - Added `kShadeRGBOnly` so RealityVK can modulate texture RGB with shade/constant while leaving texture alpha unchanged in canonical cases that require `alpha := texelAlpha`.
    - This is now used by the new two-cycle post-modulate combiner path for the `texel0*shade` + `alpha texel0` pattern.
48. Paper Mario combiner coverage signal is now clean under the intro gate path:
    - With `REALITYVK_VK_DEBUG_COMBINER_COVERAGE=1`, baseline and strict+experimental intro runs now report no unresolved combiner keys.
    - Coverage instrumentation now skips `COPY`/`FILL` cycle keys to avoid non-actionable noise.
49. Strict blend-mux hygiene update:
    - Removed the `REALITYVK_VK_STRICT_LAST_FRAG_MODE` surrogate path because matrix testing (`0..4`) produced identical metrics and no measurable gain.
    - Strict-path intro metrics remain unchanged (`rmse=0.088289`, `mae=0.012818`), so remaining divergence is still attributed to missing real framebuffer-fetch / shader-blender semantics rather than the removed surrogate knob.
50. Strict blend telemetry and dominant-path targeting were added:
    - Added optional strict blend mux coverage logging under `REALITYVK_VK_DEBUG_STRICT_BLEND_MUX=1`.
    - Paper Mario intro strict path is currently dominated by a very small mux set:
      - `mux1=0x8c mux2=0x8c params=0x97`
      - `mux1=0x8c mux2=0x10 params=0x97`
      - `mux1=0x8c mux2=0x10 params=0xd7`
51. Strict blender equations were aligned closer to upstream for the active dual-source path:
    - Updated Vulkan strict blend shader equations to use upstream-style mux setup (`muxA[0]=clampedColor.a`, `muxB[0]=1.0-muxa`, cycle2 `muxF[0]=dstFactor1`).
    - Kept primary output on combiner/base color for now (to avoid darkening regressions from missing non-dummy last-fragment color).
52. Strict parity signal improved after the shader-blender equation fix:
    - previous strict combined path: `rmse=0.088289`, `mae=0.012818`
    - current strict combined path: `rmse=0.065098`, `mae=0.005897`
    - baseline gate remains unchanged: `rmse=0.011766`, `mae=0.000274` (stale cached reference)
53. Follow-up parameter sweeps completed (no further gain over current setting):
    - `REALITYVK_VK_STRICT_SRC_MIX` sweep favored `0.0` (combiner/base primary).
    - `REALITYVK_VK_STRICT_ALPHA_SCALE` sweep favored `1.0` (upstream-equivalent default).
54. Strict blend-state telemetry now confirms dominant Paper Mario strict packets share one Vulkan blend contract:
    - Added optional strict blend state logging under `REALITYVK_VK_DEBUG_STRICT_BLEND_STATE=1`.
    - Dominant strict packets use:
      - `srcColor=ONE`, `dstColor=SRC1_COLOR`
      - `srcAlpha=ONE`, `dstAlpha=SRC1_ALPHA`
    - This narrows strict divergence to shader-side mux/secondary-factor semantics rather than per-packet blend-state variance.
55. Strict packet filtering is now extended with blend-params gating:
    - Added `REALITYVK_VK_STRICT_ONLY_PARAMS=<int|hex>` to isolate strict behavior by packed blend params.
    - Intro isolation runs show both dominant params (`0x97`, `0xd7`) participate in the current strict signal; strict-on-both remains better than strict-on-either alone.
56. Present-vs-offscreen strict routing is now confirmed for Paper Mario intro:
    - With `REALITYVK_VK_DEBUG_PRESENT=1` + `REALITYVK_VK_DEBUG_PRESENT_PACKETS=1`, present packet logs show no strict-flag packets on this gate path.
    - Strict behavior is exercised in offscreen draw execution for this scenario.
57. Offscreen strict contribution was isolated:
    - Added `REALITYVK_VK_STRICT_OFFSCREEN_DISABLE=1` diagnostic gate.
    - Disabling strict on offscreen packets regresses strict metrics back to `rmse=0.088289`, `mae=0.012818`.
    - Current best strict checkpoint (`rmse=0.065098`, `mae=0.005897`) therefore comes from offscreen strict blending, not present-pass strict packets.
58. Real offscreen last-fragment texture fetch prototype was tested and shelved:
    - A strict offscreen snapshot/fetch experiment (shader-side composition with captured last-fragment color) was evaluated.
    - Result regressed heavily on intro (`rmse=0.324029`, `mae=0.160195`) and was removed from the active path.
    - Conclusion: the missing semantics are not solved by naive snapshot+compose replacement; next work should focus on tighter mux-accurate decomposition on the current dual-source path.
59. Paper Mario compare workflow now has a one-command viewer path:
    - `scripts/paper_mario_compare_view.sh` runs parity capture, builds a labeled compare image, closes the prior viewer window, and opens the latest image.
60. Current stock-baseline signal (Paper Mario intro, default Vulkan path) is a large structural mismatch:
    - `rmse=0.458848`, `mae=0.361386`.
    - Candidate non-black coverage is much lower than reference (`~36.37%` vs `~94.48%`).
    - Missing-content ratio where reference has content but candidate is black is `~59.28%`.
61. Image-diff and spatial analysis point to geometry placement loss (not only combiner/color mismatch):
    - Artifacts:
      - `build/parity-runs/paper-mario/paper_mario_intro.analysis.deep.json`
      - `build/parity-runs/paper-mario/paper_mario_intro.missing_extra_overlay.png`
      - `build/parity-runs/paper-mario/paper_mario_intro.mask_triptych.png`
    - Edge-energy in candidate is much lower than reference (`x ratio ~0.33`, `y ratio ~0.43`), consistent with missing composed layers.
62. Offscreen debug traces isolate a high-confidence normalization issue:
    - Offscreen rect packets frequently arrive with pre-pos in ~`0..320 x 0..240` space.
    - They are normalized into NDC against a much larger extent (`1440x1080`), yielding corner-constrained output (`x/y` often near `[-1.0, -0.56]` for fullscreen-ish rects).
    - Trace artifacts:
      - `build/parity-runs/paper-mario/paper_mario_intro.vk_offscreen_debug_summary.json`
      - `/tmp/realityvk-paper-mario-vk-offscreen.log`
63. A targeted diagnostic toggle confirms this path materially affects parity:
    - Added debug env gate: `REALITYVK_VK_DISABLE_FORCE_RASTER_RECT_TRANSFORM=1`.
    - With default path: `rmse=0.458848`, `mae=0.361386`, non-black `0.363709`.
    - With toggle enabled: `rmse=0.372826`, `mae=0.224774`, non-black `0.951944`.
    - This does not solve parity alone, but it strongly indicates offscreen rect position normalization is currently over-constraining render coverage.
64. Immediate implementation direction from this analysis:
    - Replace current offscreen rect normalization basis with a logical target-space basis (not raw render extent), likely derived from active VI/RDP viewport domain for that packet path.
    - Keep `REALITYVK_VK_DISABLE_FORCE_RASTER_RECT_TRANSFORM` as a temporary diagnostic switch until the corrected basis is fully validated.
    - Re-run the same Paper Mario compare gate after this normalization fix, then return to combiner/blend semantic deltas on the now-correct coverage.
65. Offscreen normalization fix was applied as default behavior:
    - Offscreen raster-rect transform is no longer forced by default in the offscreen execute path.
    - Legacy behavior is still available behind a diagnostic env:
      - `REALITYVK_VK_FORCE_OFFSCREEN_RASTER_RECT_TRANSFORM=1`
66. Current Paper Mario intro baseline after the offscreen fix remains a semantic/compositing mismatch (coverage mostly restored):
    - `rmse=0.372826`, `mae=0.224774` (default before latest combiner tuning below).
    - Candidate non-black coverage is close to reference (~95% vs ~94%), but mean luma is still materially higher on candidate.
67. Combiner override isolation identified one regressing heuristic family:
    - Runtime sweep across override families showed only `modulate` override affects this gate path, and it worsens parity when enabled.
    - Best result in the sweep:
      - `disable_modulate`: `rmse=0.360191`, `mae=0.215409`
    - No measurable change from toggling direct/add/alpha-scale/two-cycle-post overrides for this frame.
68. Combiner modulate override contract was promoted to default-on with opt-out:
    - New behavior:
      - `REALITYVK_VK_DISABLE_COMBINER_MODULATE_OVERRIDE=1` to disable modulate override for diagnostics.
    - On the refreshed stock-reference gate, keeping modulate override enabled performs better than disabling it:
      - enabled (default): `rmse=0.280210`, `mae=0.183982`
      - disabled: `rmse=0.365333`, `mae=0.220497`
69. Post-process special-pass toggles were added for isolation and tested:
    - `REALITYVK_VK_DISABLE_SPECIAL_GAMMA=1`
    - `REALITYVK_VK_DISABLE_SPECIAL_FXAA=1`
    - On this Paper Mario intro frame, toggling either/both has no measurable effect on parity metrics, so they are not the active blocker here.
70. Compare-view artifact selection was corrected to remove stale/raw-orientation ambiguity:
    - `scripts/paper_mario_compare_view.sh` now resolves reference/candidate sources after parity capture and prefers normalized PNG outputs generated by parity.
    - OpenGL panel input is now normalized to PNG as well when available.
    - This removes a stale-path bug where pre-run source selection could compose from old or raw `.ppm` files.
71. RT-layer orientation tracing for Paper Mario intro was expanded and run on handle `13`:
    - Added detailed RT trace payload for both writes and reads:
      - per-packet position and texcoord ranges
      - first 4 vertices (`x/y/s/t`) for direct orientation checks
      - sampled source handles for writes (`sample0/sample1`) plus RT provenance flags
    - Traces confirm present reads of handle `13` are consistent and upright in mapping:
      - top-screen vertices use low `t`, bottom-screen vertices use high `t`
      - read packets are stable fullscreen texrects with `mux=0x0000000000000000`
72. Historical orientation sweep note (stale-reference period):
    - The `rmse=0.214191` baseline and “RT flip regresses” result were recorded before the refreshed true-stock reference path and are superseded by item 78.
    - Offscreen flip toggles (`...OFFSCREEN_TEXRECT_Y`, `...OFFSCREEN_TEXRECT_POS_Y`, `...OFFSCREEN_FILLRECT_POS_Y`, `...OFFSCREEN_TRIANGLE_Y`) still show no measurable change on this gate frame.
73. Updated orientation conclusion for the Paper Mario intro gate:
    - Capture-path orientation handling is explicit and deterministic.
    - With true-stock reference capture, RT-texrect Y correction is a real contributor (item 78), but does not fully close parity.
    - Remaining visual delta is now primarily semantic/compositing, not a single unresolved capture-policy flip bug.
74. Handle-scoped offscreen texrect flip probe was added and tested:
    - New diagnostic:
      - `REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TEXRECT_Y=1`
      - `REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TEXRECT_HANDLE=<texture_handle>`
    - Sweeps across active sampled handles feeding RT `13` (`21`, `52`, `53`, `64`, `22`..`27`) produced no measurable metric change on this frame.
    - This further supports shelving global/handle-specific Y-flip hypotheses for the current Paper Mario intro blocker.
75. Agent capture orientation contract is now explicit and request-only:
    - Added optional `flip_y` support to `framebuffer_dump` / `framebuffer_dump_preset` in `mupen64plus-ui-console` and exposed it via `agentctl.py` (`--flip-y` / `--no-flip-y`).
    - Default remains unflipped (`flip_y=0`); smoke/parity scripts now only request flip when explicitly set (`REALITYVK_SMOKE_DUMPFB_FLIP_Y`, `REALITYVK_PM_DUMPFB_FLIP_Y`).
    - Validation:
      - `flip_y=1` candidate is an exact vertical inversion of `flip_y=0` candidate (`rmse=0.0` after `flipud`), confirming deterministic explicit control with no automatic normalization.
76. Live-window orientation probe (RealityVK, Paper Mario) confirmed which capture path matches what appears on screen:
    - Probe artifacts:
      - `build/parity-runs/paper-mario/live_orient_probe_noload_20260301_134530/`
      - includes `live_window.png`, `dumpfb_flip0.png`, `dumpfb_flip1.png`, `core_screenshot.png`
    - Pixel-identity results:
      - `live_window == dumpfb_flip1` (`rmse=0.0`)
      - `live_window == core_screenshot` (`rmse=0.0`)
      - `live_window == flipud(dumpfb_flip0)` (`rmse=0.0`)
    - Outcome:
      - default `dumpfb` orientation is vertically inverted relative to live window for this Vulkan path.
      - Paper Mario parity default now explicitly requests `flip_y=1` via agent command so comparison captures match live-screen orientation.
      - Reference cache in parity is now keyed by capture method/orientation (`dumpfb_flip0`, `dumpfb_flip1`, `screenshot`) to avoid stale mixed-orientation baselines.
77. Reference baseline capture is now pinned to upstream core while candidate stays on RealityVK core:
    - `paper_mario_parity.sh` now uses:
      - reference core default: `/home/auro/code/mupen/mupen64plus-core-upstream/projects/unix/libmupen64plus.so.2`
      - candidate core default: `/home/auro/code/mupen/mupen64plus-core/projects/unix/libmupen64plus.so.2`
      - reference capture default: screenshot (`REALITYVK_PM_REFERENCE_FORCE_SCREENSHOT_CAPTURE=1`)
    - Reason: upstream GLideN64 plugin currently crashes on custom core startup (`SIGSEGV` near `CoreVideoInitCommand`) but runs on upstream core.
    - Refreshed intro metrics with this true reference path:
      - `rmse=0.345479`, `mae=0.257753`, `max_abs_diff=1.000000`
    - Interpretation: Vulkan parity is still a large structural mismatch; old near-zero metrics from stale reference cache are invalid as quality signals.
78. RT-texrect orientation fix is now enabled by default in Vulkan present-path packet normalization:
    - Behavior:
      - Texrect packets that sample render-target textures now apply Y-flip correction by default during present packet normalization.
      - Debug behavior remains available; default correction can be disabled with `REALITYVK_VK_DISABLE_RT_TEXRECT_Y_FLIP=1`.
    - Paper Mario intro impact (true stock reference path):
      - before: `rmse=0.345479`, `mae=0.257753`
      - after: `rmse=0.283516`, `mae=0.186630`
    - Interpretation:
      - Orientation mismatch in RT-sampled texrect composition was a real contributor to the delta.
      - Remaining delta is still substantial and now shifts priority toward combiner/blend/compositing semantics.
79. Parity run provenance is now explicit via sidecar artifact:
    - `paper_mario_parity.sh` writes `<scenario>.capture-context.json` under run root with:
      - reference plugin/core/capture method/path
      - candidate plugin/core/capture method/path
    - This removes ambiguity when comparing historical parity metrics across runtime/capture configuration changes.
80. Final-layer compositing tracing is now targeted and labeled:
    - New focused trace envs:
      - `REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_HANDLES=<comma-separated handles>`
      - `REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_LIMIT=<n>`
    - Present-path now emits `VK composite trace(final): ...` only for packets that sample those handles, with labels such as `final_fullscreen_texrect` and explicit flip/strict/blend metadata.
    - Paper Mario intro observation with `handle=13`:
      - dominant final-layer packets are fullscreen texrect copies (`mux=0x0`, cycle `0`) with `strictBlend=0` and `blendEnabled=0`.
81. RT present-flip semantics were tightened for multi-texture packets:
    - RT Y-flip now applies per sampled RT slot (`t0` for slot0, `t1` for slot1) instead of always flipping both UV sets.
    - Intro metrics are unchanged by this tightening (`rmse=0.283516`, `mae=0.186630`), so it is a correctness/hygiene fix without current-signal regression.
82. Strict fetch/blend paths were revalidated on the current baseline:
    - `STRICT_FB_FETCH_COLOR`: `rmse=0.320348`, `mae=0.214847`
    - `STRICT_DUAL_SOURCE_BLEND`: `rmse=0.320349`, `mae=0.215135`
    - Combined strict path: `rmse=0.320349`, `mae=0.215135`
    - Conclusion unchanged: strict paths still regress this gate and are not merge-up candidates yet.
83. Final RT present-quad canonicalization is now default in Vulkan present path:
    - Fullscreen texrect packets that sample RT-backed textures now normalize to canonical fullscreen clip-space corners (`[-1,1]` quad) before present encode.
    - Diagnostic opt-out:
      - `REALITYVK_VK_DISABLE_CANONICAL_FINAL_RT_TEXRECT_POS=1`
    - Paper Mario intro impact (true stock reference path):
      - default canonicalized: `rmse=0.269621`, `mae=0.173193`
      - opt-out (legacy positions): `rmse=0.280210`, `mae=0.183982`
    - Strict-path recheck after canonicalization:
      - `STRICT_FB_FETCH_COLOR`: `rmse=0.326053`, `mae=0.219866`
      - `STRICT_DUAL_SOURCE_BLEND` / combined strict: `rmse=0.325999`, `mae=0.220125`
84. Final RT present-quad canonical UV mapping is now default in Vulkan present path:
    - Fullscreen texrect packets that sample RT-backed textures now normalize UVs to canonical fullscreen mapping (`s=0..1`, `t` orientation derived from RT flip state) before present encode.
    - Diagnostic opt-out:
      - `REALITYVK_VK_DISABLE_CANONICAL_FINAL_RT_TEXRECT_UV=1`
    - Paper Mario intro impact on top of item 83:
      - default (pos+uv canonical): `rmse=0.259169`, `mae=0.164378`
      - UV opt-out (`DISABLE_CANONICAL_FINAL_RT_TEXRECT_UV=1`): `rmse=0.269621`, `mae=0.173193`
      - pos+uv opt-out (`DISABLE_CANONICAL_FINAL_RT_TEXRECT_POS=1` + `..._UV=1`): `rmse=0.280210`, `mae=0.183982`
    - Interpretation:
      - The remaining “border vs main layer orientation/layout” issue is tied to final RT sampling window semantics, not only offscreen writer Y flips.
      - RT13 border content is produced in offscreen passes (`fbo=14`, dominant writer muxes including `0x3ffffffffffe793c`) and consumed by final fullscreen texrect reads in present.

## 7) Roadmap Status

1. Done:
   - Depth-copy closure baseline is in place with strict candidate gating (`REALITYVK_PM_REQUIRE_NO_DEPTH_BLIT_FAIL=1`, `REALITYVK_PM_REQUIRE_DEPTH_BLIT_STATS=1`) and per-run depth summaries.
   - Runtime capture determinism fixes are in place for state-based checkpoints.
   - Parity pipeline now captures a true stock-reference baseline without GL startup crashes by routing reference captures through upstream core.
2. In progress:
   - Paper Mario intro parity against refreshed stock reference remains the active quality signal (`rmse=0.259169`, `mae=0.164378`) after RT-texrect orientation plus final-quad canonical position/UV normalization.
   - Combiner parity foundation: extending canonical semantic translation beyond current `FILL`/`COPY` and direct-output cases.
   - Capability bring-up (`#3`): default-safe features are on; risky fetch/blend features are under explicit per-feature gating while semantic support is implemented.
   - `#3` strict-path status on the refreshed baseline remains regressed (`~0.320` RMSE / `~0.215` MAE), so strict fetch/blend stays exploratory only.
   - Strict blend bring-up has packet+shader infrastructure and targeted telemetry, but current strict semantics are not parity-safe for Paper Mario intro.
   - Offscreen coverage-collapse blocker is closed for the current gate path; active blocker is now semantic/compositing parity in combiner/blend behavior (coverage mostly restored but luma/composition still off).
3. Next:
   - Deferred batch (`#1/#4/#5`): shader edge cases, offscreen skip-path closure, and broader runtime validation checkpoints/scenes.
   - Continue capability bring-up by reducing experimental-path diffs (`FB_FETCH_COLOR` / `DUAL_SOURCE`) and promoting stable paths out of experimental gating.
