# Local CI and Validation

## Required Gate

```bash
./scripts/local_gate.sh
```

Default gate stages:
- stale-doc reference validation
- shellcheck (`REALITYVK_GATE_RUN_SHELLCHECK=1`)
- Python script tests (`REALITYVK_GATE_RUN_SCRIPT_TESTS=1`)
- release build + debug build
- `rvk2_unit_tests` (release + debug)
- `rvk2_conformance_tests` (release + debug)

## Gate with Smoke/Parity

```bash
REALITYVK_GATE_WITH_SMOKE=1 ./scripts/local_gate.sh
```

Smoke/parity defaults in gate mode:
- profile: `basic`
- candidate: this repo (`RealityVK`)
- reference: upstream `GLideN64`
- backend: Vulkan only
- packet replay check: enabled (`REALITYVK_GATE_RVK2_TRACE_REPLAY=1`)
- reference `GLideN64` dumpfb in agent-mode is expected black in this workflow; parity script bypasses reference non-black validation for that path.

## Deep Telemetry from One Smoke Run

```bash
REALITYVK_GATE_WITH_SMOKE=1 \
REALITYVK_GATE_SMOKE_DEEP_TELEMETRY=1 \
./scripts/local_gate.sh
```

Deep gate defaults:
- profile switched to `deep`
- telemetry root: `build/local-gate/paper-mario-telemetry/`
- stateful replay: `REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REPLAY_STATEFUL=1`
- replay report reuse: `REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REUSE_REPLAY_REPORT=1`

Key deep artifacts:
- `paper_mario_intro.candidate.packet.tsv`
- `paper_mario_intro.candidate.packet.replay.json`
- `paper_mario_intro.candidate.frame-forensics.tsv`
- `paper_mario_intro.candidate.missing-region-focus.json`
- `paper_mario_intro.candidate.command-census.json`
- `paper_mario_intro.candidate.history-merge.tsv`
- `paper_mario_intro.candidate.overwrite.tsv`
- `paper_mario_intro.candidate.triangle-packet.tsv`
- `paper_mario_intro.deviation/` (`diff.png`, `mask.png`, `overlay.png`, `boxes.json`, `summary.json`)
- `paper_mario_intro.telemetry.bundle.json`

## Core Validation Commands

```bash
./scripts/paper_mario_parity.sh
./scripts/local_smoke.sh
./scripts/paper_mario_compare_view.sh
```

Profile-based parity usage:

```bash
REALITYVK_PM_PROFILE=basic ./scripts/paper_mario_parity.sh
REALITYVK_PM_PROFILE=deep  ./scripts/paper_mario_parity.sh
```

Fast frame override:
```bash
REALITYVK_PM_FRAMES_OVERRIDE=20 ./scripts/paper_mario_parity.sh
```

Iteration helper commands:
```bash
./scripts/paper_mario_iterate.sh --preset fast --runs 1 --frames 20
./scripts/paper_mario_focus_deep.sh --frames 120
./scripts/paper_mario_shadow_ab.sh --frames 20
```

Deep profile safety defaults:
- candidate plugin freshness preflight: `REALITYVK_PM_CANDIDATE_PLUGIN_FRESHNESS_CHECK=1`
- telemetry root prune before run: `REALITYVK_PM_TELEMETRY_PRUNE_ENABLE=1`

Dry-run plan (no emulator launch):

```bash
REALITYVK_PM_DRY_RUN=1 ./scripts/paper_mario_parity.sh
```

## Baseline/Reference Refresh

```bash
REALITYVK_SMOKE_UPDATE_BASELINES=1 ./scripts/local_smoke.sh
REALITYVK_PM_REFRESH_REFERENCE=1 ./scripts/paper_mario_parity.sh
```

## Archive Compare

```bash
python3 scripts/rvk2_archive_compare.py \
  --index build/parity-runs/paper-mario/archive/index.tsv \
  --scenario paper_mario_intro \
  --json-out build/parity-runs/paper-mario/archive/compare.latest.json \
  --md-out build/parity-runs/paper-mario/archive/compare.latest.md
```

## Knob History

```bash
python3 scripts/rvk2_knob_history.py summary \
  --history build/parity-runs/paper-mario/knob-history.tsv \
  --scenario-id paper_mario_intro \
  --limit 20
```

## Replay Validation (Manual)

```bash
python3 scripts/rvk2_packet_trace_replay.py \
  --input build/local-gate/rvk2.packet.tsv \
  --forensics-file build/local-gate/rvk2.frame-forensics.tsv \
  --json-out build/local-gate/rvk2.packet.replay.json \
  --progress-interval-seconds 15 \
  --jobs 0 --strict
```

## Runtime/Trace Invariants

- Runtime path is `rvk2` only.
- Trace/packet schema tag is `rvk2_schema_v1`.
- Trace controls:
  - `REALITYVK_RVK2_TRACE_FILE`
  - `REALITYVK_RVK2_PACKET_TRACE_FILE`
