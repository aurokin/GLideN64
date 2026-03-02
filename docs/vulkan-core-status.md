# RVK2 Core Status (2026-03-02)

## Snapshot

- Runtime execution path: `rvk2` only.
- Build target policy: Mupen64Plus-only plugin integration.
- Smoke contract: Vulkan-only.
- Comparison contract: upstream `GLideN64` reference vs local `RealityVK` candidate.
- Trace contract: strict schema-v1 replay (`S=72`, `R=71`, `W=109`).
- Texture replacement contract: `.hts` (`RKVHTS1`) only.
- Current blocker: runtime output is still unstable (black/noise), now exposed after full cutover cleanup.

## Roadmap Progress

Estimated total completion: **~99%**.

| Phase | Status | Notes |
| --- | --- | --- |
| A: Contracts and Determinism | Done | Trace schema, replay, and gate integration are stable and strict. |
| B: Semantic Pipeline Backbone | Done | Command -> semantic -> raster -> render-work -> submission flow is RVK2-native. |
| C: Core Rendering Correctness | Done | Deterministic synthetic correctness closure landed for scoped contract. |
| D: VI and Presentation | Done | Register-driven VI model is implemented and covered by tests. |
| E: Texture Replacement | Done | Deterministic replacement store, `.hts` IO, control tooling, and bounds policy are landed. |
| F: Cutover and Deletion | In progress | Runtime/config/script/doc path cleanup is nearly complete; active work is debugging RVK2 present behavior. |

## Remaining Work

1. Debug and fix RVK2 present output instability (black/noise).
2. Refresh maintained parity/trace fixtures after the strict single-path cleanup.
3. Remove any additional dead code found during output-debug pass.

## Cleanup Landed in This Sweep

1. Removed non-Mupen plugin API branches and dead framebuffer list export plumbing.
2. Removed remaining compile-time references to deleted Windows/UI compatibility surfaces.
3. Renamed remaining old-path runtime/debug terminology in active Vulkan paths.
4. Simplified smoke/parity scripts to deterministic dumpfb capture flow.
5. Restricted smoke backend support to Vulkan only.
6. Set upstream `GLideN64` as default parity reference target.
7. Condensed and rebuilt maintainer docs into a minimal canonical set.

## Next Debug Session Entry Points

- `src/Graphics/VulkanContext/vulkan_ContextImpl.cpp`
- `src/Graphics/VulkanContext/vulkan_PacketNormalize.cpp`
- `src/Graphics/RealityVK2/rvk2_Executor.cpp`
- `scripts/paper_mario_parity.sh`
- `build/parity-runs/paper-mario/`
