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
  - `paper_mario_intro.candidate.command-census.json`
  - `paper_mario_intro.candidate.command-census.md`
  - `paper_mario_intro.candidate.missing-region-focus.json`
  - `paper_mario_intro.deviation/`
    - `diff.png`
    - `mask_raw.png`
    - `mask.png`
    - `overlay.png`
    - `boxes.json`
    - `summary.json`
    - `playbook_snippet.md`
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

Deep telemetry deviation/census defaults:
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_PLAYBOOK=1`
- `REALITYVK_PM_DEEP_TELEMETRY_COMMAND_CENSUS=1`

Diff playbook tuning knobs:
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_MODE` (default `missing_non_black`; options: `absdiff`, `missing_non_black`, `extra_non_black`)
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_THRESHOLD` (default `20`)
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_MIN_AREA` (default `256`)
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_MAX_BOXES` (default `32`)
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_DILATE` (default `1`)
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_REF_NONBLACK_THRESHOLD` (default `8`)
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_TEST_NONBLACK_THRESHOLD` (default `8`)
- `REALITYVK_PM_DEEP_TELEMETRY_DIFF_IGNORE_BOXES` (default `238,245,482,380`; semicolon-separated `x0,y0,x1,y1` boxes)

Command census tuning knob:
- `REALITYVK_PM_DEEP_TELEMETRY_COMMAND_FOCUS_WINDOW` (default `1`, frames around replay first-failure)

## Core Validation Commands

```bash
./scripts/paper_mario_parity.sh
./scripts/local_smoke.sh
./scripts/paper_mario_compare_view.sh
```

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

## Archive Compare

```bash
python3 scripts/rvk2_archive_compare.py \
  --index build/parity-runs/paper-mario/archive/index.tsv \
  --scenario paper_mario_intro \
  --json-out build/parity-runs/paper-mario/archive/compare.latest.json \
  --md-out build/parity-runs/paper-mario/archive/compare.latest.md
```

## Runtime/Trace Invariants

- Runtime path is `rvk2` only.
- Schema tag for trace/packet tooling is `rvk2_schema_v1`.
- Trace controls:
  - `REALITYVK2_TRACE_FILE`
  - `REALITYVK2_PACKET_TRACE_FILE`
