# RealityVK (Vulkan-First N64 Video Core)

RealityVK is a Vulkan-first N64 video plugin fork focused on rebuilding the renderer around explicit N64 semantics and deterministic validation.

This branch is not preserving legacy GLideN64 architecture as a constraint. The target is a cleaner and more accurate Vulkan core (`rvk2`).

## Current Direction

1. Rewrite toward `rvk2` semantic pipeline (`command -> draw semantic -> raster op -> render work -> submission -> executor`).
2. Keep feature scope tight and correctness-driven.
3. Treat determinism and replayability as first-class quality gates.

## Scope Policy

Kept product features:

1. 4:3 and 16:9 output scaling.
2. Hi-res texture pack support.
3. `.hts` texture cache support.

Everything else is optional and must justify complexity against correctness.

## Repository Status

1. Vulkan-only branch for renderer execution paths.
2. `rvk2` scaffolding, trace schema, replay validator, and local gate integration are active.
3. Phases A-E are landed; remaining roadmap work is cutover/deletion, parity closure, and strictness tightening.

For live status, see:

1. `docs/vulkan-core-status.md`
2. `docs/vulkan-core-rebuild-plan.md`

## Quick Start (Linux)

### Build

```bash
cmake -S src -B build/release-vulkan-smoke \
  -DCMAKE_BUILD_TYPE=Release \
  -DMUPENPLUSAPI=ON

cmake --build build/release-vulkan-smoke -j$(nproc)
```

### Required local gate

```bash
./scripts/local_gate.sh
```

### Gate with smoke + rvk2 replay

```bash
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
```

### Control replay CPU workers

```bash
REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS=0 ./scripts/local_gate.sh
```

`0` means auto/all cores. Use `1` for single-process replay.

## Runtime / Validation Commands

```bash
# Paper Mario parity compare
./scripts/paper_mario_parity.sh

# Deterministic smoke scenarios
./scripts/local_smoke.sh

# Visual compare viewer for parity outputs
./scripts/paper_mario_compare_view.sh

# Refresh Vulkan smoke baseline checksums (intentional changes only)
REALITYVK_SMOKE_BACKENDS=Vulkan REALITYVK_SMOKE_UPDATE_BASELINES=1 ./scripts/local_smoke.sh

# Refresh Paper Mario reference capture cache
REALITYVK_PM_REFRESH_REFERENCE=1 ./scripts/paper_mario_parity.sh
```

## rvk2 Trace Replay Tool

`rvk2` packet traces can be replay-validated offline:

```bash
python3 scripts/rvk2_packet_trace_replay.py \
  --input build/local-gate/rvk2.packet.tsv \
  --json-out build/local-gate/rvk2.packet.replay.json \
  --jobs 0
```

## Texture Pack Index Tooling

Maintain deterministic hi-res pack index files (`rkv2_pack_index_v1.tsv`):

```bash
# Generate canonical index rows from .rgba32 names and dimensions
python3 scripts/rvk2_texture_pack_index.py generate --pack-dir /path/to/pack

# Validate index contract and file-size consistency
python3 scripts/rvk2_texture_pack_index.py validate --pack-dir /path/to/pack
```

Runtime replacement observability:

```bash
# Optional log output (per-frame entries/pixels/samples/hits/misses)
REALITYVK_RVK2_TX_LOG_SUMMARY=1 <your-emulator-launch-command>
```

Runtime replacement control (hot reload without emulator restart):

```bash
# 1) Point rvk2 at a runtime control file
export REALITYVK_RVK2_TX_CONTROL_FILE=/tmp/rvk2_tx_control.txt

# 2) Configure replacement sources + optional summary file output
python3 scripts/rvk2_tx_control.py set \
  --control-file /tmp/rvk2_tx_control.txt \
  --enable 1 \
  --cache-path /path/to/pack.hts \
  --pack-path /path/to/pack \
  --summary-path /tmp/rvk2_tx_summary.txt \
  --log-summary 1

# 3) Force reload after updating cache/pack content
python3 scripts/rvk2_tx_control.py bump-reload --control-file /tmp/rvk2_tx_control.txt

# 4) Force invalidate/disable if needed
python3 scripts/rvk2_tx_control.py set --control-file /tmp/rvk2_tx_control.txt --enable 0
python3 scripts/rvk2_tx_control.py bump-invalidate --control-file /tmp/rvk2_tx_control.txt
```

## Key Paths

1. `src/Graphics/RealityVK2/`: rvk2 rewrite modules.
2. `src/Graphics/VulkanContext/`: current Vulkan backend implementation.
3. `scripts/`: local gate, parity, smoke, replay tools.
4. `tests/smoke/`: deterministic scenario manifests and baselines.
5. `docs/`: canonical project docs.

## Documentation Index

Start here:

1. `docs/README.md`
2. `docs/vulkan-core-rebuild-plan.md`
3. `docs/vulkan-core-status.md`
4. `docs/local-ci.md`
5. `docs/local-smoke.md`
6. `docs/n64-runtime-validation-checklist.md`

## Development Rules (Short Form)

1. Prefer correctness and determinism over heuristic hacks.
2. Keep changes measurable through traces/tests/smoke artifacts.
3. Do not reintroduce legacy OpenGL compatibility paths unless explicitly required.
4. Keep docs compact: one canonical plan and one canonical status tracker.

## License

RealityVK is distributed under GPLv2. See `LICENSE`.
