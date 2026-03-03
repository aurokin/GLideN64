# Local CI and Validation

## Required Gate

```bash
./scripts/local_gate.sh
```

Runs:
- release build
- debug build
- `rvk2_unit_tests` (release + debug)
- `rvk2_conformance_tests` (release + debug)

## Gate With Smoke/Parity

```bash
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
```

Smoke/parity configuration:
- candidate: this repo (`RealityVK`)
- comparison reference: upstream `GLideN64`
- smoke backend: `Vulkan` only
- packet trace replay: enabled in smoke gate

## Deep Telemetry (Single Smoke Run)

```bash
REALITYVK_GATE_WITH_SMOKE=1 \
REALITYVK_GATE_SMOKE_DEEP_TELEMETRY=1 \
./scripts/local_gate.sh
```

Deep telemetry artifacts (Paper Mario intro) are emitted under:
- `build/local-gate/paper-mario-telemetry/`
  - `paper_mario_intro.candidate.packet.tsv`
  - `paper_mario_intro.candidate.packet.replay.json`
  - `paper_mario_intro.candidate.frame-forensics.tsv`
  - `paper_mario_intro.candidate.frame-forensics.summary.txt`
  - `paper_mario_intro.candidate.frame-forensics.active.summary.txt`
  - `paper_mario_intro.candidate.trace.tsv`
  - `paper_mario_intro.candidate.launch.log`
  - `paper_mario_intro.telemetry.bundle.json`

Key deep-telemetry provenance fields:
- frame-forensics rows now include raw VI register snapshot plus VI stage hashes:
  - `vi_hash_decode`
  - `vi_hash_filter`
  - `vi_hash_gdither`
- frame-forensics rows also include pre-VI selected surface fingerprint:
  - `selected_surface_hash`
- replay/bundle correlation reports include stage and surface-hash mismatch counts for present-hash failures.

Deep telemetry replay defaults:
- `REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REPLAY_STATEFUL=1` (carry state across frames for lower-noise mismatch classification)
- `REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REUSE_REPLAY_REPORT=1` (reuse parity-generated replay report in gate smoke check)

## Core Validation Commands

```bash
./scripts/paper_mario_parity.sh
./scripts/local_smoke.sh
./scripts/paper_mario_compare_view.sh
```

For RVK2 logic-gap closure workflow, see `docs/vulkan-core-status.md`.

## Baseline/Reference Refresh

```bash
REALITYVK_SMOKE_UPDATE_BASELINES=1 ./scripts/local_smoke.sh
REALITYVK_PM_REFRESH_REFERENCE=1 ./scripts/paper_mario_parity.sh
```

## Replay Validation

```bash
python3 scripts/rvk2_packet_trace_replay.py \
  --input build/local-gate/rvk2.packet.tsv \
  --forensics-file build/local-gate/rvk2.frame-forensics.tsv \
  --json-out build/local-gate/rvk2.packet.replay.json \
  --jobs 0 --strict
```

When deep telemetry smoke is enabled, `local_gate.sh` auto-wires replay to
`paper_mario_intro.candidate.frame-forensics.tsv` if present.

## Runtime/Trace Invariants

- Runtime path is `rvk2` only.
- Schema tag for trace/packet tooling is `rvk2_schema_v1`.
- Trace controls:
  - `REALITYVK2_TRACE_FILE`
  - `REALITYVK2_PACKET_TRACE_FILE`
