# Local CI and Validation

## Required Gate

```bash
./scripts/local_gate.sh
```

## Gate Stages

- Release build (`src/`, Mupen plugin target).
- Debug build.
- `rvk2_unit_tests` on both builds.
- `rvk2_conformance_tests` on both builds.

## Smoke and Parity Gate

```bash
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
```

- Candidate: this repo's `RealityVK` Vulkan plugin.
- Reference: upstream `GLideN64` plugin (default in parity script).
- Smoke backend support: `Vulkan` only.
- Trace replay check: enabled by default in smoke gate.

## Core Validation Commands

```bash
# Paper Mario parity (upstream GLideN64 vs RealityVK)
./scripts/paper_mario_parity.sh

# Vulkan-only deterministic smoke
./scripts/local_smoke.sh

# Visual triptych (reference/candidate/diff)
./scripts/paper_mario_compare_view.sh
```

## Baseline and Reference Refresh

```bash
# Update committed Vulkan smoke checksums
REALITYVK_SMOKE_UPDATE_BASELINES=1 ./scripts/local_smoke.sh

# Refresh cached parity reference capture
REALITYVK_PM_REFRESH_REFERENCE=1 ./scripts/paper_mario_parity.sh
```

## Replay Validation

```bash
python3 scripts/rvk2_packet_trace_replay.py \
  --input build/local-gate/rvk2.packet.tsv \
  --json-out build/local-gate/rvk2.packet.replay.json \
  --jobs 0 --strict
```

## Frame Forensics Summary

```bash
REALITYVK2_FRAME_FORENSICS_FILE=/tmp/rvk2-forensics.tsv ./scripts/paper_mario_parity.sh
python3 scripts/rvk2_forensics_summary.py --input /tmp/rvk2-forensics.tsv --active-only
```

Key outputs to watch:
- stage deltas: `stage_texel_to_combiner_delta_rate`, `stage_combiner_to_blender_delta_rate`, `stage_texel_to_final_delta_rate`
- dominant packet classes: `stage_*_top_classes` (bucket tags such as `b10`, `b11`)
- texel source split per write: `stage_texel_source_tmem_rate`, `stage_texel_source_rdram_rate`
- workload mix: `work_*_share` and `write_*_share`

## Texture Pack Utilities

```bash
python3 scripts/rvk2_texture_pack_index.py generate --pack-dir /path/to/pack
python3 scripts/rvk2_texture_pack_index.py validate --pack-dir /path/to/pack
```

## Determinism Utility

```bash
./scripts/rvk2_trace_determinism.sh
```
