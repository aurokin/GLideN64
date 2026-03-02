# RealityVK (RVK2 Vulkan N64 Core)

RealityVK in this branch is a Vulkan-only N64 video plugin rewrite.

## Scope

- Runtime renderer path: `rvk2` only.
- Maintained product features:
  - 4:3 and 16:9 output scaling.
  - Hi-res texture packs.
  - `.hts` cache support.
- Comparison target for parity tools: upstream `GLideN64` only.

## Current State

- Phases A-E are complete.
- Phase F cleanup is in final deletion/debugging.
- Current blocking runtime issue: present output is unstable (black/noise), which is the next debugging target.

## Build

```bash
cmake -S src -B build/release-vulkan-smoke -DCMAKE_BUILD_TYPE=Release
cmake --build build/release-vulkan-smoke -j$(nproc)
```

## Required Gate

```bash
./scripts/local_gate.sh
```

## Gate with Smoke + Trace Replay

```bash
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
```

## Core Maintainer Commands

```bash
# Paper Mario parity (reference: upstream GLideN64, candidate: RealityVK)
./scripts/paper_mario_parity.sh

# Deterministic Vulkan smoke baseline checks
./scripts/local_smoke.sh

# Visual compare panel
./scripts/paper_mario_compare_view.sh

# Update Vulkan smoke baseline (intentional behavior changes only)
REALITYVK_SMOKE_UPDATE_BASELINES=1 ./scripts/local_smoke.sh

# Refresh parity reference capture cache
REALITYVK_PM_REFRESH_REFERENCE=1 ./scripts/paper_mario_parity.sh

# Replay validation for a captured packet trace
python3 scripts/rvk2_packet_trace_replay.py \
  --input build/local-gate/rvk2.packet.tsv \
  --json-out build/local-gate/rvk2.packet.replay.json \
  --jobs 0 --strict
```

## Key Paths

- `src/Graphics/RealityVK2/`: RVK2 command/semantic/raster/render-work/submission/executor pipeline.
- `src/Graphics/VulkanContext/`: Vulkan frontend/context integration.
- `scripts/`: local gate, parity, smoke, trace tools.
- `tests/smoke/`: deterministic scenario manifest and Vulkan baselines.
- `docs/`: concise maintainer docs.

## Docs

- `docs/README.md`
- `docs/vulkan-core-status.md`
- `docs/local-ci.md`
- `docs/references/n64/README.md`

## License

GPLv2 (`LICENSE`).
